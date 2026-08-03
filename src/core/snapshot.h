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

/* Load path into d. now_ms decides which expiries are already dead.
 * Returns 0 on success, -1 on unreadable/corrupt file (d untouched). */
int snapshot_load(db *d, const char *path, uint64_t now_ms);

#endif /* DDUP_SNAPSHOT_H */
