/* snapshot.c - RDB-style binary snapshot; see snapshot.h for the format. */
#include "core/snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds/obj.h"
#include "core/crc64.h"
#include "pal/pal_file.h"
#include "resp/resp_writer.h"

/* ------------------------------------------------------------------ */
/* little-endian write helpers (resp_buf append)                      */
/* ------------------------------------------------------------------ */

static void buf_u8(resp_buf *b, uint8_t v)
{
    resp_buf_reserve(b, 1);
    b->data[b->len++] = (char)v;
}

static void buf_u16le(resp_buf *b, uint16_t v)
{
    int i;
    resp_buf_reserve(b, 2);
    for (i = 0; i < 2; i++)
        b->data[b->len++] = (char)((v >> (8 * i)) & 0xFFu);
}

static void buf_u32le(resp_buf *b, uint32_t v)
{
    int i;
    resp_buf_reserve(b, 4);
    for (i = 0; i < 4; i++)
        b->data[b->len++] = (char)((v >> (8 * i)) & 0xFFu);
}

static void buf_u64le(resp_buf *b, uint64_t v)
{
    int i;
    resp_buf_reserve(b, 8);
    for (i = 0; i < 8; i++)
        b->data[b->len++] = (char)((v >> (8 * i)) & 0xFFu);
}

static void buf_f64le(resp_buf *b, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    buf_u64le(b, u);
}

static void buf_bytes(resp_buf *b, const char *p, size_t n)
{
    resp_buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

/* ------------------------------------------------------------------ */
/* save                                                               */
/* ------------------------------------------------------------------ */

typedef struct save_ctx {
    db *d;
    resp_buf *buf;
} save_ctx;

/* append a raw rh entry (field/member) list item: u32 len + bytes */
static void dump_pair_cb(const char *f, size_t flen, const char *v,
                         size_t vlen, void *c)
{
    resp_buf *buf = (resp_buf *)c;
    buf_u32le(buf, (uint32_t)flen);
    buf_bytes(buf, f, flen);
    buf_u32le(buf, (uint32_t)vlen);
    buf_bytes(buf, v, vlen);
}

static void dump_member_cb(const char *m, size_t mlen, const char *v,
                           size_t vlen, void *c)
{
    resp_buf *buf = (resp_buf *)c;
    (void)v;
    (void)vlen;
    buf_u32le(buf, (uint32_t)mlen);
    buf_bytes(buf, m, mlen);
}

/* append a value's payload (no key/expiry) in the per-type encoding */
static void write_value_payload(resp_buf *buf, int tag, const char *val,
                                size_t vlen)
{
    switch (tag) {
    case DDUP_OBJ_STRING: {
        const char *s;
        size_t sl;
        obj_str(val, vlen, &s, &sl);
        buf_u32le(buf, (uint32_t)sl);
        buf_bytes(buf, s, sl);
        break;
    }
    case DDUP_OBJ_HASH: {
        obj_hash *h = (obj_hash *)obj_unpack_ptr(val, vlen);
        buf_u32le(buf, (uint32_t)rh_size(&h->fields));
        rh_each(&h->fields, dump_pair_cb, buf);
        break;
    }
    case DDUP_OBJ_LIST: {
        obj_list *l = (obj_list *)obj_unpack_ptr(val, vlen);
        list_node *n;
        buf_u32le(buf, (uint32_t)l->len);
        for (n = l->head; n != NULL; n = n->next) {
            buf_u32le(buf, n->len);
            buf_bytes(buf, n->data, n->len);
        }
        break;
    }
    case DDUP_OBJ_SET: {
        obj_set *st = (obj_set *)obj_unpack_ptr(val, vlen);
        buf_u32le(buf, (uint32_t)rh_size(&st->members));
        rh_each(&st->members, dump_member_cb, buf);
        break;
    }
    case DDUP_OBJ_ZSET: {
        obj_zset *z = (obj_zset *)obj_unpack_ptr(val, vlen);
        zsl_node *n;
        buf_u32le(buf, (uint32_t)rh_size(&z->dict));
        for (n = z->sl->header->forward[0]; n != NULL; n = n->forward[0]) {
            buf_u32le(buf, n->mlen);
            buf_bytes(buf, n->member, n->mlen);
            buf_f64le(buf, n->score);
        }
        break;
    }
    default:
        break; /* unknown tag: skipped by the loader too */
    }
}

static void save_entry_cb(const char *key, size_t klen, const char *val,
                          size_t vlen, void *c)
{
    save_ctx *ctx = (save_ctx *)c;
    resp_buf *buf = ctx->buf;
    int tag = obj_tag_of(val, vlen);
    const char *ev;
    size_t evl;
    int64_t expire = -1;

    if (rh_get(&ctx->d->expires, key, klen, &ev, &evl) && evl == 8) {
        uint64_t u;
        memcpy(&u, ev, 8);
        expire = (int64_t)u;
    }
    buf_u8(buf, (uint8_t)tag);
    buf_u32le(buf, (uint32_t)klen);
    buf_bytes(buf, key, klen);
    buf_u64le(buf, (uint64_t)expire);
    write_value_payload(buf, tag, val, vlen);
}

void snapshot_serialize(db *d, resp_buf *out)
{
    save_ctx ctx;
    buf_bytes(out, "DDUP0001", 8);
    ctx.d = d;
    ctx.buf = out;
    rh_each(&d->table, save_entry_cb, &ctx);
}

int snapshot_save(db *d, const char *path)
{
    resp_buf buf;
    char tmp[1024];
    pal_file *f;
    int rc = -1;

    resp_buf_init(&buf);
    snapshot_serialize(d, &buf);

    if (strlen(path) + 5 < sizeof(tmp)) {
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        f = pal_file_open_write(tmp);
        if (f != NULL) {
            if (pal_file_write(f, buf.data, buf.len) == (ptrdiff_t)buf.len &&
                pal_file_flush(f) == 0) {
                pal_file_close(f);
                rc = pal_file_rename(tmp, path);
            } else {
                pal_file_close(f);
                pal_file_unlink(tmp);
            }
        }
    }
    resp_buf_free(&buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* load                                                               */
/* ------------------------------------------------------------------ */

typedef struct reader {
    const char *p;
    size_t len;
    size_t off;
    int ok;
} reader;

static uint8_t rd_u8(reader *r)
{
    if (r->off + 1 > r->len) {
        r->ok = 0;
        return 0;
    }
    return (uint8_t)r->p[r->off++];
}

static uint32_t rd_u32le(reader *r)
{
    uint32_t v = 0;
    int i;
    if (r->off + 4 > r->len) {
        r->ok = 0;
        r->off = r->len;
        return 0;
    }
    for (i = 0; i < 4; i++)
        v |= (uint32_t)(uint8_t)r->p[r->off++] << (8 * i);
    return v;
}

static uint64_t rd_u64le(reader *r)
{
    uint64_t v = 0;
    int i;
    if (r->off + 8 > r->len) {
        r->ok = 0;
        r->off = r->len;
        return 0;
    }
    for (i = 0; i < 8; i++)
        v |= (uint64_t)(uint8_t)r->p[r->off++] << (8 * i);
    return v;
}

static double rd_f64le(reader *r)
{
    uint64_t u = rd_u64le(r);
    double v = 0.0;
    memcpy(&v, &u, 8);
    return v;
}

static const char *rd_bytes(reader *r, size_t n)
{
    const char *p;
    if (!r->ok || r->off + n > r->len) {
        r->ok = 0;
        return NULL;
    }
    p = r->p + r->off;
    r->off += n;
    return p;
}

/* Parse one entry's payload into a value blob. For STRINGs a fresh tagged
 * blob is malloc'd and returned (caller frees); for objects NULL is
 * returned and the 9-byte packed pointer lands in `blob`. *out_len is the
 * blob length in both cases. Sets r->ok = 0 on truncation/bad tag. */
static char *load_payload(reader *r, int tag, char blob[9], size_t *out_len)
{
    uint32_t n, i;
    switch (tag) {
    case DDUP_OBJ_STRING: {
        uint32_t sl = rd_u32le(r);
        const char *s = rd_bytes(r, sl);
        char *b;
        if (!r->ok)
            return NULL;
        b = (char *)malloc((size_t)sl + 1);
        if (b == NULL) {
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        b[0] = (char)DDUP_OBJ_STRING;
        memcpy(b + 1, s, sl);
        *out_len = (size_t)sl + 1;
        return b;
    }
    case DDUP_OBJ_HASH: {
        obj_hash *h = obj_hash_new();
        n = rd_u32le(r);
        for (i = 0; i < n && r->ok; i++) {
            uint32_t fl = rd_u32le(r);
            const char *f = rd_bytes(r, fl);
            uint32_t vl = rd_u32le(r);
            const char *v = rd_bytes(r, vl);
            if (r->ok)
                obj_hash_set(h, f, fl, v, vl);
        }
        if (!r->ok) {
            obj_hash_free(h);
            return NULL;
        }
        obj_pack_ptr(blob, DDUP_OBJ_HASH, h);
        *out_len = 9;
        return NULL;
    }
    case DDUP_OBJ_LIST: {
        obj_list *l = obj_list_new();
        n = rd_u32le(r);
        for (i = 0; i < n && r->ok; i++) {
            uint32_t el = rd_u32le(r);
            const char *e = rd_bytes(r, el);
            if (r->ok)
                obj_list_push(l, 0, e, el);
        }
        if (!r->ok) {
            obj_list_free(l);
            return NULL;
        }
        obj_pack_ptr(blob, DDUP_OBJ_LIST, l);
        *out_len = 9;
        return NULL;
    }
    case DDUP_OBJ_SET: {
        obj_set *st = obj_set_new();
        n = rd_u32le(r);
        for (i = 0; i < n && r->ok; i++) {
            uint32_t ml = rd_u32le(r);
            const char *m = rd_bytes(r, ml);
            if (r->ok)
                obj_set_add(st, m, ml);
        }
        if (!r->ok) {
            obj_set_free(st);
            return NULL;
        }
        obj_pack_ptr(blob, DDUP_OBJ_SET, st);
        *out_len = 9;
        return NULL;
    }
    case DDUP_OBJ_ZSET: {
        obj_zset *z = obj_zset_new();
        n = rd_u32le(r);
        for (i = 0; i < n && r->ok; i++) {
            uint32_t ml = rd_u32le(r);
            const char *m = rd_bytes(r, ml);
            double sc = rd_f64le(r);
            if (r->ok)
                obj_zset_add(z, m, ml, sc);
        }
        if (!r->ok) {
            obj_zset_free(z);
            return NULL;
        }
        obj_pack_ptr(blob, DDUP_OBJ_ZSET, z);
        *out_len = 9;
        return NULL;
    }
    default:
        r->ok = 0;
        return NULL;
    }
}

int snapshot_load_mem(db *d, const char *buf, size_t len, uint64_t now_ms)
{
    reader r;
    db tmp;
    int ok;

    if (len < 8 || memcmp(buf, "DDUP0001", 8) != 0)
        return -1;
    r.p = buf;
    r.len = len;
    r.off = 8;
    r.ok = 1;

    /* parse into a temporary db: all-or-nothing */
    db_init(&tmp);
    while (r.ok && r.off < r.len) {
        int tag = rd_u8(&r);
        uint32_t klen = rd_u32le(&r);
        const char *key = rd_bytes(&r, klen);
        int64_t expire;
        char blob[9];
        char *owned;
        const char *vblob;
        size_t vbloblen = 0;
        if (!r.ok)
            break;
        expire = (int64_t)rd_u64le(&r);
        owned = load_payload(&r, tag, blob, &vbloblen);
        if (!r.ok)
            break;
        vblob = owned != NULL ? owned : blob;
        if (expire >= 0 && (uint64_t)expire <= now_ms) {
            /* already dead at load time: skip */
            if (owned != NULL)
                free(owned);
            else
                obj_free_value(vblob, vbloblen);
            continue;
        }
        db_install_blob(&tmp, key, klen, vblob, vbloblen, now_ms);
        if (expire >= 0)
            db_install_expiry(&tmp, key, klen, (uint64_t)expire);
        if (owned != NULL)
            free(owned);
    }
    ok = r.ok && r.off == r.len;

    if (!ok) {
        db_destroy(&tmp);
        return -1;
    }
    db_destroy(d);
    *d = tmp; /* move the parsed db into place */
    return 0;
}

int snapshot_load(db *d, const char *path, uint64_t now_ms)
{
    pal_file *f = pal_file_open_read(path);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int rc;

    if (f == NULL)
        return -1;
    for (;;) {
        ptrdiff_t n;
        if (len == cap) {
            size_t ncap = cap == 0 ? 65536 : cap * 2;
            char *nb = (char *)realloc(buf, ncap);
            if (nb == NULL) {
                free(buf);
                pal_file_close(f);
                return -1;
            }
            buf = nb;
            cap = ncap;
        }
        n = pal_file_read(f, buf + len, cap - len);
        if (n < 0) {
            free(buf);
            pal_file_close(f);
            return -1;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    pal_file_close(f);

    rc = snapshot_load_mem(d, buf, len, now_ms);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* per-key serialization (DUMP/RESTORE/MIGRATE)                       */
/* ------------------------------------------------------------------ */

int snapshot_dump_key(db *d, const char *key, size_t klen, resp_buf *out)
{
    const char *v;
    size_t vl, start;

    if (!rh_get(&d->table, key, klen, &v, &vl))
        return -1;
    start = out->len;
    buf_u16le(out, SNAPSHOT_DUMP_VERSION);
    buf_u8(out, (uint8_t)obj_tag_of(v, vl));
    write_value_payload(out, obj_tag_of(v, vl), v, vl);
    buf_u64le(out, crc64(0, out->data + start, out->len - start));
    return 0;
}

int snapshot_restore_key(db *d, const char *key, size_t klen,
                         const char *payload, size_t plen,
                         uint64_t expire_ms, int replace, uint64_t now_ms)
{
    reader r;
    uint16_t version;
    int tag;
    char blob[9];
    char *owned;
    const char *vblob;
    size_t vbloblen = 0;
    uint64_t stored = 0;
    int i;

    db_expire_if_needed(d, key, klen, now_ms);
    if (!replace && rh_get(&d->table, key, klen, &vblob, &vbloblen))
        return 1;

    if (plen < 11) /* u16 version + u8 tag + u64 crc */
        return -1;
    for (i = 0; i < 8; i++)
        stored |= (uint64_t)(uint8_t)payload[plen - 8 + i] << (8 * i);
    if (crc64(0, payload, plen - 8) != stored)
        return -1;

    r.p = payload;
    r.len = plen - 8;
    r.off = 0;
    r.ok = 1;
    version = (uint16_t)(rd_u8(&r) | ((uint16_t)rd_u8(&r) << 8));
    if (version != SNAPSHOT_DUMP_VERSION)
        return -1;
    tag = rd_u8(&r);
    owned = load_payload(&r, tag, blob, &vbloblen);
    if (!r.ok || r.off != r.len) {
        if (r.ok) {
            /* parsed but trailing bytes remain: free the decoded value */
            if (owned != NULL)
                free(owned);
            else
                obj_free_value(blob, vbloblen);
        }
        return -1;
    }
    vblob = owned != NULL ? owned : blob;
    db_install_blob(d, key, klen, vblob, vbloblen, now_ms);
    if (expire_ms != 0)
        db_install_expiry(d, key, klen, expire_ms);
    if (owned != NULL)
        free(owned);
    return 0;
}
