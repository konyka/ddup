/* main.c - ddup-server entry point. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"
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
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("listening on port %u\n", (unsigned)server_port(s));
    fflush(stdout);

    while (!g_stop)
        server_run_once(s, 100);

    printf("shutting down\n");
    server_destroy(s);
    pal_socket_cleanup();
    return 0;
}
