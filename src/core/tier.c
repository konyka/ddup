/* tier.c - append-only cold log and record index; see tier.h. */
#include "core/tier.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/rhtable.h"
#include "pal/pal_file.h"
#include "pal/pal_platform.h"

#define TIER_MAGIC "DDUPTC1"
#define TIER_MAGIC_LEN 8
#define TIER_OP_PUT 1
#define TIER_OP_DEL 2
#define TIER_OP_FLUSH_DB 3
#define TIER_OP_FLUSH_ALL 4

typedef struct tier_loc {
    uint64_t off;
    uint32_t len;
    uint32_t vlen;
    uint32_t value_off;
    uint32_t db_index;
} tier_loc;

struct tier_store {
    pal_file *f;
    char *path;
    rh_table index;       /* record_id bytes -> tier_loc */
    uint64_t next_id;
    uint64_t end;
    uint64_t max_disk_bytes;
    uint64_t disk_bytes;
    int failed;
};

typedef struct collect_ctx {
    uint64_t *rids;
    size_t n;
    size_t cap;
    int fail;
    unsigned int db_index;
} collect_ctx;
static void flush_collect_cb(const char *key, size_t klen, const char *val,
                             size_t vlen, void *arg);

static void put_u32le(unsigned char *p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++)
        p[i] = (unsigned char)((v >> (8 * i)) & 0xFFu);
}

static void put_u64le(unsigned char *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (unsigned char)((v >> (8 * i)) & 0xFFu);
}

static uint32_t get_u32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64le(const unsigned char *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--)
        v = (v << 8) | (uint64_t)p[i];
    return v;
}

static int file_write_all(pal_file *f, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        ptrdiff_t w = pal_file_write(f, p, n);
        if (w <= 0)
            return -1;
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

static int file_read_all(pal_file *f, void *buf, size_t n)
{
    char *p = (char *)buf;
    while (n > 0) {
        ptrdiff_t r = pal_file_read(f, p, n);
        if (r <= 0)
            return -1;
        p += (size_t)r;
        n -= (size_t)r;
    }
    return 0;
}

static int file_skip_exact(pal_file *f, uint64_t n)
{
    unsigned char scratch[4096];
    while (n > 0) {
        size_t want = n > sizeof(scratch) ? sizeof(scratch) : (size_t)n;
        ptrdiff_t r = pal_file_read(f, scratch, want);
        if (r <= 0 || (size_t)r != want)
            return -1;
        n -= (uint64_t)r;
    }
    return 0;
}

static int tier_advance_id(tier_store *t, uint64_t rid)
{
    if (rid == UINT64_MAX)
        return -1;
    if (rid >= t->next_id)
        t->next_id = rid + 1;
    return 0;
}

static int index_set(rh_table *idx, uint64_t rid, const tier_loc *loc)
{
    unsigned char key[8];
    put_u64le(key, rid);
    return rh_set(idx, (const char *)key, sizeof(key),
                  (const char *)loc, sizeof(*loc)) < 0
               ? -1
               : 0;
}

static int index_get(const rh_table *idx, uint64_t rid, tier_loc *loc)
{
    unsigned char key[8];
    const char *v;
    size_t vl;
    put_u64le(key, rid);
    if (!rh_get((rh_table *)idx, (const char *)key, sizeof(key), &v, &vl) ||
        vl != sizeof(*loc))
        return 0;
    memcpy(loc, v, sizeof(*loc));
    return 1;
}

static void index_del(rh_table *idx, uint64_t rid)
{
    unsigned char key[8];
    put_u64le(key, rid);
    rh_del(idx, (const char *)key, sizeof(key));
}

static int append_record(tier_store *t, unsigned char *hdr, size_t hdrlen,
                         const char *key, size_t klen, const char *val,
                         size_t vlen)
{
    uint64_t total;
    if (t->failed)
        return -1;
    if (hdrlen > SIZE_MAX - klen || hdrlen + klen > SIZE_MAX - vlen)
        return -1;
    total = (uint64_t)(hdrlen + klen + vlen);
    if (t->end > UINT64_MAX - total)
        return -1;
    if (t->max_disk_bytes != 0 &&
        (t->end > t->max_disk_bytes || hdrlen > t->max_disk_bytes - t->end ||
         klen > t->max_disk_bytes - t->end - hdrlen ||
         vlen > t->max_disk_bytes - t->end - hdrlen - klen))
        return -1;
    if (pal_file_seek(t->f, t->end) != 0)
        goto fail;
    if (file_write_all(t->f, hdr, hdrlen) != 0)
        goto fail;
    if (klen > 0 && file_write_all(t->f, key, klen) != 0)
        goto fail;
    if (vlen > 0 && file_write_all(t->f, val, vlen) != 0)
        goto fail;
    if (pal_file_flush(t->f) != 0)
        goto fail;
    t->end += hdrlen + klen + vlen;
    t->disk_bytes = t->end;
    return 0;
fail:
    t->failed = 1;
    return -1;
}

int tier_open(tier_store **out, const char *path, uint64_t max_disk_bytes)
{
    tier_store *t;
    pal_file *f;
    int existed;

    if (out == NULL || path == NULL || path[0] == '\0')
        return -1;
    *out = NULL;
    existed = pal_file_exists(path);
    f = pal_file_open_update(path);
    if (f == NULL)
        return -1;

    t = (tier_store *)calloc(1, sizeof(*t));
    if (t == NULL) {
        pal_file_close(f);
        return -1;
    }
    t->path = strdup(path);
    if (t->path == NULL) {
        pal_file_close(f);
        free(t);
        return -1;
    }
    rh_init(&t->index);
    t->f = f;
    t->max_disk_bytes = max_disk_bytes;
    t->next_id = 1;
    t->end = 0;
    t->disk_bytes = 0;

    if (!existed) {
        if (file_write_all(f, TIER_MAGIC, TIER_MAGIC_LEN) != 0 ||
            pal_file_flush(f) != 0) {
            t->failed = 1;
            tier_close(t);
            return -1;
        }
        t->end = TIER_MAGIC_LEN;
        t->disk_bytes = t->end;
        *out = t;
        return 0;
    }

    /* Replay existing records. */
    {
        unsigned char magic[TIER_MAGIC_LEN];
        if (pal_file_seek(f, 0) != 0 ||
            file_read_all(f, magic, sizeof(magic)) != 0 ||
            memcmp(magic, TIER_MAGIC, sizeof(magic)) != 0) {
            tier_close(t);
            return -1;
        }
        t->end = TIER_MAGIC_LEN;
        for (;;) {
            unsigned char hdr[26];
            ptrdiff_t got;
            if (pal_file_seek(f, t->end) != 0) {
                t->failed = 1;
                break;
            }
            got = pal_file_read(f, hdr, sizeof(hdr));
            if (got == 0)
                break;
            if (got != (ptrdiff_t)sizeof(hdr)) {
                t->failed = 1;
                break;
            }
            {
                unsigned char op = hdr[0];
                uint32_t klen = get_u32le(hdr + 2);
                uint32_t vlen = get_u32le(hdr + 6);
                uint64_t expire = get_u64le(hdr + 10);
                uint64_t rid = get_u64le(hdr + 18);
                uint64_t body = (uint64_t)klen + vlen;
                tier_loc loc;
                if (body > UINT32_MAX - sizeof(hdr) ||
                    tier_advance_id(t, rid) != 0)
                    {
                        t->failed = 1;
                        break;
                    }
                if (t->end > UINT64_MAX - sizeof(hdr) - body ||
                    file_skip_exact(f, body) != 0) {
                    t->failed = 1;
                    break;
                }
                if (op == TIER_OP_FLUSH_ALL) {
                    rh_destroy(&t->index);
                    rh_init(&t->index);
                    t->end += sizeof(hdr) + body;
                    continue;
                }
                if (op == TIER_OP_FLUSH_DB) {
                    collect_ctx c;
                    memset(&c, 0, sizeof(c));
                    c.db_index = hdr[1];
                    rh_each(&t->index, flush_collect_cb, &c);
                    if (!c.fail) {
                        size_t j;
                        for (j = 0; j < c.n; j++)
                            index_del(&t->index, c.rids[j]);
                    }
                    free(c.rids);
                    if (c.fail)
                        t->failed = 1;
                    t->end += sizeof(hdr) + body;
                    continue;
                }
                if (op == TIER_OP_DEL) {
                    index_del(&t->index, rid);
                    t->end += sizeof(hdr) + body;
                    continue;
                }
                if (op != TIER_OP_PUT) {
                    t->failed = 1;
                    break;
                }
                loc.off = t->end;
                loc.len = (uint32_t)(sizeof(hdr) + body);
                loc.vlen = vlen;
                loc.value_off = (uint32_t)(sizeof(hdr) + klen);
                loc.db_index = hdr[1];
                if (pal_file_seek(f, t->end + (uint64_t)body) != 0) {
                    t->failed = 1;
                    break;
                }
                if (index_set(&t->index, rid, &loc) != 0) {
                    t->failed = 1;
                    break;
                }
                (void)expire;
                t->end += sizeof(hdr) + body;
            }
        }
        t->disk_bytes = t->end;
        if (t->failed) {
            tier_close(t);
            return -1;
        }
    }

    *out = t;
    return 0;
}

void tier_close(tier_store *t)
{
    if (t == NULL)
        return;
    if (t->f != NULL)
        pal_file_close(t->f);
    rh_destroy(&t->index);
    free(t->path);
    free(t);
}

int tier_put(tier_store *t, unsigned int db_index, const char *key,
             size_t klen, const char *val, size_t vlen, uint64_t expire_ms,
             uint64_t *record_id)
{
    unsigned char hdr[26];
    uint64_t rid;
    tier_loc loc;
    size_t body;
    if (t == NULL || t->failed || db_index > 255 ||
        (key == NULL && klen != 0) || (val == NULL && vlen != 0) ||
        klen > UINT32_MAX || vlen > UINT32_MAX)
        return -1;
    rid = t->next_id;
    body = klen + vlen;
    if (t->next_id == 0 || t->next_id == UINT64_MAX ||
        body < klen || body < vlen || body > UINT32_MAX - sizeof(hdr) ||
        (uint64_t)body > UINT64_MAX - sizeof(hdr))
        return -1;
    hdr[0] = TIER_OP_PUT;
    hdr[1] = (unsigned char)db_index;
    put_u32le(hdr + 2, (uint32_t)klen);
    put_u32le(hdr + 6, (uint32_t)vlen);
    put_u64le(hdr + 10, expire_ms);
    put_u64le(hdr + 18, rid);
    loc.off = t->end;
    loc.len = (uint32_t)(sizeof(hdr) + body);
    loc.vlen = (uint32_t)vlen;
    loc.value_off = (uint32_t)(sizeof(hdr) + klen);
    loc.db_index = db_index;
    if (append_record(t, hdr, sizeof(hdr), key, klen, val, vlen) != 0)
        return -1;
    if (index_set(&t->index, rid, &loc) != 0) {
        t->failed = 1;
        return -1;
    }
    t->next_id = rid + 1;
    if (record_id != NULL)
        *record_id = rid;
    return 0;
}

int tier_get(tier_store *t, uint64_t record_id, char **val, size_t *vlen,
             uint64_t *expire_ms)
{
    tier_loc loc;
    char *buf;
    if (t == NULL || t->failed || val == NULL || vlen == NULL)
        return -1;
    if (!index_get(&t->index, record_id, &loc))
        return -1;
    buf = (char *)malloc(loc.vlen == 0 ? 1 : loc.vlen);
    if (buf == NULL)
        return -1;
    if (pal_file_seek(t->f, loc.off + loc.value_off) != 0 ||
        file_read_all(t->f, buf, loc.vlen) != 0) {
        free(buf);
        t->failed = 1;
        return -1;
    }
    *val = buf;
    *vlen = loc.vlen;
    if (expire_ms != NULL) {
        unsigned char hdr[26];
        if (pal_file_seek(t->f, loc.off) != 0 ||
            file_read_all(t->f, hdr, sizeof(hdr)) != 0) {
            free(buf);
            t->failed = 1;
            return -1;
        }
        *expire_ms = get_u64le(hdr + 10);
    }
    return 0;
}

int tier_del(tier_store *t, uint64_t record_id)
{
    unsigned char hdr[26];
    if (t == NULL || t->failed)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = TIER_OP_DEL;
    hdr[1] = 0;
    put_u64le(hdr + 18, record_id);
    if (append_record(t, hdr, sizeof(hdr), NULL, 0, NULL, 0) != 0)
        return -1;
    index_del(&t->index, record_id);
    return 0;
}

typedef struct compact_item {
    uint64_t rid;
    tier_loc loc;
} compact_item;

typedef struct compact_ctx {
    compact_item *items;
    size_t n;
    size_t cap;
    int fail;
} compact_ctx;

static void flush_collect_cb(const char *key, size_t klen, const char *val,
                             size_t vlen, void *arg)
{
    collect_ctx *c = (collect_ctx *)arg;
    tier_loc loc;
    if (c->fail || klen != 8 || vlen != sizeof(tier_loc))
        return;
    memcpy(&loc, val, sizeof(loc));
    if (loc.db_index != c->db_index)
        return;
    if (c->n == c->cap) {
        size_t ncap = c->cap == 0 ? 256 : c->cap * 2;
        uint64_t *ni = (uint64_t *)realloc(c->rids, ncap * sizeof(*ni));
        if (ni == NULL) {
            c->fail = 1;
            return;
        }
        c->rids = ni;
        c->cap = ncap;
    }
    c->rids[c->n++] = get_u64le((const unsigned char *)key);
}

int tier_flush_db(tier_store *t, unsigned int db_index)
{
    unsigned char hdr[26];
    collect_ctx c;
    size_t i;
    if (t == NULL || t->failed || db_index > 255)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = TIER_OP_FLUSH_DB;
    hdr[1] = (unsigned char)db_index;
    if (append_record(t, hdr, sizeof(hdr), NULL, 0, NULL, 0) != 0)
        return -1;
    memset(&c, 0, sizeof(c));
    c.db_index = db_index;
    rh_each(&t->index, flush_collect_cb, &c);
    if (c.fail) {
        free(c.rids);
        t->failed = 1;
        return -1;
    }
    for (i = 0; i < c.n; i++)
        index_del(&t->index, c.rids[i]);
    free(c.rids);
    return 0;
}

static void compact_collect_cb(const char *key, size_t klen,
                               const char *val, size_t vlen, void *arg)
{
    compact_ctx *c = (compact_ctx *)arg;
    compact_item it;
    (void)key;
    if (c->fail || klen != 8 || vlen != sizeof(tier_loc))
        return;
    it.rid = get_u64le((const unsigned char *)key);
    memcpy(&it.loc, val, sizeof(it.loc));
    if (c->n == c->cap) {
        size_t ncap = c->cap == 0 ? 256 : c->cap * 2;
        compact_item *ni = (compact_item *)realloc(c->items,
                                                   ncap * sizeof(*ni));
        if (ni == NULL) {
            c->fail = 1;
            return;
        }
        c->items = ni;
        c->cap = ncap;
    }
    c->items[c->n++] = it;
}

int tier_compact(tier_store *t)
{
    char *tmp_path;
    pal_file *nf;
    compact_ctx c;
    uint64_t new_end;
    size_t i;
    int ok = 1;

    if (t == NULL || t->failed || t->path == NULL || t->path[0] == '\0')
        return -1;
    {
        size_t plen = strlen(t->path);
        if (plen > SIZE_MAX - sizeof(".tmp"))
            return -1;
        tmp_path = (char *)malloc(plen + sizeof(".tmp"));
    }
    if (tmp_path == NULL)
        return -1;
    (void)snprintf(tmp_path, strlen(t->path) + sizeof(".tmp"), "%s.tmp",
                   t->path);
    nf = pal_file_open_write(tmp_path);
    if (nf == NULL) {
        free(tmp_path);
        return -1;
    }
    if (file_write_all(nf, TIER_MAGIC, TIER_MAGIC_LEN) != 0) {
        ok = 0;
    }
    new_end = TIER_MAGIC_LEN;
    memset(&c, 0, sizeof(c));
    if (ok) {
        rh_each(&t->index, compact_collect_cb, &c);
        ok = !c.fail;
    }
    if (ok) {
        for (i = 0; i < c.n; i++) {
            char *rec = NULL;
            uint64_t off = new_end;
            /* Read the full old record, then append it unchanged. */
            rec = (char *)malloc(c.items[i].loc.len);
            if (rec == NULL) {
                ok = 0;
                break;
            }
            if (pal_file_seek(t->f, c.items[i].loc.off) != 0 ||
                file_read_all(t->f, rec, c.items[i].loc.len) != 0 ||
                file_write_all(nf, rec, c.items[i].loc.len) != 0) {
                free(rec);
                ok = 0;
                break;
            }
            free(rec);
            c.items[i].loc.off = off;
            if (new_end > UINT64_MAX - c.items[i].loc.len) {
                ok = 0;
                break;
            }
            new_end += c.items[i].loc.len;
        }
    }
    if (ok && pal_file_flush(nf) != 0)
        ok = 0;
    if (pal_file_close(nf) != 0)
        ok = 0;
    if (!ok) {
        free(c.items);
        pal_file_unlink(tmp_path);
        free(tmp_path);
        t->failed = 1;
        return -1;
    }

    /* Swap files and rebuild the index from collected offsets. */
    if (pal_file_close(t->f) != 0) {
        free(c.items);
        free(tmp_path);
        t->failed = 1;
        return -1;
    }
    if (pal_file_rename(tmp_path, t->path) != 0) {
        free(c.items);
        t->f = pal_file_open_update(t->path);
        free(tmp_path);
        t->failed = 1;
        return -1;
    }
    free(tmp_path);
    t->f = pal_file_open_update(t->path);
    if (t->f == NULL) {
        free(c.items);
        t->failed = 1;
        return -1;
    }
    rh_destroy(&t->index);
    rh_init(&t->index);
    for (i = 0; i < c.n; i++)
        if (index_set(&t->index, c.items[i].rid, &c.items[i].loc) != 0) {
            free(c.items);
            t->failed = 1;
            return -1;
        }
    free(c.items);
    t->end = new_end;
    t->disk_bytes = new_end;
    return 0;
}

uint64_t tier_disk_bytes(const tier_store *t)
{
    return t == NULL ? 0 : t->disk_bytes;
}

uint64_t tier_live_records(const tier_store *t)
{
    return t == NULL ? 0 : (uint64_t)rh_size(&t->index);
}

int tier_failed(const tier_store *t)
{
    return t == NULL || t->failed;
}
