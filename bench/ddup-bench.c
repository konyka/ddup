/* ddup-bench.c - redis-benchmark-style load client for ddup-server.
 *
 * Usage: ddup-bench [-h host] [-p port] [-n requests] [-c clients]
 *                   [-P pipeline] [-t set|get] [-r keyspace]
 *
 * All -c client connections are live SIMULTANEOUSLY: non-blocking sockets
 * driven by one readiness loop (pal_event). Each connection keeps up to -P
 * requests in flight; wall time runs from loop start to the last reply.
 * Keys are "bench:" + 12 digits; with -r N each command picks a random key
 * in [0, N). For -t get, run -t set with the same -n first so keys exist.
 *
 * Correctness: replies are parsed and counted (must equal requests), SET
 * replies must be +OK; error replies and protocol/IO failures abort.
 *
 * This is a benchmarking tool, not a ctest test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "pal/pal_event.h"
#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "resp/resp_parser.h"

#define VALUE_SIZE 16
#define CONN_RCAP (64 * 1024)
#define MAX_EVENTS 64
#define STALL_MS 30000 /* abort when no reply arrives for this long */

static const char *g_host = "127.0.0.1";
static uint16_t g_port = 6379;
static long g_requests = 100000;
static long g_clients = 50;
static long g_pipe = 1;
static long g_rand_range = 0; /* -r: random keyspace size, 0 = sequential */
static int g_is_set = 1;

/* latency histogram: log2 microsecond buckets + exact min/max */
static uint64_t g_hist[64];
static uint64_t g_lat_min = (uint64_t)-1;
static uint64_t g_lat_max = 0;
static long g_error_replies = 0;

static void usage(const char *prog)
{
    printf("Usage: %s [-h host] [-p port] [-n requests] [-c clients]\n"
           "          [-P pipeline] [-t set|get] [-r keyspace]\n",
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

static void hist_record(uint64_t us)
{
    int b = 0;
    while (b < 63 && (us >> b) > 1)
        b++;
    g_hist[b]++;
    if (us < g_lat_min)
        g_lat_min = us;
    if (us > g_lat_max)
        g_lat_max = us;
}

/* smallest bucket lower bound covering pct% of all samples */
static uint64_t hist_percentile(double pct)
{
    uint64_t total = 0, seen = 0, want;
    int i;
    for (i = 0; i < 64; i++)
        total += g_hist[i];
    if (total == 0)
        return 0;
    want = (uint64_t)((double)total * pct / 100.0) + 1;
    for (i = 0; i < 64; i++) {
        seen += g_hist[i];
        if (seen >= want)
            return i == 0 ? 0 : (uint64_t)1 << (i - 1);
    }
    return (uint64_t)1 << 62;
}

typedef struct bconn {
    pal_socket_t fd;
    int active; /* still registered in the loop */
    int want_write;
    char *sbuf; /* commands not yet fully sent */
    size_t sblen, ssent, sbcap;
    char *rbuf; /* reply bytes not yet fully parsed */
    size_t rlen;
    long share; /* requests this connection must complete */
    long sent;  /* requests written */
    long done;  /* replies parsed */
    long key_next;
    uint32_t rng;
    uint64_t *ts; /* per-in-flight-request send timestamps (ring of -P) */
} bconn;

static long conn_next_key(bconn *c)
{
    if (g_rand_range > 0) {
        /* xorshift32 */
        uint32_t x = c->rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c->rng = x;
        return (long)(x % (uint32_t)g_rand_range);
    }
    return c->key_next++;
}

static void conn_finish(pal_loop *l, bconn *c)
{
    pal_loop_del(l, c->fd);
    pal_close(c->fd);
    c->active = 0;
}

/* write interest: pending bytes, or budget for more in-flight requests */
static int conn_want_write(const bconn *c)
{
    if (c->ssent < c->sblen)
        return 1;
    return c->sent < c->share && c->sent - c->done < g_pipe;
}

static void conn_arm(pal_loop *l, bconn *c)
{
    int want = conn_want_write(c);
    if (want != c->want_write) {
        pal_loop_mod(l, c->fd, 1, want, c);
        c->want_write = want;
    }
}

/* send pending bytes, then top up the pipeline; returns -1 on IO error */
static int conn_pump_write(bconn *c)
{
    for (;;) {
        while (c->ssent < c->sblen) {
            ptrdiff_t w = pal_send(c->fd, c->sbuf + c->ssent,
                                   c->sblen - c->ssent);
            if (w > 0) {
                c->ssent += (size_t)w;
                continue;
            }
            if (w < 0 && pal_would_block(pal_socket_error()))
                return 0; /* socket full: wait for the next writable */
            return -1;
        }
        c->sblen = 0;
        c->ssent = 0;
        while (c->sent < c->share && c->sent - c->done < g_pipe &&
               c->sblen + 96 <= c->sbcap) {
            c->sblen += build_cmd(c->sbuf + c->sblen, conn_next_key(c),
                                  g_is_set);
            c->ts[c->sent % g_pipe] = pal_now_us();
            c->sent++;
        }
        if (c->sblen == 0)
            return 0; /* pipeline at capacity: wait for replies */
    }
}

static int conn_check_reply(const resp_value *v)
{
    if (v->type == RESP_ERROR || v->type == RESP_BLOB_ERROR) {
        g_error_replies++;
        return 0;
    }
    if (g_is_set &&
        !(v->type == RESP_SIMPLE_STRING && v->len == 2 &&
          memcmp(v->str, "OK", 2) == 0)) {
        g_error_replies++;
        return 0;
    }
    return 0;
}

/* recv + parse replies; returns -1 on IO/protocol error, 1 when the
 * connection completed its share, 0 otherwise */
static int conn_pump_read(bconn *c, arena *a)
{
    size_t off = 0;
    for (;;) {
        ptrdiff_t n = pal_recv(c->fd, c->rbuf + c->rlen,
                               CONN_RCAP - c->rlen);
        if (n > 0) {
            c->rlen += (size_t)n;
            continue;
        }
        if (n < 0 && pal_would_block(pal_socket_error()))
            break;
        if (n == 0 && c->done == c->share)
            break; /* orderly close after the last reply */
        return -1;
    }
    while (off < c->rlen) {
        resp_value v;
        ptrdiff_t used = resp_parse(c->rbuf + off, c->rlen - off, &v, a);
        if (used < 0)
            return -1;
        if (used == 0)
            break;
        conn_check_reply(&v);
        hist_record(pal_now_us() - c->ts[c->done % g_pipe]);
        c->done++;
        off += (size_t)used;
        arena_reset(a);
    }
    if (off > 0) {
        memmove(c->rbuf, c->rbuf + off, c->rlen - off);
        c->rlen -= off;
    }
    if (c->rlen == CONN_RCAP)
        return -1; /* no parse progress on a full buffer */
    return c->done == c->share ? 1 : 0;
}

int main(int argc, char **argv)
{
    int i;
    long ci;
    bconn *conns;
    arena a;
    pal_loop *loop;
    pal_event evs[MAX_EVENTS];
    uint64_t t0, t1, last_progress;
    long total = 0, live;
    int failed = 0;

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
            g_pipe = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            g_rand_range = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            const char *t = argv[++i];
            if (strcmp(t, "set") == 0)
                g_is_set = 1;
            else if (strcmp(t, "get") == 0)
                g_is_set = 0;
            else {
                usage(argv[0]);
                return 1;
            }
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (g_requests <= 0 || g_clients <= 0 || g_pipe <= 0 || g_port == 0 ||
        g_rand_range < 0) {
        usage(argv[0]);
        return 1;
    }

    if (pal_socket_init() != 0) {
        fprintf(stderr, "socket init failed\n");
        return 1;
    }
    loop = pal_loop_create();
    conns = (bconn *)calloc((size_t)g_clients, sizeof(bconn));
    if (loop == NULL || conns == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    arena_init(&a, 4096);

    /* connect everybody up front (fast on loopback), all non-blocking */
    {
        long base = 0;
        for (ci = 0; ci < g_clients; ci++) {
            bconn *c = &conns[ci];
            c->share = g_requests / g_clients +
                       (ci < g_requests % g_clients ? 1 : 0);
            c->key_next = base;
            base += c->share;
            c->rng = 0x9E3779B9u ^ (uint32_t)(ci + 1);
            c->sbcap = (size_t)g_pipe * 96 + 96;
            c->sbuf = (char *)malloc(c->sbcap);
            c->rbuf = (char *)malloc(CONN_RCAP);
            c->ts = (uint64_t *)malloc((size_t)g_pipe * sizeof(uint64_t));
            if (c->sbuf == NULL || c->rbuf == NULL || c->ts == NULL) {
                fprintf(stderr, "out of memory\n");
                return 1;
            }
            if (c->share == 0)
                continue; /* more clients than requests */
            c->fd = pal_tcp_connect(g_host, g_port);
            if (c->fd == PAL_SOCKET_INVALID ||
                pal_set_nonblocking(c->fd, 1) != 0 ||
                pal_loop_add(loop, c->fd, 1, 1, c) != 0) {
                fprintf(stderr, "connect failed to %s:%u\n", g_host,
                        (unsigned)g_port);
                return 1;
            }
            c->active = 1;
            c->want_write = 1;
        }
    }

    printf("====== %s ======\n", g_is_set ? "SET" : "GET");
    printf("  %ld requests, %ld clients (parallel), pipeline %ld\n",
           g_requests, g_clients, g_pipe);
    printf("  %d bytes payload\n", VALUE_SIZE);
    if (g_rand_range > 0)
        printf("  random keys in [0, %ld)\n", g_rand_range);

    t0 = pal_now_ms();
    last_progress = t0;
    live = g_clients;
    for (ci = 0; ci < g_clients; ci++)
        if (conns[ci].share == 0)
            live--;
    while (live > 0) {
        int nev = pal_loop_wait(loop, evs, MAX_EVENTS, 1000);
        int ei;
        if (nev < 0) {
            fprintf(stderr, "event loop error\n");
            failed = 1;
            break;
        }
        for (ei = 0; ei < nev; ei++) {
            bconn *c = (bconn *)evs[ei].userdata;
            int rc;
            if (!c->active)
                continue;
            if (evs[ei].writable && conn_pump_write(c) != 0) {
                fprintf(stderr, "send failed\n");
                failed = 1;
                conn_finish(loop, c);
                live--;
                continue;
            }
            if (!evs[ei].readable) {
                conn_arm(loop, c);
                continue;
            }
            rc = conn_pump_read(c, &a);
            if (rc > 0) {
                last_progress = pal_now_ms();
                conn_finish(loop, c);
                live--;
                continue;
            }
            if (rc < 0) {
                fprintf(stderr, "recv/parse failed\n");
                failed = 1;
                conn_finish(loop, c);
                live--;
                continue;
            }
            last_progress = pal_now_ms();
            conn_arm(loop, c);
        }
        if (pal_now_ms() - last_progress > STALL_MS) {
            fprintf(stderr, "no progress for %d ms: aborting\n", STALL_MS);
            failed = 1;
            break;
        }
    }
    t1 = pal_now_ms();

    for (ci = 0; ci < g_clients; ci++) {
        if (conns[ci].done != conns[ci].share) {
            fprintf(stderr,
                    "client %ld: %ld/%ld replies (count mismatch)\n", ci,
                    conns[ci].done, conns[ci].share);
            failed = 1;
        }
        total += conns[ci].done;
    }
    if (g_error_replies > 0) {
        fprintf(stderr, "%ld error replies from the server\n",
                g_error_replies);
        failed = 1;
    }

    {
        double secs = (double)(t1 - t0) / 1000.0;
        double qps = secs > 0.0 ? (double)total / secs : (double)total;
        printf("  %ld requests completed in %.2f seconds\n", total, secs);
        printf("  latency (us): min=%llu p50=%llu p99=%llu max=%llu\n",
               (unsigned long long)(g_lat_min == (uint64_t)-1 ? 0
                                                              : g_lat_min),
               (unsigned long long)hist_percentile(50.0),
               (unsigned long long)hist_percentile(99.0),
               (unsigned long long)g_lat_max);
        printf("  %.2f requests per second\n", qps); /* keep LAST (CI grep) */
    }

    for (ci = 0; ci < g_clients; ci++) {
        free(conns[ci].sbuf);
        free(conns[ci].rbuf);
        free(conns[ci].ts);
    }
    free(conns);
    arena_destroy(&a);
    pal_loop_free(loop);
    pal_socket_cleanup();
    return failed ? 1 : 0;
}
