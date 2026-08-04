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

typedef struct server server;

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

/* Point SAVE at path (not owned) and, if the file exists, load it into the
 * db. load returns 0 on success, -1 on corrupt/missing file. */
void server_set_snapshot_path(server *s, const char *path);
int server_load_snapshot(server *s);

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

void server_destroy(server *s);

#endif /* DDUP_SERVER_H */
