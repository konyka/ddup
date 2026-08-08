/* main.c - ddup-server entry point. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/cluster.h"
#include "core/config.h"
#include "pal/pal_file.h"
#include "pal/pal_iocp.h"
#include "pal/pal_platform.h"
#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "server/mt_server.h"
#include "server/server.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *prog)
{
    printf("Usage: %s [path/to/ddup.conf] [--key value ...] [--help]\n",
           prog);
    printf("  config file  redis-style 'key value' lines (see ddup.conf)\n");
    printf("  --key value  inline override (e.g. --port 6380)\n");
    printf("  --help       show this help and exit\n");
}

int main(int argc, char **argv)
{
    ddup_config cfg;
    server *s;
    int i;
    /* must outlive main()'s stack frame usage: db.snapshot_path points here */
    static char snap_path[1024];

    config_init(&cfg);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            if (i + 1 >= argc ||
                config_apply(&cfg, argv[i] + 2, argv[i + 1]) != 0) {
                fprintf(stderr, "invalid option: %s %s\n", argv[i],
                        i + 1 < argc ? argv[i + 1] : "");
                return 1;
            }
            i++;
        } else {
            if (config_load_file(&cfg, argv[i]) != 0)
                return 1;
        }
    }

    printf("ddup-server %d.%d.%d (platform: %s, C standard: C%d)\n",
           DDUP_VERSION_MAJOR, DDUP_VERSION_MINOR, DDUP_VERSION_PATCH,
           DDUP_OS_NAME, DDUP_C_STD);

    if (pal_socket_init() != 0) {
        fprintf(stderr, "failed to initialize sockets\n");
        return 1;
    }

    /* thread-per-core path: io-threads > 1 runs the mt worker pool.
     * Cluster/replication are rejected by config_validate in this mode
     * (documented limitation). */
    if (cfg.io_threads > 1) {
        mt_server *ms;
        char verr[256];
        int mt_backend = SERVER_BACKEND_SELECT;
        /* worker backend: IOCP on Windows, io_uring when asked, readiness
         * otherwise; TLS needs the readiness backend */
        if (cfg.tls_port == 0) {
            if (strcmp(cfg.io, "iouring") == 0) {
                mt_backend = SERVER_BACKEND_IOURING;
            } else {
                pal_iocp *probe = pal_iocp_create();
                if (probe != NULL) {
                    pal_iocp_free(probe);
                    mt_backend = SERVER_BACKEND_IOCP;
                }
            }
        }
        if (config_validate(&cfg, verr, sizeof(verr)) != 0) {
            fprintf(stderr, "config error: %s\n", verr);
            pal_socket_cleanup();
            return 1;
        }
        ms = mt_server_create_ex(cfg.bind, cfg.port, cfg.io_threads,
                                 mt_backend);
        if (ms == NULL) {
            fprintf(stderr, "failed to listen on %s:%u\n", cfg.bind,
                    (unsigned)cfg.port);
            pal_socket_cleanup();
            return 1;
        }
        if (cfg.tls_port > 0) {
            if (mt_server_enable_tls(ms, cfg.bind, cfg.tls_port,
                                     cfg.tls_cert_file,
                                     cfg.tls_key_file) != 0) {
                fprintf(stderr, "failed to enable TLS on port %u\n",
                        (unsigned)cfg.tls_port);
                mt_server_destroy(ms);
                pal_socket_cleanup();
                return 1;
            }
            printf("TLS listening on port %u\n",
                   (unsigned)mt_server_tls_port(ms));
        }
        /* per-worker persistence (worker-<id>-<file> under dir) */
        if (cfg.requirepass[0] != '\0')
            mt_server_set_requirepass(ms, cfg.requirepass);
        mt_server_set_maxmemory(ms, cfg.maxmemory, cfg.maxmemory_policy);
        mt_server_set_proto_max_request_bytes(ms,
                                              (size_t)cfg.proto_max_request_bytes);
        mt_server_set_repl_max_snapshot_bytes(ms,
                                               (size_t)cfg.repl_max_snapshot_bytes);
        if (cfg.appendonly) {
            if (mt_server_enable_aof(ms, cfg.dir,
                                     cfg.appendfilename) != 0) {
                fprintf(stderr, "failed to open per-worker AOF in '%s'\n",
                        cfg.dir);
                mt_server_destroy(ms);
                pal_socket_cleanup();
                return 1;
            }
            printf("AOF enabled (per worker): %s/worker-*-%s\n", cfg.dir,
                   cfg.appendfilename);
        } else {
            if (mt_server_enable_snapshots(ms, cfg.dir, cfg.dbfilename,
                                           cfg.save_sec) != 0) {
                fprintf(stderr, "failed to load per-worker snapshot\n");
                mt_server_destroy(ms);
                pal_socket_cleanup();
                return 1;
            }
        }
        if (mt_server_start(ms) != 0) {
            fprintf(stderr, "failed to start %d io threads\n",
                    cfg.io_threads);
            mt_server_destroy(ms);
            pal_socket_cleanup();
            return 1;
        }
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        printf("io backend: mt (%d workers)\n", cfg.io_threads);
        printf("listening on port %u\n", (unsigned)mt_server_port(ms));
        fflush(stdout);
        while (!g_stop)
            pal_sleep_ms(50);
        printf("shutting down\n");
        mt_server_stop(ms);
        mt_server_destroy(ms);
        pal_socket_cleanup();
        return 0;
    }

    s = NULL;
    {
        /* default backend: IOCP where available (Windows), readiness
         * elsewhere; --io overrides; TLS forces readiness (unsupported on
         * the IOCP backend) */
        pal_iocp *probe = pal_iocp_create();
        int backend = probe != NULL ? SERVER_BACKEND_IOCP
                                    : SERVER_BACKEND_SELECT;
        const char *bname;
        if (probe != NULL)
            pal_iocp_free(probe);
        if (cfg.io[0] != '\0') {
            if (strcmp(cfg.io, "iocp") == 0)
                backend = SERVER_BACKEND_IOCP;
            else if (strcmp(cfg.io, "iouring") == 0)
                backend = SERVER_BACKEND_IOURING;
            else if (strcmp(cfg.io, "iouring-op") == 0)
                backend = SERVER_BACKEND_IOURING_OP;
            else
                backend = SERVER_BACKEND_SELECT;
        }
        if (cfg.tls_port > 0 && (backend == SERVER_BACKEND_IOCP ||
                                 backend == SERVER_BACKEND_IOURING_OP)) {
            printf("note: TLS is unsupported on proactor backends; "
                   "using readiness\n");
            backend = SERVER_BACKEND_SELECT;
        }
        s = server_create_ex(cfg.bind, cfg.port, backend);
        bname = backend == SERVER_BACKEND_IOCP      ? "iocp"
                : backend == SERVER_BACKEND_IOURING ? "iouring"
                : backend == SERVER_BACKEND_IOURING_OP
                                                    ? "iouring-op"
                                                    : "select";
        printf("io backend: %s\n", bname);
    }
    if (s == NULL) {
        fprintf(stderr, "failed to listen on %s:%u\n", cfg.bind,
                (unsigned)cfg.port);
        pal_socket_cleanup();
        return 1;
    }
    if (cfg.requirepass[0] != '\0')
        server_set_requirepass(s, cfg.requirepass);
    server_set_maxmemory(s, cfg.maxmemory, cfg.maxmemory_policy);

    {
        char verr[256];
        if (config_validate(&cfg, verr, sizeof(verr)) != 0) {
            fprintf(stderr, "config error: %s\n", verr);
            server_destroy(s);
            pal_socket_cleanup();
            return 1;
        }
    }
    server_set_proto_max_request_bytes(s, (size_t)cfg.proto_max_request_bytes);
    server_set_repl_max_snapshot_bytes(s, (size_t)cfg.repl_max_snapshot_bytes);
    if (cfg.tls_port > 0) {
        if (server_enable_tls(s, cfg.bind, cfg.tls_port, cfg.tls_cert_file,
                              cfg.tls_key_file) != 0) {
            fprintf(stderr,
                    "failed to start TLS listener on port %u (TLS support "
                    "may be unavailable in this build)\n",
                    (unsigned)cfg.tls_port);
            server_destroy(s);
            pal_socket_cleanup();
            return 1;
        }
    }

    if (cfg.appendonly) {
        char aof_path[1024];
        snprintf(aof_path, sizeof(aof_path), "%s/%s", cfg.dir,
                 cfg.appendfilename);
        if (server_enable_aof(s, aof_path) != 0) {
            fprintf(stderr, "failed to open AOF '%s'\n", aof_path);
            server_destroy(s);
            pal_socket_cleanup();
            return 1;
        }
        printf("AOF enabled: %s\n", aof_path);
    } else {
        /* AOF wins over the snapshot when both exist (Redis rule) */
        snprintf(snap_path, sizeof(snap_path), "%s/%s", cfg.dir,
                 cfg.dbfilename);
        server_set_snapshot_path(s, snap_path);
        if (pal_file_exists(snap_path)) {
            if (server_load_snapshot(s) != 0) {
                fprintf(stderr, "failed to load snapshot '%s'\n", snap_path);
                server_destroy(s);
                pal_socket_cleanup();
                return 1;
            }
            printf("snapshot loaded: %s\n", snap_path);
        }
    }
    server_set_save_interval(s, cfg.save_sec);
    if (server_set_backlog_size(s, (size_t)cfg.repl_backlog_size) != 0) {
        fprintf(stderr, "failed to allocate replication backlog (%llu bytes)\n",
                (unsigned long long)cfg.repl_backlog_size);
        server_destroy(s);
        pal_socket_cleanup();
        return 1;
    }

    if (cfg.cluster_enabled) {
        char cpath[1024];
        char nid[41];
        snprintf(cpath, sizeof(cpath), "%s/%s", cfg.dir,
                 cfg.cluster_config_file);
        if (cluster_node_id_load_or_create(cpath, nid) != 0) {
            fprintf(stderr, "failed to load/create cluster config '%s'\n",
                    cpath);
            server_destroy(s);
            pal_socket_cleanup();
            return 1;
        }
        server_load_nodes(s, cpath);
        server_enable_cluster(s, nid);
        server_set_nodes_path(s, cpath);
        if (strcmp(cfg.cluster_bus_protocol, "redis") == 0) {
            server_set_bus_protocol(s, SERVER_BUS_PROTOCOL_REDIS);
            printf("cluster: bus protocol redis\n");
        }
        printf("cluster: enabled, node %s\n", nid);
    }
    if (cfg.replicaof_port > 0) {
        if (server_replicaof(s, cfg.replicaof_host, cfg.replicaof_port) !=
            0)
            fprintf(stderr,
                    "warning: connect to master %s:%u failed; will retry\n",
                    cfg.replicaof_host, (unsigned)cfg.replicaof_port);
        else
            printf("replica of %s:%u\n", cfg.replicaof_host,
                   (unsigned)cfg.replicaof_port);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("listening on port %u\n", (unsigned)server_port(s));
    if (cfg.tls_port > 0)
        printf("listening on TLS port %u\n", (unsigned)server_tls_port(s));
    fflush(stdout);

    while (!g_stop && !server_shutdown_requested(s))
        server_run_once(s, 100);

    printf("shutting down\n");
    server_graceful_stop(s);
    server_destroy(s);
    pal_socket_cleanup();
    return 0;
}
