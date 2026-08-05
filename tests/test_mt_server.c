/* test_mt_server.c - integration tests for the thread-per-core mt_server.
 *
 * Workers run on real background threads; the test acts as a loopback client
 * and talks to the public listener.
 */
#include "test.h"

#include <stdio.h>
#include <string.h>

#include "core/hashslot.h"
#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "server/mt_server.h"

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
    DD_CHECK_EQ_INT((long long)elen, (long long)got);
    DD_CHECK_MEM(expected, elen, buf, got);
}

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
    pal_socket_t a;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms));

    roundtrip(a, "*1\r\n$5\r\nMULTI\r\n",
              "-ERR command not supported in mt mode\r\n");
    roundtrip(a, "*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n",
              "-ERR command not supported in mt mode\r\n");
    /* The session still works for normal commands afterwards. */
    roundtrip(a, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");

    pal_close(a);
    mt_server_stop(ms);
    mt_server_destroy(ms);
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

int main(void)
{
    DD_RUN(test_two_workers_shared_keyspace);
    DD_RUN(test_routed_cross_worker_commands);
    DD_RUN(test_pipeline_mixed_targets_keeps_order);
    DD_RUN(test_blocked_commands_in_mt_mode);
    DD_RUN(test_many_connections_across_workers);
    return DD_TEST_SUMMARY();
}
