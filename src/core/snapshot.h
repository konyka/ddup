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

/* Write the whole db to path. Returns 0 on success, -1 on IO error. */
int snapshot_save(db *d, const char *path);

/* Serialize the whole db (magic + all entries) into out. Same format. */
void snapshot_serialize(db *d, resp_buf *out);

/* Load path into d. now_ms decides which expiries are already dead.
 * Returns 0 on success, -1 on unreadable/corrupt file (d untouched). */
int snapshot_load(db *d, const char *path, uint64_t now_ms);

/* Load from an in-memory buffer (same all-or-nothing policy). */
int snapshot_load_mem(db *d, const char *buf, size_t len, uint64_t now_ms);

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
 * Returns 0 on success, -1 when the key does not exist. Callers expire
 * lazily first (db_expire_if_needed) so dead keys report -1. */
int snapshot_dump_key(db *d, const char *key, size_t klen, resp_buf *out);

/* Install a key from a payload produced by snapshot_dump_key.
 * expire_ms is an absolute wall-ms expiry, 0 for none.
 * Returns 0 on success, 1 when the key exists and replace is 0,
 * -1 on a malformed payload or crc mismatch (db untouched). */
int snapshot_restore_key(db *d, const char *key, size_t klen,
                         const char *payload, size_t plen,
                         uint64_t expire_ms, int replace, uint64_t now_ms);

#endif /* DDUP_SNAPSHOT_H */
