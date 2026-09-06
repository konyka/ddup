/* obj.c - typed value objects; see obj.h. */
#include "ds/listpack.h"
#include "ds/obj.h"

#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int obj_tag_of(const char *val, size_t vlen)
{
    if (vlen == 0)
        return DDUP_OBJ_STRING;
    return (unsigned char)val[0];
}

void obj_str(const char *val, size_t vlen, const char **s, size_t *len)
{
    if (vlen == 0) {
        *s = val;
        *len = 0;
        return;
    }
    *s = val + 1;
    *len = vlen - 1;
}

void obj_pack_ptr(char buf[9], int tag, const void *ptr)
{
    buf[0] = (char)tag;
    memcpy(buf + 1, &ptr, 8);
}

void *obj_unpack_ptr(const char *val, size_t vlen)
{
    void *ptr = NULL;
    if (vlen >= 9)
        memcpy(&ptr, val + 1, 8);
    return ptr;
}

void obj_tier_pack(char buf[17], uint64_t record_id, uint64_t expire_ms)
{
    int i;
    buf[0] = (char)DDUP_OBJ_TIER;
    for (i = 0; i < 8; i++)
        buf[1 + i] = (char)((record_id >> (8 * i)) & 0xFFu);
    for (i = 0; i < 8; i++)
        buf[9 + i] = (char)((expire_ms >> (8 * i)) & 0xFFu);
}

void obj_tier_unpack(const char *val, size_t vlen, uint64_t *record_id,
                     uint64_t *expire_ms)
{
    uint64_t rid = 0;
    uint64_t exp = 0;
    int i;
    if (vlen < 17) {
        if (record_id != NULL)
            *record_id = 0;
        if (expire_ms != NULL)
            *expire_ms = 0;
        return;
    }
    for (i = 7; i >= 0; i--)
        rid = (rid << 8) | (uint64_t)(unsigned char)val[1 + i];
    for (i = 7; i >= 0; i--)
        exp = (exp << 8) | (uint64_t)(unsigned char)val[9 + i];
    if (record_id != NULL)
        *record_id = rid;
    if (expire_ms != NULL)
        *expire_ms = exp;
}

int obj_is_tier(const char *val, size_t vlen)
{
    return obj_tag_of(val, vlen) == DDUP_OBJ_TIER;
}

uint64_t obj_extra_mem(const char *val, size_t vlen)
{
    switch (obj_tag_of(val, vlen)) {
    case DDUP_OBJ_HASH:
        return obj_hash_mem((obj_hash *)obj_unpack_ptr(val, vlen));
    case DDUP_OBJ_LIST:
        return obj_list_mem((obj_list *)obj_unpack_ptr(val, vlen));
    case DDUP_OBJ_SET:
        return obj_set_mem((obj_set *)obj_unpack_ptr(val, vlen));
    case DDUP_OBJ_ZSET:
        return obj_zset_mem((obj_zset *)obj_unpack_ptr(val, vlen));
    case DDUP_OBJ_STREAM:
        return obj_stream_mem((obj_stream *)obj_unpack_ptr(val, vlen));
    case DDUP_OBJ_ARRAY:
        return obj_array_mem((obj_array *)obj_unpack_ptr(val, vlen));
    case DDUP_OBJ_TIER:
        return 0; /* tier-ref is accounted by the db entry itself */
    default:
        return 0;
    }
}

void obj_free_value(const char *val, size_t vlen)
{
    switch (obj_tag_of(val, vlen)) {
    case DDUP_OBJ_HASH:
        obj_hash_free((obj_hash *)obj_unpack_ptr(val, vlen));
        break;
    case DDUP_OBJ_LIST:
        obj_list_free((obj_list *)obj_unpack_ptr(val, vlen));
        break;
    case DDUP_OBJ_SET:
        obj_set_free((obj_set *)obj_unpack_ptr(val, vlen));
        break;
    case DDUP_OBJ_ZSET:
        obj_zset_free((obj_zset *)obj_unpack_ptr(val, vlen));
        break;
    case DDUP_OBJ_STREAM:
        obj_stream_free((obj_stream *)obj_unpack_ptr(val, vlen));
        break;
    case DDUP_OBJ_ARRAY:
        obj_array_free((obj_array *)obj_unpack_ptr(val, vlen));
        break;
    case DDUP_OBJ_TIER:
        break; /* no owned object */
    default:
        break;
    }
}

static void array_key(char key[8], uint64_t index)
{
    int i;
    for (i = 0; i < 8; i++)
        key[i] = (char)((index >> (8 * i)) & 0xffu);
}

static uint64_t array_entry_mem(size_t len)
{
    return (uint64_t)sizeof(rh_entry) + 16 + 8 + len;
}

obj_array *obj_array_new(void)
{
    obj_array *a = (obj_array *)calloc(1, sizeof(*a));
    if (a == NULL)
        return NULL;
    rh_init(&a->values);
    a->mem = sizeof(*a);
    return a;
}

void obj_array_free(obj_array *a)
{
    if (a == NULL)
        return;
    rh_destroy(&a->values);
    free(a->history);
    free(a);
}

uint64_t obj_array_mem(const obj_array *a)
{
    return a == NULL ? 0 : a->mem;
}

int obj_array_set(obj_array *a, uint64_t index, const char *const *values,
                  const size_t *lengths, size_t n, size_t *empty_slots)
{
    size_t i;
    size_t empty = 0;
    if (a == NULL || values == NULL || lengths == NULL || n == 0 ||
        index > UINT64_MAX - (uint64_t)n)
        return -1;
    for (i = 0; i < n; i++) {
        if (lengths[i] > UINT32_MAX)
            return -1;
    }
    for (i = 0; i < n; i++) {
        char key[8];
        const char *old;
        size_t old_len;
        uint64_t idx = index + (uint64_t)i;
        array_key(key, idx);
        if (!rh_get(&a->values, key, sizeof(key), &old, &old_len))
            empty++;
        if (rh_set(&a->values, key, sizeof(key), values[i], lengths[i]) < 0)
            return -1;
        if (old != NULL)
            a->mem -= array_entry_mem(old_len);
        a->mem += array_entry_mem(lengths[i]);
    }
    a->count += (uint64_t)empty;
    if (index + (uint64_t)n > a->length)
        a->length = index + (uint64_t)n;
    if (empty_slots != NULL)
        *empty_slots = empty;
    return 0;
}

int obj_array_set_cursor(obj_array *a, uint64_t index)
{
    if (a == NULL) return 0;
    a->next_insert = index;
    return 1;
}

uint64_t obj_array_next(const obj_array *a)
{
    return a == NULL ? 0 : a->next_insert;
}

int obj_array_history_push(obj_array *a, uint64_t index)
{
    uint64_t *p;
    size_t cap;
    if (a->history_len == a->history_cap) {
        cap = a->history_cap ? a->history_cap * 2 : 16;
        if (cap < a->history_cap || cap > SIZE_MAX / sizeof(*p)) return -1;
        p = (uint64_t *)realloc(a->history, cap * sizeof(*p));
        if (!p) return -1;
        a->history = p;
        a->history_cap = cap;
    }
    a->history[a->history_len++] = index;
    a->mem += sizeof(uint64_t);
    return 0;
}

int obj_array_insert(obj_array *a, const char *const *values,
                     const size_t *lengths, size_t n, uint64_t *last_index)
{
    uint64_t start;
    size_t i;
    if (!a || !values || !lengths || n == 0 || a->next_insert > UINT64_MAX - n)
        return -1;
    start = a->next_insert;
    if (obj_array_set(a, start, values, lengths, n, NULL) != 0) return -1;
    for (i = 0; i < n; i++) if (obj_array_history_push(a, start + i) != 0) return -1;
    a->next_insert = start + n;
    if (last_index) *last_index = start + n - 1;
    return 0;
}

int obj_array_ring(obj_array *a, uint64_t size, const char *const *values,
                   const size_t *lengths, size_t n, uint64_t *last_index)
{
    size_t i;
    uint64_t last = 0;
    if (!a || size == 0 || !values || !lengths) return -1;
    a->ring_size = size;
    for (i = 0; i < n; i++) {
        uint64_t index = a->next_insert % size;
        const char *v = values[i];
        size_t l = lengths[i];
        if (obj_array_set(a, index, &v, &l, 1, NULL) != 0) return -1;
        if (obj_array_history_push(a, index) != 0) return -1;
        a->next_insert = index + 1;
        last = index;
    }
    if (last_index) *last_index = last;
    return 0;
}

size_t obj_array_history(const obj_array *a, uint64_t *out, size_t cap, int rev)
{
    size_t n, i;
    if (!a || !out || cap == 0) return 0;
    n = a->history_len < cap ? a->history_len : cap;
    for (i = 0; i < n; i++) {
        size_t pos = rev ? a->history_len - 1 - i : a->history_len - n + i;
        out[i] = a->history[pos];
    }
    return n;
}

int obj_array_get(obj_array *a, uint64_t index, const char **value,
                  size_t *length)
{
    char key[8];
    if (a == NULL)
        return 0;
    array_key(key, index);
    return rh_get(&a->values, key, sizeof(key), value, length);
}

int obj_array_del(obj_array *a, uint64_t index)
{
    char key[8];
    const char *old;
    size_t old_len;
    if (a == NULL)
        return 0;
    array_key(key, index);
    if (!rh_get(&a->values, key, sizeof(key), &old, &old_len))
        return 0;
    if (!rh_del(&a->values, key, sizeof(key)))
        return 0;
    a->mem -= array_entry_mem(old_len);
    if (a->count > 0)
        a->count--;
    return 1;
}

uint64_t obj_array_del_range(obj_array *a, uint64_t start, uint64_t end)
{
    uint64_t deleted = 0;
    uint64_t i;
    if (a == NULL || start > end)
        return 0;
    /* Sparse arrays make this proportional to the requested span. Bound the
     * loop to live length so a huge tail range cannot burn CPU. */
    if (start >= a->length)
        return 0;
    if (end >= a->length)
        end = a->length - 1;
    for (i = start; i <= end; i++) {
        if (obj_array_del(a, i))
            deleted++;
        if (i == UINT64_MAX)
            break;
    }
    while (a->length > 0) {
        char key[8];
        const char *tail;
        size_t tail_len;
        array_key(key, a->length - 1);
        if (rh_get(&a->values, key, sizeof(key), &tail, &tail_len))
            break;
        a->length--;
    }
    return deleted;
}

uint64_t obj_array_len(const obj_array *a) { return a == NULL ? 0 : a->length; }
uint64_t obj_array_count(const obj_array *a) { return a == NULL ? 0 : a->count; }
void obj_array_each(const obj_array *a, rh_iter_fn fn, void *ctx)
{
    if (a != NULL)
        rh_each(&a->values, fn, ctx);
}

/* ------------------------------------------------------------------ */
/* runtime encoding limits (see obj.h)                                 */
/* ------------------------------------------------------------------ */

static obj_limits g_obj_limits = {
    (int)DDUP_QL_FILL,
    OBJ_HASH_MAX_LISTPACK_ENTRIES, OBJ_HASH_MAX_LISTPACK_VALUE,
    OBJ_SET_MAX_LISTPACK_ENTRIES,  OBJ_SET_MAX_LISTPACK_VALUE,
    OBJ_ZSET_MAX_LISTPACK_ENTRIES, OBJ_ZSET_MAX_LISTPACK_VALUE
};

void obj_limits_apply(const obj_limits *lim)
{
    if (lim == NULL)
        return;
    g_obj_limits = *lim;
    quicklist_set_fill(lim->list_fill);
}

void obj_limits_get(obj_limits *out)
{
    if (out == NULL)
        return;
    *out = g_obj_limits;
}

/* ------------------------------------------------------------------ */
/* hash object: listpack for small hashes, rh_table beyond             */
/* ------------------------------------------------------------------ */

/* Same per-entry estimate as the db layer uses for the main table. */
static uint64_t field_bytes(size_t flen, size_t vlen)
{
    return (uint64_t)sizeof(rh_entry) + 16 + flen + vlen;
}

static uint64_t ttl_bytes(size_t flen)
{
    return (uint64_t)sizeof(rh_entry) + 16 + flen + 8;
}

static void pack_expire(char buf[8], uint64_t expire_ms)
{
    int i;
    for (i = 0; i < 8; i++)
        buf[i] = (char)((expire_ms >> (8 * i)) & 0xFFu);
}

static uint64_t unpack_expire(const char *buf)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--)
        v = (v << 8) | (uint64_t)(unsigned char)buf[i];
    return v;
}

/* LP mode: struct + one malloc for the flat listpack. */
static uint64_t hash_lp_mem(const obj_hash *h)
{
    return (uint64_t)sizeof(*h) + 16 + lp_bytes(h->lp) + h->ttl_mem;
}

obj_hash *obj_hash_new(void)
{
    obj_hash *h = (obj_hash *)malloc(sizeof(*h));
    if (h == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    h->encoding = OBJ_HASH_LP;
    h->lp = lp_new();
    rh_init(&h->fields);
    rh_init(&h->expires);
    h->ttl_mem = 0;
    h->mem = hash_lp_mem(h);
    return h;
}

void obj_hash_free(obj_hash *h)
{
    if (h == NULL)
        return;
    if (h->encoding == OBJ_HASH_LP)
        lp_free(h->lp);
    else
        rh_destroy(&h->fields);
    rh_destroy(&h->expires);
    free(h);
}

uint64_t obj_hash_mem(const obj_hash *h)
{
    return h == NULL ? 0 : h->mem;
}

uint64_t obj_hash_len(const obj_hash *h)
{
    if (h == NULL)
        return 0;
    if (h->encoding == OBJ_HASH_LP)
        return lp_length(h->lp) / 2;
    return rh_size(&h->fields);
}

int obj_hash_is_listpack(const obj_hash *h)
{
    return h != NULL && h->encoding == OBJ_HASH_LP;
}

/* One-way conversion LP -> HT. */
static void obj_hash_convert(obj_hash *h)
{
    unsigned char *p;
    uint64_t ttl = h->ttl_mem;
    h->mem = (uint64_t)sizeof(*h) + ttl;
    p = lp_first(h->lp);
    while (p != NULL) {
        unsigned char fb[24], vb[24];
        uint32_t fl = 0, vl = 0;
        const unsigned char *f = lp_get_str(p, fb, &fl);
        const unsigned char *v;
        p = lp_next(h->lp, p); /* the value entry */
        v = lp_get_str(p, vb, &vl);
        if (rh_set(&h->fields, (const char *)f, fl, (const char *)v, vl) == 0)
            h->mem += field_bytes(fl, vl);
        p = lp_next(h->lp, p);
    }
    lp_free(h->lp);
    h->lp = NULL;
    h->encoding = OBJ_HASH_HT;
}

int obj_hash_set(obj_hash *h, const char *f, size_t flen, const char *v,
                 size_t vlen)
{
    if (h == NULL || (f == NULL && flen != 0) ||
        (v == NULL && vlen != 0) || flen > UINT32_MAX ||
        vlen > UINT32_MAX || flen > SIZE_MAX - vlen)
        return -1;
    {
        const char *ev;
        size_t evl;
        if (rh_get(&h->expires, f, flen, &ev, &evl)) {
            h->ttl_mem -= ttl_bytes(flen);
            if (h->encoding == OBJ_HASH_LP)
                h->mem = hash_lp_mem(h);
            else
                h->mem -= ttl_bytes(flen);
            rh_del(&h->expires, f, flen);
        }
    }
    if (h->encoding == OBJ_HASH_LP) {
        unsigned char *fp = NULL;
        int fits = flen <= (size_t)g_obj_limits.hash_value &&
                   vlen <= (size_t)g_obj_limits.hash_value;
        if (fits)
            fp = lp_find(h->lp, NULL, (const unsigned char *)f,
                         (uint32_t)flen);
        if (!fits ||
            (fp == NULL &&
             lp_length(h->lp) / 2 >= (uint64_t)g_obj_limits.hash_entries)) {
            obj_hash_convert(h);
            /* fall through to the HT path below */
        } else if (fp != NULL) {
            unsigned char *vp = lp_next(h->lp, fp);
            h->lp = lp_replace(h->lp, vp, (const unsigned char *)v,
                               (uint32_t)vlen);
            h->mem = hash_lp_mem(h);
            return 0;
        } else {
            h->lp = lp_append(h->lp, (const unsigned char *)f, (uint32_t)flen);
            h->lp = lp_append(h->lp, (const unsigned char *)v, (uint32_t)vlen);
            h->mem = hash_lp_mem(h);
            return 1;
        }
    }
    {
        const char *old;
        size_t oldl;
        int is_new = !rh_get(&h->fields, f, flen, &old, &oldl);
        if (rh_set(&h->fields, f, flen, v, vlen) < 0)
            return -1;
        if (!is_new)
            h->mem -= field_bytes(flen, oldl);
        h->mem += field_bytes(flen, vlen);
        return is_new;
    }
}

int obj_hash_get(obj_hash *h, const char *f, size_t flen, const char **v,
                 size_t *vlen)
{
    if (h == NULL || v == NULL || vlen == NULL ||
        (f == NULL && flen != 0))
        return 0;
    if (h->encoding == OBJ_HASH_LP) {
        unsigned char *fp;
        unsigned char *vp;
        uint32_t vl = 0;
        const unsigned char *vv;
        if (flen > UINT32_MAX)
            return 0;
        fp = lp_find(h->lp, NULL, (const unsigned char *)f, (uint32_t)flen);
        if (fp == NULL)
            return 0;
        vp = lp_next(h->lp, fp);
        vv = lp_get_str(vp, h->vtmp, &vl);
        *v = (const char *)vv;
        *vlen = vl;
        return 1;
    }
    return rh_get(&h->fields, f, flen, v, vlen);
}

int obj_hash_del(obj_hash *h, const char *f, size_t flen)
{
    if (h == NULL || (f == NULL && flen != 0))
        return 0;
    {
        const char *ev;
        size_t evl;
        if (rh_get(&h->expires, f, flen, &ev, &evl)) {
            h->ttl_mem -= ttl_bytes(flen);
            if (h->encoding == OBJ_HASH_LP)
                h->mem = hash_lp_mem(h);
            else
                h->mem -= ttl_bytes(flen);
            rh_del(&h->expires, f, flen);
        }
    }
    if (h->encoding == OBJ_HASH_LP) {
        unsigned char *fp;
        size_t foff, voff;
        if (flen > UINT32_MAX)
            return 0;
        fp = lp_find(h->lp, NULL, (const unsigned char *)f, (uint32_t)flen);
        if (fp == NULL)
            return 0;
        /* delete by offset, higher first so the lower offset stays valid */
        foff = (size_t)(fp - h->lp);
        voff = (size_t)(lp_next(h->lp, fp) - h->lp);
        h->lp = lp_delete(h->lp, h->lp + voff, NULL);
        h->lp = lp_delete(h->lp, h->lp + foff, NULL);
        h->mem = hash_lp_mem(h);
        return 1;
    }
    {
        const char *old;
        size_t oldl;
        if (!rh_get(&h->fields, f, flen, &old, &oldl))
            return 0;
        h->mem -= field_bytes(flen, oldl);
        rh_del(&h->fields, f, flen);
        return 1;
    }
}

static void hash_ttl_mem_delta(obj_hash *h, int64_t delta)
{
    if (delta == 0)
        return;
    if (h->encoding == OBJ_HASH_LP)
        h->mem = hash_lp_mem(h);
    else
        h->mem = (uint64_t)((int64_t)h->mem + delta);
}

int obj_hash_get_at(obj_hash *h, const char *f, size_t flen, uint64_t now_ms,
                    const char **v, size_t *vlen)
{
    const char *ev;
    size_t evl;
    if (h == NULL)
        return -1;
    if (rh_get(&h->expires, f, flen, &ev, &evl) &&
        unpack_expire(ev) <= now_ms) {
        (void)obj_hash_del(h, f, flen);
        return 0;
    }
    return obj_hash_get(h, f, flen, v, vlen);
}

int obj_hash_set_at(obj_hash *h, const char *f, size_t flen, const char *v,
                    size_t vlen, uint64_t now_ms, int keep_ttl)
{
    uint64_t expire = 0;
    int had_ttl = 0;
    int rc;
    if (h == NULL || (f == NULL && flen != 0) ||
        (v == NULL && vlen != 0))
        return -1;
    if (keep_ttl) {
        const char *ev;
        size_t evl;
        if (rh_get(&h->expires, f, flen, &ev, &evl)) {
            expire = unpack_expire(ev);
            had_ttl = 1;
            if (expire <= now_ms) {
                (void)obj_hash_del(h, f, flen);
                had_ttl = 0;
            }
        }
    }
    rc = obj_hash_set(h, f, flen, v, vlen);
    if (rc < 0)
        return rc;
    if (keep_ttl && had_ttl) {
        char buf[8];
        const char *ev;
        size_t evl;
        pack_expire(buf, expire);
        if (!rh_get(&h->expires, f, flen, &ev, &evl)) {
            h->ttl_mem += ttl_bytes(flen);
            hash_ttl_mem_delta(h, (int64_t)ttl_bytes(flen));
        }
        if (rh_set(&h->expires, f, flen, buf, 8) < 0)
            return -1;
    }
    return rc;
}

int obj_hash_del_at(obj_hash *h, const char *f, size_t flen, uint64_t now_ms)
{
    (void)now_ms;
    return obj_hash_del(h, f, flen);
}

int obj_hash_expire_get(obj_hash *h, const char *f, size_t flen,
                        uint64_t *expire_ms)
{
    const char *ev;
    size_t evl;
    if (h == NULL || !obj_hash_get(h, f, flen, &ev, &evl))
        return -1;
    if (!rh_get(&h->expires, f, flen, &ev, &evl))
        return 0;
    if (expire_ms != NULL)
        *expire_ms = unpack_expire(ev);
    return 1;
}

int obj_hash_expire_set(obj_hash *h, const char *f, size_t flen,
                        uint64_t expire_ms, uint64_t now_ms)
{
    char buf[8];
    const char *ev;
    size_t evl;
    int existed;
    if (!obj_hash_get_at(h, f, flen, now_ms, &ev, &evl))
        return 0;
    pack_expire(buf, expire_ms);
    existed = rh_get(&h->expires, f, flen, &ev, &evl);
    if (rh_set(&h->expires, f, flen, buf, 8) < 0)
        return -1;
    if (!existed) {
        h->ttl_mem += ttl_bytes(flen);
        hash_ttl_mem_delta(h, (int64_t)ttl_bytes(flen));
    }
    return 1;
}

int obj_hash_expire_persist(obj_hash *h, const char *f, size_t flen,
                            uint64_t now_ms)
{
    const char *ev;
    size_t evl;
    if (!obj_hash_get_at(h, f, flen, now_ms, &ev, &evl))
        return -1;
    if (!rh_get(&h->expires, f, flen, &ev, &evl))
        return 0;
    rh_del(&h->expires, f, flen);
    h->ttl_mem -= ttl_bytes(flen);
    hash_ttl_mem_delta(h, -(int64_t)ttl_bytes(flen));
    return 1;
}

int obj_hash_ttl(obj_hash *h, const char *f, size_t flen, uint64_t now_ms,
                 uint64_t *ttl_ms)
{
    const char *v;
    size_t vl;
    uint64_t expire;
    if (h == NULL || ttl_ms == NULL ||
        !obj_hash_get_at(h, f, flen, now_ms, &v, &vl))
        return -1;
    if (!rh_get(&h->expires, f, flen, &v, &vl))
        return 0;
    expire = unpack_expire(v);
    *ttl_ms = expire > now_ms ? expire - now_ms : 0;
    return 1;
}

typedef struct hash_purge_ctx {
    obj_hash *h;
    uint64_t now_ms;
} hash_purge_ctx;

static int hash_purge_cb(const char *key, size_t klen, const char *val,
                         size_t vlen, void *ctx)
{
    hash_purge_ctx *c = (hash_purge_ctx *)ctx;
    (void)vlen;
    if (unpack_expire(val) <= c->now_ms)
        (void)obj_hash_del(c->h, key, klen);
    return 0;
}

void obj_hash_purge_expired(obj_hash *h, uint64_t now_ms)
{
    hash_purge_ctx ctx;
    ctx.h = h;
    ctx.now_ms = now_ms;
    (void)rh_scan(&h->expires, 0, SIZE_MAX, hash_purge_cb, &ctx);
}

uint64_t obj_hash_len_at(obj_hash *h, uint64_t now_ms)
{
    obj_hash_purge_expired(h, now_ms);
    return obj_hash_len(h);
}

void obj_hash_each_at(obj_hash *h, rh_iter_fn fn, void *ctx, uint64_t now_ms)
{
    obj_hash_purge_expired(h, now_ms);
    obj_hash_each(h, fn, ctx);
}

void obj_hash_each(obj_hash *h, rh_iter_fn fn, void *ctx)
{
    if (h == NULL || fn == NULL)
        return;
    if (h->encoding == OBJ_HASH_LP) {
        unsigned char *p = lp_first(h->lp);
        while (p != NULL) {
            unsigned char fb[24];
            uint32_t fl = 0, vl = 0;
            const unsigned char *f = lp_get_str(p, fb, &fl);
            const unsigned char *v;
            p = lp_next(h->lp, p); /* the value entry */
            v = lp_get_str(p, h->vtmp, &vl);
            fn((const char *)f, fl, (const char *)v, vl, ctx);
            p = lp_next(h->lp, p);
        }
        return;
    }
    rh_each(&h->fields, fn, ctx);
}

int obj_hash_pair_at(obj_hash *h, uint64_t idx, const char **f, size_t *flen,
                     const char **v, size_t *vlen)
{
    unsigned char *p;
    uint32_t fl = 0;
    const unsigned char *fv;
    if (h == NULL || f == NULL || flen == NULL ||
        (v == NULL) != (vlen == NULL) || h->encoding != OBJ_HASH_LP ||
        idx >= lp_length(h->lp) / 2)
        return 0;
    p = lp_seek(h->lp, (long)(idx * 2));
    if (p == NULL)
        return 0;
    fv = lp_get_str(p, h->ftmp, &fl);
    *f = (const char *)fv;
    *flen = fl;
    if (v != NULL) {
        uint32_t vl = 0;
        const unsigned char *vv = lp_get_str(lp_next(h->lp, p), h->vtmp, &vl);
        *v = (const char *)vv;
        *vlen = vl;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* list object: quicklist of listpack nodes                            */
/* ------------------------------------------------------------------ */

obj_list *obj_list_new(void)
{
    obj_list *l = (obj_list *)malloc(sizeof(*l));
    if (l == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    ql_init(&l->ql);
    return l;
}

void obj_list_free(obj_list *l)
{
    if (l == NULL)
        return;
    ql_release(&l->ql);
    free(l);
}

uint64_t obj_list_mem(const obj_list *l)
{
    return l == NULL ? 0 : ql_mem(&l->ql);
}

uint64_t obj_list_len(const obj_list *l)
{
    return l == NULL ? 0 : l->ql.len;
}

int obj_list_push(obj_list *l, int left, const char *data, size_t len)
{
    if (l == NULL || (data == NULL && len != 0))
        return -1;
    return ql_push(&l->ql, left, data, len);
}

int obj_list_push_many(obj_list *l, int left, const char *const *data,
                       const size_t *lens, size_t count)
{
    size_t i;
    if (l == NULL || (data == NULL && count != 0) ||
        (lens == NULL && count != 0))
        return -1;
    /* prevalidate so a length error commits nothing */
    for (i = 0; i < count; i++) {
        if ((data[i] == NULL && lens[i] != 0) || lens[i] > UINT32_MAX)
            return -1;
    }
    for (i = 0; i < count; i++) {
        if (ql_push(&l->ql, left, data[i], lens[i]) != 0)
            return -1; /* unreachable after prevalidation */
    }
    return 0;
}

int obj_list_pop(obj_list *l, int left, char **data, size_t *len)
{
    if (l == NULL)
        return 0;
    return ql_pop(&l->ql, left, data, len);
}

int obj_list_seek(obj_list *l, size_t idx, obj_list_iter *it)
{
    if (l == NULL)
        return 0;
    return ql_seek(&l->ql, idx, it);
}

int obj_list_first(obj_list *l, obj_list_iter *it)
{
    if (l == NULL)
        return 0;
    return ql_first(&l->ql, it);
}

int obj_list_last(obj_list *l, obj_list_iter *it)
{
    if (l == NULL)
        return 0;
    return ql_last(&l->ql, it);
}

int obj_list_iter_next(obj_list_iter *it)
{
    return ql_iter_next(it);
}

int obj_list_iter_prev(obj_list_iter *it)
{
    return ql_iter_prev(it);
}

const char *obj_list_iter_value(obj_list_iter *it, size_t *len)
{
    return ql_iter_value(it, len);
}

int obj_list_set_at(obj_list *l, size_t idx, const char *data, size_t len)
{
    obj_list_iter it;
    if (l == NULL || (data == NULL && len != 0) || len > UINT32_MAX)
        return -1;
    if (!ql_seek(&l->ql, idx, &it))
        return 0;
    return ql_set(&it, data, len) == 1 ? 1 : -1;
}

int obj_list_remove_at(obj_list_iter *it)
{
    ql_remove(it);
    return it->entry != NULL;
}

int obj_list_insert(obj_list_iter *it, int after, const char *data,
                    size_t len)
{
    return ql_insert(it, after, data, len);
}

/* ------------------------------------------------------------------ */
/* set object: listpack for small sets, rh_table beyond                */
/* ------------------------------------------------------------------ */

/* LP mode: struct + one malloc for the flat listpack. */
static uint64_t set_lp_mem(const obj_set *s)
{
    return (uint64_t)sizeof(*s) + 16 + lp_bytes(s->lp);
}

obj_set *obj_set_new(void)
{
    obj_set *s = (obj_set *)malloc(sizeof(*s));
    if (s == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    s->encoding = OBJ_SET_LP;
    s->lp = lp_new();
    s->mem = set_lp_mem(s);
    return s;
}

void obj_set_free(obj_set *s)
{
    if (s == NULL)
        return;
    if (s->encoding == OBJ_SET_LP)
        lp_free(s->lp);
    else
        rh_destroy(&s->members);
    free(s);
}

uint64_t obj_set_mem(const obj_set *s)
{
    return s == NULL ? 0 : s->mem;
}

uint64_t obj_set_len(const obj_set *s)
{
    if (s == NULL)
        return 0;
    if (s->encoding == OBJ_SET_LP)
        return lp_length(s->lp);
    return rh_size(&s->members);
}

int obj_set_is_listpack(const obj_set *s)
{
    return s != NULL && s->encoding == OBJ_SET_LP;
}

/* One-way conversion LP -> HT. */
static void obj_set_convert(obj_set *s)
{
    unsigned char *lp = s->lp;
    unsigned char *p = lp_first(lp);
    rh_init(&s->members);
    s->mem = (uint64_t)sizeof(*s);
    while (p != NULL) {
        unsigned char mb[24];
        uint32_t ml = 0;
        const unsigned char *mv = lp_get_str(p, mb, &ml);
        if (rh_set(&s->members, (const char *)mv, ml, "", 0) == 0)
            s->mem += field_bytes(ml, 0);
        p = lp_next(lp, p);
    }
    lp_free(lp);
    s->lp = NULL;
    s->encoding = OBJ_SET_HT;
}

int obj_set_add(obj_set *s, const char *m, size_t mlen)
{
    if (s == NULL || (m == NULL && mlen != 0) || mlen > UINT32_MAX)
        return -1;
    if (s->encoding == OBJ_SET_LP) {
        if (mlen <= (size_t)g_obj_limits.set_value &&
            lp_find(s->lp, NULL, (const unsigned char *)m, (uint32_t)mlen) !=
                NULL)
            return 0;
        if (mlen > (size_t)g_obj_limits.set_value ||
            lp_length(s->lp) >= (uint64_t)g_obj_limits.set_entries) {
            obj_set_convert(s);
            /* fall through to the HT path below */
        } else {
            s->lp = lp_append(s->lp, (const unsigned char *)m, (uint32_t)mlen);
            s->mem = set_lp_mem(s);
            return 1;
        }
    }
    {
        const char *old;
        size_t oldl;
        if (rh_get(&s->members, m, mlen, &old, &oldl))
            return 0;
        if (rh_set(&s->members, m, mlen, "", 0) < 0)
            return -1;
        s->mem += field_bytes(mlen, 0);
        return 1;
    }
}

int obj_set_has(obj_set *s, const char *m, size_t mlen)
{
    if (s == NULL || (m == NULL && mlen != 0))
        return 0;
    if (s->encoding == OBJ_SET_LP) {
        if (mlen > UINT32_MAX)
            return 0;
        return lp_find(s->lp, NULL, (const unsigned char *)m,
                       (uint32_t)mlen) != NULL;
    }
    {
        const char *old;
        size_t oldl;
        return rh_get(&s->members, m, mlen, &old, &oldl);
    }
}

int obj_set_rem(obj_set *s, const char *m, size_t mlen)
{
    if (s == NULL || (m == NULL && mlen != 0))
        return 0;
    if (s->encoding == OBJ_SET_LP) {
        unsigned char *p;
        if (mlen > UINT32_MAX)
            return 0;
        p = lp_find(s->lp, NULL, (const unsigned char *)m, (uint32_t)mlen);
        if (p == NULL)
            return 0;
        /* callers must not pass m pointing into the listpack without
         * copying it first: lp_delete reallocs (SPOP copies to a local) */
        s->lp = lp_delete(s->lp, p, NULL);
        s->mem = set_lp_mem(s);
        return 1;
    }
    {
        if (!obj_set_has(s, m, mlen))
            return 0;
        rh_del(&s->members, m, mlen);
        s->mem -= field_bytes(mlen, 0);
        return 1;
    }
}

void obj_set_each(obj_set *s, rh_iter_fn fn, void *ctx)
{
    if (s == NULL || fn == NULL)
        return;
    if (s->encoding == OBJ_SET_LP) {
        unsigned char *p = lp_first(s->lp);
        while (p != NULL) {
            unsigned char mb[24];
            uint32_t ml = 0;
            const unsigned char *mv = lp_get_str(p, mb, &ml);
            fn((const char *)mv, ml, "", 0, ctx);
            p = lp_next(s->lp, p);
        }
        return;
    }
    rh_each(&s->members, fn, ctx);
}

int obj_set_member_at(obj_set *s, uint64_t idx, const char **m, size_t *mlen)
{
    unsigned char *p;
    uint32_t ml = 0;
    const unsigned char *mv;
    if (s == NULL || m == NULL || mlen == NULL ||
        s->encoding != OBJ_SET_LP || idx >= lp_length(s->lp) ||
        idx > (uint64_t)LONG_MAX)
        return 0;
    p = lp_seek(s->lp, (long)idx);
    if (p == NULL)
        return 0;
    mv = lp_get_str(p, s->mtmp, &ml);
    *m = (const char *)mv;
    *mlen = ml;
    return 1;
}

/* ------------------------------------------------------------------ */
/* zset object: listpack for small zsets, dict + skiplist beyond       */
/* ------------------------------------------------------------------ */

static void put_score(char buf[8], double score)
{
    memcpy(buf, &score, 8);
}

static double get_score(const char *buf)
{
    double v;
    memcpy(&v, buf, 8);
    return v;
}

/* LP mode: struct + one malloc for the flat listpack. */
static uint64_t zset_lp_mem(const obj_zset *z)
{
    return (uint64_t)sizeof(*z) + 16 + lp_bytes(z->lp);
}

/* Score entries are %.17g decimal strings (possibly int-encoded when
 * integral); both forms materialize to text that strtod parses back to
 * the exact same double. */
static double zlp_entry_score(const unsigned char *p, unsigned char buf[24])
{
    uint32_t sl = 0;
    const unsigned char *sv = lp_get_str(p, buf, &sl);
    char tmp[32];
    memcpy(tmp, sv, sl);
    tmp[sl] = '\0';
    return strtod(tmp, NULL);
}

/* Three-way compare of the entry at p against (m, mlen), prefix rule,
 * same tiebreak as the skiplist. */
static int zlp_cmp_member(const unsigned char *p, unsigned char buf[24],
                          const char *m, size_t mlen)
{
    uint32_t el = 0;
    const unsigned char *ev = lp_get_str(p, buf, &el);
    size_t minl = (size_t)el < mlen ? (size_t)el : mlen;
    int c = minl > 0 ? memcmp(ev, m, minl) : 0;
    if (c != 0)
        return c < 0 ? -1 : 1;
    if ((size_t)el < mlen)
        return -1;
    if ((size_t)el > mlen)
        return 1;
    return 0;
}

/* Locate the member entry of m in the listpack (NULL when absent). */
static unsigned char *zlp_find_member(obj_zset *z, const char *m, size_t mlen)
{
    unsigned char buf[24];
    unsigned char *p = lp_first(z->lp);
    while (p != NULL) {
        if (zlp_cmp_member(p, buf, m, mlen) == 0)
            return p;
        p = lp_next(z->lp, p); /* the score entry */
        if (p != NULL)
            p = lp_next(z->lp, p);
    }
    return NULL;
}

/* First member entry e with (e.score, e.member) > (score, m); NULL when
 * the new pair belongs at the tail. */
static unsigned char *zlp_insert_pos(obj_zset *z, const char *m, size_t mlen,
                                     double score)
{
    unsigned char mbuf[24], sbuf[24];
    unsigned char *p = lp_first(z->lp);
    while (p != NULL) {
        unsigned char *sp = lp_next(z->lp, p);
        double es = zlp_entry_score(sp, sbuf);
        if (es > score)
            return p;
        if (es == score && zlp_cmp_member(p, mbuf, m, mlen) > 0)
            return p;
        p = lp_next(z->lp, sp);
    }
    return NULL;
}

/* Insert a NEW (member, score) pair at its sorted position. */
static void zlp_insert_pair(obj_zset *z, const char *m, size_t mlen,
                            double score)
{
    char sbuf[32];
    int slen = snprintf(sbuf, sizeof(sbuf), "%.17g", score);
    unsigned char *pos = zlp_insert_pos(z, m, mlen, score);
    unsigned char *mp = NULL;
    z->lp = lp_insert(z->lp, (const unsigned char *)m, (uint32_t)mlen, pos,
                      LP_BEFORE, &mp);
    z->lp = lp_insert(z->lp, (const unsigned char *)sbuf, (uint32_t)slen, mp,
                      LP_AFTER, NULL);
}

/* Delete the member entry at mp together with its score entry. */
static void zlp_delete_pair(obj_zset *z, unsigned char *mp)
{
    /* delete by offset, higher first so the lower offset stays valid */
    size_t moff = (size_t)(mp - z->lp);
    size_t soff = (size_t)(lp_next(z->lp, mp) - z->lp);
    z->lp = lp_delete(z->lp, z->lp + soff, NULL);
    z->lp = lp_delete(z->lp, z->lp + moff, NULL);
}

obj_zset *obj_zset_new(void)
{
    obj_zset *z = (obj_zset *)malloc(sizeof(*z));
    if (z == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    z->encoding = OBJ_ZSET_LP;
    z->lp = lp_new();
    z->sl = NULL;
    z->dict_mem = 0;
    return z;
}

void obj_zset_free(obj_zset *z)
{
    if (z == NULL)
        return;
    if (z->encoding == OBJ_ZSET_LP) {
        lp_free(z->lp);
    } else {
        rh_destroy(&z->dict);
        zsl_free(z->sl);
    }
    free(z);
}

uint64_t obj_zset_mem(const obj_zset *z)
{
    if (z == NULL)
        return 0;
    if (z->encoding == OBJ_ZSET_LP)
        return zset_lp_mem(z);
    return (uint64_t)sizeof(*z) + z->dict_mem + z->sl->mem;
}

uint64_t obj_zset_len(const obj_zset *z)
{
    if (z == NULL)
        return 0;
    if (z->encoding == OBJ_ZSET_LP)
        return lp_length(z->lp) / 2;
    return rh_size(&z->dict);
}

int obj_zset_is_listpack(const obj_zset *z)
{
    return z != NULL && z->encoding == OBJ_ZSET_LP;
}

/* One-way conversion LP -> HT. */
static void obj_zset_convert(obj_zset *z)
{
    unsigned char *lp = z->lp;
    unsigned char *p = lp_first(lp);
    rh_init(&z->dict);
    z->sl = zsl_create();
    z->dict_mem = 0;
    while (p != NULL) {
        unsigned char mbuf[24], sbuf[24];
        uint32_t ml = 0;
        const unsigned char *mv = lp_get_str(p, mbuf, &ml);
        double score = zlp_entry_score(lp_next(lp, p), sbuf);
        char b[8];
        put_score(b, score);
        if (rh_set(&z->dict, (const char *)mv, ml, b, 8) == 0)
            z->dict_mem += field_bytes(ml, 8);
        zsl_insert(z->sl, score, (const char *)mv, ml);
        p = lp_next(lp, lp_next(lp, p));
    }
    lp_free(lp);
    z->lp = NULL;
    z->encoding = OBJ_ZSET_HT;
}

int obj_zset_add(obj_zset *z, const char *m, size_t mlen, double score)
{
    if (z == NULL || (m == NULL && mlen != 0) || mlen > UINT32_MAX)
        return -1;
    if (z->encoding == OBJ_ZSET_LP) {
        unsigned char *mp = NULL;
        if (mlen <= (size_t)g_obj_limits.zset_value)
            mp = zlp_find_member(z, m, mlen);
        if (mp != NULL) {
            unsigned char sbuf[24];
            double oldscore = zlp_entry_score(lp_next(z->lp, mp), sbuf);
            if (oldscore == score)
                return 0;
            zlp_delete_pair(z, mp);
            zlp_insert_pair(z, m, mlen, score);
            return 0;
        }
        if (mlen > (size_t)g_obj_limits.zset_value ||
            lp_length(z->lp) / 2 >= (uint64_t)g_obj_limits.zset_entries) {
            obj_zset_convert(z);
            /* fall through to the HT path below */
        } else {
            zlp_insert_pair(z, m, mlen, score);
            return 1;
        }
    }
    {
        const char *old;
        size_t oldl;
        char b[8];
        if (rh_get(&z->dict, m, mlen, &old, &oldl) && oldl == 8) {
            double oldscore = get_score(old);
            if (oldscore == score)
                return 0;
            put_score(b, score);
            if (rh_set(&z->dict, m, mlen, b, 8) < 0)
                return -1;
            zsl_delete(z->sl, oldscore, m, mlen);
            zsl_insert(z->sl, score, m, mlen);
            return 0;
        }
        put_score(b, score);
        if (rh_set(&z->dict, m, mlen, b, 8) < 0)
            return -1;
        z->dict_mem += field_bytes(mlen, 8);
        zsl_insert(z->sl, score, m, mlen);
        return 1;
    }
}

int obj_zset_score(obj_zset *z, const char *m, size_t mlen, double *score)
{
    if (z == NULL || score == NULL || (m == NULL && mlen != 0))
        return 0;
    if (z->encoding == OBJ_ZSET_LP) {
        unsigned char sbuf[24];
        unsigned char *mp = zlp_find_member(z, m, mlen);
        if (mp == NULL)
            return 0;
        *score = zlp_entry_score(lp_next(z->lp, mp), sbuf);
        return 1;
    }
    {
        const char *old;
        size_t oldl;
        if (!rh_get(&z->dict, m, mlen, &old, &oldl) || oldl != 8)
            return 0;
        *score = get_score(old);
        return 1;
    }
}

int obj_zset_rem(obj_zset *z, const char *m, size_t mlen)
{
    if (z == NULL || (m == NULL && mlen != 0))
        return 0;
    if (z->encoding == OBJ_ZSET_LP) {
        unsigned char *mp = zlp_find_member(z, m, mlen);
        if (mp == NULL)
            return 0;
        zlp_delete_pair(z, mp);
        return 1;
    }
    {
        double score;
        if (!obj_zset_score(z, m, mlen, &score))
            return 0;
        /* delete from the dict BEFORE zsl_delete: when m points into the
         * skiplist node (obj_zset_rem_range_by_* spans), freeing the node
         * first would leave rh_del hashing dangling memory (zcard/exists
         * kept stale entries). */
        rh_del(&z->dict, m, mlen);
        zsl_delete(z->sl, score, m, mlen);
        z->dict_mem -= field_bytes(mlen, 8);
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* zset iterators                                                      */
/* ------------------------------------------------------------------ */

int obj_zset_seek(obj_zset *z, size_t idx, obj_zset_iter *it)
{
    it->z = z;
    if (z->encoding == OBJ_ZSET_LP) {
        if (idx >= lp_length(z->lp) / 2)
            return 0;
        it->u.lp.p = lp_seek(z->lp, (long)(idx * 2));
        return it->u.lp.p != NULL;
    }
    it->u.node = zsl_at(z->sl, idx);
    return it->u.node != NULL;
}

int obj_zset_first(obj_zset *z, obj_zset_iter *it)
{
    it->z = z;
    if (z->encoding == OBJ_ZSET_LP) {
        it->u.lp.p = lp_first(z->lp);
        return it->u.lp.p != NULL;
    }
    it->u.node = z->sl->header->level[0].forward;
    return it->u.node != NULL;
}

int obj_zset_last(obj_zset *z, obj_zset_iter *it)
{
    it->z = z;
    if (z->encoding == OBJ_ZSET_LP) {
        unsigned char *sp = lp_last(z->lp); /* score entry of the tail */
        it->u.lp.p = sp != NULL ? lp_prev(z->lp, sp) : NULL;
        return it->u.lp.p != NULL;
    }
    it->u.node = z->sl->tail;
    return it->u.node != NULL;
}

int obj_zset_iter_next(obj_zset_iter *it)
{
    obj_zset *z = it->z;
    if (z->encoding == OBJ_ZSET_LP) {
        unsigned char *sp = lp_next(z->lp, it->u.lp.p);
        it->u.lp.p = sp != NULL ? lp_next(z->lp, sp) : NULL;
        return it->u.lp.p != NULL;
    }
    it->u.node = it->u.node->level[0].forward;
    return it->u.node != NULL;
}

int obj_zset_iter_prev(obj_zset_iter *it)
{
    obj_zset *z = it->z;
    if (z->encoding == OBJ_ZSET_LP) {
        unsigned char *sp = lp_prev(z->lp, it->u.lp.p);
        it->u.lp.p = sp != NULL ? lp_prev(z->lp, sp) : NULL;
        return it->u.lp.p != NULL;
    }
    it->u.node = it->u.node->backward;
    return it->u.node != NULL;
}

int obj_zset_iter_eq(const obj_zset_iter *a, const obj_zset_iter *b)
{
    if (a->z->encoding == OBJ_ZSET_LP)
        return a->u.lp.p == b->u.lp.p;
    return a->u.node == b->u.node;
}

const char *obj_zset_iter_member(obj_zset_iter *it, size_t *mlen)
{
    if (it->z->encoding == OBJ_ZSET_LP) {
        uint32_t ml = 0;
        const unsigned char *mv = lp_get_str(it->u.lp.p, it->u.lp.mbuf, &ml);
        *mlen = ml;
        return (const char *)mv;
    }
    *mlen = it->u.node->mlen;
    return it->u.node->member;
}

double obj_zset_iter_score(obj_zset_iter *it)
{
    if (it->z->encoding == OBJ_ZSET_LP) {
        unsigned char sbuf[24];
        return zlp_entry_score(lp_next(it->z->lp, it->u.lp.p), sbuf);
    }
    return it->u.node->score;
}

/* ------------------------------------------------------------------ */
/* zset range queries                                                  */
/* ------------------------------------------------------------------ */

/* Same range/lex predicates as the skiplist's statics. */
static int zscore_gte_min(double score, const zrangespec *r)
{
    return r->minex ? score > r->min : score >= r->min;
}

static int zscore_lte_max(double score, const zrangespec *r)
{
    return r->maxex ? score < r->max : score <= r->max;
}

static int zlex_gte_min(const char *m, size_t mlen, const zlexbound *min)
{
    if (min->inf != 0)
        return min->inf < 0;
    {
        size_t minl = mlen < min->len ? mlen : min->len;
        int c = minl > 0 ? memcmp(m, min->s, minl) : 0;
        if (c == 0)
            c = mlen < min->len ? -1 : mlen > min->len ? 1 : 0;
        return min->ex ? c > 0 : c >= 0;
    }
}

static int zlex_lte_max(const char *m, size_t mlen, const zlexbound *max)
{
    if (max->inf != 0)
        return max->inf > 0;
    {
        size_t minl = mlen < max->len ? mlen : max->len;
        int c = minl > 0 ? memcmp(m, max->s, minl) : 0;
        if (c == 0)
            c = mlen < max->len ? -1 : mlen > max->len ? 1 : 0;
        return max->ex ? c < 0 : c <= 0;
    }
}

int obj_zset_first_in_range(obj_zset *z, const zrangespec *r,
                            obj_zset_iter *it)
{
    it->z = z;
    if (z->encoding == OBJ_ZSET_LP) {
        unsigned char sbuf[24];
        unsigned char *p = lp_first(z->lp);
        while (p != NULL) {
            double es = zlp_entry_score(lp_next(z->lp, p), sbuf);
            if (zscore_gte_min(es, r)) {
                if (!zscore_lte_max(es, r))
                    return 0;
                it->u.lp.p = p;
                return 1;
            }
            p = lp_next(z->lp, lp_next(z->lp, p));
        }
        return 0;
    }
    it->u.node = zsl_first_in_range(z->sl, r);
    return it->u.node != NULL;
}

int obj_zset_last_in_range(obj_zset *z, const zrangespec *r,
                           obj_zset_iter *it)
{
    it->z = z;
    if (z->encoding == OBJ_ZSET_LP) {
        unsigned char sbuf[24];
        unsigned char *sp = lp_last(z->lp);
        while (sp != NULL) {
            unsigned char *mp = lp_prev(z->lp, sp);
            double es = zlp_entry_score(sp, sbuf);
            if (zscore_lte_max(es, r)) {
                if (!zscore_gte_min(es, r))
                    return 0;
                it->u.lp.p = mp;
                return 1;
            }
            sp = lp_prev(z->lp, mp);
        }
        return 0;
    }
    it->u.node = zsl_last_in_range(z->sl, r);
    return it->u.node != NULL;
}

size_t obj_zset_count_in_range(obj_zset *z, const zrangespec *r)
{
    if (z->encoding == OBJ_ZSET_LP) {
        obj_zset_iter it;
        size_t n = 0;
        if (!obj_zset_first_in_range(z, r, &it))
            return 0;
        for (;;) {
            n++;
            if (!obj_zset_iter_next(&it) ||
                !zscore_lte_max(obj_zset_iter_score(&it), r))
                break;
        }
        return n;
    }
    return zsl_count_in_range(z->sl, r);
}

int obj_zset_first_in_lex_range(obj_zset *z, const zlexrangespec *r,
                                obj_zset_iter *it)
{
    it->z = z;
    if (z->encoding == OBJ_ZSET_LP) {
        unsigned char mbuf[24];
        unsigned char *p = lp_first(z->lp);
        while (p != NULL) {
            uint32_t ml = 0;
            const unsigned char *mv = lp_get_str(p, mbuf, &ml);
            if (zlex_gte_min((const char *)mv, ml, &r->min)) {
                if (!zlex_lte_max((const char *)mv, ml, &r->max))
                    return 0;
                it->u.lp.p = p;
                return 1;
            }
            p = lp_next(z->lp, lp_next(z->lp, p));
        }
        return 0;
    }
    it->u.node = zsl_first_in_lex_range(z->sl, r);
    return it->u.node != NULL;
}

int obj_zset_last_in_lex_range(obj_zset *z, const zlexrangespec *r,
                               obj_zset_iter *it)
{
    it->z = z;
    if (z->encoding == OBJ_ZSET_LP) {
        unsigned char mbuf[24];
        unsigned char *sp = lp_last(z->lp);
        while (sp != NULL) {
            unsigned char *mp = lp_prev(z->lp, sp);
            uint32_t ml = 0;
            const unsigned char *mv = lp_get_str(mp, mbuf, &ml);
            if (zlex_lte_max((const char *)mv, ml, &r->max)) {
                if (!zlex_gte_min((const char *)mv, ml, &r->min))
                    return 0;
                it->u.lp.p = mp;
                return 1;
            }
            sp = lp_prev(z->lp, mp);
        }
        return 0;
    }
    it->u.node = zsl_last_in_lex_range(z->sl, r);
    return it->u.node != NULL;
}

long obj_zset_rank(obj_zset *z, double score, const char *m, size_t mlen)
{
    if (z->encoding == OBJ_ZSET_LP) {
        unsigned char mbuf[24];
        unsigned char *p = lp_first(z->lp);
        long idx = 0;
        (void)score; /* member is unique; its position implies the score */
        while (p != NULL) {
            if (zlp_cmp_member(p, mbuf, m, mlen) == 0)
                return idx;
            idx++;
            p = lp_next(z->lp, lp_next(z->lp, p));
        }
        return -1;
    }
    return zsl_rank(z->sl, score, m, mlen);
}

/* ------------------------------------------------------------------ */
/* zset bulk removals / pop                                            */
/* ------------------------------------------------------------------ */

/* Delete the inclusive level-0 node span [first, last] from both sides
 * of the HT zset (dict + skiplist); each node's forward link is captured
 * before it is freed. */
static uint64_t zset_rem_node_span(obj_zset *z, zsl_node *first,
                                   zsl_node *last)
{
    uint64_t removed = 0;
    while (first != NULL) {
        zsl_node *next = first->level[0].forward;
        removed += (uint64_t)obj_zset_rem(z, first->member, first->mlen);
        if (first == last)
            break;
        first = next;
    }
    return removed;
}

uint64_t obj_zset_rem_range_by_rank(obj_zset *z, size_t start, size_t stop)
{
    uint64_t removed = 0;
    if (z->encoding == OBJ_ZSET_LP) {
        size_t n = stop - start + 1;
        while (n-- > 0) {
            unsigned char *mp;
            if (start >= lp_length(z->lp) / 2)
                break;
            mp = lp_seek(z->lp, (long)(start * 2));
            if (mp == NULL)
                break;
            zlp_delete_pair(z, mp);
            removed++;
        }
        return removed;
    }
    return zset_rem_node_span(z, zsl_at(z->sl, start), zsl_at(z->sl, stop));
}

uint64_t obj_zset_rem_range_by_score(obj_zset *z, const zrangespec *r)
{
    uint64_t removed = 0;
    if (z->encoding == OBJ_ZSET_LP) {
        /* the in-range span is contiguous: delete its head repeatedly */
        for (;;) {
            unsigned char sbuf[24];
            unsigned char *p = lp_first(z->lp);
            while (p != NULL &&
                   !zscore_gte_min(zlp_entry_score(lp_next(z->lp, p), sbuf),
                                   r))
                p = lp_next(z->lp, lp_next(z->lp, p));
            if (p == NULL ||
                !zscore_lte_max(zlp_entry_score(lp_next(z->lp, p), sbuf), r))
                break;
            zlp_delete_pair(z, p);
            removed++;
        }
        return removed;
    }
    {
        zsl_node *n = zsl_first_in_range(z->sl, r);
        while (n != NULL && zscore_lte_max(n->score, r)) {
            zsl_node *next = n->level[0].forward;
            removed += (uint64_t)obj_zset_rem(z, n->member, n->mlen);
            n = next;
        }
        return removed;
    }
}

uint64_t obj_zset_rem_range_by_lex(obj_zset *z, const zlexrangespec *r)
{
    uint64_t removed = 0;
    if (z->encoding == OBJ_ZSET_LP) {
        for (;;) {
            unsigned char mbuf[24];
            unsigned char *p = lp_first(z->lp);
            uint32_t ml = 0;
            const unsigned char *mv;
            while (p != NULL) {
                mv = lp_get_str(p, mbuf, &ml);
                if (zlex_gte_min((const char *)mv, ml, &r->min))
                    break;
                p = lp_next(z->lp, lp_next(z->lp, p));
            }
            if (p == NULL)
                break;
            mv = lp_get_str(p, mbuf, &ml);
            if (!zlex_lte_max((const char *)mv, ml, &r->max))
                break;
            zlp_delete_pair(z, p);
            removed++;
        }
        return removed;
    }
    {
        zsl_node *first = zsl_first_in_lex_range(z->sl, r);
        zsl_node *last = zsl_last_in_lex_range(z->sl, r);
        if (first == NULL || last == NULL)
            return 0;
        return zset_rem_node_span(z, first, last);
    }
}

int obj_zset_pop(obj_zset *z, int min, char **member, size_t *mlen,
                 double *score)
{
    if (z == NULL || member == NULL || mlen == NULL || score == NULL)
        return 0;
    if (z->encoding == OBJ_ZSET_LP) {
        unsigned char mbuf[24], sbuf[24];
        unsigned char *mp;
        uint32_t ml = 0;
        const unsigned char *mv;
        char *copy;
        if (min) {
            mp = lp_first(z->lp);
        } else {
            unsigned char *sp = lp_last(z->lp);
            mp = sp != NULL ? lp_prev(z->lp, sp) : NULL;
        }
        if (mp == NULL)
            return 0;
        mv = lp_get_str(mp, mbuf, &ml);
        copy = (char *)malloc(ml > 0 ? ml : 1);
        if (copy == NULL) {
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        memcpy(copy, mv, ml);
        *score = zlp_entry_score(lp_next(z->lp, mp), sbuf);
        *member = copy;
        *mlen = ml;
        zlp_delete_pair(z, mp);
        return 1;
    }
    {
        zsl_node *node =
            min ? z->sl->header->level[0].forward : z->sl->tail;
        char *copy;
        if (node == NULL)
            return 0;
        copy = (char *)malloc(node->mlen > 0 ? node->mlen : 1);
        if (copy == NULL) {
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        memcpy(copy, node->member, node->mlen);
        *score = node->score;
        *member = copy;
        *mlen = node->mlen;
        obj_zset_rem(z, node->member, node->mlen);
        return 1;
    }
}
