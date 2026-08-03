/* server.h - single-threaded RESP server over the pal event loop.
 *
 * One server owns one event loop, one listening socket, one db and a set of
 * connections. Thread-per-core (one server per IO thread) arrives in a later
 * phase; the API already allows creating several independent servers.
 */
#ifndef DDUP_SERVER_H
#define DDUP_SERVER_H

#include <stdint.h>

typedef struct server server;

/* Create a server bound to host:port. host may be NULL (any interface);
 * port 0 picks an ephemeral port (read it back with server_port()).
 * Returns NULL on failure. */
server *server_create(const char *host, uint16_t port);

/* Actual bound port. */
uint16_t server_port(const server *s);

/* Run exactly one event-loop iteration: wait up to timeout_ms for readiness,
 * accept new connections and service ready ones. Returns the number of
 * readiness events handled (0 = timeout). */
int server_run_once(server *s, int timeout_ms);

void server_destroy(server *s);

#endif /* DDUP_SERVER_H */
