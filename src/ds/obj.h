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

enum {
    DDUP_OBJ_STRING = 0,
    DDUP_OBJ_HASH = 1,
    DDUP_OBJ_LIST = 2
};

/* Type tag of a value blob. */
int obj_tag_of(const char *val, size_t vlen);

/* String payload view (val must be DDUP_OBJ_STRING). */
void obj_str(const char *val, size_t vlen, const char **s, size_t *len);

/* Pack/unpack an object pointer blob (9 bytes: tag + pointer). */
void obj_pack_ptr(char buf[9], int tag, const void *ptr);
void *obj_unpack_ptr(const char *val, size_t vlen);

/* Extra bytes owned by the value beyond the rh_table entry itself
 * (0 for strings; object struct + elements for hash/list). */
uint64_t obj_extra_mem(const char *val, size_t vlen);

/* Free the owned object of a value blob (no-op for strings). */
void obj_free_value(const char *val, size_t vlen);

/* ------------------------------------------------------------------ */
/* hash object: nested rh_table of field -> raw string (untagged)     */
/* ------------------------------------------------------------------ */
typedef struct obj_hash {
    rh_table fields;
    uint64_t mem; /* sizeof(obj_hash) + per-field entry cost, incremental */
} obj_hash;

obj_hash *obj_hash_new(void);
void obj_hash_free(obj_hash *h);
uint64_t obj_hash_mem(const obj_hash *h);

/* Returns 1 if the field is new. */
int obj_hash_set(obj_hash *h, const char *f, size_t flen, const char *v,
                 size_t vlen);
int obj_hash_get(obj_hash *h, const char *f, size_t flen, const char **v,
                 size_t *vlen);
int obj_hash_del(obj_hash *h, const char *f, size_t flen);

#endif /* DDUP_OBJ_H */
