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

/* 1 when the exact 40-byte sha1 (hex, either case) names a cached script,
 * else 0. sha1 need not be NUL-terminated. */
int script_cached(db *d, const char *sha1, size_t sha1_len);

/* Push the cached chunk's registry reference for script_exec; returns the
 * ref or LUA_NOREF when missing (script.c internal, declared for the
 * dispatch layer). */
int script_ref(db *d, const char *sha1, size_t sha1_len);

/* Drop every cached script (SCRIPT FLUSH semantics). */
void script_flush(db *d);

/* db teardown: free the cache and the interpreter (db_destroy calls). */
void script_cleanup(db *d);

/* ------------------------------------------------------------------ */
/* execution (EVAL family)                                            */
/* ------------------------------------------------------------------ */

struct session; /* session.h */

/* Dispatch callback used by redis.call/redis.pcall: the same entry point
 * client commands take. Registered once by the command layer (db_init). */
typedef void (*script_command_fn)(struct session *s, const resp_value *argv,
                                  size_t argc, resp_buf *out,
                                  uint64_t now_ms);
void script_set_command_fn(script_command_fn fn);

/* Run a cached script: binds KEYS (argv[0..nkeys)) and ARGV (the rest),
 * pcalls the chunk and converts the Lua return value to a RESP reply.
 * sha1 may be either case. NOSCRIPT guard included (callers pre-check). */
void script_exec(struct session *s, const char *sha1, const resp_value *argv,
                 size_t nkeys, size_t nargs, resp_buf *out, uint64_t now_ms);

#endif /* DDUP_SCRIPT_H */
