/* migrate.h - MIGRATE: live key transfer between ddup instances.
 *
 * The source dumps each key (snapshot per-key payload + remaining ttl),
 * opens a blocking-ish TCP connection to the target and sends pipelined
 * RESTORE commands, then waits for one reply per key. While waiting, an
 * optional pump hook runs: production leaves it NULL (the server simply
 * stalls, like Redis), in-process multi-server tests pump the target's
 * event loop from it since both servers share the test thread.
 */
#ifndef DDUP_MIGRATE_H
#define DDUP_MIGRATE_H

#include <stddef.h>
#include <stdint.h>

#include "core/command.h"

#define MIGRATE_OK 0
#define MIGRATE_IOERR (-1)

typedef void (*migrate_pump_fn)(void *ctx);
void migrate_set_pump_hook(migrate_pump_fn fn, void *ctx);

/* Transfer the given keys (string-typed argv values) to host:port.
 * Missing keys are skipped. timeout_ms bounds the whole transfer; on the
 * first failure the transfer stops and only target-confirmed keys are
 * deleted locally (unless copy). Returns MIGRATE_OK or MIGRATE_IOERR. */
int migrate_run(db *d, const char *host, uint16_t port,
                const resp_value *keys, size_t nkeys, uint64_t timeout_ms,
                int copy, int replace, uint64_t now_ms);

#endif /* DDUP_MIGRATE_H */
