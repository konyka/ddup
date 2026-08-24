/* command.h - RESP command dispatch over the in-memory store. */
#ifndef DDUP_COMMAND_H
#define DDUP_COMMAND_H

#include "core/command.h"

#include <stddef.h>
#include <stdint.h>

#include "core/cluster.h"
#include "core/rhtable.h"
#include "resp/resp.h"
#include "resp/resp_writer.h"

struct session;
struct tier_store;

/* Eviction policies (db.maxmemory_policy). */
#define DB_POLICY_ALLKEYS_LRU 0 /* default */
#define DB_POLICY_NOEVICTION 1

/* commandstats arrays are indexed by CMD_* id; sized with room to spare
 * (CMD_MAX must stay below this). */
#define CMD_STATS_SLOTS 512

/* One logical database. Shared-nothing: each IO thread owns its own.
 * `expires` maps key -> 8-byte absolute wall-ms expiry (raw uint64).
 * used_memory is an incremental estimate: per live entry
 * sizeof(rh_entry) + 16 (malloc overhead) + klen + vlen, for both tables. */
typedef struct db {
    rh_table table;
    rh_table expires;
    rh_table keyvers;  /* WATCH: key -> uint64 modification version */
    uint64_t watch_refs;  /* active watch entries; 0 = skip keyvers writes */
    uint64_t flush_epoch; /* bumped by FLUSHDB (invalidates all watches) */
    uint64_t expired_keys; /* lazy + active expirations */
    uint64_t evicted_keys;
    uint64_t used_memory;
    uint64_t dirty;        /* mutation counter (AOF hook trigger) */
    uint64_t maxmemory;    /* bytes; 0 = unlimited */
    int maxmemory_policy;  /* DB_POLICY_* */
    int cluster_enabled;   /* single-node cluster mode */
    char node_id[41];      /* 40-hex cluster node id (when enabled) */
    cluster_node nodes[CLUSTER_MAX_NODES]; /* cluster node table */
    int nnodes;
    uint16_t slot_owner[16384]; /* node index per slot; 0xFFFF = unassigned */
    int slot_owner_dirty;       /* rebuild slot_owner lazily */
    uint16_t slot_migrating[16384]; /* per slot: target node idx, 0xFFFF none */
    uint16_t slot_importing[16384]; /* per slot: source node idx, 0xFFFF none */
    uint64_t cluster_changes;   /* bumped on any node-table mutation */
    uint64_t cluster_current_epoch; /* max config epoch seen (starts 1) */
    uint64_t cluster_node_timeout_ms; /* failure detection + report window */
    uint64_t last_vote_epoch;   /* failover: last epoch we voted for */
    uint64_t failover_req_epoch; /* election epoch we requested, 0 = none */
    uint32_t failover_ack_mask; /* node indexes that granted a vote */
    int failover_ack_count;
    char fail_broadcast_id[41]; /* FAIL frame to broadcast ("" = none) */
    char cluster_ip[64];   /* bind address reported by CLUSTER SLOTS */
    uint16_t cluster_port; /* listen port reported by CLUSTER SLOTS */
    const char *snapshot_path; /* SAVE target (not owned; may be NULL) */
    uint64_t last_save;    /* unix seconds of the last successful SAVE */
    uint32_t rng_state;    /* sampling PRNG (xorshift32, always nonzero) */
    rh_table scripts;      /* Lua script cache: sha1 hex -> registry ref */
    rh_table function_libs; /* FUNCTION LOAD code: library name -> source */
    struct tier_store *tier;  /* optional cold layer (NULL = tiering off) */
    int tier_db_index;        /* logical db index used in tier records */
    int tier_enabled;         /* CONFIG tiered-storage toggle */
    char tier_dir[512];       /* configured cold-layer directory */
    uint64_t tier_max_disk_bytes; /* configured disk cap, 0 = unlimited */
    int tier_io_error;        /* set when a tier read/write fails */
    void *lua_state;       /* shared interpreter, lazy (script.c owns it) */
    /* commandstats: per-command-id call count and cumulative microseconds
     * (indexed by CMD_* id, room to spare) */
    uint64_t cmd_calls[CMD_STATS_SLOTS];
    uint64_t cmd_usecs[CMD_STATS_SLOTS];
} db;

void db_init(db *d);
void db_destroy(db *d);
/* Attach a cold layer to this logical db. tier may be NULL to disable. */
void db_set_tier(db *d, struct tier_store *tier, int db_index);

/* ------------------------------------------------------------------ */
/* INFO statistics (shared by the single-thread path and mt aggregation) */
/* ------------------------------------------------------------------ */

#define INFO_STATS_MAX_DBS 16

/* Always-on server IO counters (Phase 27): cheap increments on the hot
 * path; per-server in mt mode, summed across workers via the INFO
 * __STATS__ transport. reads/writes count raw calls (incl. would-block
 * and overlapped posts), the byte counters only successful transfers.
 * Client and replication conns are covered; the cluster bus is not. */
typedef struct io_counters {
    uint64_t loops;         /* server_run_once iterations */
    uint64_t events;        /* loop events / completions dispatched */
    uint64_t reads;         /* recv-side calls or overlapped recv posts */
    uint64_t writes;        /* send-side calls or overlapped send posts */
    uint64_t bytes_read;
    uint64_t bytes_written;
} io_counters;

/* Numeric snapshot behind INFO. mt mode transports one snapshot per worker
 * in a machine format (INFO __STATS__) and sums them into this struct. */
typedef struct info_stats {
    uint64_t used_memory;
    uint64_t expired_keys;
    uint64_t evicted_keys;
    uint64_t tier_disk_bytes;
    uint64_t tier_live_records;
    uint64_t tier_failed;
    uint64_t dbsize; /* current db only (Redis DBSIZE semantics) */
    int ndbs;        /* logical dbs covered by db_keys/db_expires */
    uint64_t db_keys[INFO_STATS_MAX_DBS];
    uint64_t db_expires[INFO_STATS_MAX_DBS];
    uint64_t cmd_calls[CMD_STATS_SLOTS];  /* indexed by CMD_* id */
    uint64_t cmd_usecs[CMD_STATS_SLOTS];
    io_counters io;
} info_stats;

/* Render an INFO bulk reply from a stats snapshot. Per-process scalars
 * (maxmemory, policy, cluster flag) come from home; repl may be NULL. */
struct repl_info;
void command_info_render(const db *home, const struct repl_info *repl,
                         const info_stats *st, resp_buf *out);

/* Empty the db (data + expiries), keeping configuration and stats. */
void db_flush(db *d);

/* WATCH support: bump/read the modification version of a key. Versions are
 * monotonic per key name (never reused, even across delete/recreate).
 * db_touch_key returns -1 when the key length cannot be represented. */
int db_touch_key(db *d, const char *key, size_t klen);
uint64_t db_key_version(db *d, const char *key, size_t klen);

/* Persistence loaders: install a pre-built value blob (with accounting and
 * LRU/version bookkeeping). Return 0 on success, -1 on rejected length. */
int db_install_blob(db *d, const char *key, size_t klen, const char *blob,
                    size_t bloblen, uint64_t now_ms);
int db_install_expiry(db *d, const char *key, size_t klen, uint64_t when_ms);

/* Lazy expiration: if key has an expiry <= now_ms, delete it (and its
 * expiry entry) and return 1. Otherwise return 0. */
int db_expire_if_needed(db *d, const char *key, size_t klen, uint64_t now_ms);

/* Delete key and expiry (and any owned object). Returns 1 if existed. */
int db_del_kv(db *d, const char *key, size_t klen);

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

/* ------------------------------------------------------------------ */
/* Command ID table                                                   */
/* ------------------------------------------------------------------ */

#define CMD_ID_UNKNOWN 0

enum {
    CMD_PING = 1,
    CMD_ECHO,
    CMD_GET,
    CMD_SET,
    CMD_DUMP,
    CMD_RESTORE,
    CMD_MIGRATE,
    CMD_ASKING,
    CMD_DEL,
    CMD_UNLINK,
    CMD_EXISTS,
    CMD_INCR,
    CMD_DECR,
    CMD_APPEND,
    CMD_STRLEN,
    CMD_MGET,
    CMD_MSET,
    CMD_EXPIRE,
    CMD_PEXPIRE,
    CMD_EXPIREAT,
    CMD_PEXPIREAT,
    CMD_TTL,
    CMD_PTTL,
    CMD_PERSIST,
    CMD_DBSIZE,
    CMD_FLUSHDB,
    CMD_CONFIG,
    CMD_INFO,
    CMD_HSET,
    CMD_HMSET,
    CMD_HGET,
    CMD_HDEL,
    CMD_HEXISTS,
    CMD_HLEN,
    CMD_HGETALL,
    CMD_HKEYS,
    CMD_HVALS,
    CMD_HMGET,
    CMD_HINCRBY,
    CMD_HSETNX,
    CMD_LPUSH,
    CMD_RPUSH,
    CMD_LPUSHX,
    CMD_RPUSHX,
    CMD_LPOP,
    CMD_RPOP,
    CMD_LLEN,
    CMD_LRANGE,
    CMD_LINDEX,
    CMD_LSET,
    CMD_SADD,
    CMD_SREM,
    CMD_SISMEMBER,
    CMD_SMISMEMBER,
    CMD_SCARD,
    CMD_SMEMBERS,
    CMD_SPOP,
    CMD_SRANDMEMBER,
    CMD_SMOVE,
    CMD_SINTER,
    CMD_SUNION,
    CMD_SDIFF,
    CMD_ZADD,
    CMD_ZSCORE,
    CMD_ZCARD,
    CMD_ZINCRBY,
    CMD_ZREM,
    CMD_ZRANGE,
    CMD_ZREVRANGE,
    CMD_ZRANK,
    CMD_ZREVRANK,
    CMD_ZCOUNT,
    CMD_ZRANGEBYSCORE,
    CMD_ZREMRANGEBYSCORE,
    CMD_MULTI,
    CMD_EXEC,
    CMD_DISCARD,
    CMD_WATCH,
    CMD_UNWATCH,
    CMD_SUBSCRIBE,
    CMD_UNSUBSCRIBE,
    CMD_PUBLISH,
    CMD_QUIT,
    CMD_SYNC,
    CMD_PSYNC,
    CMD_REPLICAOF,
    CMD_SAVE,
    CMD_LASTSAVE,
    CMD_SHUTDOWN,
    CMD_CLUSTER,
    CMD_AUTH,
    CMD_SELECT,
    CMD_SWAPDB,
    CMD_EVAL,
    CMD_EVALSHA,
    CMD_SCRIPT,
    CMD_SSUBSCRIBE,
    CMD_SUNSUBSCRIBE,
    CMD_SPUBLISH,
    CMD_PUBSUB,
    CMD_TYPE,
    CMD_KEYS,
    CMD_SCAN,
    CMD_RENAME,
    CMD_RENAMENX,
    CMD_TOUCH,
    CMD_RANDOMKEY,
    CMD_EXPIRETIME,
    CMD_PEXPIRETIME,
    CMD_GETDEL,
    CMD_GETEX,
    CMD_SETEX,
    CMD_PSETEX,
    CMD_GETSET,
    CMD_SETRANGE,
    CMD_GETRANGE,
    CMD_INCRBY,
    CMD_DECRBY,
    CMD_INCRBYFLOAT,
    CMD_HSTRLEN,
    CMD_HRANDFIELD,
    CMD_LPOS,
    CMD_LREM,
    CMD_LTRIM,
    CMD_RPOPLPUSH,
    CMD_LINSERT,
    CMD_LMOVE,
    CMD_LMOVEM,
    CMD_LMPOP,
    CMD_SINTERCARD,
    CMD_SINTERSTORE,
    CMD_SUNIONSTORE,
    CMD_SDIFFSTORE,
    CMD_ZPOPMIN,
    CMD_ZPOPMAX,
    CMD_ZREMRANGEBYRANK,
    CMD_ZMSCORE,
    CMD_ZRANDMEMBER,
    CMD_ZRANGEBYLEX,
    CMD_ZREVRANGEBYLEX,
    CMD_ZREMRANGEBYLEX,
    CMD_PSUBSCRIBE,
    CMD_PUNSUBSCRIBE,
    CMD_COPY,
    CMD_OBJECT,
    CMD_SETNX,
    CMD_MSETNX,
    CMD_GETBIT,
    CMD_SETBIT,
    CMD_BITCOUNT,
    CMD_BITPOS,
    CMD_BITOP,
    CMD_BITFIELD,
    CMD_BITFIELD_RO,
    CMD_ZUNIONSTORE,
    CMD_ZINTERSTORE,
    CMD_ZDIFFSTORE,
    CMD_ZUNION,
    CMD_ZINTER,
    CMD_ZDIFF,
    CMD_ZINTERCARD,
    CMD_ZLEXCOUNT,
    CMD_ZREVRANGEBYSCORE,
    CMD_ZRANGESTORE,
    CMD_ZMPOP,
    CMD_HSCAN,
    CMD_SSCAN,
    CMD_ZSCAN,
    CMD_FLUSHALL,
    CMD_TIME,
    CMD_HINCRBYFLOAT,
    CMD_READONLY,
    CMD_READWRITE,
    CMD_SUBSTR,
    CMD_SLAVEOF,
    CMD_MOVE,
    CMD_ROLE,
    CMD_RESET,
    CMD_HELLO,
    CMD_LCS,
    CMD_SORT,
    CMD_SORT_RO,
    CMD_PFADD,
    CMD_PFCOUNT,
    CMD_PFDEBUG,
    CMD_PFMERGE,
    CMD_PFSELFTEST,
    CMD_GEOADD,
    CMD_GEODIST,
    CMD_GEOHASH,
    CMD_GEOPOS,
    CMD_GEORADIUS,
    CMD_GEORADIUS_RO,
    CMD_GEORADIUSBYMEMBER,
    CMD_GEORADIUSBYMEMBER_RO,
    CMD_GEOSEARCH,
    CMD_GEOSEARCHSTORE,
    CMD_COMMAND,
    CMD_CLIENT,
    CMD_MEMORY,
    CMD_SLOWLOG,
    CMD_BGSAVE,
    CMD_BGREWRITEAOF,
    CMD_XADD,
    CMD_XLEN,
    CMD_XRANGE,
    CMD_XREVRANGE,
    CMD_XDEL,
    CMD_XDELEX,
    CMD_XTRIM,
    CMD_XGROUP,
    CMD_XACK,
    CMD_XACKDEL,
    CMD_XPENDING,
    CMD_XCLAIM,
    CMD_XAUTOCLAIM,
    CMD_XNACK,
    CMD_XREAD,
    CMD_XREADGROUP,
    CMD_XINFO,
    CMD_XSETID,
    CMD_XCFGSET,
    CMD_XIDMPRECORD,
    CMD_EVAL_RO,
    CMD_EVALSHA_RO,
    CMD_RESTORE_ASKING,
    CMD_LOLWUT,
    CMD_FCALL,
    CMD_FCALL_RO,
    CMD_FUNCTION,
    CMD_BLPOP,
    CMD_BRPOP,
    CMD_BRPOPLPUSH,
    CMD_BLMOVE,
    CMD_BLMOVEM,
    CMD_BLMPOP,
    CMD_BZPOPMIN,
    CMD_BZPOPMAX,
    CMD_BZMPOP,
    CMD_WAIT,
    CMD_WAITAOF,
    CMD_REPLCONF,
    CMD_FAILOVER,
    CMD_MONITOR,
    CMD_ACL,
    CMD_DEBUG,
    CMD_LATENCY,
    CMD_MODULE,
    CMD_SENTINEL,
    CMD_HGETDEL,
    CMD_HSETEX,
    CMD_HGETEX,
    CMD_HEXPIRE,
    CMD_HPEXPIRE,
    CMD_HEXPIREAT,
    CMD_HPEXPIREAT,
    CMD_HPERSIST,
    CMD_HTTL,
    CMD_HPTTL,
    CMD_HEXPIRETIME,
    CMD_HPEXPIRETIME,
    CMD_SUNIONCARD,
    CMD_SDIFFCARD,
    CMD_INCREX,
    CMD_MSETEX,
    CMD_DELEX,
    CMD_DIGEST,
    CMD_SFLUSH,
    CMD_TRIMSLOTS,
    CMD_BACKUP,
    CMD_HIMPORT,
    CMD_HOTKEYS
};

#define CMD_MAX CMD_HOTKEYS

/* Resolve a command name to its stable ID; case-insensitive. */
uint16_t cmd_resolve(const char *name, size_t len);

/* Helpers that index the command table by ID (O(1)). */
int cmd_is_write(uint16_t cmd_id);
int cmd_min_argc(uint16_t cmd_id);
int cmd_max_argc(uint16_t cmd_id);
int cmd_parity(uint16_t cmd_id);

/* Retry a session suspended by a blocking command (server event-loop
 * integration). Writes the reply into `out` and clears the blocked state
 * when the command is ready or its deadline has expired; otherwise leaves
 * the session blocked. Returns 1 when the session was unblocked, 0 when it
 * remains blocked. */
int command_blocked_try(struct session *s, resp_buf *out, uint64_t now_ms);

#endif /* DDUP_COMMAND_H */
