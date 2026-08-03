/* ddup-bench.c - redis-benchmark-style load client for ddup-server.
 *
 * Usage: ddup-bench [-h host] [-p port] [-n requests] [-c clients]
 *                   [-P pipeline] [-t set|get]
 *
 * Clients are sequential (each of the -c clients connects, runs its share
 * of the -n requests with pipelining -P, disconnects). Keys are
 * pre-generated as bench:000000000001... For -t get, run -t set with the
 * same -n first so the keys exist.
 *
 * This is a benchmarking tool, not a ctest test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "resp/resp_parser.h"

#define VALUE_SIZE 16
#define RECV_CAP (256 * 1024)

static const char *g_host = "127.0.0.1";
static uint16_t g_port = 6379;
static long g_requests = 100000;
static long g_clients = 50;
static long g_pipeline = 1;
static int g_test_set = 1; /* 1 = set, 0 = get */

static void usage(const char *prog)
{
    printf("Usage: %s [-h host] [-p port] [-n requests] [-c clients]\n"
           "          [-P pipeline] [-t set|get]\n",
           prog);
}

/* Append one SET/GET command for key index i to buf. Keys are 18 bytes:
 * "bench:" + 12 zero-padded digits. */
static size_t build_cmd(char *buf, long i, int is_set)
{
    if (is_set)
        return (size_t)snprintf(buf, 96,
                                "*3\r\n$3\r\nSET\r\n$18\r\nbench:%012ld\r\n"
                                "$16\r\n0123456789abcdef\r\n",
                                i);
    return (size_t)snprintf(buf, 96,
                            "*2\r\n$3\r\nGET\r\n$18\r\nbench:%012ld\r\n", i);
}

/* Send all of buf; returns -1 on error. */
static int send_all(pal_socket_t fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ptrdiff_t n = pal_send(fd, buf + sent, len - sent);
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Read exactly nreplies RESP replies from fd (blocking). rbuf/rlen/rcap are
 * the persistent receive state. Returns -1 on protocol/IO error. */
static int read_replies(pal_socket_t fd, long nreplies, char *rbuf,
                        size_t *rlen, size_t rcap, arena *a)
{
    long got = 0;
    size_t off = 0;
    while (got < nreplies) {
        resp_value v;
        ptrdiff_t used;
        if (off == *rlen) {
            ptrdiff_t n = pal_recv(fd, rbuf + *rlen, rcap - *rlen);
            if (n <= 0)
                return -1;
            *rlen += (size_t)n;
            continue;
        }
        used = resp_parse(rbuf + off, *rlen - off, &v, a);
        if (used < 0)
            return -1;
        if (used == 0) {
            /* incomplete: compact and recv more */
            memmove(rbuf, rbuf + off, *rlen - off);
            *rlen -= off;
            off = 0;
            if (*rlen == rcap)
                return -1; /* single reply larger than the buffer */
            continue;
        }
        arena_reset(a);
        off += (size_t)used;
        got++;
    }
    if (off > 0) {
        memmove(rbuf, rbuf + off, *rlen - off);
        *rlen -= off;
    }
    return 0;
}

/* Run one client: nreq requests with pipelining. Returns replies checked,
 * or -1 on error. */
static long run_client(long nreq, long key_base, int is_set, char *rbuf,
                       char *sbuf, arena *a)
{
    pal_socket_t fd = pal_tcp_connect(g_host, g_port);
    size_t rlen = 0;
    long done = 0;

    if (fd == PAL_SOCKET_INVALID) {
        fprintf(stderr, "connect failed to %s:%u\n", g_host,
                (unsigned)g_port);
        return -1;
    }
    while (done < nreq) {
        long batch = g_pipeline;
        size_t blen = 0;
        long j;
        if (batch > nreq - done)
            batch = nreq - done;
        for (j = 0; j < batch; j++)
            blen += build_cmd(sbuf + blen, key_base + done + j, is_set);
        if (send_all(fd, sbuf, blen) != 0 ||
            read_replies(fd, batch, rbuf, &rlen, RECV_CAP, a) != 0) {
            pal_close(fd);
            return -1;
        }
        done += batch;
    }
    pal_close(fd);
    return done;
}

int main(int argc, char **argv)
{
    int i;
    char *rbuf;
    char *sbuf;
    arena a;
    uint64_t t0, t1;
    long total = 0;
    long key_base = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            g_host = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_port = (uint16_t)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            g_requests = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            g_clients = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
            g_pipeline = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            const char *t = argv[++i];
            if (strcmp(t, "set") == 0)
                g_test_set = 1;
            else if (strcmp(t, "get") == 0)
                g_test_set = 0;
            else {
                usage(argv[0]);
                return 1;
            }
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (g_requests <= 0 || g_clients <= 0 || g_pipeline <= 0 ||
        g_port == 0) {
        usage(argv[0]);
        return 1;
    }

    if (pal_socket_init() != 0) {
        fprintf(stderr, "socket init failed\n");
        return 1;
    }
    rbuf = (char *)malloc(RECV_CAP);
    sbuf = (char *)malloc((size_t)g_pipeline * 96);
    if (rbuf == NULL || sbuf == NULL) {
        free(rbuf);
        free(sbuf);
        return 1;
    }
    arena_init(&a, 4096);

    printf("====== %s ======\n", g_test_set ? "SET" : "GET");
    printf("  %ld requests, %ld clients (sequential), pipeline %ld\n",
           g_requests, g_clients, g_pipeline);
    printf("  %d bytes payload\n", VALUE_SIZE);

    t0 = pal_now_ms();
    for (i = 0; i < g_clients; i++) {
        long share = g_requests / g_clients +
                     (i < g_requests % g_clients ? 1 : 0);
        if (run_client(share, key_base, g_test_set, rbuf, sbuf, &a) < 0) {
            fprintf(stderr, "client %d failed\n", i);
            free(rbuf);
            free(sbuf);
            arena_destroy(&a);
            pal_socket_cleanup();
            return 1;
        }
        total += share;
        key_base += share;
    }
    t1 = pal_now_ms();

    {
        double secs = (double)(t1 - t0) / 1000.0;
        double qps = secs > 0.0 ? (double)total / secs : (double)total;
        printf("  %ld requests completed in %.2f seconds\n", total, secs);
        printf("  %.2f requests per second\n", qps);
    }

    free(rbuf);
    free(sbuf);
    arena_destroy(&a);
    pal_socket_cleanup();
    return 0;
}
