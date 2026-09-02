/* test_mt_server.c - integration tests for the thread-per-core mt_server.
 *
 * Workers run on real background threads; the test acts as a loopback client
 * and talks to the public listener.
 */
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/cluster.h"
#include "core/hashslot.h"
#include "pal/pal_file.h"
#include "pal/pal_socket.h"
#include "pal/pal_thread.h"
#include "pal/pal_time.h"
#include "server/mt_server.h"
#include "server/server.h"

static ptrdiff_t fail_aof_write(pal_file *f, const void *buf, size_t n)
{
    (void)f;
    (void)buf;
    (void)n;
    return -1;
}

typedef struct server_thread_ctx {
    server *srv;
    volatile int running;
} server_thread_ctx;

static void *server_thread_main(void *arg)
{
    server_thread_ctx *ctx = (server_thread_ctx *)arg;
    while (ctx->running)
        (void)server_run_once(ctx->srv, 20);
    return NULL;
}

static pal_socket_t connect_client(uint16_t port)
{
    pal_socket_t c = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(c != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));
    return c;
}

static void roundtrip(pal_socket_t c, const char *req, const char *expected)
{
    size_t elen = strlen(expected);
    size_t rlen = strlen(req);
    size_t sent = 0, got = 0;
    char buf[1024];
    uint64_t deadline = pal_now_ms() + 5000;

    DD_CHECK(elen <= sizeof(buf));
    while (sent < rlen && pal_now_ms() < deadline) {
        ptrdiff_t n = pal_send(c, req + sent, rlen - sent);
        if (n > 0)
            sent += (size_t)n;
        else
            pal_sleep_ms(1);
    }
    DD_CHECK_EQ_INT((long long)rlen, (long long)sent);

    while (got < elen && pal_now_ms() < deadline) {
        ptrdiff_t n = pal_recv(c, buf + got, sizeof(buf) - got);
        if (n > 0)
            got += (size_t)n;
        else
            pal_sleep_ms(1);
    }
    if (got != elen || memcmp(expected, buf, elen) != 0) {
        fprintf(stderr, "roundtrip mismatch:\n  req      : %.*s\n  expected : %.*s\n  got(%zu) : %.*s\n",
                (int)rlen, req, (int)elen, expected, got, (int)got, buf);
    }
    DD_CHECK_EQ_INT((long long)elen, (long long)got);
    DD_CHECK_MEM(expected, elen, buf, got);
}

static void send_raw(pal_socket_t c, const char *req)
{
    size_t sent = 0, len = strlen(req);
    uint64_t deadline = pal_now_ms() + 5000;
    while (sent < len && pal_now_ms() < deadline) {
        ptrdiff_t n = pal_send(c, req + sent, len - sent);
        if (n > 0)
            sent += (size_t)n;
        else
            pal_sleep_ms(1);
    }
    DD_CHECK_EQ_INT((long long)len, (long long)sent);
}

static size_t recv_deadline(pal_socket_t c, char *buf, size_t len,
                            uint64_t ms);

/* Find a key that maps to the given worker: worker = hash_slot(key) % nw. */
static void pick_key_for_worker(int wanted, int nworkers, char *out,
                                size_t cap)
{
    int i;
    for (i = 0;; i++) {
        snprintf(out, cap, "key:%d", i);
        if ((int)(hash_slot(out, strlen(out)) % (uint32_t)nworkers) ==
            wanted)
            return;
    }
}

/* Send req and read one full reply (bulk-string aware: reads until the
 * declared payload plus trailing CRLF arrived). Returns bytes in buf,
 * NUL-terminated (buf must have room for cap+1... caller keeps cap <= buf). */
static size_t request_full(pal_socket_t c, const char *req, char *buf,
                           size_t cap)
{
    size_t rlen = strlen(req);
    size_t sent = 0, got = 0;
    uint64_t deadline = pal_now_ms() + 5000;

    while (sent < rlen && pal_now_ms() < deadline) {
        ptrdiff_t n = pal_send(c, req + sent, rlen - sent);
        if (n > 0)
            sent += (size_t)n;
        else
            pal_sleep_ms(1);
    }
    DD_CHECK_EQ_INT((long long)rlen, (long long)sent);

    while (pal_now_ms() < deadline) {
        ptrdiff_t n = pal_recv(c, buf + got, cap - 1 - got);
        if (n > 0) {
            got += (size_t)n;
            buf[got] = '\0';
            if (got >= 3 && buf[0] == '$') {
                const char *eol = strstr(buf, "\r\n");
                if (eol != NULL) {
                    long blen = strtol(buf + 1, NULL, 10);
                    size_t hdr = (size_t)(eol - buf) + 2;
                    if (blen >= 0 && got >= hdr + (size_t)blen + 2)
                        break;
                }
            } else if (got >= 2 && memcmp(buf + got - 2, "\r\n", 2) == 0) {
                break;
            }
        } else {
            pal_sleep_ms(1);
        }
    }
    return got;
}

static void pipeline_roundtrip(pal_socket_t c, const char *req,
                               const char *expected)
{
    size_t rlen = strlen(req), elen = strlen(expected), sent = 0, got = 0;
    char buf[2048];
    uint64_t deadline = pal_now_ms() + 5000;
    while (sent < rlen && pal_now_ms() < deadline) {
        ptrdiff_t n = pal_send(c, req + sent, rlen - sent);
        if (n > 0)
            sent += (size_t)n;
        else
            pal_sleep_ms(1);
    }
    DD_CHECK_EQ_INT((long long)rlen, (long long)sent);
    while (got < elen && pal_now_ms() < deadline) {
        ptrdiff_t n = pal_recv(c, buf + got, sizeof(buf) - got);
        if (n > 0)
            got += (size_t)n;
        else
            pal_sleep_ms(1);
    }
    DD_CHECK_EQ_INT((long long)elen, (long long)got);
    if (got != elen)
        fprintf(stderr, "pipeline got(%zu): %.*s\n", got, (int)got, buf);
    DD_CHECK_MEM(expected, elen, buf, got);
}

static void test_two_workers_shared_keyspace(void)
{
    mt_server *ms;
    pal_socket_t a, b;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    DD_CHECK(mt_server_port(ms) != 0);

    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));

    roundtrip(a, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");
    roundtrip(b, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");

    /* Single shared keyspace across workers (routing, not partitioning). */
    roundtrip(a, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", "+OK\r\n");
    roundtrip(a, "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n", "$3\r\nbar\r\n");
    roundtrip(b, "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n", "$3\r\nbar\r\n");

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_routed_cross_worker_commands(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char k0[32], k1[32];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));

    a = connect_client(mt_server_port(ms)); /* -> worker 0 */
    b = connect_client(mt_server_port(ms)); /* -> worker 1 */

    /* a writes a worker-1-owned key: routed there and back. */
    {
        char req[128];
        snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv1\r\n",
                 strlen(k1), k1);
        roundtrip(a, req, "+OK\r\n");
        snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
                 strlen(k1), k1);
        roundtrip(a, req, "$2\r\nv1\r\n");
        /* b reads it locally on worker 1. */
        roundtrip(b, req, "$2\r\nv1\r\n");
    }

    /* b writes a worker-0-owned key: routed to worker 0. */
    {
        char req[128];
        snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv0\r\n",
                 strlen(k0), k0);
        roundtrip(b, req, "+OK\r\n");
        snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
                 strlen(k0), k0);
        roundtrip(b, req, "$2\r\nv0\r\n");
        roundtrip(a, req, "$2\r\nv0\r\n");
    }

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_redis8_single_key_commands_route_to_owner(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char key[64], req[512];
    char arkey[64];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, key, sizeof(key));
    for (int i = 0;; i++) {
        snprintf(arkey, sizeof(arkey), "array:%d", i);
        if ((int)(hash_slot(arkey, strlen(arkey)) % 2u) == 1 &&
            strcmp(arkey, key) != 0)
            break;
    }
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req),
             "*10\r\n$6\r\nHSETEX\r\n$%zu\r\n%s\r\n$2\r\nEX\r\n$2\r\n10\r\n$6\r\nFIELDS\r\n$1\r\n2\r\n$2\r\nf1\r\n$2\r\nv1\r\n$2\r\nf2\r\n$2\r\nv2\r\n",
             strlen(key), key);
    roundtrip(a, req, ":1\r\n");
    snprintf(req, sizeof(req), "*3\r\n$4\r\nHGET\r\n$%zu\r\n%s\r\n$2\r\nf1\r\n",
             strlen(key), key);
    roundtrip(b, req, "$2\r\nv1\r\n");

    snprintf(req, sizeof(req), "*5\r\n$5\r\nARSET\r\n$%zu\r\n%s\r\n$1\r\n2\r\n$1\r\nx\r\n$1\r\ny\r\n",
             strlen(arkey), arkey);
    roundtrip(a, req, ":2\r\n");
    snprintf(req, sizeof(req), "*3\r\n$5\r\nARGET\r\n$%zu\r\n%s\r\n$1\r\n2\r\n",
             strlen(arkey), arkey);
    roundtrip(b, req, "$1\r\nx\r\n");
    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_redis8_multikey_commands_route_to_owner(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char k0[32], k0b[32], k1[32], s0[32], s0b[32], req[768];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    for (int i = 0;; i++) {
        snprintf(k0b, sizeof(k0b), "msetex:%d", i);
        if ((int)(hash_slot(k0b, strlen(k0b)) % 2u) == 0 &&
            strcmp(k0b, k0) != 0)
            break;
    }
    pick_key_for_worker(1, 2, k1, sizeof(k1));
    for (int i = 0;; i++) {
        snprintf(s0, sizeof(s0), "set-a:%d", i);
        if ((int)(hash_slot(s0, strlen(s0)) % 2u) == 0 &&
            strcmp(s0, k0) != 0 && strcmp(s0, k0b) != 0)
            break;
    }
    for (int i = 0;; i++) {
        snprintf(s0b, sizeof(s0b), "set-b:%d", i);
        if ((int)(hash_slot(s0b, strlen(s0b)) % 2u) == 0 &&
            strcmp(s0b, s0) != 0 && strcmp(s0b, k0) != 0 &&
            strcmp(s0b, k0b) != 0)
            break;
    }

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));

    /* MSETEX routes by key positions, while ignoring values/options. */
    snprintf(req, sizeof(req),
             "*8\r\n$6\r\nMSETEX\r\n$1\r\n2\r\n$%zu\r\n%s\r\n$2\r\nv0\r\n"
             "$%zu\r\n%s\r\n$2\r\nv1\r\n$2\r\nEX\r\n$2\r\n10\r\n",
             strlen(k0), k0, strlen(k0b), k0b);
    roundtrip(a, req, ":1\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k0b), k0b);
    roundtrip(b, req, "$2\r\nv1\r\n");

    snprintf(req, sizeof(req),
             "*6\r\n$6\r\nMSETEX\r\n$1\r\n2\r\n$%zu\r\n%s\r\n$1\r\nx\r\n"
             "$%zu\r\n%s\r\n$1\r\ny\r\n",
             strlen(k0), k0, strlen(k1), k1);
    roundtrip(a, req,
              "-CROSSSLOT Keys in request don't hash to the same slot\r\n");

    /* Set-cardinality commands use their declared numkeys set. */
    snprintf(req, sizeof(req), "*4\r\n$4\r\nSADD\r\n$%zu\r\n%s\r\n$1\r\nx\r\n$1\r\ny\r\n",
             strlen(s0), s0);
    roundtrip(a, req, ":2\r\n");
    snprintf(req, sizeof(req), "*4\r\n$4\r\nSADD\r\n$%zu\r\n%s\r\n$1\r\ny\r\n$1\r\nz\r\n",
             strlen(s0b), s0b);
    roundtrip(a, req, ":2\r\n");
    snprintf(req, sizeof(req), "*4\r\n$10\r\nSUNIONCARD\r\n$1\r\n2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
             strlen(s0), s0, strlen(s0b), s0b);
    roundtrip(b, req, ":3\r\n");
    snprintf(req, sizeof(req), "*4\r\n$9\r\nSDIFFCARD\r\n$1\r\n2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
             strlen(s0), s0, strlen(s0b), s0b);
    roundtrip(b, req, ":1\r\n");

    /* Script commands with declared keys must execute on that key owner. */
    {
        static const char script[] =
            "return redis.call('SET', KEYS[1], ARGV[1])";
        snprintf(req, sizeof(req),
             "*5\r\n$4\r\nEVAL\r\n$%zu\r\n%s\r\n"
             "$1\r\n1\r\n$%zu\r\n%s\r\n$5\r\nvalue\r\n",
             strlen(script), script, strlen(k1), k1);
    }
    roundtrip(a, req, "$2\r\nOK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(b, req, "$5\r\nvalue\r\n");

    snprintf(req, sizeof(req), "*4\r\n$10\r\nSUNIONCARD\r\n$1\r\n2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
             strlen(s0), s0, strlen(k1), k1);
    roundtrip(a, req,
              "-CROSSSLOT Keys in request don't hash to the same slot\r\n");

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_pipeline_mixed_targets_keeps_order(void)
{
    mt_server *ms;
    pal_socket_t a;
    char k0[32], k1[32];
    char req[512];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms)); /* -> worker 0 */

    /* Mix routed and local commands in one pipeline; replies must arrive in
     * request order even though worker-1 commands cross threads. */
    snprintf(req, sizeof(req),
             "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n"
             "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\ny\r\n"
             "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n"
             "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n"
             "*1\r\n$4\r\nPING\r\n",
             strlen(k1), k1, strlen(k0), k0, strlen(k1), k1, strlen(k0),
             k0);
    roundtrip(a, req, "+OK\r\n+OK\r\n$1\r\nx\r\n$1\r\ny\r\n+PONG\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_blocked_commands_in_mt_mode(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    int i;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));

    roundtrip(a, "*2\r\n$10\r\nPSUBSCRIBE\r\n$6\r\nnews.*\r\n",
              "*3\r\n$10\r\npsubscribe\r\n$6\r\nnews.*\r\n:1\r\n");
    roundtrip(b, "*3\r\n$7\r\nPUBLISH\r\n$9\r\nnews.tech\r\n$5\r\nhello\r\n",
              ":1\r\n");
    roundtrip(a, "*1\r\n$12\r\nPUNSUBSCRIBE\r\n",
              "*4\r\n$8\r\npmessage\r\n$6\r\nnews.*\r\n$9\r\nnews.tech\r\n$5\r\nhello\r\n"
              "*3\r\n$12\r\npunsubscribe\r\n$6\r\nnews.*\r\n:0\r\n");
    roundtrip(a, "*1\r\n$6\r\nASKING\r\n", "+OK\r\n");
    /* The session still works for normal commands afterwards. */
    roundtrip(a, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");

    /* SHUTDOWN is a no-reply command; the coordinated pool stops after the
     * home worker executes the existing server shutdown hook. */
    send_raw(a, "*1\r\n$8\r\nSHUTDOWN\r\n");
    for (i = 0; i < 100 && mt_server_test_running(ms); i++)
        pal_sleep_ms(5);
    DD_CHECK_EQ_INT(0, mt_server_test_running(ms));

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_blocking_pop_cross_worker(void)
{
    mt_server *ms;
    pal_socket_t blocked, producer;
    char key[32], req[256], buf[128];
    const char *reply = "*2\r\n$%zu\r\n%s\r\n$5\r\nvalue\r\n";
    size_t n;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, key, sizeof(key));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    blocked = connect_client(mt_server_port(ms));
    producer = connect_client(mt_server_port(ms));

    snprintf(req, sizeof(req), "*3\r\n$5\r\nBLPOP\r\n$%zu\r\n%s\r\n$1\r\n0\r\n",
             strlen(key), key);
    send_raw(blocked, req);
    /* A zero-timeout BLPOP must remain pending until the owner receives data. */
    DD_CHECK_EQ_INT(0, (long long)recv_deadline(blocked, buf, sizeof(buf), 150));

    snprintf(req, sizeof(req), "*3\r\n$5\r\nLPUSH\r\n$%zu\r\n%s\r\n$5\r\nvalue\r\n",
             strlen(key), key);
    roundtrip(producer, req, ":1\r\n");
    n = recv_deadline(blocked, buf, sizeof(buf), 3000);
    DD_CHECK(n > 0);
    if (n > 0) {
        char expected[128];
        snprintf(expected, sizeof(expected), reply, strlen(key), key);
        DD_CHECK_MEM(expected, strlen(expected), buf, n);
    }
    pal_close(blocked);
    pal_close(producer);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_blocking_pop_timeout_and_crossslot(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char k0[32], k1[32], req[256], buf[256];
    size_t n;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));

    snprintf(req, sizeof(req),
             "*4\r\n$5\r\nBLPOP\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$4\r\n0.05\r\n",
             strlen(k0), k0, strlen(k1), k1);
    roundtrip(a, req,
              "-CROSSSLOT Keys in request don't hash to the same slot\r\n");

    snprintf(req, sizeof(req), "*3\r\n$5\r\nBLPOP\r\n$%zu\r\n%s\r\n$4\r\n0.05\r\n",
             strlen(k1), k1);
    send_raw(a, req);
    n = recv_deadline(a, buf, sizeof(buf), 1000);
    DD_CHECK(n > 0);
    if (n > 0)
        DD_CHECK_MEM("$-1\r\n", 5, buf, n);

    /* A waiter that disconnects must be removed from the owner registry. */
    snprintf(req, sizeof(req), "*3\r\n$5\r\nBLPOP\r\n$%zu\r\n%s\r\n$1\r\n0\r\n",
             strlen(k0), k0);
    send_raw(b, req);
    DD_CHECK_EQ_INT(0, (long long)recv_deadline(b, buf, sizeof(buf), 100));
    pal_close(b);
    pal_sleep_ms(100);
    snprintf(req, sizeof(req), "*3\r\n$5\r\nLPUSH\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(k0), k0);
    roundtrip(a, req, ":1\r\n");

    /* A blocked waiter cannot be embedded in a sessionless EXEC replay. */
    roundtrip(a, "*1\r\n$5\r\nMULTI\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$5\r\nBLPOP\r\n$%zu\r\n%s\r\n$1\r\n0\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+QUEUED\r\n");
    roundtrip(a, "*1\r\n$4\r\nEXEC\r\n",
              "-EXECABORT Transaction discarded because of: blocking command "
              "cannot run in mt transaction\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_migrate_supported_on_single_mt_worker(void)
{
    mt_server *ms;
    pal_socket_t a;
    char req[256];
    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 1);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    roundtrip(a, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n",
              "+OK\r\n");
    snprintf(req, sizeof(req),
             "*6\r\n$7\r\nMIGRATE\r\n$9\r\n127.0.0.1\r\n$1\r\n1\r\n$1\r\nk\r\n$1\r\n5\r\n$4\r\n1000\r\n");
    roundtrip(a, req, "-ERR DB index is out of range\r\n");
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

typedef struct mt_target_runner {
    server *srv;
    volatile int running;
    pal_thread thread;
} mt_target_runner;

static void *mt_target_runner_main(void *arg)
{
    mt_target_runner *r = (mt_target_runner *)arg;
    while (r->running)
        (void)server_run_once(r->srv, 5);
    return NULL;
}

static size_t mt_target_request(pal_socket_t c, const char *req, char *buf,
                                size_t cap)
{
    size_t sent = 0, got = 0;
    uint64_t deadline = pal_now_ms() + 5000;
    while (sent < strlen(req) && pal_now_ms() < deadline) {
        ptrdiff_t n = pal_send(c, req + sent, strlen(req) - sent);
        if (n > 0)
            sent += (size_t)n;
        else
            pal_sleep_ms(1);
    }
    while (got < cap - 1 && pal_now_ms() < deadline) {
        ptrdiff_t n = pal_recv(c, buf + got, cap - 1 - got);
        if (n > 0) {
            got += (size_t)n;
            if (got >= 2 && memcmp(buf + got - 2, "\r\n", 2) == 0)
                break;
        } else {
            pal_sleep_ms(1);
        }
    }
    buf[got] = '\0';
    return got;
}

static void test_migrate_cross_worker_external_target(void)
{
    mt_server *ms;
    server *target;
    mt_target_runner runner;
    pal_socket_t src, dst;
    char key[32], port[16], req[512], buf[256];
    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, key, sizeof(key));
    target = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(target != NULL);
    runner.srv = target;
    runner.running = 1;
    DD_CHECK_EQ_INT(0, pal_thread_create(&runner.thread,
                                         mt_target_runner_main, &runner));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    src = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$5\r\nvalue\r\n",
             strlen(key), key);
    roundtrip(src, req, "+OK\r\n");
    snprintf(port, sizeof(port), "%u", (unsigned)server_port(target));
    snprintf(req, sizeof(req),
             "*6\r\n$7\r\nMIGRATE\r\n$9\r\n127.0.0.1\r\n$%zu\r\n%s\r\n"
             "$%zu\r\n%s\r\n$1\r\n0\r\n$4\r\n1000\r\n",
             strlen(port), port, strlen(key), key);
    roundtrip(src, req, "+OK\r\n");
    dst = connect_client(server_port(target));
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(key), key);
    DD_CHECK(mt_target_request(dst, req, buf, sizeof(buf)) > 0);
    DD_CHECK(strstr(buf, "$5\r\nvalue\r\n") != NULL);
    snprintf(req, sizeof(req), "*2\r\n$6\r\nEXISTS\r\n$%zu\r\n%s\r\n",
             strlen(key), key);
    roundtrip(src, req, ":0\r\n");
    pal_close(src);
    pal_close(dst);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    runner.running = 0;
    (void)pal_thread_join(&runner.thread, NULL);
    server_destroy(target);
    pal_socket_cleanup();
}

static void test_randomkey_aggregates_workers(void)
{
    mt_server *ms;
    pal_socket_t a;
    char k0[32], k1[32], req[256], reply[128];
    size_t n;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));

    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv0\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv1\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+OK\r\n");

    snprintf(req, sizeof(req), "*1\r\n$9\r\nRANDOMKEY\r\n");
    n = request_full(a, req, reply, sizeof(reply));
    DD_CHECK(n > 4);
    DD_CHECK(reply[0] == '$');
    DD_CHECK(strstr(reply, k0) != NULL || strstr(reply, k1) != NULL);

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_keys_aggregates_workers(void)
{
    mt_server *ms;
    pal_socket_t a;
    char k0[32], k1[32], req[256], reply[256];
    size_t n;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));

    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv0\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv1\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+OK\r\n");

    snprintf(req, sizeof(req), "*2\r\n$4\r\nKEYS\r\n$1\r\n*\r\n");
    n = request_full(a, req, reply, sizeof(reply));
    DD_CHECK(n > 4);
    DD_CHECK(reply[0] == '*');
    DD_CHECK(strstr(reply, k0) != NULL);
    DD_CHECK(strstr(reply, k1) != NULL);

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_scan_composite_cursor_across_workers(void)
{
    mt_server *ms;
    pal_socket_t a;
    char k0[32], k1[32], req[256], reply[512];
    unsigned long long cursor = 0;
    int seen0 = 0, seen1 = 0, rounds;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));

    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv0\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv1\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+OK\r\n");

    for (rounds = 0; rounds < 32; rounds++) {
        int n;
        char cursor_text[32];
        char *eol;
        char *cursor_start;
        snprintf(cursor_text, sizeof(cursor_text), "%llu", cursor);
        n = snprintf(req, sizeof(req),
                     "*4\r\n$4\r\nSCAN\r\n$%zu\r\n%llu\r\n$5\r\nCOUNT\r\n$1\r\n1\r\n",
                     strlen(cursor_text), cursor);
        DD_CHECK(n > 0 && (size_t)n < sizeof(req));
        n = (int)request_full(a, req, reply, sizeof(reply));
        DD_CHECK(n > 8 && reply[0] == '*');
        eol = strstr(reply, "\r\n");
        DD_CHECK(eol != NULL);
        if (eol == NULL)
            break;
        cursor_start = eol + 2;
        eol = strstr(cursor_start, "\r\n");
        DD_CHECK(eol != NULL);
        if (eol == NULL)
            break;
        cursor_start = eol + 2;
        eol = strstr(cursor_start, "\r\n");
        DD_CHECK(eol != NULL);
        if (eol == NULL)
            break;
        cursor = strtoull(cursor_start, NULL, 10);
        if (strstr(reply, k0) != NULL)
            seen0 = 1;
        if (strstr(reply, k1) != NULL)
            seen1 = 1;
        if (cursor == 0)
            break;
    }
    DD_CHECK(seen0 && seen1);
    DD_CHECK_EQ_INT(0, (long long)cursor);

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_cluster_control_plane_mt(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char nid[41];
    char req[128];
    char expected[128];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    cluster_gen_id(nid);
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0,
                    mt_server_enable_cluster(ms, nid, "", "127.0.0.1"));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));

    snprintf(req, sizeof(req), "*2\r\n$7\r\nCLUSTER\r\n$4\r\nMYID\r\n");
    snprintf(expected, sizeof(expected), "$40\r\n%s\r\n", nid);
    roundtrip(a, req, expected);
    roundtrip(b, req, expected);

    /* Slot-wide destructive commands must not run on one shard only. */
    roundtrip(a, "*3\r\n$6\r\nSFLUSH\r\n$1\r\n0\r\n$2\r\n10\r\n",
              "-ERR command not supported in mt mode\r\n");
    roundtrip(a, "*5\r\n$9\r\nTRIMSLOTS\r\n$6\r\nRANGES\r\n$1\r\n1\r\n$1\r\n0\r\n$2\r\n10\r\n",
              "-ERR command not supported in mt mode\r\n");

    /* REPLICAOF NO ONE is now handled by the worker-0 control plane. */
    roundtrip(a, "*3\r\n$9\r\nREPLICAOF\r\n$2\r\nNO\r\n$3\r\nONE\r\n",
              "+OK\r\n");

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_himport_cross_worker_fails_closed(void)
{
    mt_server *ms;
    pal_socket_t a;
    char key[32], local[32], req[256];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, key, sizeof(key));
    pick_key_for_worker(0, 2, local, sizeof(local));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));

    /* Fieldsets are connection-local and cannot be replayed sessionlessly. */
    roundtrip(a, "*4\r\n$7\r\nHIMPORT\r\n$7\r\nPREPARE\r\n$2\r\nfs\r\n$1\r\nf\r\n",
              "+OK\r\n");
    roundtrip(a, "*4\r\n$7\r\nHIMPORT\r\n$7\r\nPREPARE\r\n$3\r\nloc\r\n$1\r\nf\r\n",
              "+OK\r\n");
    snprintf(req, sizeof(req),
             "*5\r\n$7\r\nHIMPORT\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$3\r\nloc\r\n$1\r\nv\r\n",
             strlen(local), local);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req),
             "*5\r\n$7\r\nHIMPORT\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nfs\r\n$1\r\nv\r\n",
             strlen(key), key);
    roundtrip(a, req, "-ERR command not supported in mt mode\r\n");

    /* No partial write on the home shard or the key owner. */
    snprintf(req, sizeof(req), "*2\r\n$6\r\nEXISTS\r\n$%zu\r\n%s\r\n",
             strlen(key), key);
    roundtrip(a, req, ":0\r\n");

    /* MEMORY USAGE is key-scoped and must follow the owning worker. */
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(key), key);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$6\r\nMEMORY\r\n$5\r\nUSAGE\r\n$%zu\r\n%s\r\n",
             strlen(key), key);
    {
        char reply[128];
        size_t got = request_full(a, req, reply, sizeof(reply));
        DD_CHECK(got > 3 && reply[0] == ':');
    }

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_sort_store_cross_worker_fails_closed(void)
{
    mt_server *ms;
    pal_socket_t a;
    char src[32], dst[32], req[320];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, src, sizeof(src));
    pick_key_for_worker(1, 2, dst, sizeof(dst));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*4\r\n$5\r\nRPUSH\r\n$%zu\r\n%s\r\n$1\r\n2\r\n$1\r\n1\r\n",
             strlen(src), src);
    roundtrip(a, req, ":2\r\n");
    snprintf(req, sizeof(req), "*4\r\n$4\r\nSORT\r\n$%zu\r\n%s\r\n$5\r\nSTORE\r\n$%zu\r\n%s\r\n",
             strlen(src), src, strlen(dst), dst);
    roundtrip(a, req, "-CROSSSLOT Keys in request don't hash to the same slot\r\n");
    snprintf(req, sizeof(req), "*2\r\n$6\r\nEXISTS\r\n$%zu\r\n%s\r\n",
             strlen(dst), dst);
    roundtrip(a, req, ":0\r\n");
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_script_cache_broadcast_reaches_key_owner(void)
{
    mt_server *ms;
    pal_socket_t a;
    char key[32], req[512], sha[64], reply[128];
    size_t got;
    static const char script[] =
        "return redis.call('SET', KEYS[1], ARGV[1])";

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, key, sizeof(key));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));

    snprintf(req, sizeof(req), "*3\r\n$6\r\nSCRIPT\r\n$4\r\nLOAD\r\n$%zu\r\n%s\r\n",
             strlen(script), script);
    got = request_full(a, req, reply, sizeof(reply));
    DD_CHECK(got == 47 && reply[0] == '$');
    if (got >= 47) {
        memcpy(sha, reply + 5, 40);
        sha[40] = '\0';
    } else {
        sha[0] = '\0';
    }

    snprintf(req, sizeof(req),
             "*5\r\n$7\r\nEVALSHA\r\n$40\r\n%s\r\n$1\r\n1\r\n$%zu\r\n%s\r\n$5\r\nvalue\r\n",
             sha, strlen(key), key);
    roundtrip(a, req, "$2\r\nOK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(key), key);
    roundtrip(a, req, "$5\r\nvalue\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_config_mutations_broadcast_to_workers(void)
{
    mt_server *ms;
    pal_socket_t a, b;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));

    roundtrip(a, "*4\r\n$6\r\nCONFIG\r\n$3\r\nSET\r\n$9\r\nmaxmemory\r\n$5\r\n12345\r\n",
              "+OK\r\n");
    roundtrip(b, "*3\r\n$6\r\nCONFIG\r\n$3\r\nGET\r\n$9\r\nmaxmemory\r\n",
              "*2\r\n$9\r\nmaxmemory\r\n$5\r\n12345\r\n");

    roundtrip(a, "*4\r\n$6\r\nCONFIG\r\n$3\r\nSET\r\n$11\r\nappendfsync\r\n$6\r\nalways\r\n",
              "+OK\r\n");
    roundtrip(b, "*3\r\n$6\r\nCONFIG\r\n$3\r\nGET\r\n$11\r\nappendfsync\r\n",
              "*2\r\n$11\r\nappendfsync\r\n$6\r\nalways\r\n");

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_save_covers_all_selected_databases(void)
{
    mt_server *ms;
    pal_socket_t a;
    char req[192];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    (void)pal_file_unlink("./worker-0-mtdbsave.ddr");
    (void)pal_file_unlink("./worker-1-mtdbsave.ddr");
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_enable_snapshots(ms, ".", "mtdbsave.ddr", 0));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    roundtrip(a, "*2\r\n$6\r\nSELECT\r\n$1\r\n1\r\n", "+OK\r\n");
    roundtrip(a, "*3\r\n$3\r\nSET\r\n$2\r\nd1\r\n$1\r\nv\r\n", "+OK\r\n");
    roundtrip(a, "*1\r\n$4\r\nSAVE\r\n", "+OK\r\n");
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_enable_snapshots(ms, ".", "mtdbsave.ddr", 0));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    roundtrip(a, "*2\r\n$6\r\nSELECT\r\n$1\r\n1\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$2\r\nd1\r\n");
    roundtrip(a, req, "$1\r\nv\r\n");
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    (void)pal_file_unlink("./worker-0-mtdbsave.ddr");
    (void)pal_file_unlink("./worker-1-mtdbsave.ddr");
    pal_socket_cleanup();
}

static void test_bgsave_covers_all_selected_databases(void)
{
    mt_server *ms;
    pal_socket_t a;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    (void)pal_file_unlink("./worker-0-mtdbsavebg.ddr");
    (void)pal_file_unlink("./worker-1-mtdbsavebg.ddr");
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_enable_snapshots(ms, ".", "mtdbsavebg.ddr", 0));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    roundtrip(a, "*2\r\n$6\r\nSELECT\r\n$1\r\n1\r\n", "+OK\r\n");
    roundtrip(a, "*3\r\n$3\r\nSET\r\n$2\r\nbg\r\n$1\r\nv\r\n", "+OK\r\n");
    roundtrip(a, "*1\r\n$6\r\nBGSAVE\r\n", "+Background saving started\r\n");
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_enable_snapshots(ms, ".", "mtdbsavebg.ddr", 0));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    roundtrip(a, "*2\r\n$6\r\nSELECT\r\n$1\r\n1\r\n", "+OK\r\n");
    roundtrip(a, "*2\r\n$3\r\nGET\r\n$2\r\nbg\r\n", "$1\r\nv\r\n");
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    (void)pal_file_unlink("./worker-0-mtdbsavebg.ddr");
    (void)pal_file_unlink("./worker-1-mtdbsavebg.ddr");
    pal_socket_cleanup();
}

static void test_bgrewriteaof_is_broadcast_to_workers(void)
{
    mt_server *ms;
    pal_socket_t a;
    uint64_t before;
    uint64_t deadline;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    before = mt_server_tasks_executed(ms);
    roundtrip(a, "*1\r\n$12\r\nBGREWRITEAOF\r\n",
              "+Background append only file rewriting started\r\n");
    deadline = pal_now_ms() + 2000;
    while (mt_server_tasks_executed(ms) < before + 1 &&
           pal_now_ms() < deadline)
        pal_sleep_ms(1);
    DD_CHECK(mt_server_tasks_executed(ms) >= before + 1);
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_client_list_covers_all_workers(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char reply[4096];
    size_t n, ids = 0, i;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));
    (void)b;
    n = request_full(a, "*2\r\n$6\r\nCLIENT\r\n$4\r\nLIST\r\n",
                     reply, sizeof(reply));
    DD_CHECK(n > 0);
    for (i = 0; i + 3 < n; i++)
        if (memcmp(reply + i, "id=", 3) == 0)
            ids++;
    DD_CHECK(ids >= 2);
    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_hotkeys_control_is_broadcast(void)
{
    mt_server *ms;
    pal_socket_t a;
    char reply[2048];
    size_t n;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    roundtrip(a, "*5\r\n$7\r\nHOTKEYS\r\n$5\r\nSTART\r\n$7\r\nMETRICS\r\n$1\r\n1\r\n$3\r\nCPU\r\n",
              "+OK\r\n");
    n = request_full(a, "*2\r\n$7\r\nHOTKEYS\r\n$3\r\nGET\r\n",
                     reply, sizeof(reply));
    DD_CHECK(n > 0 && strstr(reply, "active") != NULL);
    roundtrip(a, "*2\r\n$7\r\nHOTKEYS\r\n$4\r\nSTOP\r\n", "+OK\r\n");
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void pick_key_for_slot(int wanted, char *out, size_t cap)
{
    int i;
    for (i = 0;; i++) {
        snprintf(out, cap, "slotkey:%d", i);
        if ((int)hash_slot(out, strlen(out)) == wanted)
            return;
    }
}

static void test_cluster_state_propagates_to_workers(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char nid[41];
    char k0[32], k1[32];
    char req[160];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    cluster_gen_id(nid);
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0,
                    mt_server_enable_cluster(ms, nid, "", "127.0.0.1"));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));

    roundtrip(a, "*3\r\n$7\r\nCLUSTER\r\n$8\r\nADDSLOTS\r\n$1\r\n0\r\n",
              "+OK\r\n");
    /* Fire-and-forget metadata fan-out has a very short propagation window;
     * poll CLUSTER INFO on the second worker until it observes the slot. */
    {
        uint64_t deadline = pal_now_ms() + 5000;
        char info[512];
        for (;;) {
            size_t got = request_full(
                b, "*2\r\n$7\r\nCLUSTER\r\n$4\r\nINFO\r\n", info,
                sizeof(info));
            if (got > 0 && strstr(info, "cluster_slots_assigned:1") != NULL)
                break;
            if (pal_now_ms() >= deadline) {
                fprintf(stderr, "cluster state did not propagate: %.*s\n",
                        (int)got, info);
                DD_CHECK(0);
            }
            pal_sleep_ms(10);
        }
    }

    pick_key_for_slot(0, k0, sizeof(k0));
    pick_key_for_slot(1, k1, sizeof(k1));
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "-CLUSTERDOWN Hash slot not served\r\n");
    roundtrip(b, req, "-CLUSTERDOWN Hash slot not served\r\n");

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void wait_for_bulk(pal_socket_t c, const char *key,
                          const char *expected)
{
    char req[128];
    char buf[512];
    size_t elen = strlen(expected);
    uint64_t deadline = pal_now_ms() + 5000;

    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(key), key);
    while (pal_now_ms() < deadline) {
        size_t got = request_full(c, req, buf, sizeof(buf));
        if (got == elen && memcmp(buf, expected, elen) == 0)
            return;
        pal_sleep_ms(10);
    }
    fprintf(stderr, "timed out waiting for GET %s = %s; last reply: %.*s\n",
            key, expected, (int)strlen(buf), buf);
    DD_CHECK(0);
}

static void wait_for_mt_repl_synced(const mt_server *ms)
{
    uint64_t deadline = pal_now_ms() + 30000;

    while (pal_now_ms() < deadline) {
        if (mt_server_repl_synced(ms))
            return;
        pal_sleep_ms(10);
    }
    fprintf(stderr, "timed out waiting for mt replica full sync\n");
    DD_CHECK(0);
}

static void test_mt_replica_partitions_full_sync(void)
{
    server *master;
    server_thread_ctx mt_ctx;
    pal_thread master_thread;
    mt_server *ms;
    pal_socket_t mc, c;
    char k0[32], k1[32];
    char req[256];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    master = server_create("127.0.0.1", 0);
    DD_CHECK(master != NULL);
    mt_ctx.srv = master;
    mt_ctx.running = 1;
    DD_CHECK_EQ_INT(0,
                    pal_thread_create(&master_thread, server_thread_main,
                                      &mt_ctx));

    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));
    mc = connect_client(server_port(master));
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv0\r\n",
             strlen(k0), k0);
    roundtrip(mc, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv1\r\n",
             strlen(k1), k1);
    roundtrip(mc, req, "+OK\r\n");
    pal_close(mc);

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0,
                    mt_server_replicaof(ms, "127.0.0.1",
                                        server_port(master)));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    wait_for_mt_repl_synced(ms);

    c = connect_client(mt_server_port(ms));
    wait_for_bulk(c, k0, "$2\r\nv0\r\n");
    wait_for_bulk(c, k1, "$2\r\nv1\r\n");

    /* Post-sync stream is routed to the owning worker exactly like client
     * traffic. */
    mc = connect_client(server_port(master));
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv2\r\n",
             strlen(k0), k0);
    roundtrip(mc, req, "+OK\r\n");
    pal_close(mc);
    wait_for_bulk(c, k0, "$2\r\nv2\r\n");
    pal_close(c);

    mt_server_stop(ms);
    mt_server_destroy(ms);
    mt_ctx.running = 0;
    pal_thread_join(&master_thread, NULL);
    server_destroy(master);
    pal_socket_cleanup();
}

static void test_mt_master_serves_replica_full_sync(void)
{
    mt_server *ms;
    server *replica;
    server_thread_ctx rt;
    pal_thread replica_thread;
    pal_socket_t mc, c;
    char k0[32], k1[32];
    char req[256];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));

    mc = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv0\r\n",
             strlen(k0), k0);
    roundtrip(mc, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv1\r\n",
             strlen(k1), k1);
    roundtrip(mc, req, "+OK\r\n");
    pal_close(mc);

    replica = server_create("127.0.0.1", 0);
    DD_CHECK(replica != NULL);
    rt.srv = replica;
    rt.running = 1;
    DD_CHECK_EQ_INT(0,
                    pal_thread_create(&replica_thread, server_thread_main,
                                      &rt));
    DD_CHECK_EQ_INT(0,
                    server_replicaof(replica, "127.0.0.1",
                                     mt_server_port(ms)));

    pal_sleep_ms(200);
    c = connect_client(server_port(replica));
    wait_for_bulk(c, k0, "$2\r\nv0\r\n");
    wait_for_bulk(c, k1, "$2\r\nv1\r\n");

    mc = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv2\r\n",
             strlen(k0), k0);
    roundtrip(mc, req, "+OK\r\n");
    pal_close(mc);
    wait_for_bulk(c, k0, "$2\r\nv2\r\n");
    pal_close(c);

    mt_server_stop(ms);
    mt_server_destroy(ms);
    rt.running = 0;
    pal_thread_join(&replica_thread, NULL);
    server_destroy(replica);
    pal_socket_cleanup();
}

static void test_mt_replication_forwards_mutations(void)
{
    mt_server *ms;
    server *replica;
    server_thread_ctx rt;
    pal_thread replica_thread;
    pal_socket_t mc, c;
    char k0[32], k1[32];
    char req[256];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));

    replica = server_create("127.0.0.1", 0);
    DD_CHECK(replica != NULL);
    rt.srv = replica;
    rt.running = 1;
    DD_CHECK_EQ_INT(0,
                    pal_thread_create(&replica_thread, server_thread_main,
                                      &rt));
    DD_CHECK_EQ_INT(0,
                    server_replicaof(replica, "127.0.0.1",
                                     mt_server_port(ms)));
    pal_sleep_ms(200);

    mc = connect_client(mt_server_port(ms));
    c = connect_client(server_port(replica));

    /* Baseline: routed SETs on both workers reach the replica. */
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv0\r\n",
             strlen(k0), k0);
    roundtrip(mc, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv1\r\n",
             strlen(k1), k1);
    roundtrip(mc, req, "+OK\r\n");
    wait_for_bulk(c, k0, "$2\r\nv0\r\n");
    wait_for_bulk(c, k1, "$2\r\nv1\r\n");

    /* MULTI/EXEC is applied as individual replicated commands. */
    roundtrip(mc, "*1\r\n$5\r\nMULTI\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv2\r\n",
             strlen(k1), k1);
    roundtrip(mc, req, "+QUEUED\r\n");
    roundtrip(mc, "*1\r\n$4\r\nEXEC\r\n", "*1\r\n+OK\r\n");
    wait_for_bulk(c, k1, "$2\r\nv2\r\n");

    /* MOVE to db 1 must replicate. */
    snprintf(req, sizeof(req), "*3\r\n$4\r\nMOVE\r\n$%zu\r\n%s\r\n$1\r\n1\r\n",
             strlen(k1), k1);
    roundtrip(mc, req, ":1\r\n");
    wait_for_bulk(c, k1, "$-1\r\n");
    roundtrip(c, "*2\r\n$6\r\nSELECT\r\n$1\r\n1\r\n", "+OK\r\n");
    wait_for_bulk(c, k1, "$2\r\nv2\r\n");
    roundtrip(c, "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n", "+OK\r\n");

    /* SWAPDB is broadcast on the master but must be replicated once. */
    roundtrip(mc, "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nd0\r\n",
             strlen(k0), k0);
    roundtrip(mc, req, "+OK\r\n");
    roundtrip(mc, "*2\r\n$6\r\nSELECT\r\n$1\r\n1\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nd1\r\n",
             strlen(k1), k1);
    roundtrip(mc, req, "+OK\r\n");
    roundtrip(mc, "*3\r\n$6\r\nSWAPDB\r\n$1\r\n0\r\n$1\r\n1\r\n", "+OK\r\n");
    wait_for_bulk(c, k0, "$-1\r\n");
    wait_for_bulk(c, k1, "$2\r\nd1\r\n");
    roundtrip(c, "*2\r\n$6\r\nSELECT\r\n$1\r\n1\r\n", "+OK\r\n");
    wait_for_bulk(c, k1, "$-1\r\n");
    wait_for_bulk(c, k0, "$2\r\nd0\r\n");
    roundtrip(c, "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n", "+OK\r\n");

    /* FLUSHALL clears every db on the replica. */
    roundtrip(mc, "*1\r\n$8\r\nFLUSHALL\r\n", "+OK\r\n");
    wait_for_bulk(c, k1, "$-1\r\n");
    roundtrip(c, "*2\r\n$6\r\nSELECT\r\n$1\r\n1\r\n", "+OK\r\n");
    wait_for_bulk(c, k0, "$-1\r\n");
    roundtrip(c, "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n", "+OK\r\n");
    roundtrip(mc, "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n", "+OK\r\n");

    /* FLUSHDB flushes only the selected db. */
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nfd\r\n",
             strlen(k0), k0);
    roundtrip(mc, req, "+OK\r\n");
    wait_for_bulk(c, k0, "$2\r\nfd\r\n");
    roundtrip(mc, "*1\r\n$7\r\nFLUSHDB\r\n", "+OK\r\n");
    wait_for_bulk(c, k0, "$-1\r\n");

    pal_close(mc);
    pal_close(c);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    rt.running = 0;
    pal_thread_join(&replica_thread, NULL);
    server_destroy(replica);
    pal_socket_cleanup();
}

static void test_mt_info_replication(void)
{
    mt_server *ms;
    server *replica;
    server_thread_ctx rt;
    pal_thread replica_thread;
    pal_socket_t c;
    char info[65536];
    size_t got;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));

    replica = server_create("127.0.0.1", 0);
    DD_CHECK(replica != NULL);
    rt.srv = replica;
    rt.running = 1;
    DD_CHECK_EQ_INT(0,
                    pal_thread_create(&replica_thread, server_thread_main,
                                      &rt));
    DD_CHECK_EQ_INT(0,
                    server_replicaof(replica, "127.0.0.1",
                                     mt_server_port(ms)));
    pal_sleep_ms(200);

    c = connect_client(mt_server_port(ms));
    got = request_full(c, "*1\r\n$4\r\nINFO\r\n", info, sizeof(info));
    DD_CHECK(got > 0);
    info[got] = '\0';
    DD_CHECK(strstr(info, "role:master\r\n") != NULL);
    DD_CHECK(strstr(info, "connected_slaves:1\r\n") != NULL);
    pal_close(c);

    mt_server_stop(ms);
    mt_server_destroy(ms);
    rt.running = 0;
    pal_thread_join(&replica_thread, NULL);
    server_destroy(replica);
    pal_socket_cleanup();
}

static void test_mt_swapdb_replicates_once_three_workers(void)
{
    mt_server *ms;
    server *replica;
    server_thread_ctx rt;
    pal_thread replica_thread;
    pal_socket_t mc, c;
    char k0[32], k1[32];
    char req[256];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 3, k0, sizeof(k0));
    pick_key_for_worker(1, 3, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 3);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));

    replica = server_create("127.0.0.1", 0);
    DD_CHECK(replica != NULL);
    rt.srv = replica;
    rt.running = 1;
    DD_CHECK_EQ_INT(0,
                    pal_thread_create(&replica_thread, server_thread_main,
                                      &rt));
    DD_CHECK_EQ_INT(0,
                    server_replicaof(replica, "127.0.0.1",
                                     mt_server_port(ms)));
    pal_sleep_ms(200);

    mc = connect_client(mt_server_port(ms));
    c = connect_client(server_port(replica));

    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nd0\r\n",
             strlen(k0), k0);
    roundtrip(mc, req, "+OK\r\n");
    roundtrip(mc, "*2\r\n$6\r\nSELECT\r\n$1\r\n1\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nd1\r\n",
             strlen(k1), k1);
    roundtrip(mc, req, "+OK\r\n");
    roundtrip(mc, "*3\r\n$6\r\nSWAPDB\r\n$1\r\n0\r\n$1\r\n1\r\n", "+OK\r\n");

    wait_for_bulk(c, k0, "$-1\r\n");
    wait_for_bulk(c, k1, "$2\r\nd1\r\n");
    roundtrip(c, "*2\r\n$6\r\nSELECT\r\n$1\r\n1\r\n", "+OK\r\n");
    wait_for_bulk(c, k1, "$-1\r\n");
    wait_for_bulk(c, k0, "$2\r\nd0\r\n");

    pal_close(mc);
    pal_close(c);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    rt.running = 0;
    pal_thread_join(&replica_thread, NULL);
    server_destroy(replica);
    pal_socket_cleanup();
}

/* Find two distinct keys that both map to the given worker. */
static void pick_two_keys_for_worker(int wanted, int nworkers, char *out1,
                                     size_t cap1, char *out2, size_t cap2)
{
    int i;
    int found = 0;
    for (i = 0; found < 2; i++) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "mk:%d", i);
        if ((int)(hash_slot(tmp, strlen(tmp)) % (uint32_t)nworkers) !=
            wanted)
            continue;
        if (found == 0)
            snprintf(out1, cap1, "%s", tmp);
        else
            snprintf(out2, cap2, "%s", tmp);
        found++;
    }
}

static void test_multikey_same_worker(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char ka[32], kb[32];
    char req[256];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_two_keys_for_worker(0, 2, ka, sizeof(ka), kb, sizeof(kb));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms)); /* -> worker 0 */
    b = connect_client(mt_server_port(ms)); /* -> worker 1 */

    /* MSET with both keys on worker 0: local on a. */
    snprintf(req, sizeof(req),
             "*5\r\n$4\r\nMSET\r\n$%zu\r\n%s\r\n$2\r\nv1\r\n$%zu\r\n%s\r\n$2\r\nv2\r\n",
             strlen(ka), ka, strlen(kb), kb);
    roundtrip(a, req, "+OK\r\n");

    /* MGET from b: routed to worker 0 as one unit. */
    snprintf(req, sizeof(req), "*3\r\n$4\r\nMGET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
             strlen(ka), ka, strlen(kb), kb);
    roundtrip(b, req, "*2\r\n$2\r\nv1\r\n$2\r\nv2\r\n");

    /* DEL both keys from b (routed); EXISTS from a afterwards. */
    snprintf(req, sizeof(req), "*3\r\n$3\r\nDEL\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
             strlen(ka), ka, strlen(kb), kb);
    roundtrip(b, req, ":2\r\n");
    snprintf(req, sizeof(req), "*3\r\n$6\r\nEXISTS\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
             strlen(ka), ka, strlen(kb), kb);
    roundtrip(a, req, ":0\r\n");

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_multikey_crossslot_rejected(void)
{
    mt_server *ms;
    pal_socket_t a;
    char k0[32], k1[32];
    char req[256];
    const char *crossslot =
        "-CROSSSLOT Keys in request don't hash to the same slot\r\n";

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));

    snprintf(req, sizeof(req), "*3\r\n$4\r\nMGET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
             strlen(k0), k0, strlen(k1), k1);
    roundtrip(a, req, crossslot);

    snprintf(req, sizeof(req),
             "*5\r\n$4\r\nMSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n$%zu\r\n%s\r\n$1\r\ny\r\n",
             strlen(k0), k0, strlen(k1), k1);
    roundtrip(a, req, crossslot);

    snprintf(req, sizeof(req), "*3\r\n$3\r\nDEL\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
             strlen(k0), k0, strlen(k1), k1);
    roundtrip(a, req, crossslot);

    snprintf(req, sizeof(req), "*4\r\n$5\r\nSMOVE\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$1\r\nm\r\n",
             strlen(k0), k0, strlen(k1), k1);
    roundtrip(a, req, crossslot);

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_smove_same_worker(void)
{
    mt_server *ms;
    pal_socket_t a;
    char sa[32], sb[32];
    char req[256];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_two_keys_for_worker(1, 2, sa, sizeof(sa), sb, sizeof(sb));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms)); /* -> worker 0 */

    snprintf(req, sizeof(req), "*3\r\n$4\r\nSADD\r\n$%zu\r\n%s\r\n$1\r\nm\r\n",
             strlen(sa), sa);
    roundtrip(a, req, ":1\r\n");
    snprintf(req, sizeof(req), "*4\r\n$5\r\nSMOVE\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$1\r\nm\r\n",
             strlen(sa), sa, strlen(sb), sb);
    roundtrip(a, req, ":1\r\n");
    snprintf(req, sizeof(req), "*3\r\n$9\r\nSISMEMBER\r\n$%zu\r\n%s\r\n$1\r\nm\r\n",
             strlen(sb), sb);
    roundtrip(a, req, ":1\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_lmovem_same_worker(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char src[32], dst[32], req[256];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_two_keys_for_worker(1, 2, src, sizeof(src), dst, sizeof(dst));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*4\r\n$5\r\nRPUSH\r\n$%zu\r\n%s\r\n$1\r\na\r\n$1\r\nb\r\n",
             strlen(src), src);
    roundtrip(a, req, ":2\r\n");
    snprintf(req, sizeof(req), "*8\r\n$6\r\nLMOVEM\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$4\r\nLEFT\r\n$5\r\nRIGHT\r\n$5\r\nCOUNT\r\n$1\r\n1\r\n$4\r\nBULK\r\n",
             strlen(src), src, strlen(dst), dst);
    roundtrip(b, req, "*1\r\n$1\r\na\r\n");
    snprintf(req, sizeof(req), "*4\r\n$6\r\nLRANGE\r\n$%zu\r\n%s\r\n$1\r\n0\r\n$2\r\n-1\r\n",
             strlen(dst), dst);
    roundtrip(a, req, "*1\r\n$1\r\na\r\n");
    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_object_key_command_routes_to_owner(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char key[32], req[256];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, key, sizeof(key));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(key), key);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$6\r\nOBJECT\r\n$8\r\nENCODING\r\n$%zu\r\n%s\r\n",
             strlen(key), key);
    roundtrip(b, req, "$3\r\nraw\r\n");
    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_stream_extension_commands_route_to_owner(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char key[32], home_key[32], req[320];
    uint64_t tasks_before, tasks_after;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, key, sizeof(key));
    pick_key_for_worker(0, 2, home_key, sizeof(home_key));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));
    /* Bind both connections to worker 0 so operations on the worker-1
     * stream must use the routed-task path instead of connection migration. */
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(home_key), home_key);
    roundtrip(a, req, "+OK\r\n");
    roundtrip(b, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*5\r\n$4\r\nXADD\r\n$%zu\r\n%s\r\n$3\r\n1-0\r\n$1\r\nf\r\n$1\r\nv\r\n",
             strlen(key), key);
    roundtrip(a, req, "$3\r\n1-0\r\n");
    tasks_before = mt_server_tasks_executed(ms);
    snprintf(req, sizeof(req), "*5\r\n$6\r\nXDELEX\r\n$%zu\r\n%s\r\n$3\r\nIDS\r\n$1\r\n1\r\n$3\r\n1-0\r\n",
             strlen(key), key);
    roundtrip(b, req, "*1\r\n:1\r\n");
    tasks_after = mt_server_tasks_executed(ms);
    DD_CHECK(tasks_after > tasks_before);
    /* The remaining Redis 8 stream control commands must take the same
     * owner route, including their group-aware argument layouts. */
    snprintf(req, sizeof(req), "*5\r\n$4\r\nXADD\r\n$%zu\r\n%s\r\n$3\r\n2-0\r\n$1\r\nf\r\n$1\r\nv\r\n",
             strlen(key), key);
    roundtrip(a, req, "$3\r\n2-0\r\n");
    snprintf(req, sizeof(req), "*5\r\n$6\r\nXGROUP\r\n$6\r\nCREATE\r\n$%zu\r\n%s\r\n$1\r\ng\r\n$3\r\n0-0\r\n",
             strlen(key), key);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*6\r\n$7\r\nXACKDEL\r\n$%zu\r\n%s\r\n$1\r\ng\r\n$3\r\nIDS\r\n$1\r\n1\r\n$3\r\n2-0\r\n",
             strlen(key), key);
    roundtrip(b, req, "*1\r\n:-1\r\n");
    snprintf(req, sizeof(req), "*7\r\n$5\r\nXNACK\r\n$%zu\r\n%s\r\n$1\r\ng\r\n$4\r\nFAIL\r\n$3\r\nIDS\r\n$1\r\n1\r\n$3\r\n2-0\r\n",
             strlen(key), key);
    roundtrip(a, req, ":0\r\n");
    snprintf(req, sizeof(req), "*2\r\n$4\r\nXLEN\r\n$%zu\r\n%s\r\n",
             strlen(key), key);
    roundtrip(a, req, ":1\r\n");
    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_copy_same_worker(void)
{
    mt_server *ms;
    pal_socket_t a;
    char ka[32], kb[32], k0[32], k1[32];
    char req[300];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_two_keys_for_worker(1, 2, ka, sizeof(ka), kb, sizeof(kb));
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms)); /* -> worker 0 */

    /* same-worker pair: routed as one unit */
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv1\r\n",
             strlen(ka), ka);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req),
             "*3\r\n$4\r\nCOPY\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n", strlen(ka),
             ka, strlen(kb), kb);
    roundtrip(a, req, ":1\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(kb), kb);
    roundtrip(a, req, "$2\r\nv1\r\n");

    /* DB 0 (the current db) is a same-db copy: routed normally */
    snprintf(req, sizeof(req),
             "*6\r\n$4\r\nCOPY\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$2\r\nDB\r\n"
             "$1\r\n0\r\n$7\r\nREPLACE\r\n",
             strlen(ka), ka, strlen(kb), kb);
    roundtrip(a, req, ":1\r\n");

    /* keys on different workers: crossslot */
    snprintf(req, sizeof(req),
             "*3\r\n$4\r\nCOPY\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n", strlen(k0),
             k0, strlen(k1), k1);
    roundtrip(a, req,
              "-CROSSSLOT Keys in request don't hash to the same slot\r\n");

    /* cross-db COPY is executed by a full worker-local session and must
     * preserve the source while installing the serialized value in DB 1. */
    snprintf(req, sizeof(req),
             "*5\r\n$4\r\nCOPY\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$2\r\nDB\r\n"
             "$1\r\n1\r\n",
             strlen(ka), ka, strlen(kb), kb);
    roundtrip(a, req, ":1\r\n");
    roundtrip(a, "*2\r\n$6\r\nSELECT\r\n$1\r\n1\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(kb), kb);
    roundtrip(a, req, "$2\r\nv1\r\n");
    roundtrip(a, "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(ka), ka);
    roundtrip(a, req, "$2\r\nv1\r\n");

    /* The same full-session path is required when COPY DB is queued in
     * MULTI/EXEC. */
    roundtrip(a, "*1\r\n$5\r\nMULTI\r\n", "+OK\r\n");
    snprintf(req, sizeof(req),
             "*6\r\n$4\r\nCOPY\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$2\r\nDB\r\n$1\r\n1\r\n$7\r\nREPLACE\r\n",
             strlen(ka), ka, strlen(kb), kb);
    roundtrip(a, req, "+QUEUED\r\n");
    roundtrip(a, "*1\r\n$4\r\nEXEC\r\n", "*1\r\n:1\r\n");
    roundtrip(a, "*2\r\n$6\r\nSELECT\r\n$1\r\n1\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(kb), kb);
    roundtrip(a, req, "$2\r\nv1\r\n");
    roundtrip(a, "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n", "+OK\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_quit_closes_connection_mt(void)
{
    mt_server *ms;
    pal_socket_t c;
    char buf[64];
    ptrdiff_t n = -1;
    uint64_t deadline;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    c = connect_client(mt_server_port(ms));

    /* QUIT is keyless-local on the home worker: +OK, then the conn closes */
    roundtrip(c, "*1\r\n$4\r\nQUIT\r\n", "+OK\r\n");
    deadline = pal_now_ms() + 5000;
    while (n < 0 && pal_now_ms() < deadline) {
        n = pal_recv(c, buf, sizeof(buf));
        if (n < 0)
            pal_sleep_ms(1);
    }
    DD_CHECK_EQ_INT(0, n);

    pal_close(c);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

/* mt_server_enable_tls with unusable cert/key fails cleanly: the plain
 * listener keeps working and destroy is unaffected. (A full TLS handshake
 * roundtrip lives in test_tls.c, built only when OpenSSL is available.) */
static void test_tls_enable_failure_is_clean(void)
{
    mt_server *ms;
    pal_socket_t a;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(-1, mt_server_enable_tls(ms, "127.0.0.1", 0,
                                             "no-such-cert.pem",
                                             "no-such-key.pem"));
    DD_CHECK_EQ_INT(0, mt_server_tls_port(ms));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    roundtrip(a, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

/* INFO aggregates across workers: keyspace/memory/commandstats are summed
 * over the whole shared keyspace, not just the connection's home worker. */
static void test_info_aggregation(void)
{
    mt_server *ms;
    pal_socket_t a;
    char k0[32], k1[32];
    char req[192];
    char buf[8192];
    size_t got;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));

    /* one key on each worker (both written through conn a) */
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\ny\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+OK\r\n");

    got = request_full(a, "*1\r\n$4\r\nINFO\r\n", buf, sizeof(buf));
    DD_CHECK(got > 0);
    buf[got] = '\0';
    /* dbsize/keyspace sum both workers' shards of db0 */
    DD_CHECK(strstr(buf, "dbsize:2\r\n") != NULL);
    DD_CHECK(strstr(buf, "db0:keys=2,expires=0") != NULL);
    /* commandstats sum the per-worker counters (1 SET each) */
    DD_CHECK(strstr(buf, "cmdstat_set:calls=2,") != NULL);
    /* memory accounting is non-zero and aggregated */
    DD_CHECK(strstr(buf, "used_memory:0\r\n") == NULL);
    /* IO counters (Phase 27) aggregate across workers too */
    DD_CHECK(strstr(buf, "io_loops:") != NULL);
    DD_CHECK(strstr(buf, "io_reads:0\r\n") == NULL);

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_three_worker_aggregate_completion(void)
{
    mt_server *ms;
    pal_socket_t a;
    char key[32], req[160], buf[8192];
    size_t got;
    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    pick_key_for_worker(0, 3, key, sizeof(key));
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(key), key);
    roundtrip(a, req, "+OK\r\n");
    got = request_full(a, "*1\r\n$6\r\nDBSIZE\r\n", buf, sizeof(buf));
    DD_CHECK_EQ_INT(4, (long long)got);
    DD_CHECK_MEM(":1\r\n", 4, buf, got);
    got = request_full(a, "*1\r\n$4\r\nINFO\r\n", buf, sizeof(buf));
    DD_CHECK(got > 0);
    DD_CHECK(strstr(buf, "dbsize:1\r\n") != NULL);
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_two_worker_aggregate_completion_push_failure(void)
{
    mt_server *ms;
    pal_socket_t a;
    char buf[256];
    size_t got;
    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    mt_server_fail_next_completion_pushes(ms, 1);
    got = request_full(a, "*1\r\n$6\r\nDBSIZE\r\n", buf, sizeof(buf));
    DD_CHECK_EQ_INT(0, (long long)got);
    {
        uint64_t deadline = pal_now_ms() + 5000;
        while (mt_server_completion_pushes_consumed(ms) < 1 &&
               pal_now_ms() < deadline)
            pal_sleep_ms(1);
    }
    DD_CHECK_EQ_INT(1, mt_server_completion_pushes_consumed(ms));
    DD_CHECK_EQ_INT(0, mt_server_abandoned_aggregate_count(ms));
    mt_server_stop(ms);
    pal_close(a);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_aggregate_dbsize_and_flushdb(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char k0[32], k1[32];
    char req[192];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms)); /* -> worker 0 */
    b = connect_client(mt_server_port(ms)); /* -> worker 1 */

    /* one key on each worker */
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\ny\r\n",
             strlen(k1), k1);
    roundtrip(b, req, "+OK\r\n");

    /* DBSIZE aggregates across workers, regardless of the connection's home. */
    roundtrip(a, "*1\r\n$6\r\nDBSIZE\r\n", ":2\r\n");
    roundtrip(b, "*1\r\n$6\r\nDBSIZE\r\n", ":2\r\n");

    /* FLUSHDB broadcasts to every worker. */
    roundtrip(b, "*1\r\n$7\r\nFLUSHDB\r\n", "+OK\r\n");
    roundtrip(a, "*1\r\n$6\r\nDBSIZE\r\n", ":0\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n", strlen(k0),
             k0);
    roundtrip(b, req, "$-1\r\n");

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_aggregate_alloc_failure_fails_closed(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char k0[32], k1[32], req[160];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));

    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\ny\r\n",
             strlen(k1), k1);
    roundtrip(b, req, "+OK\r\n");

    mt_server_fail_next_aggregate_allocs(ms, 1);
    roundtrip(a, "*1\r\n$7\r\nFLUSHDB\r\n", "-ERR out of memory\r\n");

    /* The failed broadcast must not partially flush either shard. */
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "$1\r\nx\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "$1\r\ny\r\n");

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_set_algebra_same_slot_routing(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char ka[32], kb[32], k0[32], k1[32];
    char req[384];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_two_keys_for_worker(0, 2, ka, sizeof(ka), kb, sizeof(kb));
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms)); /* -> worker 0 */
    b = connect_client(mt_server_port(ms)); /* -> worker 1 */

    /* build two sets on worker 0 */
    snprintf(req, sizeof(req), "*4\r\n$4\r\nSADD\r\n$%zu\r\n%s\r\n$1\r\nx\r\n$1\r\ny\r\n",
             strlen(ka), ka);
    roundtrip(a, req, ":2\r\n");
    snprintf(req, sizeof(req), "*4\r\n$4\r\nSADD\r\n$%zu\r\n%s\r\n$1\r\ny\r\n$1\r\nz\r\n",
             strlen(kb), kb);
    roundtrip(a, req, ":2\r\n");

    /* SINTER from b: both keys on worker 0, routed as one unit */
    snprintf(req, sizeof(req), "*3\r\n$6\r\nSINTER\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
             strlen(ka), ka, strlen(kb), kb);
    roundtrip(b, req, "*1\r\n$1\r\ny\r\n");

    /* SUNION from b (member order is hash-table order: check the set
     * contents rather than an exact byte sequence) */
    {
        char got[128];
        size_t g = 0;
        uint64_t deadline;
        snprintf(req, sizeof(req), "*3\r\n$6\r\nSUNION\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                 strlen(ka), ka, strlen(kb), kb);
        {
            size_t sent = 0, rl = strlen(req);
            while (sent < rl) {
                ptrdiff_t n = pal_send(b, req + sent, rl - sent);
                if (n > 0)
                    sent += (size_t)n;
            }
        }
        deadline = pal_now_ms() + 5000;
        while (g < 25 && pal_now_ms() < deadline) {
            ptrdiff_t n = pal_recv(b, got + g, sizeof(got) - g);
            if (n > 0)
                g += (size_t)n;
            else
                pal_sleep_ms(1);
        }
        DD_CHECK(g >= 4);
        DD_CHECK_MEM("*3\r\n", 4, got, 4);
        DD_CHECK(g == 25);
        got[g] = '\0';
        DD_CHECK(strstr(got, "$1\r\nx\r\n") != NULL);
        DD_CHECK(strstr(got, "$1\r\ny\r\n") != NULL);
        DD_CHECK(strstr(got, "$1\r\nz\r\n") != NULL);
    }

    /* SDIFF from b */
    snprintf(req, sizeof(req), "*3\r\n$5\r\nSDIFF\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
             strlen(ka), ka, strlen(kb), kb);
    roundtrip(b, req, "*1\r\n$1\r\nx\r\n");

    /* cross-worker SINTER is rejected */
    snprintf(req, sizeof(req), "*3\r\n$6\r\nSINTER\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
             strlen(k0), k0, strlen(k1), k1);
    roundtrip(a, req, "-CROSSSLOT Keys in request don't hash to the same slot\r\n");

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_multi_exec_routed(void)
{
    mt_server *ms;
    pal_socket_t a;
    char k1[32];
    char req[256];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms)); /* -> worker 0 */

    roundtrip(a, "*1\r\n$5\r\nMULTI\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv1\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+QUEUED\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+QUEUED\r\n");
    roundtrip(a, "*1\r\n$4\r\nEXEC\r\n", "*2\r\n+OK\r\n$2\r\nv1\r\n");

    /* the transaction really executed on worker 1 */
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "$2\r\nv1\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_multi_exec_crossslot_aborts(void)
{
    mt_server *ms;
    pal_socket_t a;
    char k0[32], k1[32];
    char req[256];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));

    roundtrip(a, "*1\r\n$5\r\nMULTI\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+QUEUED\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\ny\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+QUEUED\r\n");
    roundtrip(a, "*1\r\n$4\r\nEXEC\r\n",
              "-EXECABORT Transaction discarded because of: keys hash to "
              "different slots\r\n");

    /* nothing was applied */
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "$-1\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "$-1\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_discard(void)
{
    mt_server *ms;
    pal_socket_t a;
    char k0[32];
    char req[192];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));

    roundtrip(a, "*1\r\n$5\r\nMULTI\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+QUEUED\r\n");
    roundtrip(a, "*1\r\n$7\r\nDISCARD\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "$-1\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_watch_aborts_exec_on_change(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char k0[32];
    char req[192];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms)); /* -> worker 0 */
    b = connect_client(mt_server_port(ms)); /* -> worker 1 */

    /* a watches k0 (local on worker 0) */
    snprintf(req, sizeof(req), "*2\r\n$5\r\nWATCH\r\n$%zu\r\n%s\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+OK\r\n");

    /* b changes k0 (routed to worker 0) */
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv2\r\n",
             strlen(k0), k0);
    roundtrip(b, req, "+OK\r\n");

    /* a's EXEC must abort with a null array */
    roundtrip(a, "*1\r\n$5\r\nMULTI\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+QUEUED\r\n");
    roundtrip(a, "*1\r\n$4\r\nEXEC\r\n", "*-1\r\n");

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_watch_routed_and_unwatch(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char k1[32];
    char req[192];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms)); /* -> worker 0 */
    b = connect_client(mt_server_port(ms)); /* -> worker 1 */

    /* a watches a worker-1 key: the WATCH itself is routed */
    snprintf(req, sizeof(req), "*2\r\n$5\r\nWATCH\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+OK\r\n");

    /* UNWATCH clears it: a later change must not abort EXEC */
    roundtrip(a, "*1\r\n$7\r\nUNWATCH\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv2\r\n",
             strlen(k1), k1);
    roundtrip(b, req, "+OK\r\n");
    roundtrip(a, "*1\r\n$5\r\nMULTI\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+QUEUED\r\n");
    roundtrip(a, "*1\r\n$4\r\nEXEC\r\n", "*1\r\n$2\r\nv2\r\n");

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_watch_pipeline_controls_are_ordered(void)
{
    mt_server *ms;
    pal_socket_t a;
    char key[32], req[512];
    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, key, sizeof(key));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req),
             "*2\r\n$5\r\nWATCH\r\n$%zu\r\n%s\r\n"
             "*1\r\n$5\r\nMULTI\r\n"
             "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n"
             "*1\r\n$4\r\nEXEC\r\n", strlen(key), key,
             strlen(key), key);
    pipeline_roundtrip(a, req, "+OK\r\n+OK\r\n+QUEUED\r\n*1\r\n$-1\r\n");
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_watch_pipeline_remote_get_is_not_queued(void)
{
    mt_server *ms;
    pal_socket_t a;
    char key[32], req[256];
    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, key, sizeof(key));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*2\r\n$5\r\nWATCH\r\n$%zu\r\n%s\r\n"
             "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n", strlen(key), key,
             strlen(key), key);
    pipeline_roundtrip(a, req, "+OK\r\n$-1\r\n");
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_watch_pipeline_two_remote_gets_are_not_queued(void)
{
    mt_server *ms;
    pal_socket_t a;
    char key[32], req[384];
    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, key, sizeof(key));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*2\r\n$5\r\nWATCH\r\n$%zu\r\n%s\r\n"
             "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n"
             "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n", strlen(key), key,
             strlen(key), key, strlen(key), key);
    pipeline_roundtrip(a, req, "+OK\r\n$-1\r\n$-1\r\n");
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_watch_pipeline_unwatch_is_ordered_and_disconnect_safe(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char key[32], req[256];
    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, key, sizeof(key));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*2\r\n$5\r\nWATCH\r\n$%zu\r\n%s\r\n"
             "*1\r\n$7\r\nUNWATCH\r\n", strlen(key), key);
    pipeline_roundtrip(a, req, "+OK\r\n+OK\r\n");
    pal_close(a);

    a = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*2\r\n$5\r\nWATCH\r\n$%zu\r\n%s\r\n",
             strlen(key), key);
    (void)pal_send(a, req, strlen(req));
    pal_close(a);
    pal_sleep_ms(100);
    b = connect_client(mt_server_port(ms));
    roundtrip(b, "*1\r\n$5\r\nMULTI\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(key), key);
    roundtrip(b, req, "+QUEUED\r\n");
    roundtrip(b, "*1\r\n$4\r\nEXEC\r\n", "*1\r\n$-1\r\n");
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_watch_shutdown_releases_remote_owner(void)
{
    mt_server *ms;
    pal_socket_t a, home0;
    char key[32], req[160];
    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, key, sizeof(key));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    /* Accept round-robin: consume home worker 0, then home worker 1. */
    home0 = connect_client(mt_server_port(ms));
    a = connect_client(mt_server_port(ms));
    pick_key_for_worker(0, 2, key, sizeof(key));
    snprintf(req, sizeof(req), "*2\r\n$5\r\nWATCH\r\n$%zu\r\n%s\r\n",
             strlen(key), key);
    roundtrip(a, req, "+OK\r\n");
    mt_server_stop(ms);
    pal_close(home0);
    pal_close(a);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

/* Read exactly len bytes from c with a deadline (for async pushes). */
static size_t recv_deadline(pal_socket_t c, char *buf, size_t len,
                            uint64_t ms)
{
    size_t got = 0;
    uint64_t deadline = pal_now_ms() + ms;
    while (got < len && pal_now_ms() < deadline) {
        ptrdiff_t n = pal_recv(c, buf + got, len - got);
        if (n > 0)
            got += (size_t)n;
        else
            pal_sleep_ms(1);
    }
    return got;
}

static void test_pubsub_cross_worker(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char buf[64];
    const char *push = "*3\r\n$7\r\nmessage\r\n$2\r\nch\r\n$5\r\nhello\r\n";

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms)); /* -> worker 0 */
    b = connect_client(mt_server_port(ms)); /* -> worker 1 */

    roundtrip(a, "*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n",
              "*3\r\n$9\r\nsubscribe\r\n$2\r\nch\r\n:1\r\n");
    roundtrip(b, "*3\r\n$7\r\nPUBLISH\r\n$2\r\nch\r\n$5\r\nhello\r\n",
              ":1\r\n");

    /* the push arrives on a without a sending anything */
    DD_CHECK_EQ_INT((long long)strlen(push),
                    (long long)recv_deadline(a, buf, strlen(push), 3000));
    DD_CHECK_MEM(push, strlen(push), buf, strlen(push));

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_sharded_pubsub_cross_worker(void)
{
    mt_server *ms;
    pal_socket_t sub, pub;
    char ch[32], req[256], buf[256];
    size_t n;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, ch, sizeof(ch));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms == NULL)
        return;
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    sub = connect_client(mt_server_port(ms));
    pub = connect_client(mt_server_port(ms));

    snprintf(req, sizeof(req), "*2\r\n$10\r\nSSUBSCRIBE\r\n$%zu\r\n%s\r\n",
             strlen(ch), ch);
    snprintf(buf, sizeof(buf), "*3\r\n$10\r\nssubscribe\r\n$%zu\r\n%s\r\n:1\r\n",
             strlen(ch), ch);
    roundtrip(sub, req, buf);
    snprintf(req, sizeof(req), "*3\r\n$8\r\nSPUBLISH\r\n$%zu\r\n%s\r\n$5\r\nhello\r\n",
             strlen(ch), ch);
    roundtrip(pub, req, ":1\r\n");
    n = recv_deadline(sub, buf, sizeof(buf), 3000);
    DD_CHECK(n > 0);
    if (n > 0) {
        char expected[256];
        snprintf(expected, sizeof(expected), "*3\r\n$8\r\nsmessage\r\n$%zu\r\n%s\r\n$5\r\nhello\r\n",
                 strlen(ch), ch);
        DD_CHECK_MEM(expected, strlen(expected), buf, n);
    }
    snprintf(req, sizeof(req), "*3\r\n$6\r\nPUBSUB\r\n$11\r\nSHARDNUMSUB\r\n$%zu\r\n%s\r\n",
             strlen(ch), ch);
    snprintf(buf, sizeof(buf), "*2\r\n$%zu\r\n%s\r\n:1\r\n",
             strlen(ch), ch);
    roundtrip(pub, req, buf);
    snprintf(req, sizeof(req), "*2\r\n$6\r\nPUBSUB\r\n$13\r\nSHARDCHANNELS\r\n");
    snprintf(buf, sizeof(buf), "*1\r\n$%zu\r\n%s\r\n", strlen(ch), ch);
    roundtrip(pub, req, buf);
    snprintf(req, sizeof(req), "*1\r\n$12\r\nSUNSUBSCRIBE\r\n");
    snprintf(buf, sizeof(buf), "*3\r\n$12\r\nsunsubscribe\r\n$%zu\r\n%s\r\n:0\r\n",
             strlen(ch), ch);
    roundtrip(sub, req, buf);

    pal_close(sub);
    pal_close(pub);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_unsubscribe_stops_delivery(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char buf[64];
    const char *unsub_publish =
        "*2\r\n$11\r\nUNSUBSCRIBE\r\n$2\r\nch\r\n"
        "*3\r\n$7\r\nPUBLISH\r\n$2\r\nch\r\n$5\r\nhello\r\n";
    const char *unsub_publish_reply =
        "*3\r\n$11\r\nunsubscribe\r\n$2\r\nch\r\n:0\r\n:0\r\n";

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));

    roundtrip(a, "*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n",
              "*3\r\n$9\r\nsubscribe\r\n$2\r\nch\r\n:1\r\n");
    roundtrip(a, "*2\r\n$11\r\nUNSUBSCRIBE\r\n$2\r\nch\r\n",
              "*3\r\n$11\r\nunsubscribe\r\n$2\r\nch\r\n:0\r\n");
    roundtrip(b, "*3\r\n$7\r\nPUBLISH\r\n$2\r\nch\r\n$5\r\nhello\r\n",
              ":0\r\n");

    roundtrip(a, "*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n",
              "*3\r\n$9\r\nsubscribe\r\n$2\r\nch\r\n:1\r\n");
    pipeline_roundtrip(a, unsub_publish, unsub_publish_reply);

    /* nothing should arrive on a */
    DD_CHECK_EQ_INT(0, (long long)recv_deadline(a, buf, sizeof(buf), 300));

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_pubsub_conn_close_unsubscribes(void)
{
    mt_server *ms;
    pal_socket_t a, b;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));

    roundtrip(a, "*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n",
              "*3\r\n$9\r\nsubscribe\r\n$2\r\nch\r\n:1\r\n");
    pal_close(a);
    /* give the close/unregister a moment to propagate */
    pal_sleep_ms(200);
    roundtrip(b, "*3\r\n$7\r\nPUBLISH\r\n$2\r\nch\r\n$5\r\nhello\r\n",
              ":0\r\n");

    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_pubsub_introspection_aggregates_workers(void)
{
    mt_server *ms;
    pal_socket_t a, b, c;
    char ch0[32], ch1[32], req[256], buf[512];
    size_t n;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, ch0, sizeof(ch0));
    pick_key_for_worker(1, 2, ch1, sizeof(ch1));
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));
    c = connect_client(mt_server_port(ms));

    snprintf(req, sizeof(req), "*2\r\n$9\r\nSUBSCRIBE\r\n$%zu\r\n%s\r\n",
             strlen(ch0), ch0);
    n = request_full(a, req, buf, sizeof(buf));
    DD_CHECK(strstr(buf, "subscribe") != NULL);
    snprintf(req, sizeof(req), "*2\r\n$9\r\nSUBSCRIBE\r\n$%zu\r\n%s\r\n",
             strlen(ch1), ch1);
    n = request_full(b, req, buf, sizeof(buf));
    DD_CHECK(strstr(buf, "subscribe") != NULL);
    /* Complete the variable channel/count replies with a focused query. */
    snprintf(req, sizeof(req), "*3\r\n$6\r\nPUBSUB\r\n$6\r\nNUMSUB\r\n$%zu\r\n%s\r\n",
             strlen(ch0), ch0);
    n = request_full(a, req, buf, sizeof(buf));
    DD_CHECK(n > 0);
    DD_CHECK(strstr(buf, ch0) != NULL);
    DD_CHECK(strstr(buf, ":1\r\n") != NULL);

    roundtrip(c, "*2\r\n$10\r\nPSUBSCRIBE\r\n$6\r\nnews.*\r\n",
              "*3\r\n$10\r\npsubscribe\r\n$6\r\nnews.*\r\n:1\r\n");
    roundtrip(a, "*2\r\n$6\r\nPUBSUB\r\n$6\r\nNUMPAT\r\n", ":1\r\n");

    snprintf(req, sizeof(req), "*3\r\n$6\r\nPUBSUB\r\n$8\r\nCHANNELS\r\n$1\r\n*\r\n");
    n = request_full(a, req, buf, sizeof(buf));
    DD_CHECK(n > 0);
    DD_CHECK(strstr(buf, ch0) != NULL);
    DD_CHECK(strstr(buf, ch1) != NULL);

    pal_close(a);
    pal_close(b);
    pal_close(c);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_aof_persistence_mt(void)
{
    mt_server *ms;
    pal_socket_t a;
    char k0[32], k1[32];
    char req[192];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    (void)pal_file_unlink("./worker-0-mttest.aof");
    (void)pal_file_unlink("./worker-1-mttest.aof");

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_enable_aof(ms, ".", "mttest.aof"));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));

    /* one local write, one routed write */
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv0\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv1\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+OK\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);

    DD_CHECK(pal_file_exists("./worker-0-mttest.aof"));
    DD_CHECK(pal_file_exists("./worker-1-mttest.aof"));

    /* restart: the AOF is replayed per worker */
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_enable_aof(ms, ".", "mttest.aof"));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "$2\r\nv0\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "$2\r\nv1\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    (void)pal_file_unlink("./worker-0-mttest.aof");
    (void)pal_file_unlink("./worker-1-mttest.aof");
    pal_socket_cleanup();
}

static void test_aof_aggregate_mt(void)
{
    mt_server *ms;
    pal_socket_t a;
    char k0[32], k1[32];
    char req[192];
    const char *aof0 = "./worker-0-mtaggr.aof";
    const char *aof1 = "./worker-1-mtaggr.aof";

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));
    (void)pal_file_unlink(aof0);
    (void)pal_file_unlink(aof1);

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_enable_aof(ms, ".", "mtaggr.aof"));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));

    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv0\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv1\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+OK\r\n");
    roundtrip(a, "*1\r\n$7\r\nFLUSHDB\r\n", "+OK\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_enable_aof(ms, ".", "mtaggr.aof"));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "$-1\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "$-1\r\n");
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_enable_aof(ms, ".", "mtaggr.aof"));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv2\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv2\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+OK\r\n");
    roundtrip(a, "*1\r\n$8\r\nFLUSHALL\r\n", "+OK\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_enable_aof(ms, ".", "mtaggr.aof"));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "$-1\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "$-1\r\n");
    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);

    (void)pal_file_unlink(aof0);
    (void)pal_file_unlink(aof1);
    pal_socket_cleanup();
}

static void test_aof_failure_stops_mt_workers_without_spin(void)
{
    mt_server *ms;
    pal_socket_t c;
    char key[32];
    char req[192];
    uint64_t deadline;
    uint64_t loops0, loops1;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, key, sizeof(key));
    (void)pal_file_unlink("./worker-0-mtfail.aof");
    (void)pal_file_unlink("./worker-1-mtfail.aof");
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_enable_aof(ms, ".", "mtfail.aof"));
    mt_server_test_set_aof_write_fn(ms, 0, fail_aof_write);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    c = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req),
             "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(key), key);
    roundtrip(c, req, "+OK\r\n");

    deadline = pal_now_ms() + 2000;
    while (mt_server_test_running(ms) && pal_now_ms() < deadline)
        pal_sleep_ms(1);
    DD_CHECK(!mt_server_test_running(ms));
    loops0 = mt_server_test_worker_loops(ms, 0);
    loops1 = mt_server_test_worker_loops(ms, 1);
    pal_sleep_ms(100);
    DD_CHECK_EQ_INT((long long)loops0,
                    (long long)mt_server_test_worker_loops(ms, 0));
    DD_CHECK_EQ_INT((long long)loops1,
                    (long long)mt_server_test_worker_loops(ms, 1));

    pal_close(c);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    (void)pal_file_unlink("./worker-0-mtfail.aof");
    (void)pal_file_unlink("./worker-1-mtfail.aof");
    pal_socket_cleanup();
}

static void test_snapshot_mt(void)
{
    mt_server *ms;
    pal_socket_t a;
    char k0[32], k1[32];
    char req[192];
    char buf[32];
    size_t got;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    (void)pal_file_unlink("./worker-0-mttest.ddr");
    (void)pal_file_unlink("./worker-1-mttest.ddr");

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0,
                    mt_server_enable_snapshots(ms, ".", "mttest.ddr", 0));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));

    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv0\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv1\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+OK\r\n");

    roundtrip(a, "*1\r\n$4\r\nSAVE\r\n", "+OK\r\n");
    /* LASTSAVE aggregates the workers' save times */
    {
        size_t sent = 0;
        const char *ls = "*1\r\n$8\r\nLASTSAVE\r\n";
        while (sent < strlen(ls)) {
            ptrdiff_t n = pal_send(a, ls + sent, strlen(ls) - sent);
            if (n > 0)
                sent += (size_t)n;
        }
        got = recv_deadline(a, buf, sizeof(buf) - 1, 3000);
        DD_CHECK(got >= 4);
        buf[got] = '\0';
        DD_CHECK(buf[0] == ':');
        DD_CHECK(memcmp(buf, ":0\r\n", 4) != 0);
    }

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);

    DD_CHECK(pal_file_exists("./worker-0-mttest.ddr"));
    DD_CHECK(pal_file_exists("./worker-1-mttest.ddr"));

    /* restart: snapshots are loaded per worker */
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0,
                    mt_server_enable_snapshots(ms, ".", "mttest.ddr", 0));
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "$2\r\nv0\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "$2\r\nv1\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    (void)pal_file_unlink("./worker-0-mttest.ddr");
    (void)pal_file_unlink("./worker-1-mttest.ddr");
    pal_socket_cleanup();
}

static void test_connection_migration_to_key_owner(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char k0[32], k1[32];
    char req[192];
    uint64_t before, after;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms)); /* -> worker 0 */
    b = connect_client(mt_server_port(ms)); /* -> worker 1 */

    /* The first keyed command migrates the connection to the key's owner,
     * so it executes locally there: no routed task at all. */
    before = mt_server_tasks_executed(ms);
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\ny\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "$1\r\ny\r\n");
    after = mt_server_tasks_executed(ms);
    DD_CHECK_EQ_INT((long long)before, (long long)after);

    /* keyless commands still work after migration */
    roundtrip(a, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");

    /* keys owned by the other worker are routed as before */
    before = mt_server_tasks_executed(ms);
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "$-1\r\n");
    after = mt_server_tasks_executed(ms);
    DD_CHECK_EQ_INT((long long)(before + 1), (long long)after);

    /* routed single commands recycle pooled task objects (Phase 31):
     * the first routed command's free stocked the freelist, the next
     * one must pop it */
    {
        uint64_t ph0 = mt_server_pool_hits(ms);
        snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
                 strlen(k0), k0);
        roundtrip(a, req, "$-1\r\n");
        DD_CHECK(mt_server_pool_hits(ms) > ph0);
    }

    /* and the data is visible from other connections */
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(b, req, "$1\r\ny\r\n");

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_same_target_pipeline_merges_into_one_task(void)
{
    mt_server *ms;
    pal_socket_t a;
    char ka[32], kc[32], kd[32];
    char req[512];
    uint64_t before, after;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, ka, sizeof(ka));        /* migration trigger */
    pick_two_keys_for_worker(0, 2, kc, sizeof(kc), kd, sizeof(kd));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms)); /* -> worker 0 */

    /* migrate the connection to worker 1 first (affinity is once per
     * connection), so worker-0 keys below are routed from worker 1 */
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nz\r\n",
             strlen(ka), ka);
    roundtrip(a, req, "+OK\r\n");

    before = mt_server_tasks_executed(ms);

    /* four same-target routed commands in one pipeline must merge into a
     * single cross-worker task (replies still arrive individually). */
    snprintf(req, sizeof(req),
             "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n"
             "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\ny\r\n"
             "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n"
             "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(kc), kc, strlen(kd), kd, strlen(kc), kc, strlen(kd),
             kd);
    roundtrip(a, req, "+OK\r\n+OK\r\n$1\r\nx\r\n$1\r\ny\r\n");

    after = mt_server_tasks_executed(ms);
    DD_CHECK_EQ_INT((long long)(before + 1), (long long)after);

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_mt_multidb_select_and_swapdb(void)
{
    mt_server *ms;
    pal_socket_t a;
    char k1[32];
    char req[192];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms)); /* -> worker 0 */

    /* db 3: a routed write and a routed read on the same worker */
    roundtrip(a, "*2\r\n$6\r\nSELECT\r\n$1\r\n3\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$2\r\nv1\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "$2\r\nv1\r\n");
    roundtrip(a, "*1\r\n$6\r\nDBSIZE\r\n", ":1\r\n");

    /* db 0 is unaffected */
    roundtrip(a, "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "$-1\r\n");
    roundtrip(a, "*1\r\n$6\r\nDBSIZE\r\n", ":0\r\n");

    /* SWAPDB 3 0 broadcasts: db0 now holds the key, db3 is empty */
    roundtrip(a, "*3\r\n$6\r\nSWAPDB\r\n$1\r\n3\r\n$1\r\n0\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "$2\r\nv1\r\n");
    roundtrip(a, "*2\r\n$6\r\nSELECT\r\n$1\r\n3\r\n", "+OK\r\n");
    roundtrip(a, "*1\r\n$6\r\nDBSIZE\r\n", ":0\r\n");

    /* FLUSHDB flushes only the selected db (db 3 here) */
    roundtrip(a, "*1\r\n$7\r\nFLUSHDB\r\n", "+OK\r\n");
    roundtrip(a, "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n", "+OK\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "$2\r\nv1\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

/* mt workers on the IOCP backend (falls back to readiness where IOCP is
 * unavailable): routed commands, aggregation and pipelines all work;
 * connection migration is disabled on this backend (routing via tasks). */
static void test_iocp_workers_basic(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char k0[32], k1[32];
    char req[192];
    char buf[8192];
    size_t got;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));
    pick_key_for_worker(1, 2, k1, sizeof(k1));

    ms = mt_server_create_ex("127.0.0.1", 0, 2, SERVER_BACKEND_IOCP);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));

    roundtrip(a, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+OK\r\n");
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\ny\r\n",
             strlen(k1), k1);
    roundtrip(a, req, "+OK\r\n");

    /* cross-worker reads from the other connection */
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k0), k0);
    roundtrip(b, req, "$1\r\nx\r\n");
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(k1), k1);
    roundtrip(b, req, "$1\r\ny\r\n");

    /* pipelined commands keep order on the IOCP backend too */
    roundtrip(a, "*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n",
              "+PONG\r\n+PONG\r\n+PONG\r\n");

    /* aggregates (DBSIZE sum, INFO merge) work unchanged */
    roundtrip(a, "*1\r\n$6\r\nDBSIZE\r\n", ":2\r\n");
    got = request_full(a, "*1\r\n$4\r\nINFO\r\n", buf, sizeof(buf));
    DD_CHECK(got > 0);
    buf[got] = '\0';
    DD_CHECK(strstr(buf, "dbsize:2\r\n") != NULL);

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

/* Repeated routed GET and EXEC commands make a stranded task or completion
 * observable through the public RESP socket instead of internal counters. */
static void run_routed_wakeup_stress(int backend)
{
    mt_server *ms;
    pal_socket_t a, b;
    char k0[32];
    char req[256];
    int i;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    pick_key_for_worker(0, 2, k0, sizeof(k0));

    ms = mt_server_create_ex("127.0.0.1", 0, 2, backend);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));

    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\n0\r\n",
             strlen(k0), k0);
    roundtrip(a, req, "+OK\r\n");

    for (i = 0; i < 256; i++) {
        snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
                 strlen(k0), k0);
        roundtrip(b, req, "$1\r\n0\r\n");

        snprintf(req, sizeof(req),
                 "*1\r\n$5\r\nMULTI\r\n"
                 "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n"
                 "*1\r\n$4\r\nEXEC\r\n",
                 strlen(k0), k0);
        pipeline_roundtrip(b, req,
                           "+OK\r\n+QUEUED\r\n*1\r\n$1\r\n0\r\n");
    }

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_readiness_routed_wakeup_stress(void)
{
    run_routed_wakeup_stress(SERVER_BACKEND_SELECT);
}

/* io_uring op workers use completion-based wakeups just like IOCP workers. */
static void test_iouring_op_routed_completion_stress(void)
{
    run_routed_wakeup_stress(SERVER_BACKEND_IOURING_OP);
}

static void test_aggregate_shutdown_drops_queued_parts(void)
{
    int i;
    DD_CHECK_EQ_INT(0, pal_socket_init());
    for (i = 0; i < 8; i++) {
        mt_server *ms = mt_server_create("127.0.0.1", 0, 2);
        pal_socket_t c;
        const char req[] = "*1\r\n$6\r\nDBSIZE\r\n";
        DD_CHECK(ms != NULL);
        if (ms == NULL)
            continue;
        DD_CHECK_EQ_INT(0, mt_server_start(ms));
        c = connect_client(mt_server_port(ms));
        (void)pal_send(c, req, sizeof(req) - 1);
        mt_server_stop(ms);
        pal_close(c);
        mt_server_destroy(ms);
    }
    pal_socket_cleanup();
}

static void test_subscription_shutdown_drops_queued_registration(void)
{
    int i;
    DD_CHECK_EQ_INT(0, pal_socket_init());
    for (i = 0; i < 8; i++) {
        mt_server *ms = mt_server_create("127.0.0.1", 0, 2);
        pal_socket_t c;
        const char req[] = "*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n";
        DD_CHECK(ms != NULL);
        if (ms == NULL)
            continue;
        DD_CHECK_EQ_INT(0, mt_server_start(ms));
        c = connect_client(mt_server_port(ms));
        (void)pal_send(c, req, sizeof(req) - 1);
        pal_close(c);
        mt_server_stop(ms);
        mt_server_destroy(ms);
    }
    pal_socket_cleanup();
}

static void test_many_connections_across_workers(void)
{
    mt_server *ms;
    int i;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 4);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));

    for (i = 0; i < 16; i++) {
        pal_socket_t c = connect_client(mt_server_port(ms));
        roundtrip(c, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");
        pal_close(c);
    }

    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_start_rolls_back_partial_worker_startup(void)
{
    mt_server *ms;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms != NULL) {
        pal_thread_test_fail_create_after(1);
        DD_CHECK_EQ_INT(-1, mt_server_start(ms));
        DD_CHECK_EQ_INT(0, mt_server_start(ms));
        mt_server_stop(ms);
        mt_server_destroy(ms);
    }
    pal_thread_test_fail_create_after(-1);
    pal_socket_cleanup();
}

static void test_start_rolls_back_worker_startup_on_acceptor_failure(void)
{
    mt_server *ms;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (ms != NULL) {
        pal_thread_test_fail_create_after(2);
        DD_CHECK_EQ_INT(-1, mt_server_start(ms));
        DD_CHECK_EQ_INT(0, mt_server_start(ms));
        mt_server_stop(ms);
        mt_server_destroy(ms);
    }
    pal_thread_test_fail_create_after(-1);
    pal_socket_cleanup();
}

int main(void)
{
    DD_RUN(test_two_workers_shared_keyspace);
    DD_RUN(test_routed_cross_worker_commands);
    DD_RUN(test_redis8_single_key_commands_route_to_owner);
    DD_RUN(test_redis8_multikey_commands_route_to_owner);
    DD_RUN(test_pipeline_mixed_targets_keeps_order);
    DD_RUN(test_blocked_commands_in_mt_mode);
    DD_RUN(test_blocking_pop_cross_worker);
    DD_RUN(test_blocking_pop_timeout_and_crossslot);
    DD_RUN(test_migrate_supported_on_single_mt_worker);
    DD_RUN(test_migrate_cross_worker_external_target);
    DD_RUN(test_randomkey_aggregates_workers);
    DD_RUN(test_keys_aggregates_workers);
    DD_RUN(test_scan_composite_cursor_across_workers);
    DD_RUN(test_cluster_control_plane_mt);
    DD_RUN(test_himport_cross_worker_fails_closed);
    DD_RUN(test_sort_store_cross_worker_fails_closed);
    DD_RUN(test_script_cache_broadcast_reaches_key_owner);
    DD_RUN(test_config_mutations_broadcast_to_workers);
    DD_RUN(test_save_covers_all_selected_databases);
    DD_RUN(test_bgsave_covers_all_selected_databases);
    DD_RUN(test_bgrewriteaof_is_broadcast_to_workers);
    DD_RUN(test_client_list_covers_all_workers);
    DD_RUN(test_hotkeys_control_is_broadcast);
    DD_RUN(test_cluster_state_propagates_to_workers);
    DD_RUN(test_mt_replica_partitions_full_sync);
    DD_RUN(test_mt_master_serves_replica_full_sync);
    DD_RUN(test_mt_replication_forwards_mutations);
    DD_RUN(test_mt_info_replication);
    DD_RUN(test_mt_swapdb_replicates_once_three_workers);
    DD_RUN(test_pubsub_cross_worker);
    DD_RUN(test_sharded_pubsub_cross_worker);
    DD_RUN(test_unsubscribe_stops_delivery);
    DD_RUN(test_pubsub_conn_close_unsubscribes);
    DD_RUN(test_pubsub_introspection_aggregates_workers);
    DD_RUN(test_multikey_same_worker);
    DD_RUN(test_multikey_crossslot_rejected);
    DD_RUN(test_smove_same_worker);
    DD_RUN(test_lmovem_same_worker);
    DD_RUN(test_object_key_command_routes_to_owner);
    DD_RUN(test_stream_extension_commands_route_to_owner);
    DD_RUN(test_copy_same_worker);
    DD_RUN(test_quit_closes_connection_mt);
    DD_RUN(test_aggregate_dbsize_and_flushdb);
    DD_RUN(test_aggregate_alloc_failure_fails_closed);
    DD_RUN(test_info_aggregation);
    DD_RUN(test_three_worker_aggregate_completion);
    DD_RUN(test_two_worker_aggregate_completion_push_failure);
    DD_RUN(test_tls_enable_failure_is_clean);
    DD_RUN(test_set_algebra_same_slot_routing);
    DD_RUN(test_multi_exec_routed);
    DD_RUN(test_multi_exec_crossslot_aborts);
    DD_RUN(test_discard);
    DD_RUN(test_watch_aborts_exec_on_change);
    DD_RUN(test_watch_routed_and_unwatch);
    DD_RUN(test_watch_pipeline_controls_are_ordered);
    DD_RUN(test_watch_pipeline_remote_get_is_not_queued);
    DD_RUN(test_watch_pipeline_two_remote_gets_are_not_queued);
    DD_RUN(test_watch_pipeline_unwatch_is_ordered_and_disconnect_safe);
    DD_RUN(test_watch_shutdown_releases_remote_owner);
    DD_RUN(test_aof_persistence_mt);
    DD_RUN(test_aof_aggregate_mt);
    DD_RUN(test_aof_failure_stops_mt_workers_without_spin);
    DD_RUN(test_snapshot_mt);
    DD_RUN(test_connection_migration_to_key_owner);
    DD_RUN(test_mt_multidb_select_and_swapdb);
    DD_RUN(test_same_target_pipeline_merges_into_one_task);
    DD_RUN(test_many_connections_across_workers);
    DD_RUN(test_start_rolls_back_partial_worker_startup);
    DD_RUN(test_start_rolls_back_worker_startup_on_acceptor_failure);
    DD_RUN(test_iocp_workers_basic);
    DD_RUN(test_readiness_routed_wakeup_stress);
    DD_RUN(test_iouring_op_routed_completion_stress);
    DD_RUN(test_aggregate_shutdown_drops_queued_parts);
    DD_RUN(test_subscription_shutdown_drops_queued_registration);
    return DD_TEST_SUMMARY();
}
