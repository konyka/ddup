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
    if (got != elen || memcmp(expected, buf, elen) != 0) {
        fprintf(stderr, "roundtrip mismatch:\n  req      : %.*s\n  expected : %.*s\n  got(%zu) : %.*s\n",
                (int)rlen, req, (int)elen, expected, got, (int)got, buf);
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

static void test_same_target_pipeline_merges_into_one_task(void)
{
    mt_server *ms;
    pal_socket_t a;
    char ka[32], kb[32];
    char req[512];
    uint64_t before, after;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    /* two keys that both live on worker 1 */
    pick_two_keys_for_worker(1, 2, ka, sizeof(ka), kb, sizeof(kb));

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    a = connect_client(mt_server_port(ms)); /* -> worker 0 */

    before = mt_server_tasks_executed(ms);

    /* four same-target routed commands in one pipeline must merge into a
     * single cross-worker task (replies still arrive individually). */
    snprintf(req, sizeof(req),
             "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nx\r\n"
             "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\ny\r\n"
             "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n"
             "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(ka), ka, strlen(kb), kb, strlen(ka), ka, strlen(kb),
             kb);
    roundtrip(a, req, "+OK\r\n+OK\r\n$1\r\nx\r\n$1\r\ny\r\n");

    after = mt_server_tasks_executed(ms);
    DD_CHECK_EQ_INT((long long)(before + 1), (long long)after);

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
    DD_RUN(test_multikey_same_worker);
    DD_RUN(test_multikey_crossslot_rejected);
    DD_RUN(test_smove_same_worker);
    DD_RUN(test_aggregate_dbsize_and_flushdb);
    DD_RUN(test_set_algebra_same_slot_routing);
    DD_RUN(test_same_target_pipeline_merges_into_one_task);
    DD_RUN(test_many_connections_across_workers);
    return DD_TEST_SUMMARY();
}
