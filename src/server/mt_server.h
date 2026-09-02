/* mt_server.h - thread-per-core server (shared-nothing worker pool).
 *
 * One acceptor thread owns the public listener and hands accepted fds to
 * worker threads round-robin. Each worker runs an independent readiness
 * event loop with its own db and buffer pool. Cross-worker key routing is
 * layered on top of this skeleton (later milestone).
 */
#ifndef DDUP_MT_SERVER_H
#define DDUP_MT_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include "pal/pal_file.h"

typedef struct mt_server mt_server;

/* Create the listener and worker pool (does not start any thread).
 * nworkers >= 1. Returns NULL on failure. */
mt_server *mt_server_create(const char *host, uint16_t port, int nworkers);

/* Same, with an explicit worker event-loop backend (SERVER_BACKEND_*;
 * unavailable backends fall back to readiness per worker). Note: TLS
 * requires readiness workers, and connection migration is disabled on the
 * IOCP backend (task routing is unaffected). */
mt_server *mt_server_create_ex(const char *host, uint16_t port, int nworkers,
                               int worker_backend);

/* Actual bound port. */
uint16_t mt_server_port(const mt_server *ms);

/* Total routed tasks executed across all workers (test/observability). */
uint64_t mt_server_tasks_executed(const mt_server *ms);
/* Nonzero once a replica full sync has finished restoring every shard. */
int mt_server_repl_synced(const mt_server *ms);
/* pooled-task freelist hits across workers (Phase 31 observability) */
uint64_t mt_server_pool_hits(const mt_server *ms);
/* Test seam: force the next n executed-task completion pushes to fail. */
void mt_server_fail_next_completion_pushes(mt_server *ms, int n);
int mt_server_completion_pushes_consumed(const mt_server *ms);
int mt_server_abandoned_aggregate_count(const mt_server *ms);
/* Test seam: force the next n aggregate descriptors to fail allocation. */
void mt_server_fail_next_aggregate_allocs(mt_server *ms, int n);

/* Test seams for coordinated AOF-failure shutdown. */
void mt_server_test_set_aof_write_fn(
    mt_server *ms, int worker_id,
    ptrdiff_t (*write_fn)(pal_file *f, const void *buf, size_t n));
int mt_server_test_running(const mt_server *ms);
uint64_t mt_server_test_worker_loops(const mt_server *ms, int worker_id);


/* Per-worker persistence: each worker owns
 * "<dir>/worker-<id>-<filename>". enable_aof replays the file when it
 * exists; enable_snapshots loads existing snapshots and arms the save
 * interval (0 = manual SAVE only). Return 0 on success. */
int mt_server_enable_aof(mt_server *ms, const char *dir,
                         const char *appendfilename);
/* Set the appendfsync policy on every worker (AOF_FSYNC_* from
 * server/aof.h; default everysec). */
void mt_server_set_appendfsync(mt_server *ms, int mode);
void mt_server_set_slowlog_threshold(mt_server *ms, uint64_t usec);
int mt_server_enable_snapshots(mt_server *ms, const char *dir,
                               const char *dbfilename, int save_sec);
/* Enable tiered storage on every worker using "<dir>/tier-<id>.log". */
int mt_server_enable_tiering(mt_server *ms, const char *dir,
                             uint64_t max_disk_bytes);

/* Enable cluster mode with worker 0 as the cluster control plane. node_id
 * is stable 40-hex; nodes_path is the persisted nodes.conf (empty = none).
 * The announced ip/port is the public acceptor address. */
int mt_server_enable_cluster(mt_server *ms, const char *node_id,
                             const char *nodes_path,
                             const char *announce_ip);
/* Select the cluster bus wire protocol on worker 0. */
void mt_server_set_bus_protocol(mt_server *ms, int proto);

/* Point worker 0's replica link at a master. Full sync is partitioned
 * across the worker pool; the command stream is routed like client traffic.
 * Returns 0 on success, -1 when the initial connect fails. */
int mt_server_replicaof(mt_server *ms, const char *host, uint16_t port);
int mt_server_set_replica_tls(mt_server *ms, int enabled, const char *ca_file);

/* TLS alongside the plain listener: the acceptor owns a second (TLS)
 * listener and tags accepted fds; each worker wraps them with its own TLS
 * context and drives the non-blocking handshake in its event loop.
 * Call before mt_server_start (port 0 = ephemeral). Returns 0 on success;
 * -1 when TLS is unavailable (stub build) or cert/key/listen failed. */
int mt_server_enable_tls(mt_server *ms, const char *host, uint16_t port,
                         const char *cert_file, const char *key_file);
uint16_t mt_server_tls_port(const mt_server *ms);

/* Spawn the acceptor and worker threads. Returns 0 on success. */
int mt_server_start(mt_server *ms);

/* Require AUTH before commands on every worker (Redis requirepass). */
void mt_server_set_requirepass(mt_server *ms, const char *pw);

/* Apply the memory limit and eviction policy on every worker. */
void mt_server_set_maxmemory(mt_server *ms, uint64_t bytes, int policy);
void mt_server_set_proto_max_request_bytes(mt_server *ms, size_t bytes);
void mt_server_set_repl_max_snapshot_bytes(mt_server *ms, size_t bytes);

/* Stop all threads (joins them). Safe to call once before destroy. */
void mt_server_stop(mt_server *ms);

void mt_server_destroy(mt_server *ms);

#endif /* DDUP_MT_SERVER_H */
