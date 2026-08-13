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
    if (z->encoding == OBJ_ZSET_LP)
        return zset_lp_mem(z);
    return (uint64_t)sizeof(*z) + z->dict_mem + z->sl->mem;
}

uint64_t obj_zset_len(const obj_zset *z)
{
    if (z->encoding == OBJ_ZSET_LP)
        return lp_length(z->lp) / 2;
    return rh_size(&z->dict);
}

int obj_zset_is_listpack(const obj_zset *z)
{
    return z->encoding == OBJ_ZSET_LP;
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
    if (mlen > UINT32_MAX)
        return -1;
    if (z->encoding == OBJ_ZSET_LP) {
        unsigned char *mp = NULL;
        if (mlen <= OBJ_ZSET_MAX_LISTPACK_VALUE)
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
        if (mlen > OBJ_ZSET_MAX_LISTPACK_VALUE ||
            lp_length(z->lp) / 2 >= OBJ_ZSET_MAX_LISTPACK_ENTRIES) {
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
