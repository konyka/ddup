/* main.c - ddup-server entry point. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"
#include "pal/pal_file.h"
#include "pal/pal_platform.h"
#include "pal/pal_socket.h"
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

    s = server_create(cfg.bind, cfg.port);
    if (s == NULL) {
        fprintf(stderr, "failed to listen on %s:%u\n", cfg.bind,
                (unsigned)cfg.port);
        pal_socket_cleanup();
        return 1;
    }

    {
        char verr[256];
        if (config_validate(&cfg, verr, sizeof(verr)) != 0) {
            fprintf(stderr, "config error: %s\n", verr);
            server_destroy(s);
            pal_socket_cleanup();
            return 1;
        }
    }
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
    server_set_backlog_size(s, (size_t)cfg.repl_backlog_size);
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
