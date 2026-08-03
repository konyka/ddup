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
} queued_cmd;

/* WATCH bookkeeping: key copy + key-version/db-epoch seen at WATCH time. */
typedef struct watch_entry {
    char *key;
    size_t klen;
    uint64_t version;
    uint64_t epoch;
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
    uint64_t offset; /* master side: bytes propagated so far */
} repl_info;

typedef struct session {
    db *d;
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
    size_t (*subscribe)(void *ctx, struct session *s, const char *ch,
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
    /* AOF hook (server-owned): called with the original argv of every
     * successful mutating command. NULL = no persistence logging. */
    void *aof_ctx;
    void (*aof_log)(void *ctx, const resp_value *argv, size_t argc);
    /* SHUTDOWN hook (server-owned): flips the server shutdown flag. */
    void *shutdown_ctx;
    void (*request_shutdown)(void *ctx);
    /* replication (server-owned; NULL for stack sessions) */
    const repl_info *repl;  /* INFO replication source */
    const int *role;        /* server role, for READONLY checks */
    int repl_link;          /* this session is the inbound master link */
    void (*sync_hook)(void *ctx, struct session *s);
    void *sync_ctx;
    int (*replicaof_hook)(void *ctx, const char *host, uint16_t port);
    void *replicaof_ctx;
} session;

void session_init(session *s, db *d);
/* Free queue/watch contents (not s itself). */
void session_release(session *s);
session *session_create(db *d);
void session_free(session *s);

/* Deep-copy argv onto the MULTI queue. */
void session_queue_push(session *s, const resp_value *argv, size_t argc);
void session_queue_clear(session *s);

void session_watch_add(session *s, const char *key, size_t klen,
                       uint64_t version, uint64_t epoch);
void session_watch_clear(session *s);

/* Execute one command in this session (MULTI queueing, subscribed-mode
 * restrictions); eviction check runs afterwards. */
void session_execute_at(session *s, const resp_value *argv, size_t argc,
                        resp_buf *out, uint64_t now_ms);
void session_execute(session *s, const resp_value *argv, size_t argc,
                     resp_buf *out);

#endif /* DDUP_SESSION_H */
