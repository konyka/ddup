/* main.c - ddup-server entry point. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    printf("Usage: %s [--port N] [--help]\n", prog);
    printf("  --port N   TCP port to listen on (default 6379)\n");
    printf("  --help     show this help and exit\n");
}

int main(int argc, char **argv)
{
    uint16_t port = 6379;
    server *s;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            long p = strtol(argv[++i], NULL, 10);
            if (p <= 0 || p > 65535) {
                fprintf(stderr, "invalid port: %s\n", argv[i]);
                return 1;
            }
            port = (uint16_t)p;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
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

    s = server_create(NULL, port);
    if (s == NULL) {
        fprintf(stderr, "failed to listen on port %u\n", (unsigned)port);
        pal_socket_cleanup();
        return 1;
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
