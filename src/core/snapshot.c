/* snapshot.c - RDB-style binary snapshot; see snapshot.h for the format. */
#include "core/snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds/obj.h"
#include "core/script.h"
#include "core/crc64.h"
#include "pal/pal_file.h"
#include "resp/resp_writer.h"

/* ------------------------------------------------------------------ */
/* little-endian write helpers (resp_buf append)                      */
/* ------------------------------------------------------------------ */

static int buf_reserve(resp_buf *b, size_t n)
{
    if (n > SIZE_MAX - b->len)
        return -1;
    return resp_buf_reserve(b, n);
}

static int buf_u8(resp_buf *b, uint8_t v)
{
    if (buf_reserve(b, 1) != 0)
        return -1;
    b->data[b->len++] = (char)v;
    return 0;
}

static int buf_u16le(resp_buf *b, uint16_t v)
{
    int i;
    if (buf_reserve(b, 2) != 0)
        return -1;
    for (i = 0; i < 2; i++)
        b->data[b->len++] = (char)((v >> (8 * i)) & 0xFFu);
    return 0;
}

static int buf_u32le(resp_buf *b, uint32_t v)
{
    int i;
    if (buf_reserve(b, 4) != 0)
        return -1;
    for (i = 0; i < 4; i++)
        b->data[b->len++] = (char)((v >> (8 * i)) & 0xFFu);
    return 0;
}

static int buf_u64le(resp_buf *b, uint64_t v)
{
    int i;
    if (buf_reserve(b, 8) != 0)
        return -1;
    for (i = 0; i < 8; i++)
        b->data[b->len++] = (char)((v >> (8 * i)) & 0xFFu);
    return 0;
}

static int buf_f64le(resp_buf *b, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    return buf_u64le(b, u);
}

static int buf_bytes(resp_buf *b, const char *p, size_t n)
{
    if (buf_reserve(b, n) != 0)
        return -1;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* save                                                               */
/* ------------------------------------------------------------------ */

typedef struct save_ctx {
    db *d;
    resp_buf *buf;
    int ok;
} save_ctx;

/* append a raw rh entry (field/member) list item: u32 len + bytes */
static void dump_pair_cb(const char *f, size_t flen, const char *v,
                         size_t vlen, void *c)
{
    save_ctx *ctx = (save_ctx *)c;
    if (!ctx->ok)
        return;
    if (flen > UINT32_MAX || vlen > UINT32_MAX) {
        ctx->ok = 0;
        return;
    }
    if (buf_u32le(ctx->buf, (uint32_t)flen) != 0 ||
        buf_bytes(ctx->buf, f, flen) != 0 ||
        buf_u32le(ctx->buf, (uint32_t)vlen) != 0 ||
        buf_bytes(ctx->buf, v, vlen) != 0)
        ctx->ok = 0;
}

static void dump_member_cb(const char *m, size_t mlen, const char *v,
                           size_t vlen, void *c)
{
    save_ctx *ctx = (save_ctx *)c;
    (void)v;
    (void)vlen;
    if (!ctx->ok)
        return;
    if (mlen > UINT32_MAX) {
        ctx->ok = 0;
        return;
    }
    if (buf_u32le(ctx->buf, (uint32_t)mlen) != 0 ||
        buf_bytes(ctx->buf, m, mlen) != 0)
        ctx->ok = 0;
}

static uint64_t array_index_from_key(const char *key)
{
    uint64_t index = 0;
    int i;
    for (i = 7; i >= 0; i--)
        index = (index << 8) | (uint64_t)(unsigned char)key[i];
    return index;
}

static void dump_array_cb(const char *key, size_t klen, const char *value,
                          size_t vlen, void *c)
{
    save_ctx *ctx = (save_ctx *)c;
    if (!ctx->ok || klen != 8 || vlen > UINT32_MAX ||
        buf_u64le(ctx->buf, array_index_from_key(key)) != 0 ||
        buf_u32le(ctx->buf, (uint32_t)vlen) != 0 ||
        buf_bytes(ctx->buf, value, vlen) != 0)
        ctx->ok = 0;
}

/* append a value's payload (no key/expiry) in the per-type encoding */
static void write_value_payload(save_ctx *ctx, int tag, const char *val,
                                size_t vlen)
{
    resp_buf *buf = ctx->buf;
    switch (tag) {
    case DDUP_OBJ_STRING: {
        const char *s;
        size_t sl;
        obj_str(val, vlen, &s, &sl);
        if (sl > UINT32_MAX) {
            ctx->ok = 0;
            return;
        }
        if (buf_u32le(buf, (uint32_t)sl) != 0 ||
            buf_bytes(buf, s, sl) != 0)
            ctx->ok = 0;
        break;
    }
    case DDUP_OBJ_HASH: {
        obj_hash *h = (obj_hash *)obj_unpack_ptr(val, vlen);
        if (obj_hash_len(h) > UINT32_MAX) {
            ctx->ok = 0;
            return;
        }
        if (buf_u32le(buf, (uint32_t)obj_hash_len(h)) != 0) {
            ctx->ok = 0;
            return;
        }
        obj_hash_each(h, dump_pair_cb, ctx);
        break;
    }
    case DDUP_OBJ_LIST: {
        obj_list *l = (obj_list *)obj_unpack_ptr(val, vlen);
        obj_list_iter it;
        if (obj_list_len(l) > UINT32_MAX) {
            ctx->ok = 0;
            return;
        }
        if (buf_u32le(buf, (uint32_t)obj_list_len(l)) != 0) {
            ctx->ok = 0;
            return;
        }
        if (obj_list_first(l, &it)) {
            do {
                size_t el = 0;
                const char *e = obj_list_iter_value(&it, &el);
                if (buf_u32le(buf, (uint32_t)el) != 0 ||
                    buf_bytes(buf, e, el) != 0) {
                    ctx->ok = 0;
                    return;
                }
            } while (obj_list_iter_next(&it));
        }
        break;
    }
    case DDUP_OBJ_SET: {
        obj_set *st = (obj_set *)obj_unpack_ptr(val, vlen);
        if (obj_set_len(st) > UINT32_MAX) {
            ctx->ok = 0;
            return;
        }
        if (buf_u32le(buf, (uint32_t)obj_set_len(st)) != 0) {
            ctx->ok = 0;
            return;
        }
        obj_set_each(st, dump_member_cb, ctx);
        break;
    }
    case DDUP_OBJ_ZSET: {
        obj_zset *z = (obj_zset *)obj_unpack_ptr(val, vlen);
        obj_zset_iter it;
        if (obj_zset_len(z) > UINT32_MAX) {
            ctx->ok = 0;
            return;
        }
        if (buf_u32le(buf, (uint32_t)obj_zset_len(z)) != 0) {
            ctx->ok = 0;
            return;
        }
        if (obj_zset_first(z, &it)) {
            do {
                size_t ml = 0;
                const char *mv = obj_zset_iter_member(&it, &ml);
                if (buf_u32le(buf, (uint32_t)ml) != 0 ||
                    buf_bytes(buf, mv, ml) != 0 ||
                    buf_f64le(buf, obj_zset_iter_score(&it)) != 0) {
                    ctx->ok = 0;
                    return;
                }
            } while (obj_zset_iter_next(&it));
        }
        break;
    }
    case DDUP_OBJ_STREAM: {
        obj_stream *st = (obj_stream *)obj_unpack_ptr(val, vlen);
        size_t i;
        if (obj_stream_len(st) > UINT32_MAX) {
            ctx->ok = 0;
            return;
        }
        if (buf_u32le(buf, (uint32_t)obj_stream_len(st)) != 0 ||
            buf_u64le(buf, st->last_ms) != 0 ||
            buf_u64le(buf, st->last_seq) != 0 ||
            buf_u64le(buf, st->entries_added) != 0 ||
            buf_u64le(buf, st->max_deleted_ms) != 0 ||
            buf_u64le(buf, st->max_deleted_seq) != 0) {
            ctx->ok = 0;
            return;
        }
        for (i = 0; i < obj_stream_len(st) && ctx->ok; i++) {
            const stream_entry *e = obj_stream_at(st, i);
            const char *p = e->data;
            uint32_t j;
            if (buf_u64le(buf, e->ms) != 0 ||
                buf_u64le(buf, e->seq) != 0 ||
                buf_u32le(buf, e->nfields) != 0) {
                ctx->ok = 0;
                return;
            }
            for (j = 0; j < e->nfields; j++) {
                uint32_t fl = e->lens[2 * j];
                uint32_t vl = e->lens[2 * j + 1];
                if (buf_u32le(buf, fl) != 0 || buf_bytes(buf, p, fl) != 0 ||
                    buf_u32le(buf, vl) != 0 ||
                    buf_bytes(buf, p + fl, vl) != 0) {
                    ctx->ok = 0;
                    return;
                }
                p += (size_t)fl + (size_t)vl;
            }
        }
        if (st->ngroups > UINT32_MAX) {
            ctx->ok = 0;
            return;
        }
        if (buf_u32le(buf, (uint32_t)st->ngroups) != 0) {
            ctx->ok = 0;
            return;
        }
        for (i = 0; i < st->ngroups && ctx->ok; i++) {
            const stream_group *g = &st->groups[i];
            size_t j;
            if (g->name_len > UINT32_MAX || g->nconsumers > UINT32_MAX) {
                ctx->ok = 0;
                return;
            }
            if (buf_u32le(buf, (uint32_t)g->name_len) != 0 ||
                buf_bytes(buf, g->name, g->name_len) != 0 ||
                buf_u64le(buf, g->last_ms) != 0 ||
                buf_u64le(buf, g->last_seq) != 0 ||
                buf_u64le(buf, g->entries_read) != 0 ||
                buf_u32le(buf, (uint32_t)g->nconsumers) != 0) {
                ctx->ok = 0;
                return;
            }
            for (j = 0; j < g->nconsumers && ctx->ok; j++) {
                const stream_consumer *c = &g->consumers[j];
                size_t k;
                if (c->name_len > UINT32_MAX || c->pel_len > UINT32_MAX) {
                    ctx->ok = 0;
                    return;
                }
                if (buf_u32le(buf, (uint32_t)c->name_len) != 0 ||
                    buf_bytes(buf, c->name, c->name_len) != 0 ||
                    buf_u64le(buf, c->seen_time) != 0 ||
                    buf_u64le(buf, c->active_time) != 0 ||
                    buf_u32le(buf, (uint32_t)c->pel_len) != 0) {
                    ctx->ok = 0;
                    return;
                }
                for (k = 0; k < c->pel_len && ctx->ok; k++) {
                    const stream_pending *pd = &c->pel[k];
                    if (buf_u64le(buf, pd->ms) != 0 ||
                        buf_u64le(buf, pd->seq) != 0 ||
                        buf_u64le(buf, pd->idle) != 0 ||
                        buf_u64le(buf, pd->delivery_count) != 0) {
                        ctx->ok = 0;
                        return;
                    }
                }
            }
        }
        break;
    }
    case DDUP_OBJ_ARRAY: {
        obj_array *a = (obj_array *)obj_unpack_ptr(val, vlen);
        if (obj_array_count(a) > UINT32_MAX ||
            buf_u32le(buf, (uint32_t)obj_array_count(a)) != 0 ||
            buf_u64le(buf, obj_array_next(a)) != 0 ||
            buf_u64le(buf, a->ring_size) != 0 ||
            a->history_len > UINT32_MAX ||
            buf_u32le(buf, (uint32_t)a->history_len) != 0) {
            ctx->ok = 0;
            return;
        }
        {
            size_t hi;
            for (hi = 0; hi < a->history_len; hi++)
                if (buf_u64le(buf, a->history[hi]) != 0) { ctx->ok = 0; return; }
        }
        obj_array_each(a, dump_array_cb, ctx);
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

    if (!ctx->ok)
        return;
    if (klen > UINT32_MAX) {
        ctx->ok = 0;
        return;
    }
    if (rh_get(&ctx->d->expires, key, klen, &ev, &evl) && evl == 8) {
        uint64_t u;
        memcpy(&u, ev, 8);
        expire = (int64_t)u;
    }
    if (buf_u8(buf, (uint8_t)tag) != 0 ||
        buf_u32le(buf, (uint32_t)klen) != 0 ||
        buf_bytes(buf, key, klen) != 0 ||
        buf_u64le(buf, (uint64_t)expire) != 0) {
        ctx->ok = 0;
        return;
    }
    write_value_payload(ctx, tag, val, vlen);
}

int snapshot_serialize(db *d, resp_buf *out)
{
    save_ctx ctx;
    size_t start;
    if (d == NULL || out == NULL)
        return -1;
    start = out->len;
    if (buf_bytes(out, "DDUP0001", 8) != 0)
        return -1;
    ctx.d = d;
    ctx.buf = out;
    ctx.ok = 1;
    rh_each(&d->table, save_entry_cb, &ctx);
    if (!ctx.ok) {
        out->len = start;
        return -1;
    }
    return 0;
}

int snapshot_save(db *d, const char *path)
{
    resp_buf buf;
    char *tmp;
    pal_file *f;
    int rc = -1;
    size_t path_len;

    if (d == NULL || path == NULL || path[0] == '\0')
        return -1;
    resp_buf_init(&buf);
    if (snapshot_serialize(d, &buf) != 0) {
        resp_buf_free(&buf);
        return -1;
    }

    path_len = strlen(path);
    if (path_len > SIZE_MAX - sizeof(".tmp"))
        goto done;
    tmp = (char *)malloc(path_len + sizeof(".tmp"));
    if (tmp == NULL)
        goto done;
    memcpy(tmp, path, path_len);
    memcpy(tmp + path_len, ".tmp", sizeof(".tmp"));
    f = pal_file_open_write(tmp);
    if (f != NULL) {
        if (pal_file_write(f, buf.data, buf.len) == (ptrdiff_t)buf.len &&
            pal_file_flush(f) == 0) {
            if (pal_file_close(f) == 0)
                rc = pal_file_rename(tmp, path);
        } else {
            pal_file_close(f);
        }
        if (rc != 0)
            (void)pal_file_unlink(tmp);
    }
    free(tmp);
done:
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

/* free any owned object value (data-only swap in snapshot_load_mem) */
static void free_val_cb(const char *key, size_t klen, const char *val,
                        size_t vlen, void *ctx)
{
    (void)key;
    (void)klen;
    (void)ctx;
    obj_free_value(val, vlen);
}

static uint8_t rd_u8(reader *r)
{
    if (r->off > r->len || 1 > r->len - r->off) {
        r->ok = 0;
        return 0;
    }
    return (uint8_t)r->p[r->off++];
}

static uint16_t rd_u16le(reader *r)
{
    uint16_t v = 0;
    int i;
    if (r->off > r->len || 2 > r->len - r->off) {
        r->ok = 0;
        r->off = r->len;
        return 0;
    }
    for (i = 0; i < 2; i++)
        v |= (uint16_t)((uint16_t)(uint8_t)r->p[r->off++] << (8 * i));
    return v;
}

static uint32_t rd_u32le(reader *r)
{
    uint32_t v = 0;
    int i;
    if (r->off > r->len || 4 > r->len - r->off) {
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
    if (r->off > r->len || 8 > r->len - r->off) {
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
    if (!r->ok || r->off > r->len || n > r->len - r->off) {
        r->ok = 0;
        return NULL;
    }
    p = r->p + r->off;
    r->off += n;
    return p;
}

int snapshot_test_reader_bounds(void)
{
    reader r;
    char byte = 'x';
    r.p = &byte;
    r.len = SIZE_MAX;
    r.off = SIZE_MAX;
    r.ok = 1;
    (void)rd_u8(&r);
    if (r.ok)
        return -1;
    r.ok = 1;
    r.off = SIZE_MAX - 1;
    (void)rd_bytes(&r, 2);
    return r.ok ? -1 : 0;
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
            if (r->ok && obj_hash_set(h, f, fl, v, vl) < 0)
                r->ok = 0;
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
            if (r->ok && obj_list_push(l, 0, e, el) < 0)
                r->ok = 0;
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
            if (r->ok && obj_set_add(st, m, ml) < 0)
                r->ok = 0;
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
            if (r->ok && (sc != sc || obj_zset_add(z, m, ml, sc) < 0))
                r->ok = 0;
        }
        if (!r->ok) {
            obj_zset_free(z);
            return NULL;
        }
        obj_pack_ptr(blob, DDUP_OBJ_ZSET, z);
        *out_len = 9;
        return NULL;
    }
    case DDUP_OBJ_STREAM: {
        obj_stream *st = obj_stream_new();
        uint64_t last_ms, last_seq, entries_added, max_del_ms, max_del_seq;
        n = rd_u32le(r);
        last_ms = rd_u64le(r);
        last_seq = rd_u64le(r);
        entries_added = rd_u64le(r);
        max_del_ms = rd_u64le(r);
        max_del_seq = rd_u64le(r);
        for (i = 0; i < n && r->ok; i++) {
            uint64_t ms = rd_u64le(r);
            uint64_t seq = rd_u64le(r);
            uint32_t nf = rd_u32le(r);
            const char **fields = NULL;
            const char **values = NULL;
            size_t *flens = NULL;
            size_t *vlens = NULL;
            uint32_t j;
            if (!r->ok)
                break;
            if (nf > 0) {
                fields = (const char **)malloc((size_t)nf * sizeof(*fields));
                values = (const char **)malloc((size_t)nf * sizeof(*values));
                flens = (size_t *)malloc((size_t)nf * sizeof(*flens));
                vlens = (size_t *)malloc((size_t)nf * sizeof(*vlens));
                if (fields == NULL || values == NULL || flens == NULL ||
                    vlens == NULL) {
                    fprintf(stderr, "ddup: out of memory\n");
                    exit(1);
                }
            }
            for (j = 0; j < nf && r->ok; j++) {
                uint32_t fl = rd_u32le(r);
                const char *f = rd_bytes(r, fl);
                uint32_t vl = rd_u32le(r);
                const char *v = rd_bytes(r, vl);
                fields[j] = f;
                values[j] = v;
                flens[j] = fl;
                vlens[j] = vl;
            }
            if (!r->ok ||
                obj_stream_append(st, ms, seq, fields, flens, values, vlens,
                                  nf) != OBJ_STREAM_ADD_OK) {
                r->ok = 0;
            }
            free(fields);
            free(values);
            free(flens);
            free(vlens);
        }
        if (!r->ok) {
            obj_stream_free(st);
            return NULL;
        }
        st->last_ms = last_ms;
        st->last_seq = last_seq;
        st->entries_added = entries_added;
        st->max_deleted_ms = max_del_ms;
        st->max_deleted_seq = max_del_seq;
        {
            uint32_t ng = rd_u32le(r);
            uint32_t gi;
            for (gi = 0; gi < ng && r->ok; gi++) {
                uint32_t gl = rd_u32le(r);
                const char *gname = rd_bytes(r, gl);
                uint64_t g_last_ms = rd_u64le(r);
                uint64_t g_last_seq = rd_u64le(r);
                uint64_t g_entries = rd_u64le(r);
                uint32_t nc = rd_u32le(r);
                uint32_t ci;
                stream_group *g;
                int created = 0;
                if (!r->ok)
                    break;
                g = obj_stream_group_create(st, gname, gl, g_last_ms,
                                            g_last_seq, &created);
                if (g == NULL || created != 1) {
                    r->ok = 0;
                    break;
                }
                g->entries_read = g_entries;
                for (ci = 0; ci < nc && r->ok; ci++) {
                    uint32_t cl = rd_u32le(r);
                    const char *cname = rd_bytes(r, cl);
                    uint64_t seen = rd_u64le(r);
                    uint64_t active = rd_u64le(r);
                    uint32_t np = rd_u32le(r);
                    uint32_t pi;
                    stream_consumer *c;
                    if (!r->ok)
                        break;
                    if (obj_stream_consumer_get(g, cname, cl) != NULL) {
                        r->ok = 0;
                        break;
                    }
                    c = obj_stream_consumer_create(g, cname, cl);
                    if (c == NULL) {
                        r->ok = 0;
                        break;
                    }
                    c->seen_time = seen;
                    c->active_time = active;
                    for (pi = 0; pi < np && r->ok; pi++) {
                        uint64_t p_ms = rd_u64le(r);
                        uint64_t p_seq = rd_u64le(r);
                        uint64_t p_idle = rd_u64le(r);
                        uint64_t p_delivery = rd_u64le(r);
                        if (!r->ok)
                            break;
                        if (obj_stream_consumer_pel_find(c, p_ms, p_seq) !=
                            NULL) {
                            r->ok = 0;
                            break;
                        }
                        if (obj_stream_consumer_pel_add(g, c, p_ms, p_seq,
                                                        p_idle, p_delivery) ==
                            NULL) {
                            r->ok = 0;
                            break;
                        }
                    }
                }
            }
        }
        if (!r->ok) {
            obj_stream_free(st);
            return NULL;
        }
        obj_pack_ptr(blob, DDUP_OBJ_STREAM, st);
        *out_len = 9;
        return NULL;
    }
    case DDUP_OBJ_ARRAY: {
        obj_array *a = obj_array_new();
        n = rd_u32le(r);
        a->next_insert = rd_u64le(r);
        a->ring_size = rd_u64le(r);
        {
            uint32_t hn = rd_u32le(r), hi;
            for (hi = 0; hi < hn && r->ok; hi++) {
                uint64_t hv = rd_u64le(r);
                if (obj_array_history_push(a, hv) != 0) r->ok = 0;
            }
        }
        for (i = 0; i < n && r->ok; i++) {
            uint64_t index = rd_u64le(r);
            uint32_t vl = rd_u32le(r);
            const char *value = rd_bytes(r, vl);
            const char *values[1];
            size_t lengths[1];
            values[0] = value;
            lengths[0] = vl;
            if (r->ok && obj_array_set(a, index, values, lengths, 1, NULL) != 0)
                r->ok = 0;
        }
        if (!r->ok) {
            obj_array_free(a);
            return NULL;
        }
        obj_pack_ptr(blob, DDUP_OBJ_ARRAY, a);
        *out_len = 9;
        return NULL;
    }
    default:
        r->ok = 0;
        return NULL;
    }
}

/* Parse entries into tmp. An unbounded stream is used only by DDUP0001;
 * DDUP0002 segments are always explicitly counted, including UINT32_MAX. */
static int load_entries(reader *r, db *tmp, uint32_t count, int unbounded,
                        uint64_t now_ms)
{
    uint32_t i = 0;
    while (r->ok && r->off < r->len &&
           (unbounded || i < count)) {
        int tag = rd_u8(r);
        uint32_t klen = rd_u32le(r);
        const char *key = rd_bytes(r, klen);
        int64_t expire;
        char blob[9];
        char *owned;
        const char *vblob;
        size_t vbloblen = 0;
        if (!r->ok)
            break;
        expire = (int64_t)rd_u64le(r);
        owned = load_payload(r, tag, blob, &vbloblen);
        if (!r->ok)
            break;
        vblob = owned != NULL ? owned : blob;
        if (expire >= 0 && (uint64_t)expire <= now_ms) {
            /* already dead at load time: skip */
            if (owned != NULL)
                free(owned);
            else
                obj_free_value(vblob, vbloblen);
            i++;
            continue;
        }
        if (db_install_blob(tmp, key, klen, vblob, vbloblen, now_ms) != 0) {
            if (owned != NULL)
                free(owned);
            else
                obj_free_value(vblob, vbloblen);
            return 0;
        }
        if (expire >= 0 &&
            db_install_expiry(tmp, key, klen, (uint64_t)expire) != 0) {
            db_del_kv(tmp, key, klen);
            if (owned != NULL)
                free(owned);
            return 0;
        }
        if (owned != NULL)
            free(owned);
        i++;
    }
    return r->ok && (unbounded || i == count);
}

/* Data-only swap: tmp's contents move into d (configuration, cluster
 * state, WATCH bookkeeping of d are preserved). */
static void swap_db_data(db *d, db *tmp)
{
    rh_each(&d->table, free_val_cb, NULL);
    rh_destroy(&d->table);
    rh_destroy(&d->expires);
    d->table = tmp->table;
    d->expires = tmp->expires;
    d->used_memory = tmp->used_memory;
    d->flush_epoch++;
    rh_destroy(&tmp->keyvers); /* tmp's unused empty table */
    memset(&tmp->table, 0, sizeof(tmp->table));
    memset(&tmp->expires, 0, sizeof(tmp->expires));
    memset(&tmp->keyvers, 0, sizeof(tmp->keyvers));
}

static void destroy_temp_db(db *d)
{
    script_cleanup(d);
    rh_destroy(&d->table);
    rh_destroy(&d->expires);
    rh_destroy(&d->keyvers);
    rh_destroy(&d->scripts);
}

int snapshot_load_mem(db *d, const char *buf, size_t len, uint64_t now_ms)
{
    reader r;
    db tmp;
    int ok;

    if (d == NULL || buf == NULL || len < 8 || memcmp(buf, "DDUP0001", 8) != 0)
        return -1;
    r.p = buf;
    r.len = len;
    r.off = 8;
    r.ok = 1;

    /* parse into a temporary db: all-or-nothing */
    db_init(&tmp);
    ok = load_entries(&r, &tmp, 0, 1, now_ms) && r.off == r.len;

    if (!ok) {
        db_destroy(&tmp);
        return -1;
    }
    swap_db_data(d, &tmp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* multi-database snapshots (DDUP0002)                                 */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* multi-shard snapshots (DDUPMT01)                                    */
/*                                                                     */
/*   magic: 8 bytes "DDUPMT01"                                         */
/*   u16   nshards                                                     */
/*   per shard:                                                        */
/*     u32   snapshot length                                           */
/*     bytes DDUP0002 snapshot for that shard                          */
/*                                                                     */
/* Loading accepts DDUP0001 (falls into db 0), DDUP0002 and DDUPMT01.  */
/* DDUPMT01 is loaded into temporary dbs and merged by db index.       */
/* ------------------------------------------------------------------ */

typedef struct db_array_ctx {
    db *dbs;
    int ndbs;
} db_array_ctx;

static db *db_array_get(void *ctx, int idx)
{
    db_array_ctx *ac = (db_array_ctx *)ctx;
    if (ac == NULL || idx < 0 || idx >= ac->ndbs)
        return NULL;
    return &ac->dbs[idx];
}

static uint64_t rd_u64le_bytes(const char *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--)
        v = (v << 8) | (uint8_t)p[i];
    return v;
}

typedef struct merge_ctx {
    db *dst;
    uint64_t now_ms;
    int ok;
} merge_ctx;

static void merge_entry_cb(const char *key, size_t klen, const char *val,
                           size_t vlen, void *arg)
{
    merge_ctx *mc = (merge_ctx *)arg;
    if (mc->ok && db_install_blob(mc->dst, key, klen, val, vlen,
                                  mc->now_ms) != 0)
        mc->ok = 0;
}

static void merge_expiry_cb(const char *key, size_t klen, const char *val,
                            size_t vlen, void *arg)
{
    merge_ctx *mc = (merge_ctx *)arg;
    if (mc->ok && vlen == 8 &&
        db_install_expiry(mc->dst, key, klen,
                          rd_u64le_bytes(val)) != 0)
        mc->ok = 0;
}

static int merge_shard_db(db *dst, db *src, uint64_t now_ms)
{
    merge_ctx mc;
    mc.dst = dst;
    mc.now_ms = now_ms;
    mc.ok = 1;
    rh_each(&src->table, merge_entry_cb, &mc);
    if (mc.ok)
        rh_each(&src->expires, merge_expiry_cb, &mc);
    return mc.ok ? 0 : -1;
}

int snapshot_serialize_multi_shards(void *const *ctxs, snapshot_db_get get,
                                    int nshards, int ndbs, resp_buf *out)
{
    int i;
    size_t start;
    if (out == NULL || ctxs == NULL || get == NULL || nshards < 0 ||
        nshards > UINT16_MAX)
        return -1;
    start = out->len;
    if (buf_bytes(out, "DDUPMT01", 8) != 0 ||
        buf_u16le(out, (uint16_t)nshards) != 0) {
        out->len = start;
        return -1;
    }
    for (i = 0; i < nshards; i++) {
        resp_buf shard;
        resp_buf_init(&shard);
        if (snapshot_serialize_multi(ctxs[i], get, ndbs, &shard) != 0 ||
            shard.len > UINT32_MAX ||
            buf_u32le(out, (uint32_t)shard.len) != 0 ||
            buf_bytes(out, shard.data, shard.len) != 0) {
            resp_buf_free(&shard);
            out->len = start;
            return -1;
        }
        resp_buf_free(&shard);
    }
    return 0;
}

int snapshot_serialize_multi_buffers(const char *const *bufs,
                                     const size_t *lens, int nshards,
                                     resp_buf *out)
{
    int i;
    size_t start;

    if (out == NULL || bufs == NULL || lens == NULL || nshards < 0 ||
        nshards > UINT16_MAX)
        return -1;
    start = out->len;
    if (buf_bytes(out, "DDUPMT01", 8) != 0 ||
        buf_u16le(out, (uint16_t)nshards) != 0) {
        out->len = start;
        return -1;
    }
    for (i = 0; i < nshards; i++) {
        if (lens[i] > UINT32_MAX ||
            (lens[i] > 0 && bufs[i] == NULL) ||
            buf_u32le(out, (uint32_t)lens[i]) != 0) {
            out->len = start;
            return -1;
        }
        if (lens[i] > 0 && buf_bytes(out, bufs[i], lens[i]) != 0) {
            out->len = start;
            return -1;
        }
    }
    return 0;
}

int snapshot_serialize_multi(void *ctx, snapshot_db_get get, int ndbs,
                             resp_buf *out)
{
    int i;
    size_t start;
    if (ctx == NULL || get == NULL || out == NULL || ndbs < 0 ||
        ndbs > UINT16_MAX)
        return -1;
    start = out->len;
    if (buf_bytes(out, "DDUP0002", 8) != 0 ||
        buf_u16le(out, (uint16_t)ndbs) != 0) {
        out->len = start;
        return -1;
    }
    for (i = 0; i < ndbs; i++) {
        db *d = get(ctx, i);
        save_ctx sctx;
        size_t n;
        if (d == NULL) {
            out->len = start;
            return -1;
        }
        n = rh_size(&d->table);
        if (n == 0)
            continue;
        if (n > UINT32_MAX) {
            out->len = start;
            return -1;
        }
        if (buf_u16le(out, (uint16_t)i) != 0 ||
            buf_u32le(out, (uint32_t)n) != 0) {
            out->len = start;
            return -1;
        }
        sctx.d = d;
        sctx.buf = out;
        sctx.ok = 1;
        rh_each(&d->table, save_entry_cb, &sctx);
        if (!sctx.ok) {
            out->len = start;
            return -1;
        }
    }
    return 0;
}

int snapshot_save_multi(void *ctx, snapshot_db_get get, int ndbs,
                        const char *path)
{
    resp_buf buf;
    char *tmp;
    pal_file *f;
    int rc = -1;
    size_t path_len;

    if (ctx == NULL || get == NULL || ndbs <= 0 || path == NULL ||
        path[0] == '\0')
        return -1;
    resp_buf_init(&buf);
    if (snapshot_serialize_multi(ctx, get, ndbs, &buf) != 0) {
        resp_buf_free(&buf);
        return -1;
    }

    path_len = strlen(path);
    if (path_len > SIZE_MAX - sizeof(".tmp"))
        goto done;
    tmp = (char *)malloc(path_len + sizeof(".tmp"));
    if (tmp == NULL)
        goto done;
    memcpy(tmp, path, path_len);
    memcpy(tmp + path_len, ".tmp", sizeof(".tmp"));
    f = pal_file_open_write(tmp);
    if (f != NULL) {
        if (pal_file_write(f, buf.data, buf.len) == (ptrdiff_t)buf.len &&
            pal_file_flush(f) == 0) {
            if (pal_file_close(f) == 0)
                rc = pal_file_rename(tmp, path);
        } else {
            pal_file_close(f);
        }
        if (rc != 0)
            (void)pal_file_unlink(tmp);
    }
    free(tmp);
done:
    resp_buf_free(&buf);
    return rc;
}

int snapshot_load_mem_multi(void *ctx, snapshot_db_get get, int ndbs,
                            const char *buf, size_t len, uint64_t now_ms)
{
    reader r;
    db *tmps;
    char *segs;
    int ok = 1;
    int i;

    if (ctx == NULL || get == NULL || ndbs <= 0 || buf == NULL || len < 8)
        return -1;
    if (memcmp(buf, "DDUPMT01", 8) == 0) {
        db_array_ctx shard_ctx;
        db *shard_tmps = NULL;
        uint16_t nshards;
        uint32_t sh;

        if (ndbs <= 0 || get == NULL)
            return -1;
        tmps = (db *)calloc((size_t)ndbs, sizeof(*tmps));
        if (tmps == NULL)
            return -1;
        for (i = 0; i < ndbs; i++)
            db_init(&tmps[i]);

        r.p = buf;
        r.len = len;
        r.off = 8;
        r.ok = 1;
        nshards = rd_u16le(&r);
        shard_tmps = (db *)calloc((size_t)ndbs, sizeof(*shard_tmps));
        if (!r.ok || shard_tmps == NULL) {
            ok = 0;
            goto mt_done;
        }
        shard_ctx.dbs = shard_tmps;
        shard_ctx.ndbs = ndbs;
        for (sh = 0; sh < nshards && ok; sh++) {
            uint32_t slen = rd_u32le(&r);
            const char *shbuf;
            int j;
            if (!r.ok || slen > (uint64_t)(r.len - r.off)) {
                ok = 0;
                break;
            }
            shbuf = r.p + r.off;
            r.off += slen;
            for (j = 0; j < ndbs; j++)
                db_init(&shard_tmps[j]);
            if (snapshot_load_mem_multi(&shard_ctx, db_array_get, ndbs,
                                        shbuf, slen, now_ms) != 0) {
                for (j = 0; j < ndbs; j++)
                    db_destroy(&shard_tmps[j]);
                ok = 0;
                break;
            }
            for (j = 0; j < ndbs; j++) {
                if (merge_shard_db(&tmps[j], &shard_tmps[j],
                                   now_ms) != 0) {
                    ok = 0;
                }
                db_destroy(&shard_tmps[j]);
            }
        }
        ok = ok && r.ok && r.off == r.len;
        if (ok) {
            for (i = 0; i < ndbs; i++) {
                if (get(ctx, i) == NULL) {
                    ok = 0;
                    break;
                }
            }
        }
        if (ok) {
            for (i = 0; i < ndbs; i++)
                swap_db_data(get(ctx, i), &tmps[i]);
        }
        for (i = 0; i < ndbs; i++)
            db_destroy(&tmps[i]);
mt_done:
        free(tmps);
        free(shard_tmps);
        return ok ? 0 : -1;
    }
    if (memcmp(buf, "DDUP0001", 8) == 0) {
        db *tmps;
        char *segs;
        int i;
        int ok;
        if (ndbs <= 0)
            return -1;
        tmps = (db *)calloc((size_t)ndbs, sizeof(*tmps));
        segs = (char *)calloc((size_t)ndbs, 1);
        if (tmps == NULL || segs == NULL) {
            free(tmps); free(segs); return -1;
        }
        for (i = 0; i < ndbs; i++) db_init(&tmps[i]);
        r.p = buf; r.len = len; r.off = 8; r.ok = 1;
        ok = load_entries(&r, &tmps[0], 0, 1, now_ms) && r.off == r.len;
        if (ok) {
            db *d;
            for (i = 0; i < ndbs; i++) {
                d = get(ctx, i);
                if (d == NULL) { ok = 0; break; }
            }
            if (ok)
                for (i = 0; i < ndbs; i++)
                    swap_db_data(get(ctx, i), &tmps[i]);
        }
        for (i = 0; i < ndbs; i++) destroy_temp_db(&tmps[i]);
        free(tmps); free(segs);
        return ok ? 0 : -1;
    }
    if (memcmp(buf, "DDUP0002", 8) != 0)
        return -1;

    r.p = buf;
    r.len = len;
    r.off = 8;
    r.ok = 1;
    (void)rd_u16le(&r); /* ndbs in the file (informational) */

    tmps = (db *)calloc((size_t)ndbs, sizeof(db));
    segs = (char *)calloc((size_t)ndbs, 1);
    if (tmps == NULL || segs == NULL) {
        free(tmps);
        free(segs);
        return -1;
    }

    for (i = 0; i < ndbs; i++)
        db_init(&tmps[i]);
    /* parse every segment into temporaries (all-or-nothing) */
    while (r.ok && r.off < r.len) {
        uint16_t idx = rd_u16le(&r);
        uint32_t count = rd_u32le(&r);
        if (!r.ok || idx >= ndbs)
            break;
        if (segs[idx]) {
            ok = 0;
            break;
        }
        segs[idx] = 1;
        if (!load_entries(&r, &tmps[idx], count, 0, now_ms)) {
            ok = 0;
            break;
        }
    }
    ok = ok && r.ok && r.off == r.len;

    if (ok) {
        for (i = 0; i < ndbs; i++) {
            if (get(ctx, i) == NULL) {
                ok = 0;
                break;
            }
        }
    }
    if (ok) {
        /* swap every db: segments replace, missing dbs are emptied */
        for (i = 0; i < ndbs; i++) {
            db *d = get(ctx, i);
            if (d == NULL)
                continue;
            swap_db_data(d, &tmps[i]);
        }
    } else {
        for (i = 0; i < ndbs; i++)
            destroy_temp_db(&tmps[i]);
    }
    if (ok)
        for (i = 0; i < ndbs; i++)
            destroy_temp_db(&tmps[i]);
    free(tmps);
    free(segs);
    return ok ? 0 : -1;
}

int snapshot_load_multi(void *ctx, snapshot_db_get get, int ndbs,
                        const char *path, uint64_t now_ms)
{
    pal_file *f = NULL;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int rc;

    if (ctx == NULL || get == NULL || ndbs <= 0 || path == NULL ||
        path[0] == '\0')
        return -1;
    f = pal_file_open_read(path);
    if (f == NULL)
        return -1;
    for (;;) {
        ptrdiff_t n;
        if (len == cap) {
            size_t ncap;
            if (cap == 0)
                ncap = 65536;
            else if (cap > SIZE_MAX / 2)
                ncap = SIZE_MAX;
            else
                ncap = cap * 2;
            if (ncap <= cap) {
                free(buf);
                pal_file_close(f);
                return -1;
            }
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

    rc = snapshot_load_mem_multi(ctx, get, ndbs, buf, len, now_ms);
    free(buf);
    return rc;
}

int snapshot_load(db *d, const char *path, uint64_t now_ms)
{
    pal_file *f = NULL;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int rc;

    if (d == NULL || path == NULL || path[0] == '\0')
        return -1;
    f = pal_file_open_read(path);
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
    save_ctx ctx;

    if (d == NULL || key == NULL || out == NULL || !rh_get(&d->table, key, klen, &v, &vl))
        return -1;
    start = out->len;
    ctx.d = d;
    ctx.buf = out;
    ctx.ok = 1;
    if (buf_u16le(out, SNAPSHOT_DUMP_VERSION) != 0 ||
        buf_u8(out, (uint8_t)obj_tag_of(v, vl)) != 0) {
        out->len = start;
        return -1;
    }
    write_value_payload(&ctx, obj_tag_of(v, vl), v, vl);
    if (!ctx.ok) {
        out->len = start;
        return -1;
    }
    if (buf_u64le(out, crc64(0, out->data + start, out->len - start)) != 0) {
        out->len = start;
        return -1;
    }
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

    if (d == NULL || key == NULL || payload == NULL || klen > UINT32_MAX)
        return -1;
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
    if (db_install_blob(d, key, klen, vblob, vbloblen, now_ms) != 0) {
        if (owned != NULL)
            free(owned);
        else
            obj_free_value(vblob, vbloblen);
        return -1;
    }
    if (expire_ms != 0 && db_install_expiry(d, key, klen, expire_ms) != 0) {
        db_del_kv(d, key, klen);
        if (owned != NULL)
            free(owned);
        return -1;
    }
    if (owned != NULL)
        free(owned);
    return 0;
}
