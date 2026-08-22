/* obj.h - typed value objects for the db layer (Phase 5.1).
 *
 * The db main table stays byte-generic (rh_table); every value is a blob:
 *
 *     {1-byte type tag}{payload}
 *
 *   DDUP_OBJ_STRING: payload = raw string bytes
 *   DDUP_OBJ_HASH:   payload = 8-byte pointer to obj_hash (Phase 5.1 step 2)
 *   DDUP_OBJ_LIST:   payload = 8-byte pointer to obj_list (Phase 5.1 step 3)
 *
 * The db layer owns the pointed-to objects: it frees them via
 * obj_free_value() whenever a value blob is overwritten/deleted/expired/
 * evicted/flushed, and accounts their memory via obj_extra_mem().
 */
#ifndef DDUP_OBJ_H
#define DDUP_OBJ_H

#include <stddef.h>
#include <stdint.h>

#include "core/rhtable.h"
#include "ds/quicklist.h"
#include "ds/skiplist.h"
#include "ds/stream.h"

enum {
    DDUP_OBJ_STRING = 0,
    DDUP_OBJ_HASH = 1,
    DDUP_OBJ_LIST = 2,
    DDUP_OBJ_SET = 3,
    DDUP_OBJ_ZSET = 4,
    DDUP_OBJ_STREAM = 5,
    DDUP_OBJ_TIER = 6
};

/* Type tag of a value blob. */
int obj_tag_of(const char *val, size_t vlen);

/* String payload view (val must be DDUP_OBJ_STRING). */
void obj_str(const char *val, size_t vlen, const char **s, size_t *len);

/* Pack/unpack an object pointer blob (9 bytes: tag + pointer). */
void obj_pack_ptr(char buf[9], int tag, const void *ptr);
void *obj_unpack_ptr(const char *val, size_t vlen);

/* Tier reference blob: 1 tag byte + 8-byte record id + 8-byte absolute
 * expiry (17 bytes total). */
void obj_tier_pack(char buf[17], uint64_t record_id, uint64_t expire_ms);
void obj_tier_unpack(const char *val, size_t vlen, uint64_t *record_id,
                     uint64_t *expire_ms);
int obj_is_tier(const char *val, size_t vlen);

/* Extra bytes owned by the value beyond the rh_table entry itself
 * (0 for strings; object struct + elements for hash/list). */
uint64_t obj_extra_mem(const char *val, size_t vlen);

/* Free the owned object of a value blob (no-op for strings). */
void obj_free_value(const char *val, size_t vlen);

/* ------------------------------------------------------------------ */
/* runtime encoding limits                                             */
/*                                                                     */
/* Process-wide listpack/quicklist thresholds. Defaults are the        */
/* OBJ_*_MAX_LISTPACK_* / DDUP_QL_FILL macros below; the server        */
/* applies config once at startup via obj_limits_apply() (single       */
/* write before workers start, read-only afterwards, so mt mode is     */
/* race-free). A 0 entries/value limit disables the compact encoding.  */
/* ------------------------------------------------------------------ */
typedef struct obj_limits {
    int list_fill;    /* quicklist node fill limit */
    int hash_entries; /* hash listpack entry/value limits */
    int hash_value;
    int set_entries; /* set listpack entry/value limits */
    int set_value;
    int zset_entries; /* zset listpack entry/value limits */
    int zset_value;
} obj_limits;

void obj_limits_apply(const obj_limits *lim);
void obj_limits_get(obj_limits *out);

/* ------------------------------------------------------------------ */
/* hash object: listpack for small hashes, rh_table beyond (Phase 6)   */
/*                                                                     */
/* OBJ_HASH_LP: field/value pairs alternate in one listpack. Exceeding */
/* either threshold converts once to OBJ_HASH_HT (rh_table); there is  */
/* no way back, matching Redis.                                        */
/* ------------------------------------------------------------------ */
/* Default thresholds; runtime-tunable via obj_limits. */
#define OBJ_HASH_MAX_LISTPACK_ENTRIES 128
#define OBJ_HASH_MAX_LISTPACK_VALUE 64

enum { OBJ_HASH_LP = 0, OBJ_HASH_HT = 1 };

typedef struct obj_hash {
    int encoding;        /* OBJ_HASH_LP / OBJ_HASH_HT */
    unsigned char *lp;   /* LP payload; NULL in HT mode */
    rh_table fields;     /* HT payload; uninitialized in LP mode */
    uint64_t mem;        /* sizeof(obj_hash) + payload cost, incremental */
    unsigned char ftmp[24]; /* scratch for int-encoded field materialization */
    unsigned char vtmp[24]; /* scratch for int-encoded value materialization */
} obj_hash;

obj_hash *obj_hash_new(void);
void obj_hash_free(obj_hash *h);
uint64_t obj_hash_mem(const obj_hash *h);
uint64_t obj_hash_len(const obj_hash *h);
int obj_hash_is_listpack(const obj_hash *h);

/* Returns 1 if the field is new, 0 on overwrite, -1 on invalid length. */
int obj_hash_set(obj_hash *h, const char *f, size_t flen, const char *v,
                 size_t vlen);
/* The returned value pointer borrows internal storage: valid until the
 * next obj_hash_* call on h (int-encoded values are materialized). */
int obj_hash_get(obj_hash *h, const char *f, size_t flen, const char **v,
                 size_t *vlen);
int obj_hash_del(obj_hash *h, const char *f, size_t flen);

/* Visit every field/value pair. Callback arguments borrow internal
 * storage and are valid only for the duration of the callback. */
void obj_hash_each(obj_hash *h, rh_iter_fn fn, void *ctx);
/* Fetch the idx-th pair (insertion order). LP mode only; returns 0 in HT
 * mode or when idx >= obj_hash_len. v/vlen may be NULL. Same borrowing
 * rules as obj_hash_get. */
int obj_hash_pair_at(obj_hash *h, uint64_t idx, const char **f, size_t *flen,
                     const char **v, size_t *vlen);

/* ------------------------------------------------------------------ */
/* list object: quicklist of listpack nodes (Phase 6)                  */
/* ------------------------------------------------------------------ */
typedef struct obj_list {
    quicklist ql; /* embedded; obj_list_mem() reports ql.mem */
} obj_list;

typedef ql_iter obj_list_iter;

obj_list *obj_list_new(void);
void obj_list_free(obj_list *l);
uint64_t obj_list_mem(const obj_list *l);
uint64_t obj_list_len(const obj_list *l);

/* Returns 0 on success, -1 when len cannot be represented or allocated
 * without size_t overflow. */
int obj_list_push(obj_list *l, int left, const char *data, size_t len);
/* Stage and commit all elements atomically. Returns 0 on success, -1 when
 * any element length is invalid. */
int obj_list_push_many(obj_list *l, int left, const char *const *data,
                       const size_t *lens, size_t count);
/* Returns 1 and hands the caller a malloc'd copy of the element
 * (free with free()), 0 when the list is empty. */
int obj_list_pop(obj_list *l, int left, char **data, size_t *len);

/* Iterator access. Positioning returns 1 on success, 0 when out of
 * range/empty. obj_list_iter_value() returns bytes valid until the
 * iterator moves or the list mutates. */
int obj_list_seek(obj_list *l, size_t idx, obj_list_iter *it);
int obj_list_first(obj_list *l, obj_list_iter *it);
int obj_list_last(obj_list *l, obj_list_iter *it);
int obj_list_iter_next(obj_list_iter *it);
int obj_list_iter_prev(obj_list_iter *it);
const char *obj_list_iter_value(obj_list_iter *it, size_t *len);

/* Replace element data at idx. Returns 1 on success, 0 when idx >= list
 * length, -1 when the replacement length is invalid. */
int obj_list_set_at(obj_list *l, size_t idx, const char *data, size_t len);
/* Remove the element under the iterator; the iterator lands on its
 * successor. Returns 1 when the successor is valid, 0 when the removed
 * element was the tail (iterator invalid). */
int obj_list_remove_at(obj_list_iter *it);
/* Insert before (after == 0) or after (after != 0) the current element.
 * The iterator lands on the inserted element. */
int obj_list_insert(obj_list_iter *it, int after, const char *data,
                    size_t len);

/* ------------------------------------------------------------------ */
/* set object: listpack for small sets, rh_table beyond                */
/*                                                                     */
/* OBJ_SET_LP: members live in one listpack (insertion order, dedupe   */
/* via lp_find). Exceeding either threshold converts once to           */
/* OBJ_SET_HT (rh_table member -> empty value); there is no way back,  */
/* matching Redis.                                                     */
/* ------------------------------------------------------------------ */
/* Default thresholds; runtime-tunable via obj_limits. */
#define OBJ_SET_MAX_LISTPACK_ENTRIES 128
#define OBJ_SET_MAX_LISTPACK_VALUE 64

enum { OBJ_SET_LP = 0, OBJ_SET_HT = 1 };

typedef struct obj_set {
    int encoding;        /* OBJ_SET_LP / OBJ_SET_HT */
    unsigned char *lp;   /* LP payload; NULL in HT mode */
    rh_table members;    /* HT payload; uninitialized in LP mode */
    uint64_t mem;        /* sizeof(obj_set) + payload cost, incremental */
    unsigned char mtmp[24]; /* scratch for int-encoded member materialization */
} obj_set;

obj_set *obj_set_new(void);
void obj_set_free(obj_set *s);
uint64_t obj_set_mem(const obj_set *s);
uint64_t obj_set_len(const obj_set *s);
int obj_set_is_listpack(const obj_set *s);

/* Returns 1 if the member is new, 0 if present, -1 on invalid length. */
int obj_set_add(obj_set *s, const char *m, size_t mlen);
int obj_set_has(obj_set *s, const char *m, size_t mlen);
int obj_set_rem(obj_set *s, const char *m, size_t mlen);

/* Visit every member (value args are always ""/0). Callback arguments
 * borrow internal storage and are valid only for the duration of the
 * callback. */
void obj_set_each(obj_set *s, rh_iter_fn fn, void *ctx);
/* Fetch the idx-th member (insertion order). LP mode only; returns 0 in
 * HT mode or when idx >= obj_set_len. The returned bytes borrow internal
 * storage: valid until the next obj_set_* call on s (int-encoded members
 * are materialized). */
int obj_set_member_at(obj_set *s, uint64_t idx, const char **m,
                      size_t *mlen);

/* ------------------------------------------------------------------ */
/* zset object: listpack for small zsets, dict + skiplist beyond       */
/*                                                                     */
/* OBJ_ZSET_LP: member/score pairs alternate in one listpack, kept in  */
/* (score, member) order (score ascending, member bytes tiebreak, same */
/* rule as the skiplist); scores are stored as %.17g decimal strings   */
/* (strtod round-trips doubles exactly). Exceeding either threshold    */
/* converts once to OBJ_ZSET_HT (rh_table dict member -> 8-byte score  */
/* + zskiplist); there is no way back, matching Redis.                 */
/* ------------------------------------------------------------------ */
/* Default thresholds; runtime-tunable via obj_limits. */
#define OBJ_ZSET_MAX_LISTPACK_ENTRIES 128
#define OBJ_ZSET_MAX_LISTPACK_VALUE 64

enum { OBJ_ZSET_LP = 0, OBJ_ZSET_HT = 1 };

typedef struct obj_zset {
    int encoding;      /* OBJ_ZSET_LP / OBJ_ZSET_HT */
    unsigned char *lp; /* LP payload; NULL in HT mode */
    rh_table dict;     /* HT payload; uninitialized in LP mode */
    zskiplist *sl;     /* HT payload; NULL in LP mode */
    uint64_t dict_mem;
} obj_zset;

/* Ordered cursor over a zset ((score, member) ascending). Valid until
 * the zset mutates (listpack mutations realloc; skiplist deletions free
 * nodes). obj_zset_iter_member() bytes borrow iterator-internal storage
 * and stay valid until the iterator moves. */
typedef struct obj_zset_iter {
    obj_zset *z;
    union {
        struct {
            unsigned char *p;     /* current member entry */
            unsigned char mbuf[24]; /* int-member materialization */
        } lp;
        zsl_node *node;
    } u;
} obj_zset_iter;

obj_zset *obj_zset_new(void);
void obj_zset_free(obj_zset *z);
uint64_t obj_zset_mem(const obj_zset *z);
uint64_t obj_zset_len(const obj_zset *z);
int obj_zset_is_listpack(const obj_zset *z);

/* Insert or update score. Returns 1 if new, 0 if updated, -1 on invalid
 * member length. */
int obj_zset_add(obj_zset *z, const char *m, size_t mlen, double score);
/* Returns 1 and the score when present. */
int obj_zset_score(obj_zset *z, const char *m, size_t mlen, double *score);
int obj_zset_rem(obj_zset *z, const char *m, size_t mlen);

/* Iterator access. Positioning returns 1 on success, 0 when out of
 * range/empty. */
int obj_zset_seek(obj_zset *z, size_t idx, obj_zset_iter *it);
int obj_zset_first(obj_zset *z, obj_zset_iter *it);
int obj_zset_last(obj_zset *z, obj_zset_iter *it);
int obj_zset_iter_next(obj_zset_iter *it);
int obj_zset_iter_prev(obj_zset_iter *it);
/* 1 when both iterators sit on the same member of the same zset. */
int obj_zset_iter_eq(const obj_zset_iter *a, const obj_zset_iter *b);
const char *obj_zset_iter_member(obj_zset_iter *it, size_t *mlen);
double obj_zset_iter_score(obj_zset_iter *it);

/* Range queries (score and lex variants; lex ranges are only defined
 * when all scores are equal, same caveat as the skiplist). first/last
 * return 1 and position it, or 0 when the range is empty. */
int obj_zset_first_in_range(obj_zset *z, const zrangespec *r,
                            obj_zset_iter *it);
int obj_zset_last_in_range(obj_zset *z, const zrangespec *r,
                           obj_zset_iter *it);
size_t obj_zset_count_in_range(obj_zset *z, const zrangespec *r);
int obj_zset_first_in_lex_range(obj_zset *z, const zlexrangespec *r,
                                obj_zset_iter *it);
int obj_zset_last_in_lex_range(obj_zset *z, const zlexrangespec *r,
                               obj_zset_iter *it);

/* 0-based rank of (score, member); -1 when the member is absent. */
long obj_zset_rank(obj_zset *z, double score, const char *m, size_t mlen);

/* Bulk removals, done in one pass at the obj layer (collecting member
 * pointers first would dangle under listpack realloc). Return the
 * number removed. start/stop are inclusive 0-based ranks with
 * start <= stop. */
uint64_t obj_zset_rem_range_by_rank(obj_zset *z, size_t start, size_t stop);
uint64_t obj_zset_rem_range_by_score(obj_zset *z, const zrangespec *r);
uint64_t obj_zset_rem_range_by_lex(obj_zset *z, const zlexrangespec *r);

/* Pop the lowest/highest (min != 0) member. Returns 1 and hands the
 * caller a malloc'd copy of the member (free with free()), 0 when the
 * zset is empty. */
int obj_zset_pop(obj_zset *z, int min, char **member, size_t *mlen,
                 double *score);

#endif /* DDUP_OBJ_H */
