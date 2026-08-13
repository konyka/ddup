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
    /* CLUSTER MEET hook (server-owned): open a bus conn and send MEET. */
    void *cluster_ctx;
    int (*cluster_meet)(void *ctx, const char *ip, uint16_t port);
    /* CLUSTER REPLICATE / FAILOVER hook (server-owned): start (ip != NULL)
     * or stop (ip == NULL) data replication of the given master. */
    int (*cluster_replicate)(void *ctx, const char *ip, uint16_t port);
    int asking; /* cluster: one-shot ASKING flag for the next command */
    int in_script; /* >0 inside Lua execution (nested EVAL is rejected) */
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

/* Execute one command in this session (MULTI queueing, subscribed-mode
 * restrictions); eviction check runs afterwards. */
void session_execute_at(session *s, const resp_value *argv, size_t argc,
                        resp_buf *out, uint64_t now_ms);
void session_execute(session *s, const resp_value *argv, size_t argc,
                     resp_buf *out);

#endif /* DDUP_SESSION_H */
