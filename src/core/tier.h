/* tier.h - per-worker append-only tiered storage (cold layer).
 *
 * The hot hash table stores a 17-byte tier-ref for offloaded keys; the cold
 * layer owns the value bytes in an append-only log. Records are self
 * describing and indexed in memory by a monotonically increasing record id.
 * Compaction rewrites live records to a temp file and atomically renames it.
 */
#ifndef DDUP_TIER_H
#define DDUP_TIER_H

#include <stddef.h>
#include <stdint.h>

typedef struct tier_store tier_store;

/* Open (and create if needed) the cold log at path, replaying it into the
 * in-memory record index. max_disk_bytes == 0 means unlimited. */
int tier_open(tier_store **out, const char *path, uint64_t max_disk_bytes);
void tier_close(tier_store *t);

/* Append a PUT record and return its record id. The caller keeps ownership
 * of key/value; the value is the full db value blob (type tag included).
 * Returns 0 on success, -1 on write/index/limit failure. */
int tier_put(tier_store *t, unsigned int db_index, const char *key,
             size_t klen, const char *val, size_t vlen, uint64_t expire_ms,
             uint64_t *record_id);

/* Read a live record into a malloc'd buffer (free with free()). The returned
 * value is the full db value blob. Returns 0 on success, -1 on missing/id/
 * read error. expire_ms may be NULL. */
int tier_get(tier_store *t, uint64_t record_id, char **val, size_t *vlen,
             uint64_t *expire_ms);

/* Append a DELETE record and drop the index entry. */
int tier_del(tier_store *t, uint64_t record_id);

/* Drop all cold records for one logical db (logical flush marker; old
 * records become dead). */
int tier_flush_db(tier_store *t, unsigned int db_index);

/* Rewrite live records into path.tmp and atomically replace path. */
int tier_compact(tier_store *t);

/* Observability. */
uint64_t tier_disk_bytes(const tier_store *t);
uint64_t tier_live_records(const tier_store *t);
int tier_failed(const tier_store *t);

#endif /* DDUP_TIER_H */
