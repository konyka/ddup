/* server.h - single-threaded RESP server over the pal event loop.
 *
 * One server owns one event loop, one listening socket, one db and a set of
 * connections. Thread-per-core (one server per IO thread) arrives in a later
 * phase; the API already allows creating several independent servers.
 */
#ifndef DDUP_SERVER_H
#define DDUP_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include "core/session.h"
#include "pal/pal_socket.h"

typedef struct server server;
typedef struct buf_pool buf_pool;

/* Backend selection (server_create_ex). */
#define SERVER_BACKEND_SELECT 0 /* readiness loop (epoll/kqueue/select) */
#define SERVER_BACKEND_IOCP 1   /* Windows IOCP proactor (falls back to
                                   readiness when unavailable) */

/* Create a server bound to host:port. host may be NULL (any interface);
 * port 0 picks an ephemeral port (read it back with server_port()).
 * Returns NULL on failure. */
server *server_create(const char *host, uint16_t port);
server *server_create_ex(const char *host, uint16_t port, int backend);

/* Actual bound port. */
uint16_t server_port(const server *s);

/* Start a TLS listener alongside the plain one (port 0 = ephemeral).
 * Returns 0 on success; -1 when TLS is unavailable (stub build) or the
 * cert/key/listen setup failed. */
int server_enable_tls(server *s, const char *host, uint16_t port,
                      const char *cert_file, const char *key_file);
uint16_t server_tls_port(const server *s);

/* Enable AOF persistence: replay the file if it exists, then append every
 * mutating command. Returns 0 on success. */
int server_enable_aof(server *s, const char *path);

/* Require AUTH before commands (Redis requirepass). The pointer is not
 * owned; pass NULL/"" to disable. New connections start unauthenticated. */
void server_set_requirepass(server *s, const char *pw);

/* Apply the memory limit and eviction policy to every logical db. */
void server_set_maxmemory(server *s, uint64_t bytes, int policy);

/* Point SAVE at path (not owned) and, if the file exists, load it into the
 * db. load returns 0 on success, -1 on corrupt/missing file. */
void server_set_snapshot_path(server *s, const char *path);
int server_load_snapshot(server *s);

/* Enable single-node cluster mode with a stable 40-hex node id. */
void server_enable_cluster(server *s, const char *node_id);
/* nodes.conf persistence path (empty = no persistence). */
void server_set_nodes_path(server *s, const char *path);
/* Reload persisted nodes.conf into the node table (best-effort). */
void server_load_nodes(server *s, const char *path);
/* Node failure timeout in ms (default 15000; tests can shrink it). */
void server_set_node_timeout(server *s, uint64_t ms);

/* Automatic snapshot interval in seconds (0 = off). */
void server_set_save_interval(server *s, int sec);
/* Nonzero after a SHUTDOWN command was processed. */
int server_shutdown_requested(const server *s);
/* Final persistence steps before server_destroy: flush AOF (via destroy)
 * and, if a save interval was configured and AOF is off, save a snapshot. */
void server_graceful_stop(server *s);

/* Point this server at a master (host/port), or promote it when host is
 * NULL (REPLICAOF NO ONE). Returns 0 on success. */
int server_replicaof(server *s, const char *host, uint16_t port);

/* Resize the replication backlog ring (drops current contents). */
void server_set_backlog_size(server *s, size_t bytes);

/* Run exactly one event-loop iteration: wait up to timeout_ms for readiness,
 * accept new connections and service ready ones. Returns the number of
 * readiness events handled (0 = timeout). */
int server_run_once(server *s, int timeout_ms);

/* mt_server support (readiness backend only):
 * - server_close_listener detaches and closes the public listener so the
 *   server only services adopted connections.
 * - server_adopt_fd registers an already-connected fd (non-blocking is set
 *   here; TCP_NODELAY is applied) as a new connection.
 * - server_set_wakeup registers an extra fd in the loop; when readable the
 *   callback runs inside server_run_once. */
void server_close_listener(server *s);
int server_adopt_fd(server *s, pal_socket_t fd);
int server_set_wakeup(server *s, pal_socket_t fd, void (*cb)(void *ctx),
                      void *ctx);

/* mt_server routing hooks (readiness backend only):
 * - server_set_route installs the per-command router. The router runs inside
 *   conn_process_input before session_execute; returning non-zero means the
 *   command was fully handled (locally or forwarded).
 * - The conn_* helpers let the routing layer work with connections without
 *   seeing the conn struct: opaque mt_state storage, routed-task pending
 *   counting (zombie close while tasks are in flight), ordered reply append
 *   and flush. */
typedef int (*server_route_fn)(void *ctx, void *conn, session *sess,
                               const resp_value *argv, size_t argc,
                               const char *raw, size_t rawlen, resp_buf *out);
/* Called once after each conn_process_input parse loop so the router can
 * flush batched commands. */
typedef void (*server_route_flush_fn)(void *ctx, void *conn);
/* Called by conn_close before freeing: return non-zero when the routing
 * layer keeps the conn (zombie until its pending work drains). */
typedef int (*server_mt_close_fn)(void *ctx, void *conn);
void server_set_route(server *s, server_route_fn fn,
                      server_route_flush_fn flush_fn,
                      void (*mt_state_free)(void *ctx, void *st), void *ctx);
void server_set_mt_close(server *s, server_mt_close_fn fn);
void *server_conn_mt_state(void *conn);
void server_conn_set_mt_state(void *conn, void *st);
void server_conn_free_now(server *s, void *conn);
/* Connection migration (mt connection-key affinity): detach removes the
 * conn from this server's loop and connection table; rehome rewires the
 * session hook contexts to the new server; adopt registers the conn on
 * the new server and immediately processes any buffered input. */
int server_conn_detach(server *s, void *conn);
void server_conn_rehome(server *s, void *conn);
int server_conn_adopt(server *s, void *conn);
void server_conn_out_append(server *s, void *conn, const char *data,
                            size_t len);
int server_conn_flush(server *s, void *conn);

/* Buffer-pool introspection (used by integration tests). */
const buf_pool *server_buf_pool(const server *s);
size_t server_pool_hits(const server *s);
size_t server_pool_allocs(const server *s);

/* The worker's keyspace (mt_server routed-task execution). */
db *server_db(server *s);

/* Logical db by index (0 = db0, otherwise extra_dbs[idx-1]). */
db *server_db_at(server *s, int idx);

/* Append a mutating command to the worker's AOF with the multi-db SELECT
 * prefix rule (no-op when AOF is off; used by the mt routed-task path). */
void server_aof_log_cmd(server *s, int db_index, const resp_value *argv,
                        size_t argc);

void server_destroy(server *s);

#endif /* DDUP_SERVER_H */
