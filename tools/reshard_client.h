/* reshard_client.h - blocking RESP client + slot resharding orchestration.
 *
 * Shared by the ddup-reshard command-line tool and its integration test
 * (tests/test_reshard.c). Not part of the server: a small redis-cli style
 * client over ddup_core's RESP parser/writer.
 */
#ifndef DDUP_RESHARD_CLIENT_H
#define DDUP_RESHARD_CLIENT_H

#include <stddef.h>
#include <stdio.h>

#include "core/arena.h"
#include "pal/pal_socket.h"
#include "resp/resp.h"

typedef struct rs_conn {
    pal_socket_t fd;
    char *buf;  /* receive buffer (grows on demand) */
    size_t len; /* valid bytes in buf */
    size_t cap;
} rs_conn;

/* Blocking connect. Returns 0 on success. */
int rs_connect(rs_conn *c, const char *host, uint16_t port);
void rs_close(rs_conn *c);

/* Send one command and parse one reply. args/lens: argc string items;
 * lens == NULL means strlen() per item. The reply's strings are zero-copy
 * views into the connection buffer and stay valid until the next rs_exec
 * on the same connection; nested arrays come from ar (reset per call).
 * Returns 0 on a parsed reply (check v->type for RESP_ERROR), -1 on
 * IO/protocol failure. */
int rs_exec(rs_conn *c, arena *ar, resp_value *v, int argc,
            const char *const *args, const size_t *lens);

/* Orchestrate one slot reshard from -> to (redis-cli --cluster reshard
 * style): SETSLOT MIGRATING/IMPORTING, batched GETKEYSINSLOT + MIGRATE,
 * then SETSLOT NODE on both ends. count = keys per batch, timeout_ms the
 * per-MIGRATE server-side timeout. Progress lines go to log (may be NULL).
 * Returns 0 and sets *migrated on success; -1 on failure (the slot may be
 * left in MIGRATING/IMPORTING state, mirroring redis-cli). */
int reshard_slot(const char *from_host, uint16_t from_port,
                 const char *to_host, uint16_t to_port, int slot, int count,
                 int timeout_ms, long long *migrated, FILE *log);

#endif /* DDUP_RESHARD_CLIENT_H */
