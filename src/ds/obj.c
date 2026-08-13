/* obj.c - typed value objects; see obj.h. */
#include "ds/listpack.h"
#include "ds/obj.h"

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
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* hash object: listpack for small hashes, rh_table beyond             */
/* ------------------------------------------------------------------ */

/* Same per-entry estimate as the db layer uses for the main table. */
static uint64_t field_bytes(size_t flen, size_t vlen)
{
    return (uint64_t)sizeof(rh_entry) + 16 + flen + vlen;
}

/* LP mode: struct + one malloc for the flat listpack. */
static uint64_t hash_lp_mem(const obj_hash *h)
{
    return (uint64_t)sizeof(*h) + 16 + lp_bytes(h->lp);
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
    free(h);
}

uint64_t obj_hash_mem(const obj_hash *h)
{
    return h->mem;
}

uint64_t obj_hash_len(const obj_hash *h)
{
    if (h->encoding == OBJ_HASH_LP)
        return lp_length(h->lp) / 2;
    return rh_size(&h->fields);
}

int obj_hash_is_listpack(const obj_hash *h)
{
    return h->encoding == OBJ_HASH_LP;
}

/* One-way conversion LP -> HT. */
static void obj_hash_convert(obj_hash *h)
{
    unsigned char *p;
    rh_init(&h->fields);
    h->mem = (uint64_t)sizeof(*h);
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
    if (flen > UINT32_MAX || vlen > UINT32_MAX || flen > SIZE_MAX - vlen)
        return -1;
    if (h->encoding == OBJ_HASH_LP) {
        unsigned char *fp = NULL;
        int fits = flen <= OBJ_HASH_MAX_LISTPACK_VALUE &&
                   vlen <= OBJ_HASH_MAX_LISTPACK_VALUE;
        if (fits)
            fp = lp_find(h->lp, NULL, (const unsigned char *)f,
                         (uint32_t)flen);
        if (!fits ||
            (fp == NULL &&
             lp_length(h->lp) / 2 >= OBJ_HASH_MAX_LISTPACK_ENTRIES)) {
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

void obj_hash_each(obj_hash *h, rh_iter_fn fn, void *ctx)
{
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
    if (h->encoding != OBJ_HASH_LP || idx >= lp_length(h->lp) / 2)
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
    return ql_mem(&l->ql);
}

uint64_t obj_list_len(const obj_list *l)
{
    return l->ql.len;
}

int obj_list_push(obj_list *l, int left, const char *data, size_t len)
{
    return ql_push(&l->ql, left, data, len);
}

int obj_list_push_many(obj_list *l, int left, const char *const *data,
                       const size_t *lens, size_t count)
{
    size_t i;
    /* prevalidate so a length error commits nothing */
    for (i = 0; i < count; i++) {
        if (lens[i] > UINT32_MAX)
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
    return ql_pop(&l->ql, left, data, len);
}

int obj_list_seek(obj_list *l, size_t idx, obj_list_iter *it)
{
    return ql_seek(&l->ql, idx, it);
}

int obj_list_first(obj_list *l, obj_list_iter *it)
{
    return ql_first(&l->ql, it);
}

int obj_list_last(obj_list *l, obj_list_iter *it)
{
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
    if (len > UINT32_MAX)
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

/* ------------------------------------------------------------------ */
/* set object                                                         */
/* ------------------------------------------------------------------ */

obj_set *obj_set_new(void)
{
    obj_set *s = (obj_set *)malloc(sizeof(*s));
    if (s == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    rh_init(&s->members);
    s->mem = (uint64_t)sizeof(*s);
    return s;
}

void obj_set_free(obj_set *s)
{
    if (s == NULL)
        return;
    rh_destroy(&s->members);
    free(s);
}

uint64_t obj_set_mem(const obj_set *s)
{
    return s->mem;
}

int obj_set_add(obj_set *s, const char *m, size_t mlen)
{
    const char *old;
    size_t oldl;
    if (mlen > UINT32_MAX)
        return -1;
    if (rh_get(&s->members, m, mlen, &old, &oldl))
        return 0;
    if (rh_set(&s->members, m, mlen, "", 0) < 0)
        return -1;
    s->mem += field_bytes(mlen, 0);
    return 1;
}

int obj_set_has(obj_set *s, const char *m, size_t mlen)
{
    const char *old;
    size_t oldl;
    return rh_get(&s->members, m, mlen, &old, &oldl);
}

int obj_set_rem(obj_set *s, const char *m, size_t mlen)
{
    if (!obj_set_has(s, m, mlen))
        return 0;
    rh_del(&s->members, m, mlen);
    s->mem -= field_bytes(mlen, 0);
    return 1;
}

/* ------------------------------------------------------------------ */
/* zset object                                                        */
/* ------------------------------------------------------------------ */

obj_zset *obj_zset_new(void)
{
    obj_zset *z = (obj_zset *)malloc(sizeof(*z));
    if (z == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    rh_init(&z->dict);
    z->sl = zsl_create();
    z->dict_mem = 0;
    return z;
}

void obj_zset_free(obj_zset *z)
{
    if (z == NULL)
        return;
    rh_destroy(&z->dict);
    zsl_free(z->sl);
    free(z);
}

uint64_t obj_zset_mem(const obj_zset *z)
{
    return (uint64_t)sizeof(*z) + z->dict_mem + z->sl->mem;
}

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

int obj_zset_add(obj_zset *z, const char *m, size_t mlen, double score)
{
    const char *old;
    size_t oldl;
    char b[8];
    if (mlen > UINT32_MAX)
        return -1;
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

int obj_zset_score(obj_zset *z, const char *m, size_t mlen, double *score)
{
    const char *old;
    size_t oldl;
    if (!rh_get(&z->dict, m, mlen, &old, &oldl) || oldl != 8)
        return 0;
    *score = get_score(old);
    return 1;
}

int obj_zset_rem(obj_zset *z, const char *m, size_t mlen)
{
    double score;
    if (!obj_zset_score(z, m, mlen, &score))
        return 0;
    /* delete from the dict BEFORE zsl_delete: when m points into the
     * skiplist node (ZREMRANGEBYSCORE's collected views), freeing the node
     * first would leave rh_del hashing dangling memory (zcard/exists kept
     * stale entries). */
    rh_del(&z->dict, m, mlen);
    zsl_delete(z->sl, score, m, mlen);
    z->dict_mem -= field_bytes(mlen, 8);
    return 1;
}
