/* test_mt_server.c - integration tests for the thread-per-core mt_server.
 *
 * Workers run on real background threads; the test acts as a loopback client
 * and talks to the public listener.
 */
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/hashslot.h"
#include "pal/pal_file.h"
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

    roundtrip(a, "*1\r\n$8\r\nSHUTDOWN\r\n",
              "-ERR command not supported in mt mode\r\n");
    roundtrip(a, "*1\r\n$4\r\nSYNC\r\n",
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

static void test_unsubscribe_stops_delivery(void)
{
    mt_server *ms;
    pal_socket_t a, b;
    char buf[64];

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
    DD_RUN(test_pubsub_cross_worker);
    DD_RUN(test_unsubscribe_stops_delivery);
    DD_RUN(test_pubsub_conn_close_unsubscribes);
    DD_RUN(test_multikey_same_worker);
    DD_RUN(test_multikey_crossslot_rejected);
    DD_RUN(test_smove_same_worker);
    DD_RUN(test_aggregate_dbsize_and_flushdb);
    DD_RUN(test_info_aggregation);
    DD_RUN(test_tls_enable_failure_is_clean);
    DD_RUN(test_set_algebra_same_slot_routing);
    DD_RUN(test_multi_exec_routed);
    DD_RUN(test_multi_exec_crossslot_aborts);
    DD_RUN(test_discard);
    DD_RUN(test_watch_aborts_exec_on_change);
    DD_RUN(test_watch_routed_and_unwatch);
    DD_RUN(test_aof_persistence_mt);
    DD_RUN(test_snapshot_mt);
    DD_RUN(test_connection_migration_to_key_owner);
    DD_RUN(test_mt_multidb_select_and_swapdb);
    DD_RUN(test_same_target_pipeline_merges_into_one_task);
    DD_RUN(test_many_connections_across_workers);
    return DD_TEST_SUMMARY();
}
