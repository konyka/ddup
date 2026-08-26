/* session.h - per-connection execution context (Phase 5.3).
 *
 * A session wraps a db pointer plus the connection-scoped state that
 * commands need: MULTI queue, WATCH entries, pub/sub hooks. The dispatch
 * entry point is session_execute_at(); command_execute[_at]() keep working
 * via a stack session so existing db-level tests are unaffected.
 */
#ifndef DDUP_SESSION_H
#define DDUP_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "core/command.h"
#include "resp/resp.h"
#include "resp/resp_writer.h"

/* One queued MULTI command: deep-copied argv (the recv buffer is recycled
 * between commands, so zero-copy views would dangle until EXEC). */
typedef struct queued_cmd {
    resp_value *argv; /* argc items; every str/len is an owned copy */
    size_t argc;
    int skip_log;   /* EVAL: AOF-logs effects instead of the queued argv */
} queued_cmd;

/* WATCH bookkeeping: key copy + key-version/db-epoch seen at WATCH time. */
typedef struct watch_entry {
    char *key;
    size_t klen;
    uint64_t version;
    uint64_t epoch;
    int db_index;
} watch_entry;

typedef struct himport_fieldset {
    char *name;
    size_t name_len;
    char **fields;
    size_t *field_lens;
    size_t count;
} himport_fieldset;

/* replication roles (session.repl->role and server role) */
#define SESSION_ROLE_MASTER 0
#define SESSION_ROLE_REPLICA 1

/* Read-only snapshot of a server's replication state, owned by the server
 * and shared with sessions for INFO replication. */
typedef struct repl_info {
    int role; /* SESSION_ROLE_* */
    char master_host[64];
    uint16_t master_port;
    int link_up; /* replica side: master link status */
    size_t connected_slaves;
    uint64_t offset;    /* master side: bytes propagated so far */
    char replid[41];    /* own replication id (40-hex, generated at boot) */
    char master_replid[41]; /* replica side: the master's replid */
    uint64_t master_offset; /* replica side: applied offset in the
                               master's stream (PSYNC resume point) */
} repl_info;

typedef struct session {
    db *d;
    /* AUTH state: authed defaults to 1 (no password configured); the
     * server sets it to 0 and provides requirepass when auth is enabled. */
    int authed;
    const char *requirepass; /* not owned; NULL/"" = auth disabled */
    /* multi-db selection (SELECT/SWAPDB): stack sessions have no hook and
     * only db 0 exists */
    int db_index;
    void *sel_ctx;
    db *(*sel_fn)(void *ctx, int idx);
    int sel_ndbs;
    /* MULTI state */
    int in_multi;
    int multi_error;
    queued_cmd *queue;
    size_t queue_len;
    size_t queue_cap;
    /* WATCH state */
    watch_entry *watches;
    size_t nwatch;
    size_t watch_cap;
    himport_fieldset *himport_sets;
    size_t himport_len;
    size_t himport_cap;
    /* pub/sub hooks; ps_ctx/owner are registry- and delivery-side contexts
     * (server-owned). NULL hooks = no pub/sub (stack sessions in tests). */
    void *ps_ctx;
    ptrdiff_t (*subscribe)(void *ctx, struct session *s, const char *ch,
                           size_t len);
    size_t (*unsubscribe)(void *ctx, struct session *s, const char *ch,
                          size_t len);
    void (*each_channel)(void *ctx, struct session *s,
                         void (*cb)(const char *ch, size_t len, void *arg),
                         void *arg);
    long (*publish)(void *ctx, const char *ch, size_t chlen, const char *msg,
                    size_t mlen);
    void *owner; /* delivery context (e.g. server connection) */
    void (*deliver)(void *owner, const char *ch, size_t chlen,
                    const char *msg, size_t mlen);
    size_t nsub; /* channels this session is subscribed to */
    /* shard pub/sub (Redis 7 sharded channels): parallel registry hooks,
     * same ownership model; deliver_shard writes "smessage" pushes;
     * spublish_bus propagates a SPUBLISH onto the cluster bus (server) */
    ptrdiff_t (*ssubscribe)(void *ctx, struct session *s, const char *ch,
                            size_t len);
    size_t (*sunsubscribe)(void *ctx, struct session *s, const char *ch,
                           size_t len);
    void (*each_schannel)(void *ctx, struct session *s,
                          void (*cb)(const char *ch, size_t len, void *arg),
                          void *arg);
    long (*spublish)(void *ctx, const char *ch, size_t chlen,
                     const char *msg, size_t mlen);
    long (*schannel_nsub)(void *ctx, const char *ch, size_t len);
    size_t (*shard_channels)(void *ctx, const char *pat, size_t patlen,
                             resp_buf *out);
    void (*deliver_shard)(void *owner, const char *ch, size_t chlen,
                          const char *msg, size_t mlen);
    void (*spublish_bus)(void *ctx, const char *ch, size_t chlen,
                         const char *msg, size_t mlen);
    size_t nssub; /* shard channels this session is subscribed to */
    /* pattern pub/sub (Redis PSUBSCRIBE): parallel registry hooks, same
     * ownership model; the session keeps only the count, the server owns
     * the pattern registry. deliver_pattern writes "pmessage" pushes;
     * pubsub_channels/channel_nsub/numpat back PUBSUB introspection */
    ptrdiff_t (*psubscribe)(void *ctx, struct session *s, const char *pat,
                            size_t len);
    size_t (*punsubscribe)(void *ctx, struct session *s, const char *pat,
                           size_t len);
    void (*each_pattern)(void *ctx, struct session *s,
                         void (*cb)(const char *pat, size_t len, void *arg),
                         void *arg);
    void (*deliver_pattern)(void *owner, const char *pat, size_t patlen,
                            const char *ch, size_t chlen, const char *msg,
                            size_t mlen);
    size_t (*pubsub_channels)(void *ctx, const char *pat, size_t patlen,
                              resp_buf *out);
    long (*channel_nsub)(void *ctx, const char *ch, size_t len);
    long (*numpat)(void *ctx);
    size_t npsub; /* patterns this session is subscribed to */
    /* AOF hook (server-owned): called with the session's db index and the
     * original argv of every successful mutating command. NULL = no
     * persistence logging. raw/raw_len carry the exact client request
     * bytes for top-level socket commands (NULL for replays, script
     * effects and internal executions) so the replication stream can
     * skip re-serialization. */
    void *aof_ctx;
    void (*aof_log)(void *ctx, int db_index, const resp_value *argv,
                    size_t argc, const char *raw, size_t raw_len);
    /* set by the server's input loop around each top-level command */
    const char *raw_cmd;
    size_t raw_cmd_len;
    /* SHUTDOWN hook (server-owned): flips the server shutdown flag. */
    void *shutdown_ctx;
    void (*request_shutdown)(void *ctx);
    /* set by QUIT: the server closes the connection once the reply (and
     * anything already buffered) has been flushed; no-op for stack
     * sessions */
    int quit;
    int monitor_enabled;
    void *monitor_ctx;
    int (*monitor_start)(void *ctx, struct session *s);
    void (*monitor_emit)(void *ctx, struct session *source,
                         const resp_value *argv, size_t argc);
    void *hotkeys_ctx;
    int (*hotkeys_command)(void *ctx, const resp_value *argv, size_t argc,
                           resp_buf *out);
    void *backup_ctx;
    int (*backup_command)(void *ctx, const resp_value *argv, size_t argc,
                          resp_buf *out);
    /* Blocking-command state (BLPOP/BRPOP/BRPOPLPUSH/BLMOVE/BLMPOP/
     * BZPOPMIN/BZPOPMAX/BZMPOP/XREAD/XREADGROUP). When blocked, the server stops reading
     * further commands from this connection and retries the stored argv
     * through command_blocked_try() as keys become ready or the deadline
     * expires. `blocked_deadline_ms` is wall ms; 0 means block forever. */
    int blocked;
    uint16_t blocked_cmd;
    uint64_t blocked_deadline_ms;
    resp_value *blocked_argv;
    size_t blocked_argc;
    /* Last dispatched command measurements consumed by server-owned hooks. */
    uint64_t last_cmd_usecs;
    uint64_t last_cmd_net_bytes;
    /* CLUSTER MEET hook (server-owned): open a bus conn and send MEET. */
    void *cluster_ctx;
    int (*cluster_meet)(void *ctx, const char *ip, uint16_t port);
    /* CLUSTER REPLICATE / FAILOVER hook (server-owned): start (ip != NULL)
     * or stop (ip == NULL) data replication of the given master. */
    int (*cluster_replicate)(void *ctx, const char *ip, uint16_t port);
    int asking; /* cluster: one-shot ASKING flag for the next command */
    int read_only; /* cluster: READONLY/READWRITE connection state */
    int in_script; /* >0 inside Lua execution (nested EVAL is rejected) */
    int in_ro_script; /* >0 inside EVAL_RO/EVALSHA_RO (write commands blocked) */
    int aof_skip;  /* script effects are AOF-logged individually, not EVAL */
    /* replication (server-owned; NULL for stack sessions) */
    const repl_info *repl;  /* INFO replication source */
    const int *role;        /* server role, for READONLY checks */
    const io_counters *io;  /* server IO counters for INFO (Phase 27) */
    int repl_link;          /* this session is the inbound master link */
    void (*sync_hook)(void *ctx, struct session *s);
    void *sync_ctx;
    /* PSYNC: partial-or-full resync handshake (server-owned). */
    int (*psync_hook)(void *ctx, struct session *s, const char *replid,
                      size_t replid_len, long long offset);
    void *psync_ctx;
    int (*replicaof_hook)(void *ctx, const char *host, uint16_t port);
    void *replicaof_ctx;
    /* RESET: clear connection-scoped server state (pub/sub registries,
     * MULTI/WATCH are handled by the core dispatcher). */
    void (*reset_hook)(void *ctx, struct session *s);
    void *reset_ctx;
    /* server ops/introspection hooks (server-owned; NULL in stack tests) */
    void *client_ctx;
    long long (*client_id)(void *ctx, struct session *s);
    int (*client_setname)(void *ctx, struct session *s, const char *name,
                          size_t len);
    const char *(*client_getname)(void *ctx, struct session *s,
                                  size_t *len);
    void (*client_list)(void *ctx, resp_buf *out);
    int (*client_kill)(void *ctx, const char *filter, size_t filterlen,
                       resp_buf *out);
    void *slowlog_ctx;
    void (*slowlog_add)(void *ctx, const resp_value *argv, size_t argc,
                        uint64_t usec, uint64_t now_ms);
    size_t (*slowlog_len)(void *ctx);
    void (*slowlog_get)(void *ctx, long long count, resp_buf *out);
    void (*slowlog_reset)(void *ctx);
    void *bgrewriteaof_ctx;
    void (*bgrewriteaof)(void *ctx, resp_buf *out);
    /* Server-level CONFIG parameters that cannot be represented in db. */
    void *config_ctx;
    int (*config_command)(void *ctx, const char *sub, size_t sub_len,
                          const char *param, size_t param_len,
                          const char *value, size_t value_len,
                          resp_buf *out);
} session;

void session_init(session *s, db *d);
/* Free queue/watch contents (not s itself). */
void session_release(session *s);
session *session_create(db *d);
void session_free(session *s);

/* Deep-copy argv onto the MULTI queue. */
int session_queue_push(session *s, const resp_value *argv, size_t argc);

#ifdef DDUP_TESTING
int session_test_queue_bytes(size_t cap, size_t *bytes);
int session_test_queue_growth(size_t cap, size_t *new_cap);
#endif
void session_queue_clear(session *s);

void session_watch_add(session *s, const char *key, size_t klen,
                       uint64_t version, uint64_t epoch, int db_index);
void session_watch_clear(session *s);

/* Install a deep-copied blocking request. Returns 0 on success, -1 on
 * allocation failure (the session is left unblocked). */
int session_block_start(session *s, const resp_value *argv, size_t argc,
                        uint16_t cmd_id, uint64_t deadline_ms);
void session_block_clear(session *s);

/* Execute one command in this session (MULTI queueing, subscribed-mode
 * restrictions); eviction check runs afterwards. */
void session_execute_at(session *s, const resp_value *argv, size_t argc,
                        resp_buf *out, uint64_t now_ms);
void session_execute(session *s, const resp_value *argv, size_t argc,
                     resp_buf *out);

#endif /* DDUP_SESSION_H */
