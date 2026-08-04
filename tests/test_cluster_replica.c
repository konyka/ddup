/* test_cluster_replica.c - replica role in the cluster node table:
 * CLUSTER REPLICATE, role/master_id in nodes.conf and in v2 bus frames
 * (with v1 tolerance), role gossip convergence (wire). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/session.h"
#include "test.h"

#define T0 1000000ULL
#define TEST_ID "0123456789abcdef0123456789abcdef01234567"
#define OTHER_ID "ffffffffffffffffffffffffffffffffffffffff"
#define THIRD_ID "1111111111111111111111111111111111111111"
#define UNKNOWN_ID "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"

static void exec_sess(session *s, uint64_t now, resp_buf *out, int argc, ...)
{
    resp_value argv[16];
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

/* tiny local substring search (memmem is GNU-only) */
static const char *tstr(const char *h, size_t hlen, const char *n,
                        size_t nlen)
{
    size_t i;
    if (nlen == 0 || hlen < nlen)
        return NULL;
    for (i = 0; i + nlen <= hlen; i++)
        if (memcmp(h + i, n, nlen) == 0)
            return h + i;
    return NULL;
}

static session *fresh_session(db *d)
{
    session *s = session_create(d);
    cluster_node *me;
    d->cluster_enabled = 1;
    snprintf(d->node_id, sizeof(d->node_id), "%s", TEST_ID);
    snprintf(d->cluster_ip, sizeof(d->cluster_ip), "127.0.0.1");
    d->cluster_port = 7777;
    me = cluster_node_add(d, TEST_ID);
    snprintf(me->ip, sizeof(me->ip), "127.0.0.1");
    me->port = 7777;
    me->bus_port = 17777;
    me->flags = CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER;
    return s;
}

static cluster_node *add_node(db *d, const char *id, const char *ip,
                              uint16_t port)
{
    cluster_node *n = cluster_node_add(d, id);
    snprintf(n->ip, sizeof(n->ip), "%s", ip);
    n->port = port;
    n->bus_port = (uint16_t)(port + 10000);
    n->flags = CLUSTER_NODE_MASTER;
    return n;
}

static void test_replicate_command(void)
{
    db d;
    session *s;
    resp_buf out;
    cluster_node *me, *third;
    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);
    add_node(&d, OTHER_ID, "10.0.0.2", 7002);
    third = add_node(&d, THIRD_ID, "10.0.0.3", 7003);

    exec_sess(s, T0, &out, 3, "CLUSTER", "REPLICATE", UNKNOWN_ID);
    EXPECT(out,
           "-ERR Unknown node "
           "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\r\n");
    exec_sess(s, T0, &out, 3, "CLUSTER", "REPLICATE", TEST_ID);
    EXPECT(out, "-ERR Can't replicate myself\r\n");

    exec_sess(s, T0, &out, 3, "CLUSTER", "REPLICATE", OTHER_ID);
    EXPECT(out, "+OK\r\n");
    me = cluster_myself(&d);
    DD_CHECK(me->flags & CLUSTER_NODE_SLAVE);
    DD_CHECK(!(me->flags & CLUSTER_NODE_MASTER));
    DD_CHECK_STR(OTHER_ID, me->master_id);

    /* replicating a slave is rejected */
    third->flags &= ~(uint32_t)CLUSTER_NODE_MASTER;
    third->flags |= CLUSTER_NODE_SLAVE;
    exec_sess(s, T0, &out, 3, "CLUSTER", "REPLICATE", THIRD_ID);
    EXPECT(out, "-ERR I can only replicate a master\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_nodes_conf_slave_roundtrip(void)
{
    db d, d2;
    resp_buf buf;
    cluster_node *me, *other, *n;
    db_init(&d);
    db_init(&d2);
    cluster_nodes_init(&d);
    cluster_nodes_init(&d2);
    resp_buf_init(&buf);

    me = cluster_node_add(&d, TEST_ID);
    snprintf(me->ip, sizeof(me->ip), "127.0.0.1");
    me->port = 7777;
    me->bus_port = 17777;
    me->flags = CLUSTER_NODE_MYSELF | CLUSTER_NODE_SLAVE;
    snprintf(me->master_id, sizeof(me->master_id), "%s", OTHER_ID);
    other = cluster_node_add(&d, OTHER_ID);
    snprintf(other->ip, sizeof(other->ip), "10.0.0.2");
    other->port = 7002;
    other->bus_port = 17002;
    other->flags = CLUSTER_NODE_MASTER;
    cluster_slots_set(other->slots, 0, 1);
    cluster_slots_set(other->slots, 1, 1);

    DD_CHECK_EQ_INT(0, cluster_nodes_render(&d, &buf));
    DD_CHECK(tstr(buf.data, buf.len, "myself,slave", 12) != NULL);
    DD_CHECK(tstr(buf.data, buf.len, OTHER_ID, 40) != NULL);

    {
        /* parse both lines back (line 1: myself slave, line 2: master) */
        const char *nl = memchr(buf.data, '\n', buf.len);
        DD_CHECK(nl != NULL);
        DD_CHECK_EQ_INT(0, cluster_nodes_parse_line(
                                 &d2, buf.data, (size_t)(nl - buf.data)));
        DD_CHECK_EQ_INT(0,
                        cluster_nodes_parse_line(&d2, nl + 1,
                                                 buf.len -
                                                     (size_t)(nl + 1 -
                                                              buf.data) -
                                                     1));
    }
    n = cluster_node_find(&d2, TEST_ID);
    DD_CHECK(n != NULL);
    DD_CHECK(n->flags & CLUSTER_NODE_SLAVE);
    DD_CHECK(!(n->flags & CLUSTER_NODE_MASTER));
    DD_CHECK_STR(OTHER_ID, n->master_id);
    n = cluster_node_find(&d2, OTHER_ID);
    DD_CHECK(n != NULL);
    DD_CHECK(n->flags & CLUSTER_NODE_MASTER);
    DD_CHECK_STR("-", n->master_id);
    DD_CHECK_EQ_INT(1, cluster_slots_get(n->slots, 1));

    resp_buf_free(&buf);
    db_destroy(&d2);
    db_destroy(&d);
}

static void put16t(char *p, uint16_t v)
{
    p[0] = (char)(v & 0xFFu);
    p[1] = (char)((v >> 8) & 0xFFu);
}

static void put32t(char *p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++)
        p[i] = (char)((v >> (8 * i)) & 0xFFu);
}

/* hand-build a v1 ("RCMB") PING frame: sender ID9, master, no gossip */
static size_t build_v1_frame(char *out)
{
    static const char ID9[] = "9999999999999999999999999999999999999999";
    char *p = out;
    memcpy(p, "RCMB", 4);
    p += 4;
    p += 4; /* totlen patched below */
    put16t(p, CLUSTER_MSG_PING);
    p += 2;
    memcpy(p, ID9, 40);
    p += 40;
    put16t(p, 9);
    p += 2;
    memcpy(p, "127.0.0.1", 9);
    p += 9;
    put16t(p, 7009);
    p += 2;
    put16t(p, 17009);
    p += 2;
    put32t(p, CLUSTER_NODE_MASTER);
    p += 4;
    memset(p, 0, 2048);
    p += 2048;
    put16t(p, 0); /* gossip count */
    p += 2;
    put32t(out + 4, (uint32_t)(p - out));
    return (size_t)(p - out);
}

static void test_frame_v2_and_v1_tolerance(void)
{
    db d1, d2;
    resp_buf frame, reply;
    cluster_node *me, *n;
    char v1[4096];
    size_t v1len;

    db_init(&d1);
    db_init(&d2);
    cluster_nodes_init(&d1);
    cluster_nodes_init(&d2);
    resp_buf_init(&frame);
    resp_buf_init(&reply);

    /* v2 frame carries role + master_id */
    me = cluster_node_add(&d1, TEST_ID);
    snprintf(me->ip, sizeof(me->ip), "127.0.0.1");
    me->port = 7777;
    me->bus_port = 17777;
    me->flags = CLUSTER_NODE_MYSELF | CLUSTER_NODE_SLAVE;
    snprintf(me->master_id, sizeof(me->master_id), "%s", OTHER_ID);

    cluster_bus_build_frame(&d1, CLUSTER_MSG_PING, &frame);
    DD_CHECK(frame.len > 10 && memcmp(frame.data, "RCM2", 4) == 0);
    DD_CHECK_EQ_INT(0, cluster_bus_handle_frame(&d2, frame.data, frame.len,
                                                &reply, T0));
    n = cluster_node_find(&d2, TEST_ID);
    DD_CHECK(n != NULL);
    DD_CHECK(n->flags & CLUSTER_NODE_SLAVE);
    DD_CHECK_STR(OTHER_ID, n->master_id);

    /* v1 frames still parse: no master_id -> master, "-" */
    reply.len = 0;
    v1len = build_v1_frame(v1);
    DD_CHECK_EQ_INT(0, cluster_bus_handle_frame(&d2, v1, v1len, &reply, T0));
    n = cluster_node_find(&d2, "9999999999999999999999999999999999999999");
    DD_CHECK(n != NULL);
    DD_CHECK(n->flags & CLUSTER_NODE_MASTER);
    DD_CHECK_STR("-", n->master_id);

    resp_buf_free(&reply);
    resp_buf_free(&frame);
    db_destroy(&d2);
    db_destroy(&d1);
}

/* ------------------------------------------------------------------ */
/* wire: role gossip convergence across 3 servers                     */
/* ------------------------------------------------------------------ */
#include "pal/pal_socket.h"
#include "server/server.h"

#define IDA "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define IDB "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define IDC "cccccccccccccccccccccccccccccccccccccccc"

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

static void fmt_cmd(char *buf, size_t cap, int argc, ...)
{
    va_list ap;
    size_t off = 0;
    int i;
    off += (size_t)snprintf(buf + off, cap - off, "*%d\r\n", argc);
    va_start(ap, argc);
    for (i = 0; i < argc; i++) {
        const char *a = va_arg(ap, const char *);
        off += (size_t)snprintf(buf + off, cap - off, "$%zu\r\n%s\r\n",
                                strlen(a), a);
    }
    va_end(ap);
}

static void test_wire_role_gossip(void)
{
    server *a, *b, *c;
    pal_socket_t ca, cb, cc;
    char req[512], buf[4096], port[16];
    int i, ok;

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
    cc = cli(server_port(c));

    fmt_cmd(req, sizeof(req), 4, "CLUSTER", "MEET", "127.0.0.1",
            (snprintf(port, sizeof(port), "%u", (unsigned)server_port(b)),
             port));
    ask3(a, b, c, ca, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);
    fmt_cmd(req, sizeof(req), 4, "CLUSTER", "MEET", "127.0.0.1",
            (snprintf(port, sizeof(port), "%u", (unsigned)server_port(c)),
             port));
    ask3(a, b, c, cb, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);

    /* B replicates A: cluster role + data replication both kick in */
    fmt_cmd(req, sizeof(req), 3, "CLUSTER", "REPLICATE", IDA);
    ask3(a, b, c, cb, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);

    /* A and C learn B's slave role (bounded polls) */
    ok = 0;
    for (i = 0; i < 600 && !ok; i++) {
        pump3(a, b, c);
        if (i % 50 == 0) {
            ask3(a, b, c, ca, "*2\r\n$7\r\nCLUSTER\r\n$5\r\nNODES\r\n", buf,
                 sizeof(buf));
            ask3(a, b, c, cc, "*2\r\n$7\r\nCLUSTER\r\n$5\r\nNODES\r\n", req,
                 sizeof(req));
            if (strstr(buf, "slave") != NULL && strstr(req, "slave") != NULL)
                ok = 1;
        }
    }
    DD_CHECK_EQ_INT(1, ok);

    /* data replication was wired automatically (INFO replication) */
    ok = 0;
    for (i = 0; i < 600 && !ok; i++) {
        pump3(a, b, c);
        if (i % 50 == 0) {
            ask3(a, b, c, cb, "*1\r\n$4\r\nINFO\r\n", buf, sizeof(buf));
            if (strstr(buf, "role:slave\r\n") != NULL &&
                strstr(buf, "master_link_status:up\r\n") != NULL)
                ok = 1;
        }
    }
    DD_CHECK_EQ_INT(1, ok);

    pal_close(cc);
    pal_close(cb);
    pal_close(ca);
    server_destroy(c);
    server_destroy(b);
    server_destroy(a);
    pal_socket_cleanup();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0); /* progress visible under timeout */
    DD_RUN(test_replicate_command);
    DD_RUN(test_nodes_conf_slave_roundtrip);
    DD_RUN(test_frame_v2_and_v1_tolerance);
    DD_RUN(test_wire_role_gossip);
    return DD_TEST_SUMMARY();
}
