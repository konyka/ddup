/* test_cluster_bus.c - ddup cluster bus protocol v1: frames, handler,
 * multi-server gossip convergence and failure detection. */
#include <stdio.h>
#include <string.h>

#include "core/cluster.h"
#include "core/command.h"
#include "test.h"

#define ID1 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define ID2 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define ID3 "cccccccccccccccccccccccccccccccccccccccc"
#define T0 1000000ULL

static void make_node(db *d, const char *id, const char *ip, uint16_t port,
                      uint32_t flags, int full_slots)
{
    cluster_node *n = cluster_node_add(d, id);
    snprintf(n->ip, sizeof(n->ip), "%s", ip);
    n->port = port;
    n->bus_port = (uint16_t)(port + 10000);
    n->flags = flags;
    if (full_slots)
        memset(n->slots, 0xFF, sizeof(n->slots));
}

static void test_frame_roundtrip(void)
{
    db d, d2;
    resp_buf frame, reply;
    db_init(&d);
    db_init(&d2);
    resp_buf_init(&frame);
    resp_buf_init(&reply);
    cluster_nodes_init(&d);
    cluster_nodes_init(&d2);

    make_node(&d, ID1, "127.0.0.1", 7001,
              CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER, 1);
    make_node(&d, ID2, "127.0.0.1", 7002, CLUSTER_NODE_MASTER, 0);
    cluster_slots_set(cluster_node_find(&d, ID2)->slots, 5, 1);

    cluster_bus_build_frame(&d, CLUSTER_MSG_MEET, &frame);
    DD_CHECK(frame.len > 10 && frame.len <= CLUSTER_MSG_MAX);
    DD_CHECK(memcmp(frame.data, "RCMB", 4) == 0);

    /* receiver side: unknown sender -> node created, PONG produced */
    DD_CHECK_EQ_INT(0, cluster_bus_handle_frame(&d2, frame.data, frame.len,
                                                &reply, T0));
    DD_CHECK(cluster_node_find(&d2, ID1) != NULL);
    /* gossip entry carried ID2 too */
    DD_CHECK(cluster_node_find(&d2, ID2) != NULL);
    DD_CHECK(cluster_slots_get(cluster_node_find(&d2, ID2)->slots, 5) == 1);
    /* MEET -> handshake cleared on receiver's view of the sender */
    DD_CHECK((cluster_node_find(&d2, ID1)->flags &
              CLUSTER_NODE_HANDSHAKE) == 0);
    DD_CHECK(reply.len > 10 && memcmp(reply.data, "RCMB", 4) == 0);

    /* PONG back: type 2, updates last_seen on the original sender */
    {
        uint16_t type;
        memcpy(&type, reply.data + 8, 2);
        DD_CHECK_EQ_INT(CLUSTER_MSG_PONG, type);
    }
    {
        resp_buf none;
        resp_buf_init(&none);
        make_node(&d2, ID1, "127.0.0.1", 7001,
                  CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER, 1);
        DD_CHECK_EQ_INT(0, cluster_bus_handle_frame(&d2, reply.data,
                                                    reply.len, &none, T0 + 1));
        DD_CHECK(none.len == 0); /* no reply to a PONG */
        resp_buf_free(&none);
    }

    /* malformed frames are rejected */
    DD_CHECK_EQ_INT(-1, cluster_bus_handle_frame(&d2, "garbage", 7, &reply,
                                                 T0));
    DD_CHECK_EQ_INT(-1, cluster_bus_handle_frame(&d2, frame.data, 9, &reply,
                                                 T0));

    resp_buf_free(&reply);
    resp_buf_free(&frame);
    db_destroy(&d2);
    db_destroy(&d);
}

static void test_meet_convergence(void);
static void test_gossip_carry(void);
static void test_fail_detect(void);
static void test_moved_wire(void);

int main(void)
{
    DD_RUN(test_frame_roundtrip);
    DD_RUN(test_meet_convergence);
    DD_RUN(test_gossip_carry);
    DD_RUN(test_fail_detect);
    DD_RUN(test_moved_wire);
    return DD_TEST_SUMMARY();
}

/* ------------------------------------------------------------------ */
/* multi-server gossip: MEET, convergence, failure detection          */
/* ------------------------------------------------------------------ */
#include <stdlib.h>

#include "pal/pal_socket.h"
#include "server/server.h"

#define IDA "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define IDB "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define IDC "cccccccccccccccccccccccccccccccccccccccc"

static void pump2(server *x, server *y)
{
    server_run_once(x, 5);
    server_run_once(y, 5);
}

static void pump3(server *x, server *y, server *z)
{
    server_run_once(x, 5);
    server_run_once(y, 5);
    server_run_once(z, 5);
}

static pal_socket_t cli(uint16_t port)
{
    pal_socket_t c = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(c != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));
    return c;
}

/* send a command, pump two servers, read the whole reply into buf */
static size_t ask2(server *x, server *y, pal_socket_t c, const char *req,
                   char *buf, size_t cap)
{
    size_t got = 0;
    int iter = 0;
    DD_CHECK_EQ_INT((long long)strlen(req),
                    (long long)pal_send(c, req, strlen(req)));
    while (iter < 2000) {
        ptrdiff_t n;
        iter++;
        pump2(x, y);
        n = pal_recv(c, buf + got, cap - got - 1);
        if (n > 0)
            got += (size_t)n;
        if (n <= 0 && got > 0)
            break;
        if (n > 0)
            break; /* full reply for single command */
    }
    buf[got] = '\0';
    return got;
}

static size_t ask3(server *x, server *y, server *z, pal_socket_t c,
                   const char *req, char *buf, size_t cap)
{
    size_t got = 0;
    int iter = 0;
    DD_CHECK_EQ_INT((long long)strlen(req),
                    (long long)pal_send(c, req, strlen(req)));
    while (iter < 2000) {
        ptrdiff_t n;
        iter++;
        pump3(x, y, z);
        n = pal_recv(c, buf + got, cap - got - 1);
        if (n > 0)
            got += (size_t)n;
        if (n > 0)
            break;
    }
    buf[got] = '\0';
    return got;
}

static void test_meet_convergence(void)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char buf[4096], req[128];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL && b != NULL);
    server_enable_cluster(a, IDA);
    server_enable_cluster(b, IDB);
    ca = cli(server_port(a));
    cb = cli(server_port(b));

    {
        char port[16];
        size_t pl;
        snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
        pl = strlen(port);
        snprintf(req, sizeof(req),
                 "*4\r\n$7\r\nCLUSTER\r\n$4\r\nMEET\r\n$9\r\n127.0.0.1\r\n"
                 "$%zu\r\n%s\r\n",
                 pl, port);
    }
    ask2(a, b, ca, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);

    /* both sides learn each other (bounded polls) */
    {
        int ok = 0, i;
        for (i = 0; i < 400 && !ok; i++) {
            pump2(a, b);
            if (i % 40 == 0) {
                ask2(a, b, ca, "*2\r\n$7\r\nCLUSTER\r\n$5\r\nNODES\r\n", buf,
                     sizeof(buf));
                ask2(a, b, cb, "*2\r\n$7\r\nCLUSTER\r\n$5\r\nNODES\r\n",
                     req, sizeof(req));
                if (strstr(buf, IDB) != NULL && strstr(req, IDA) != NULL)
                    ok = 1;
            }
        }
        DD_CHECK_EQ_INT(1, ok);
    }
    /* fresh-boot semantics: state is fail until slots are assigned */
    ask2(a, b, ca, "*2\r\n$7\r\nCLUSTER\r\n$4\r\nINFO\r\n", buf, sizeof(buf));
    DD_CHECK(strstr(buf, "cluster_state:fail\r\n") != NULL);

    /* assign the full range to A (chunked ADDSLOTS commands); gossip
     * carries the bitmap to B */
    {
        int base, okc = 0;
        for (base = 0; base < 16384 && okc < 8; base += 2048) {
            size_t bl = 0, off = 0, got = 0;
            int sl, iter = 0;
            char *big = malloc(32768);
            DD_CHECK(big != NULL);
            bl += (size_t)snprintf(big + bl, 32768 - bl,
                                   "*2050\r\n$7\r\nCLUSTER\r\n$8\r\nADDSLOTS\r\n");
            for (sl = base; sl < base + 2048; sl++) {
                char num[8];
                int nl = snprintf(num, sizeof(num), "%d", sl);
                bl += (size_t)snprintf(big + bl, 32768 - bl, "$%d\r\n%s\r\n",
                                       nl, num);
            }
            while (off < bl) {
                ptrdiff_t w = pal_send(ca, big + off, bl - off);
                if (w > 0)
                    off += (size_t)w;
                else
                    pump2(a, b);
            }
            while (got < 5 && iter < 2000) {
                ptrdiff_t n;
                iter++;
                pump2(a, b);
                n = pal_recv(ca, buf + got, sizeof(buf) - got);
                if (n > 0)
                    got += (size_t)n;
            }
            DD_CHECK(got >= 5 && memcmp(buf, "+OK\r\n", 5) == 0);
            free(big);
            okc++;
        }
        DD_CHECK_EQ_INT(8, okc);
    }

    /* CLUSTER INFO reflects 2 known nodes, state ok (slots gossiped over) */
    {
        int ok = 0, i;
        for (i = 0; i < 400 && !ok; i++) {
            pump2(a, b);
            if (i % 40 == 0) {
                ask2(a, b, cb, "*2\r\n$7\r\nCLUSTER\r\n$4\r\nINFO\r\n", req,
                     sizeof(req));
                if (strstr(req, "cluster_state:ok\r\n") != NULL)
                    ok = 1;
            }
        }
        DD_CHECK_EQ_INT(1, ok);
    }
    ask2(a, b, ca, "*2\r\n$7\r\nCLUSTER\r\n$4\r\nINFO\r\n", buf, sizeof(buf));
    DD_CHECK(strstr(buf, "cluster_known_nodes:2\r\n") != NULL);
    DD_CHECK(strstr(buf, "cluster_state:ok\r\n") != NULL);

    pal_close(cb);
    pal_close(ca);
    server_destroy(b);
    server_destroy(a);
    pal_socket_cleanup();
}

static void test_gossip_carry(void)
{
    server *a, *b, *c;
    pal_socket_t ca, cb;
    char buf[4096], req[128];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    c = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL && b != NULL && c != NULL);
    server_enable_cluster(a, IDA);
    server_enable_cluster(b, IDB);
    server_enable_cluster(c, IDC);
    ca = cli(server_port(a));
    cb = cli(server_port(b));

    /* A meets B; B meets C */
    {
        char port[16];
        size_t pl;
        snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
        pl = strlen(port);
        snprintf(req, sizeof(req),
                 "*4\r\n$7\r\nCLUSTER\r\n$4\r\nMEET\r\n$9\r\n127.0.0.1\r\n"
                 "$%zu\r\n%s\r\n",
                 pl, port);
        ask3(a, b, c, ca, req, buf, sizeof(buf));
        snprintf(port, sizeof(port), "%u", (unsigned)server_port(c));
        pl = strlen(port);
        snprintf(req, sizeof(req),
                 "*4\r\n$7\r\nCLUSTER\r\n$4\r\nMEET\r\n$9\r\n127.0.0.1\r\n"
                 "$%zu\r\n%s\r\n",
                 pl, port);
        ask3(a, b, c, cb, req, buf, sizeof(buf));
        DD_CHECK_STR("+OK\r\n", buf);
    }

    /* A learns C via gossip carried by B */
    {
        int ok = 0, i;
        for (i = 0; i < 600 && !ok; i++) {
            pump3(a, b, c);
            if (i % 50 == 0) {
                ask3(a, b, c, ca, "*2\r\n$7\r\nCLUSTER\r\n$5\r\nNODES\r\n",
                     buf, sizeof(buf));
                if (strstr(buf, IDC) != NULL)
                    ok = 1;
            }
        }
        DD_CHECK_EQ_INT(1, ok);
    }

    pal_close(cb);
    pal_close(ca);
    server_destroy(c);
    server_destroy(b);
    server_destroy(a);
    pal_socket_cleanup();
}

static void test_fail_detect(void)
{
    server *a, *b;
    pal_socket_t ca;
    char buf[4096], req[128];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL && b != NULL);
    server_enable_cluster(a, IDA);
    server_enable_cluster(b, IDB);
    server_set_node_timeout(a, 200);
    ca = cli(server_port(a));

    {
        char port[16];
        size_t pl;
        snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
        pl = strlen(port);
        snprintf(req, sizeof(req),
                 "*4\r\n$7\r\nCLUSTER\r\n$4\r\nMEET\r\n$9\r\n127.0.0.1\r\n"
                 "$%zu\r\n%s\r\n",
                 pl, port);
        ask2(a, b, ca, req, buf, sizeof(buf));
        DD_CHECK_STR("+OK\r\n", buf);
    }

    /* let them exchange a couple of pings */
    {
        int i;
        for (i = 0; i < 60; i++)
            pump2(a, b);
    }

    /* B goes down: A marks it disconnected -> state fail (B holds slots) */
    pal_close(ca);
    server_destroy(b);
    ca = cli(server_port(a));
    {
        int ok = 0, i;
        for (i = 0; i < 400 && !ok; i++) {
            server_run_once(a, 5);
            if (i % 20 == 0) {
                ask2(a, a, ca, "*2\r\n$7\r\nCLUSTER\r\n$4\r\nINFO\r\n", buf,
                     sizeof(buf));
                if (strstr(buf, "cluster_state:fail\r\n") != NULL)
                    ok = 1;
            }
        }
        DD_CHECK_EQ_INT(1, ok);
    }

    pal_close(ca);
    server_destroy(a);
    pal_socket_cleanup();
}

static void test_moved_wire(void)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char buf[4096], req[256], exp[128];
    int base;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL && b != NULL);
    server_enable_cluster(a, IDA);
    server_enable_cluster(b, IDB);
    ca = cli(server_port(a));
    cb = cli(server_port(b));

    /* MEET first so the tables know each other */
    {
        char port[16];
        size_t pl;
        snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
        pl = strlen(port);
        snprintf(req, sizeof(req),
                 "*4\r\n$7\r\nCLUSTER\r\n$4\r\nMEET\r\n$9\r\n127.0.0.1\r\n"
                 "$%zu\r\n%s\r\n",
                 pl, port);
        ask2(a, b, ca, req, buf, sizeof(buf));
        DD_CHECK_STR("+OK\r\n", buf);
    }

    /* A: 0..8191, B: 8192..16383 via chunked ADDSLOTS */
    for (base = 0; base < 16384; base += 2048) {
        pal_socket_t ct = base < 8192 ? ca : cb;
        size_t bl = 0, off = 0, got = 0;
        int sl, iter = 0;
        char *big = malloc(32768);
        DD_CHECK(big != NULL);
        bl += (size_t)snprintf(big + bl, 32768 - bl,
                               "*2050\r\n$7\r\nCLUSTER\r\n$8\r\nADDSLOTS\r\n");
        for (sl = base; sl < base + 2048; sl++) {
            char num[8];
            int nl = snprintf(num, sizeof(num), "%d", sl);
            bl += (size_t)snprintf(big + bl, 32768 - bl, "$%d\r\n%s\r\n", nl,
                                   num);
        }
        while (off < bl) {
            ptrdiff_t w = pal_send(ct, big + off, bl - off);
            if (w > 0)
                off += (size_t)w;
            else
                pump2(a, b);
        }
        while (got < 5 && iter < 2000) {
            ptrdiff_t n;
            iter++;
            pump2(a, b);
            n = pal_recv(ct, buf + got, sizeof(buf) - got);
            if (n > 0)
                got += (size_t)n;
        }
        DD_CHECK(got >= 5 && memcmp(buf, "+OK\r\n", 5) == 0);
        free(big);
    }

    /* wait for gossip convergence of the assignments on both sides */
    {
        int ok = 0, i;
        for (i = 0; i < 400 && !ok; i++) {
            pump2(a, b);
            if (i % 40 == 0) {
                ask2(a, b, cb, "*2\r\n$7\r\nCLUSTER\r\n$4\r\nINFO\r\n", req,
                     sizeof(req));
                if (strstr(req, "cluster_state:ok\r\n") != NULL)
                    ok = 1;
            }
        }
        DD_CHECK_EQ_INT(1, ok);
    }

    /* SET foo (slot 12182, B's) on A -> MOVED to B's ip:port */
    snprintf(exp, sizeof(exp), "-MOVED 12182 127.0.0.1:%u\r\n",
             (unsigned)server_port(b));
    ask2(a, b, ca, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$1\r\nv\r\n", buf,
         sizeof(buf));
    DD_CHECK_MEM(exp, strlen(exp), buf, strlen(buf));

    /* SET foo on B works (B owns that slot) */
    ask2(a, b, cb, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$1\r\nv\r\n", buf,
         sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);
    ask2(a, b, cb, "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n", buf, sizeof(buf));
    DD_CHECK_STR("$1\r\nv\r\n", buf);

    /* unassigned slot -> CLUSTERDOWN: delete B's slot 12182, then write */
    ask2(a, b, cb, "*3\r\n$7\r\nCLUSTER\r\n$8\r\nDELSLOTS\r\n$5\r\n12182\r\n",
         buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);
    {
        int ok = 0, i;
        for (i = 0; i < 400 && !ok; i++) {
            pump2(a, b);
            if (i % 40 == 0) {
                ask2(a, b, ca, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$1\r\nx\r\n",
                     buf, sizeof(buf));
                if (strstr(buf, "CLUSTERDOWN") != NULL)
                    ok = 1;
            }
        }
        DD_CHECK_EQ_INT(1, ok);
    }

    pal_close(cb);
    pal_close(ca);
    server_destroy(b);
    server_destroy(a);
    pal_socket_cleanup();
}
