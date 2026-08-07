/* rhtable.h - Robin Hood open-addressing hash table with incremental rehash.
 *
 * Keys and values are arbitrary byte strings; the table copies both into a
 * single owned allocation per entry (key bytes followed by value bytes).
 *
 * Growth is incremental: when the load factor crosses a threshold a new
 * table double the size is allocated and entries are migrated in small
 * batches on every subsequent operation, so no single operation pays the
 * full rehash cost (latency-spike avoidance, cache-store style).
 *
 * Not thread-safe by design: each IO thread owns its tables (shared-nothing).
 */
#ifndef DDUP_RHTABLE_H
#define DDUP_RHTABLE_H

#include <stddef.h>
#include <stdint.h>

typedef struct rh_entry {
    uint64_t hash;   /* full 64-bit hash */
    char *kv;        /* owned block: key bytes then value bytes */
    uint32_t klen;
    uint32_t vlen;
    int32_t psl;     /* probe sequence length; -1 = empty slot */
    uint32_t meta;   /* opaque to the table; db uses it as a 24-bit LRU clock */
} rh_entry;

typedef struct rh_table {
    rh_entry *slots;
    size_t cap;          /* power of two */
    size_t size;         /* live entries (both tables while migrating) */
    /* incremental rehash state */
    rh_entry *old_slots;
    size_t old_cap;
    size_t old_live;     /* entries still in old table */
    size_t migrate_pos;  /* next old bucket to migrate */
} rh_table;

void rh_init(rh_table *t);
void rh_destroy(rh_table *t);

/* Returns 1 and sets val/vlen out-params (view valid until next mutation). */
int rh_get(rh_table *t, const char *key, size_t klen,
           const char **val, size_t *vlen);

/* Same as rh_get, but also sets meta in the same lookup pass (avoids a
 * second probe for the common read+touch pattern). */
int rh_get_touch(rh_table *t, const char *key, size_t klen,
                 const char **val, size_t *vlen, uint32_t meta);

/* Insert or overwrite. The table copies key and value. */
void rh_set(rh_table *t, const char *key, size_t klen,
            const char *val, size_t vlen);

/* Insert-or-overwrite in a single probe (Phase 27): on overwrite the old
 * kv block is handed back UNFREED through old_kv/old_vlen (caller does
 * object teardown + free) and 1 is returned; on plain insert 0. meta is
 * set on the entry either way. */
int rh_set_ex(rh_table *t, const char *key, size_t klen, const char *val,
              size_t vlen, uint32_t meta, char **old_kv, size_t *old_vlen);

/* Two-part value variant (Phase 28): the stored value blob is v1||v2,
 * so callers can prepend a type tag without a temporary concatenation.
 * v2 may be NULL when n2 is 0. */
int rh_set_ex2(rh_table *t, const char *key, size_t klen, const char *v1,
               size_t n1, const char *v2, size_t n2, uint32_t meta,
               char **old_kv, size_t *old_vlen);

/* Read the meta field of an existing key (0 when absent). */
uint32_t rh_meta_of(rh_table *t, const char *key, size_t klen);

/* Returns 1 if the key existed and was removed. */
int rh_del(rh_table *t, const char *key, size_t klen);

/* Set the caller-owned meta field of an existing key. Returns 1 if found. */
int rh_touch(rh_table *t, const char *key, size_t klen, uint32_t meta);

/* Sample a pseudo-random live entry: start at bucket (rand & (cap-1)) and
 * scan forward for the first occupied slot. Returns 1 and sets views into
 * the entry (valid until the next table mutation), 0 if the table is
 * empty. meta may be NULL. Used by active expiration and eviction. */
int rh_random_entry(rh_table *t, uint32_t rand, const char **key, size_t *klen,
                    const char **val, size_t *vlen, uint32_t *meta);

size_t rh_size(const rh_table *t);

/* Visit every live entry (both tables while migrating). The callback must
 * not mutate the table. Used for whole-db teardown and HGETALL/HKEYS/HVALS. */
typedef void (*rh_iter_fn)(const char *key, size_t klen, const char *val,
                           size_t vlen, void *ctx);
void rh_each(const rh_table *t, rh_iter_fn fn, void *ctx);

#endif /* DDUP_RHTABLE_H */
