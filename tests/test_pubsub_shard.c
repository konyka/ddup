/* test_pubsub_shard.c - sharded pub/sub (SSUBSCRIBE/SPUBLISH/SUNSUBSCRIBE,
 * PUBSUB SHARDCHANNELS/SHARDNUMSUB) local semantics + cluster routing. */
#include <stdio.h>
#include <string.h>

#include "core/cluster.h"
#include "core/hashslot.h"
#include "core/session.h"
#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "server/server.h"
#include "test.h"

#define T0 1000000ULL
#define TEST_ID "0123456789abcdef0123456789abcdef01234567"
#define OTHER_ID "ffffffffffffffffffffffffffffffffffffffff"

static pal_socket_t cli(uint16_t port)
{
    pal_socket_t c = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(c != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));
    return c;
}

/* send req, pump the server, read until want appears in buf (bounded) */
static int rt(server *s, pal_socket_t c, const char *req, const char *want,
              char *buf, size_t cap)
{
    size_t got = 0;
    uint64_t dl = pal_now_ms() + 10000;
    DD_CHECK_EQ_INT((long long)strlen(req),
                    (long long)pal_send(c, req, strlen(req)));
    while (pal_now_ms() < dl) {
        ptrdiff_t n;
        server_run_once(s, 5);
        n = pal_recv(c, buf + got, cap - got - 1);
        if (n > 0) {
            got += (size_t)n;
            buf[got] = '\0';
            if (strstr(buf, want) != NULL)
                return 1;
        }
    }
    buf[got] = '\0';
    fprintf(stderr, "rt timeout: want [%s] got [%s]\n", want, buf);
    return 0;
}

static void test_local_shard_pubsub(void)
{
    server *s;
    pal_socket_t a, b;
    char buf[4096];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    s = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(s != NULL);
    a = cli(server_port(s));
    b = cli(server_port(s));

    /* subscribe on A, publish on B, push arrives on A */
    DD_CHECK(rt(s, a, "*2\r\n$10\r\nSSUBSCRIBE\r\n$3\r\nch1\r\n",
                "*3\r\n$10\r\nssubscribe\r\n$3\r\nch1\r\n:1\r\n", buf,
                sizeof(buf)));
    DD_CHECK(rt(s, b, "*3\r\n$8\r\nSPUBLISH\r\n$3\r\nch1\r\n$5\r\nhello\r\n",
                ":1\r\n", buf, sizeof(buf)));
    DD_CHECK(rt(s, a, "", "*3\r\n$8\r\nsmessage\r\n$3\r\nch1\r\n$5\r\nhello\r\n",
                buf, sizeof(buf)));

    /* publish with no subscribers -> :0 */
    DD_CHECK(rt(s, b, "*3\r\n$8\r\nSPUBLISH\r\n$3\r\nch9\r\n$1\r\nx\r\n",
                ":0\r\n", buf, sizeof(buf)));

    /* unsubscribe all (no args) */
    DD_CHECK(rt(s, a, "*1\r\n$12\r\nSUNSUBSCRIBE\r\n",
                "*3\r\n$12\r\nsunsubscribe\r\n$3\r\nch1\r\n:0\r\n", buf,
                sizeof(buf)));
    DD_CHECK(rt(s, b, "*3\r\n$8\r\nSPUBLISH\r\n$3\r\nch1\r\n$1\r\nx\r\n",
                ":0\r\n", buf, sizeof(buf)));

    pal_close(b);
    pal_close(a);
    server_destroy(s);
    pal_socket_cleanup();
}

static void test_shard_multi_and_introspection(void)
{
    server *s;
    pal_socket_t a, b;
    char buf[4096];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    s = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(s != NULL);
    a = cli(server_port(s));
    b = cli(server_port(s));

    DD_CHECK(rt(s, a,
                "*3\r\n$10\r\nSSUBSCRIBE\r\n$3\r\nch1\r\n$3\r\nch2\r\n",
                "*3\r\n$10\r\nssubscribe\r\n$3\r\nch1\r\n:1\r\n"
                "*3\r\n$10\r\nssubscribe\r\n$3\r\nch2\r\n:2\r\n",
                buf, sizeof(buf)));

    /* PUBSUB SHARDNUMSUB / SHARDCHANNELS */
    DD_CHECK(rt(s, b, "*3\r\n$6\r\nPUBSUB\r\n$11\r\nSHARDNUMSUB\r\n$3\r\nch1\r\n",
                "*2\r\n$3\r\nch1\r\n:1\r\n", buf, sizeof(buf)));
    /* unfiltered listing order is hash-dependent: accept either order */
    {
        size_t got = 0;
        uint64_t dl = pal_now_ms() + 10000;
        const char *req = "*2\r\n$6\r\nPUBSUB\r\n$13\r\nSHARDCHANNELS\r\n";
        DD_CHECK_EQ_INT((long long)strlen(req),
                        (long long)pal_send(b, req, strlen(req)));
        while (got < 22 && pal_now_ms() < dl) {
            ptrdiff_t n;
            server_run_once(s, 5);
            n = pal_recv(b, buf + got, sizeof(buf) - got - 1);
            if (n > 0)
                got += (size_t)n;
        }
        buf[got] = '\0';
        DD_CHECK(got == 22);
        DD_CHECK(memcmp(buf, "*2\r\n", 4) == 0);
        DD_CHECK(strstr(buf, "$3\r\nch1\r\n") != NULL);
        DD_CHECK(strstr(buf, "$3\r\nch2\r\n") != NULL);
    }
    /* pattern-filtered listing */
    DD_CHECK(rt(s, b,
                "*3\r\n$6\r\nPUBSUB\r\n$13\r\nSHARDCHANNELS\r\n$3\r\nc?1\r\n",
                "*1\r\n$3\r\nch1\r\n", buf, sizeof(buf)));

    /* subscribed-mode restrictions: only the pub/sub subset is allowed */
    DD_CHECK(rt(s, a, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", "-ERR Can't execute",
                buf, sizeof(buf)));
    DD_CHECK(rt(s, a, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n", buf, sizeof(buf)));

    pal_close(b);
    pal_close(a);
    server_destroy(s);
    pal_socket_cleanup();
}

/* ------------------------------------------------------------------ */
/* cluster routing: SPUBLISH must own the channel's slot               */
/* ------------------------------------------------------------------ */
#include <stdarg.h>

static void exec_sess(session *s, uint64_t now, resp_buf *out, int argc, ...)
{
    resp_value argv[10];
    va_list ap;
    int i;
    va_start(ap, argc);
    for (i = 0; i < argc; i++) {
        const char *str = va_arg(ap, const char *);
        memset(&argv[i], 0, sizeof(argv[i]));
        argv[i].type = RESP_BULK_STRING;
        argv[i].str = str;
        argv[i].len = strlen(str);
    }
    va_end(ap);
    out->len = 0;
    session_execute_at(s, argv, (size_t)argc, out, now);
}

#define EXPECT(out, s) DD_CHECK_MEM((s), strlen(s), (out).data, (out).len)

static void test_spublish_ownership(void)
{
    db d;
    session *s;
    resp_buf out;
    cluster_node *me, *other;
    uint32_t slot;
    int i;
    char exp[96];

    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);
    d.cluster_enabled = 1;
    snprintf(d.node_id, sizeof(d.node_id), "%s", TEST_ID);
    me = cluster_node_add(&d, TEST_ID);
    snprintf(me->ip, sizeof(me->ip), "127.0.0.1");
    me->port = 7777;
    me->bus_port = 17777;
    me->flags = CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER;
    other = cluster_node_add(&d, OTHER_ID);
    snprintf(other->ip, sizeof(other->ip), "10.0.0.2");
    other->port = 7002;
    other->flags = CLUSTER_NODE_MASTER;

    slot = hash_slot("chA", 3);
    cluster_slots_set(other->slots, slot, 1);
    d.slot_owner_dirty = 1;

    /* foreign slot -> MOVED */
    snprintf(exp, sizeof(exp), "-MOVED %u 10.0.0.2:7002\r\n", slot);
    exec_sess(s, T0, &out, 3, "SPUBLISH", "chA", "x");
    EXPECT(out, exp);

    /* unassigned slot -> CLUSTERDOWN */
    other->flags |= CLUSTER_NODE_DISCONNECTED;
    d.slot_owner_dirty = 1;
    exec_sess(s, T0, &out, 3, "SPUBLISH", "chA", "x");
    EXPECT(out, exp); /* MOVED still (assigned but disconnected) */

    /* own the slot -> local publish (no subscribers: :0) */
    cluster_slots_set(other->slots, slot, 0);
    cluster_slots_set(me->slots, slot, 1);
    d.slot_owner_dirty = 1;
    exec_sess(s, T0, &out, 3, "SPUBLISH", "chA", "x");
    EXPECT(out, ":0\r\n");

    /* SSUBSCRIBE is allowed on any node (no ownership check) */
    exec_sess(s, T0, &out, 2, "SSUBSCRIBE", "chA");
    DD_CHECK(out.len > 5 && out.data[0] == '*');

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
    (void)i;
}

/* ------------------------------------------------------------------ */
/* bus propagation: SPUBLISH on A reaches shard subscribers on B       */
/* ------------------------------------------------------------------ */

static void pump2(server *x, server *y)
{
    server_run_once(x, 5);
    server_run_once(y, 5);
}

/* rt() over a two-server pair so bus frames flow both ways */
static int rt2(server *x, server *y, pal_socket_t c, const char *req,
               const char *want, char *buf, size_t cap)
{
    size_t got = 0;
    uint64_t dl = pal_now_ms() + 10000;
    DD_CHECK_EQ_INT((long long)strlen(req),
                    (long long)pal_send(c, req, strlen(req)));
    while (pal_now_ms() < dl) {
        ptrdiff_t n;
        pump2(x, y);
        n = pal_recv(c, buf + got, cap - got - 1);
        if (n > 0) {
            got += (size_t)n;
            buf[got] = '\0';
            if (strstr(buf, want) != NULL)
                return 1;
        }
    }
    buf[got] = '\0';
    fprintf(stderr, "rt2 timeout: want [%s] got [%s]\n", want, buf);
    return 0;
}

static void spublish_bus_case(int proto)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char buf[4096], req[256], port[16], slotstr[16];
    uint32_t slot;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL && b != NULL);
    server_enable_cluster(a, TEST_ID);
    server_enable_cluster(b, OTHER_ID);
    server_set_bus_protocol(a, proto);
    server_set_bus_protocol(b, proto);
    ca = cli(server_port(a));
    cb = cli(server_port(b));

    /* A meets B (opens A's outbound bus conn) */
    snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
    snprintf(req, sizeof(req),
             "*4\r\n$7\r\nCLUSTER\r\n$4\r\nMEET\r\n$9\r\n127.0.0.1\r\n"
             "$%zu\r\n%s\r\n",
             strlen(port), port);
    DD_CHECK(rt2(a, b, ca, req, "+OK\r\n", buf, sizeof(buf)));

    /* A owns the channel's slot so SPUBLISH passes the ownership check */
    slot = hash_slot("busch", 5);
    snprintf(slotstr, sizeof(slotstr), "%u", (unsigned)slot);
    snprintf(req, sizeof(req),
             "*3\r\n$7\r\nCLUSTER\r\n$8\r\nADDSLOTS\r\n$%zu\r\n%s\r\n",
             strlen(slotstr), slotstr);
    DD_CHECK(rt2(a, b, ca, req, "+OK\r\n", buf, sizeof(buf)));

    /* B subscribes; A publishes (no local receivers on A) */
    DD_CHECK(rt2(a, b, cb, "*2\r\n$10\r\nSSUBSCRIBE\r\n$5\r\nbusch\r\n",
                 "*3\r\n$10\r\nssubscribe\r\n$5\r\nbusch\r\n:1\r\n", buf,
                 sizeof(buf)));
    DD_CHECK(rt2(a, b, ca,
                 "*3\r\n$8\r\nSPUBLISH\r\n$5\r\nbusch\r\n$5\r\nhello\r\n",
                 ":0\r\n", buf, sizeof(buf)));

    /* the bus frame delivers an smessage push to B's subscriber */
    DD_CHECK(rt2(a, b, cb, "",
                 "*3\r\n$8\r\nsmessage\r\n$5\r\nbusch\r\n$5\r\nhello\r\n",
                 buf, sizeof(buf)));

    pal_close(cb);
    pal_close(ca);
    server_destroy(b);
    server_destroy(a);
    pal_socket_cleanup();
}

static void test_spublish_bus_ddup(void)
{
    spublish_bus_case(SERVER_BUS_PROTOCOL_DDUP);
}

static void test_spublish_bus_redis(void)
{
    spublish_bus_case(SERVER_BUS_PROTOCOL_REDIS);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    DD_RUN(test_local_shard_pubsub);
    DD_RUN(test_shard_multi_and_introspection);
    DD_RUN(test_spublish_ownership);
    DD_RUN(test_spublish_bus_ddup);
    DD_RUN(test_spublish_bus_redis);
    return DD_TEST_SUMMARY();
}