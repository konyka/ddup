/* command.h - RESP command dispatch over the in-memory store. */
#ifndef DDUP_COMMAND_H
#define DDUP_COMMAND_H

#include <stddef.h>
#include <stdint.h>

#include "core/rhtable.h"
#include "resp/resp.h"
#include "resp/resp_writer.h"

/* Eviction policies (db.maxmemory_policy). */
#define DB_POLICY_ALLKEYS_LRU 0 /* default */
#define DB_POLICY_NOEVICTION 1

/* One logical database. Shared-nothing: each IO thread owns its own.
 * `expires` maps key -> 8-byte absolute wall-ms expiry (raw uint64).
 * used_memory is an incremental estimate: per live entry
 * sizeof(rh_entry) + 16 (malloc overhead) + klen + vlen, for both tables. */
typedef struct db {
    rh_table table;
    rh_table expires;
    rh_table keyvers;  /* WATCH: key -> uint64 modification version */
    uint64_t flush_epoch; /* bumped by FLUSHDB (invalidates all watches) */
    uint64_t expired_keys; /* lazy + active expirations */
    uint64_t evicted_keys;
    uint64_t used_memory;
    uint64_t maxmemory;    /* bytes; 0 = unlimited */
    int maxmemory_policy;  /* DB_POLICY_* */
    uint32_t rng_state;    /* sampling PRNG (xorshift32, always nonzero) */
} db;

void db_init(db *d);
void db_destroy(db *d);

/* WATCH support: bump/read the modification version of a key. Versions are
 * monotonic per key name (never reused, even across delete/recreate). */
void db_touch_key(db *d, const char *key, size_t klen);
uint64_t db_key_version(db *d, const char *key, size_t klen);

/* Lazy expiration: if key has an expiry <= now_ms, delete it (and its
 * expiry entry) and return 1. Otherwise return 0. */
int db_expire_if_needed(db *d, const char *key, size_t klen, uint64_t now_ms);

/* Execute one command with an injected wall clock (testability; unit tests
 * use synthetic time, no sleeps). argv items must be string-typed values
 * (bulk/simple); the RESP reply is appended to out. */
void command_execute_at(db *d, const resp_value *argv, size_t argc,
                        resp_buf *out, uint64_t now_ms);

/* Active expiration cycle: sample up to max_samples entries from the
 * expires table, delete those whose expiry has passed, and repeat while
 * more than 25% of a sample round was expired (max 10 rounds).
 * Returns the number of keys expired. */
size_t db_active_expire(db *d, uint64_t now_ms, int max_samples);

/* Same as command_execute_at with the real wall clock (pal_wall_ms). */
void command_execute(db *d, const resp_value *argv, size_t argc,
                     resp_buf *out);

#endif /* DDUP_COMMAND_H */
