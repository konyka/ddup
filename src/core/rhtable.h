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

/* Insert or overwrite. The table copies key and value. */
void rh_set(rh_table *t, const char *key, size_t klen,
            const char *val, size_t vlen);

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

#endif /* DDUP_RHTABLE_H */
