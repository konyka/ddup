/* snapshot.h - RDB-style binary snapshot.
 *
 * Format (all integers little-endian, written byte by byte):
 *
 *   magic: 8 bytes "DDUP0001"
 *   then per key:
 *     u8    type tag (DDUP_OBJ_*)
 *     u32   key length, then key bytes
 *     s64   absolute expiry in wall-ms, or -1 for none
 *     payload by type:
 *       STRING: u32 len, bytes
 *       LIST:   u32 n, [u32 len, bytes]...
 *       HASH:   u32 n, [u32 flen, field, u32 vlen, value]...
 *       SET:    u32 n, [u32 len, member]...
 *       ZSET:   u32 n, [u32 mlen, member, f64 score]...
 *
 * Save is atomic (write "<path>.tmp", then rename over the target).
 * Load policy: keys already expired at load time are skipped; a truncated
 * or corrupt file fails hard (-1) and the target db is left UNCHANGED
 * (all-or-nothing: the file is parsed into a temporary db first).
 */
#ifndef DDUP_SNAPSHOT_H
#define DDUP_SNAPSHOT_H

#include <stdint.h>

#include "core/command.h"

/* Write the whole db to path. Returns 0 on success, -1 on serialization or
 * IO error. Serialization failure leaves an existing target untouched. */
int snapshot_save(db *d, const char *path);

/* Serialize the whole db (magic + all entries) into out. Same format.
 * Returns 0 on success or -1 when a format length is unrepresentable; out is
 * restored to its original length on failure. */
int snapshot_serialize(db *d, resp_buf *out);

/* Load path into d. now_ms decides which expiries are already dead.
 * Returns 0 on success, -1 on unreadable/corrupt file (d untouched). */
int snapshot_load(db *d, const char *path, uint64_t now_ms);

/* Load from an in-memory buffer (same all-or-nothing policy). */
int snapshot_load_mem(db *d, const char *buf, size_t len, uint64_t now_ms);

/* ------------------------------------------------------------------ */
/* multi-database snapshots (format "DDUP0002")                        */
/*                                                                     */
/*   magic: 8 bytes "DDUP0002"                                         */
/*   u16   ndbs                                                        */
/*   per non-empty db:                                                 */
/*     u16   db index                                                  */
/*     u32   key count                                                 */
/*     entries (same per-key encoding as DDUP0001)                     */
/*                                                                     */
/* Loading accepts DDUP0001 (falls into db 0). Databases without a     */
/* segment are emptied (the file is complete). All-or-nothing: the     */
/* file is parsed into temporaries first; cluster/configuration state  */
/* of each db is preserved (data-only swap, same as DDUP0001).         */
/* ------------------------------------------------------------------ */

/* Accessor for db by index (matches the session selection hook). */
typedef db *(*snapshot_db_get)(void *ctx, int idx);

int snapshot_serialize_multi(void *ctx, snapshot_db_get get, int ndbs,
                             resp_buf *out);
int snapshot_save_multi(void *ctx, snapshot_db_get get, int ndbs,
                        const char *path);
int snapshot_load_multi(void *ctx, snapshot_db_get get, int ndbs,
                        const char *path, uint64_t now_ms);
int snapshot_load_mem_multi(void *ctx, snapshot_db_get get, int ndbs,
                            const char *buf, size_t len, uint64_t now_ms);

/* ------------------------------------------------------------------ */
/* per-key serialization (DUMP/RESTORE/MIGRATE)                       */
/* ------------------------------------------------------------------ */

/* Payload layout (all integers little-endian):
 *   u16   format version (SNAPSHOT_DUMP_VERSION)
 *   u8    type tag + value payload (same encoding as the snapshot entries,
 *         without key/expiry)
 *   u64   crc64 over all preceding bytes
 */
#define SNAPSHOT_DUMP_VERSION 1

/* Serialize one live key, appending the payload to out.
 * Returns 0 on success, -1 when the key does not exist or its payload has an
 * unrepresentable format length. On failure out keeps its original length.
 * Callers expire lazily first (db_expire_if_needed) so dead keys report -1. */
int snapshot_dump_key(db *d, const char *key, size_t klen, resp_buf *out);

/* Install a key from a payload produced by snapshot_dump_key.
 * expire_ms is an absolute wall-ms expiry, 0 for none.
 * Returns 0 on success, 1 when the key exists and replace is 0,
 * -1 on a malformed payload or crc mismatch (db untouched). */
int snapshot_restore_key(db *d, const char *key, size_t klen,
                         const char *payload, size_t plen,
                          uint64_t expire_ms, int replace, uint64_t now_ms);

/* Internal bounds regression seam; returns 0 when extreme reader offsets are
 * rejected without pointer arithmetic. */
int snapshot_test_reader_bounds(void);

#endif /* DDUP_SNAPSHOT_H */
