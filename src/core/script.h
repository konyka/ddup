/* script.h - Lua script cache (EVAL family support).
 *
 * One lua_State per db, created lazily and shared by all scripts (Redis
 * model). The cache maps lowercase 40-hex SHA1 -> Lua registry reference
 * of the compiled chunk. Only base/string/table/math libraries are opened
 * (documented sandbox simplification: no io/os/debug/package).
 */
#ifndef DDUP_SCRIPT_H
#define DDUP_SCRIPT_H

#include <stddef.h>

#include "core/command.h"

/* the per-db shared interpreter (created on first use) */
void *script_state(db *d);

/* Compile src into the cache (no-op when the sha1 is already cached).
 * On success writes the 40-hex sha1 into out_sha1 and returns 0; on a
 * compile error returns -1 with a NUL-terminated message in errbuf. */
int script_load(db *d, const char *src, size_t len, char out_sha1[41],
                char *errbuf, size_t errcap);

/* 1 when sha1 (40 hex, either case) names a cached script, else 0. */
int script_cached(db *d, const char *sha1);

/* Push the cached chunk's registry reference for script_exec; returns the
 * ref or LUA_NOREF when missing (script.c internal, declared for the
 * dispatch layer). */
int script_ref(db *d, const char *sha1);

/* Drop every cached script (SCRIPT FLUSH semantics). */
void script_flush(db *d);

/* db teardown: free the cache and the interpreter (db_destroy calls). */
void script_cleanup(db *d);

#endif /* DDUP_SCRIPT_H */
