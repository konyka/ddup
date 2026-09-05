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
#include "core/acl.h"
#include "pal/pal_file.h"
#include "pal/pal_socket.h"

typedef struct server server;
/* buf_pool comes in via core/session.h -> resp/resp_writer.h; a duplicate
 * forward typedef here trips -Wpedantic on C99 (repeat typedef is C11). */

/* Backend selection (server_create_ex). */
#define SERVER_BACKEND_SELECT 0  /* readiness loop (epoll/kqueue/select) */
#define SERVER_BACKEND_IOCP 1    /* Windows IOCP proactor (falls back to
                                    readiness when unavailable) */
#define SERVER_BACKEND_IOURING 2 /* Linux io_uring readiness (falls back to
                                    epoll when unavailable) */
#define SERVER_BACKEND_IOURING_OP 3 /* Linux io_uring proactor (op-mode:
                                    submitted RECV/SEND/ACCEPT, falls back
                                    to epoll when unavailable) */

/* Create a server bound to host:port. host may be NULL (any interface);
 * port 0 picks an ephemeral port (read it back with server_port()).
 * Returns NULL on failure. */
server *server_create(const char *host, uint16_t port);
server *server_create_ex(const char *host, uint16_t port, int backend);

/* Actual bound port. */
uint16_t server_port(const server *s);

/* SERVER_BACKEND_* this server runs on (after fallback resolution). */
int server_backend(const server *s);
/* Non-zero when the selected backend owns in-flight operations outside the
 * readiness loop (IOCP or io_uring op mode). */
int server_is_proactor(const server *s);

/* Start a TLS listener alongside the plain one (port 0 = ephemeral).
 * Returns 0 on success; -1 when TLS is unavailable (stub build) or the
 * cert/key/listen setup failed. */
int server_enable_tls(server *s, const char *host, uint16_t port,
                      const char *cert_file, const char *key_file);
uint16_t server_tls_port(const server *s);

/* TLS context only, no listener (mt workers: the acceptor owns the TLS
 * listener; each worker wraps adopted fds with its own context). Returns 0
 * on success; -1 when TLS is unavailable or the cert/key failed to load. */
int server_tls_ctx_init(server *s, const char *cert_file,
                        const char *key_file);

/* Enable AOF persistence: replay the file if it exists, then append every
 * mutating command. Returns 0 on success. */
int server_enable_aof(server *s, const char *path);
/* Set the AOF appendfsync policy (AOF_FSYNC_* from server/aof.h; default
 * everysec). Applies to the current AOF and to one enabled later. */
void server_set_appendfsync(server *s, int mode);

/* Require AUTH before commands (Redis requirepass). The pointer is not
 * owned; pass NULL/"" to disable. New connections start unauthenticated. */
void server_set_requirepass(server *s, const char *pw);
/* Read-only access for the mt control-plane ACL fan-out. */
acl_registry *server_acl_registry(server *s);

/* Configure a disjoint arithmetic sequence for CLIENT IDs. The first ID is
 * assigned to the next accepted connection; existing connections retain IDs. */
void server_set_client_id_allocator(server *s, uint64_t first, uint64_t stride);

/* Apply the memory limit and eviction policy to every logical db. */
void server_set_maxmemory(server *s, uint64_t bytes, int policy);
/* Enable tiered storage for every logical db from dir/<logname>. The store
 * is shared by all logical dbs and closed by server_destroy(). */
int server_enable_tiering(server *s, const char *dir, const char *logname,
                          uint64_t max_disk_bytes);
void server_set_proto_max_request_bytes(server *s, size_t bytes);
void server_set_repl_max_snapshot_bytes(server *s, size_t bytes);

/* Execute a server-level CONFIG hook for an internal worker session. */
int server_config_command(void *ctx, const char *sub, size_t sub_len,
                          const char *param, size_t param_len,
                          const char *value, size_t value_len,
                          resp_buf *out);

/* Point SAVE at path (not owned) and, if the file exists, load it into the
 * db. load returns 0 on success, -1 on corrupt/missing file. */
void server_set_snapshot_path(server *s, const char *path);
int server_load_snapshot(server *s);

/* Enable single-node cluster mode with a stable 40-hex node id. */
void server_enable_cluster(server *s, const char *node_id);
/* Override the address announced by CLUSTER SLOTS/bus (mt worker 0 uses
 * the public acceptor address instead of its private listener port). */
void server_set_cluster_announce(server *s, const char *ip, uint16_t port);
/* When off, the event loop skips cluster gossip/failover/nodes.conf save.
 * mt workers other than worker 0 run as cluster-state followers. */
void server_set_cluster_control(server *s, int on);
/* nodes.conf persistence path (empty = no persistence). */
void server_set_nodes_path(server *s, const char *path);
/* Reload persisted nodes.conf into the node table (best-effort). */
void server_load_nodes(server *s, const char *path);
#ifdef DDUP_TESTING
/* Invoke the nodes.conf save path without waiting for its periodic timer. */
void server_test_cluster_nodes_save(server *s);
/* Exercise the server-owned CONFIG appendfsync hook without a socket. */
int server_test_config_appendfsync(const char *value, resp_buf *out);
/* Validate SEND_ZC error cleanup without depending on kernel support. */
int server_test_send_zc_error_cleanup(void);
#endif
/* Node failure timeout in ms (default 15000; tests can shrink it). */
void server_set_node_timeout(server *s, uint64_t ms);

/* Cluster bus wire protocol: own RCM2 (default) or real Redis clusterMsg. */
#define SERVER_BUS_PROTOCOL_DDUP 0
#define SERVER_BUS_PROTOCOL_REDIS 1
void server_set_bus_protocol(server *s, int proto);
/* Enable TLS for the cluster bus. The server certificate/key are used for
 * inbound peers; ca_file enables optional peer verification for outbound
 * peers. Must be called before server_enable_cluster. */
int server_set_cluster_tls(server *s, int enabled, const char *cert_file,
                           const char *key_file, const char *ca_file);

/* SLOWLOG log-slower-than threshold in microseconds (0 logs every command). */
void server_set_slowlog_threshold(server *s, uint64_t usec);
/* Automatic snapshot interval in seconds (0 = off). */
void server_set_save_interval(server *s, int sec);
/* Nonzero after SHUTDOWN or a fatal persistence failure. */
int server_shutdown_requested(const server *s);
/* Final persistence steps before server_destroy: flush AOF (via destroy)
 * and, if a save interval was configured and AOF is off, save a snapshot. */
void server_graceful_stop(server *s);

/* Point this server at a master (host/port), or promote it when host is
 * NULL (REPLICAOF NO ONE). Returns 0 on success. */
int server_replicaof(server *s, const char *host, uint16_t port);
/* Enable TLS for outbound master links. Must be called before
 * server_replicaof; returns -1 for proactor backends or unavailable TLS. */
int server_set_replica_tls(server *s, int enabled, const char *ca_file);

/* Replace the replica-side full-snapshot loader. mt_server installs this
 * so a full SYNC is partitioned across the worker pool instead of being
 * loaded into the coordinator's db. Returns 0 on success. */
typedef int (*server_repl_snapshot_fn)(void *ctx, const char *buf,
                                       size_t len);
void server_set_repl_snapshot_hook(server *s, server_repl_snapshot_fn fn,
                                   void *ctx);

/* Replace the master-side full-sync snapshot serializer. mt_server installs
 * this so SYNC/PSYNC emits one merged snapshot from every worker. */
typedef int (*server_repl_snapshot_serialize_fn)(void *ctx, resp_buf *out);
void server_set_repl_snapshot_serialize_hook(
    server *s, server_repl_snapshot_serialize_fn fn, void *ctx);

/* In centralized mt replication mode worker-local sessions log AOF but do
 * not append to a worker-local backlog; the mt coordinator forwards the
 * exact command bytes to worker 0. */
void server_set_repl_centralized(server *s, int on);

/* Called on a worker-local server after a mutation. In centralized mt mode
 * this forwards the exact RESP command to the mt coordinator (worker 0);
 * in single-server mode it is unused. */
typedef void (*server_repl_stream_fn)(void *ctx, int db_index,
                                      const char *raw, size_t raw_len);
void server_set_repl_stream_forward(server *s, server_repl_stream_fn fn,
                                    void *ctx);
void server_repl_stream_forward(server *s, int db_index, const char *raw,
                                size_t raw_len);

/* Append an exact RESP command to the master backlog and every downstream
 * replica connection (used by the mt replication coordinator). */
void server_repl_stream_append(server *s, const char *raw, size_t raw_len);

/* Coordinator-side append that emits a SELECT prefix when db_index changes.
 * Runs on the worker-0 event loop only. */
void server_repl_stream_append_db(server *s, int db_index, const char *raw,
                                  size_t raw_len);

/* Nonzero when a downstream replica is attached or a PSYNC resume backlog
 * must be maintained. */
int server_repl_active(const server *s);

/* Snapshot of the replication role/backlog state for INFO. In mt mode the
 * worker-0 server owns the master link and the downstream replica set. */
const repl_info *server_repl_info(const server *s);

/* Flush the optional AOF immediately (replication batches call this after
 * a stream chunk; mt mode flushes every worker's AOF). */
void server_aof_flush(server *s);
/* Execute the compatibility BGREWRITEAOF hook against this server's AOF. */
void server_bgrewriteaof(server *s, resp_buf *out);
/* Render this server's local CLIENT LIST view. */
void server_client_list(server *s, resp_buf *out);
int server_client_kill(server *s, const char *filter, size_t filterlen,
                       resp_buf *out);
void server_slowlog_reset(server *s);
void server_slowlog_record(server *s, const resp_value *argv, size_t argc,
                           uint64_t usec, uint64_t now_ms);
size_t server_slowlog_len(server *s);
void server_slowlog_get(server *s, long long count, resp_buf *out);
void server_set_slowlog_id_allocator(server *s, uint64_t first,
                                     uint64_t stride);
/* Execute a server-owned HOTKEYS control command. */
int server_hotkeys_command(server *s, const resp_value *argv, size_t argc,
                           resp_buf *out);

/* Resize the replication backlog ring (drops current contents). Returns 0
 * on success, -1 when bytes is zero or allocation fails; failure preserves
 * the existing ring. */
int server_set_backlog_size(server *s, size_t bytes);

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
/* Adopt an already-accepted fd as a TLS connection (context must be
 * initialized via server_tls_ctx_init/server_enable_tls): wraps the fd and
 * starts the non-blocking handshake in the event loop. */
int server_adopt_fd_tls(server *s, pal_socket_t fd);
int server_set_wakeup(server *s, pal_socket_t fd, void (*cb)(void *ctx),
                      void *ctx);
/* Cross-thread kick for the IOCP backend: posts a WAKEUP completion that
 * makes server_run_once invoke the wakeup callback. No-op otherwise. */
void server_wakeup_kick(server *s);

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
/* Free all connections while the server and its callbacks remain usable. */
void server_free_connections(server *s);
/* Connection migration (mt connection-key affinity): detach removes the
 * conn from this server's loop and connection table; rehome rewires the
 * session hook contexts to the new server; adopt registers the conn on
 * the new server and immediately processes any buffered input. */
int server_conn_detach(server *s, void *conn);
void server_conn_rehome(server *s, void *conn);
int server_conn_adopt(server *s, void *conn);
int server_conn_out_append(server *s, void *conn, const char *data,
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

/* Selection-hook adapter matching session.sel_fn (stack sessions over a
 * server, e.g. the mt INFO __STATS__ path). */
db *server_select_db(void *ctx, int idx);

/* Total logical databases (default 16). */
int server_ndbs(const server *s);

/* Always-on IO counters (Phase 27); pointer owned by the server, used by
 * the mt INFO __STATS__ stack session. */
const io_counters *server_io_counters(server *s);

/* Focused invariant seams used by length-integrity tests. */
int server_test_pubsub_rejected_subscribe(void);
int server_test_psync_continue_reserve_failure(void);
/* Reject PSYNC output when backlog metadata is internally inconsistent. */
int server_test_psync_corrupt_backlog_rejected(void);
int server_test_publish_frame_validation(void);
void server_test_set_aof_write_fn(
    server *s,
    ptrdiff_t (*write_fn)(pal_file *f, const void *buf, size_t n));
int server_test_aof_failed(const server *s);
size_t server_test_aof_pending_bytes(const server *s);

/* Append a mutating command to the worker's AOF with the multi-db SELECT
 * prefix rule (no-op when AOF is off; used by the mt routed-task path). */
void server_aof_log_cmd(server *s, int db_index, const resp_value *argv,
                        size_t argc);

void server_destroy(server *s);

#endif /* DDUP_SERVER_H */
