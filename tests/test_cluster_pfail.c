/* test_cluster_pfail.c - PFAIL suspicion state, failure reports with a
 * validity window, gossip report wiring, and PFAIL-vs-FAIL semantics in
 * cluster_state evaluation. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/cluster.h"
#include "core/command.h"
#include "core/redbus.h"
#include "core/session.h"
#include "test.h"

#define T0 1000000ULL
#define ME "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define NB "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define NC "cccccccccccccccccccccccccccccccccccccccc"
#define NX "dddddddddddddddddddddddddddddddddddddddd"

static cluster_node *mknode(db *d, const char *id, uint32_t flags)
{
    cluster_node *n = cluster_node_add(d, id);
    snprintf(n->ip, sizeof(n->ip), "127.0.0.1");
    n->port = 7001;
    n->bus_port = 17001;
    n->flags = flags;
    return n;
}

/* ------------------------------------------------------------------ */
/* failure reports: add / refresh / expire / heal                     */
/* ------------------------------------------------------------------ */
static void test_report_window(void)
{
    db d;
    cluster_node *subj;
    db_init(&d);
    cluster_nodes_init(&d);
    d.cluster_node_timeout_ms = 1000; /* validity window: 2000 ms */
    subj = mknode(&d, NB, CLUSTER_NODE_MASTER);

    /* a fresh report counts; past timeout*2 it expires */
    cluster_report_failure(&d, subj, NC, T0);
    DD_CHECK_EQ_INT(1, cluster_report_count(&d, subj, T0 + 1999));
    DD_CHECK_EQ_INT(0, cluster_report_count(&d, subj, T0 + 2001));

    /* a refresh from the same reporter does not duplicate, and the new
     * timestamp extends the window */
    cluster_report_failure(&d, subj, NC, T0 + 500);
    cluster_report_failure(&d, subj, NC, T0 + 900);
    DD_CHECK_EQ_INT(1, cluster_report_count(&d, subj, T0 + 900 + 1999));
    DD_CHECK_EQ_INT(0, cluster_report_count(&d, subj, T0 + 900 + 2001));

    /* distinct reporters add up; heal retracts one reporter only */
    cluster_report_failure(&d, subj, NC, T0 + 4000);
    cluster_report_failure(&d, subj, ME, T0 + 4000);
    DD_CHECK_EQ_INT(2, cluster_report_count(&d, subj, T0 + 4000));
    cluster_report_heal(&d, subj, ME);
    DD_CHECK_EQ_INT(1, cluster_report_count(&d, subj, T0 + 4000));
    cluster_report_heal(&d, subj, NC);
    DD_CHECK_EQ_INT(0, cluster_report_count(&d, subj, T0 + 4000));

    db_destroy(&d);
}

/* ------------------------------------------------------------------ */
/* gossip wiring: a PFAIL/FAIL flag about a known node in a master's  */
/* gossip section becomes a failure report from the frame sender      */
/* ------------------------------------------------------------------ */
static void test_gossip_pfail_report(void)
{
    db da, db_, dc;
    resp_buf frame, reply;
    cluster_node *a_in_c;
    db_init(&da);
    db_init(&db_);
    db_init(&dc);
    cluster_nodes_init(&da);
    cluster_nodes_init(&db_);
    cluster_nodes_init(&dc);
    resp_buf_init(&frame);
    resp_buf_init(&reply);
    dc.cluster_node_timeout_ms = 1000;

    mknode(&da, ME, CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    mknode(&db_, NB, CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    mknode(&db_, ME, CLUSTER_NODE_MASTER | CLUSTER_NODE_PFAIL);
    mknode(&dc, NC, CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    mknode(&dc, ME, CLUSTER_NODE_MASTER);
    mknode(&dc, NB, CLUSTER_NODE_MASTER);

    /* B pings C, gossiping "A is PFAIL" -> C records a report from B */
    cluster_bus_build_frame(&db_, CLUSTER_MSG_PING, &frame);
    DD_CHECK_EQ_INT(0, cluster_bus_handle_frame(&dc, frame.data, frame.len,
                                                &reply, T0));
    a_in_c = cluster_node_find(&dc, ME);
    DD_CHECK(a_in_c != NULL);
    DD_CHECK_EQ_INT(1, cluster_report_count(&dc, a_in_c, T0));
    /* suspicion of others does not taint C's own view of A */
    DD_CHECK((a_in_c->flags &
              (CLUSTER_NODE_PFAIL | CLUSTER_NODE_FAIL)) == 0);

    /* B's next gossip shows A healthy -> the report is retracted */
    cluster_node_find(&db_, ME)->flags = CLUSTER_NODE_MASTER;
    frame.len = 0;
    reply.len = 0;
    cluster_bus_build_frame(&db_, CLUSTER_MSG_PING, &frame);
    DD_CHECK_EQ_INT(0, cluster_bus_handle_frame(&dc, frame.data, frame.len,
                                                &reply, T0 + 10));
    DD_CHECK_EQ_INT(0, cluster_report_count(&dc, a_in_c, T0 + 10));

    /* reports only count from masters: a slave sender is ignored */
    cluster_node_find(&db_, NB)->flags =
        CLUSTER_NODE_MYSELF | CLUSTER_NODE_SLAVE;
    cluster_node_find(&dc, NB)->flags = CLUSTER_NODE_SLAVE;
    cluster_node_find(&db_, ME)->flags =
        CLUSTER_NODE_MASTER | CLUSTER_NODE_PFAIL;
    frame.len = 0;
    reply.len = 0;
    cluster_bus_build_frame(&db_, CLUSTER_MSG_PING, &frame);
    DD_CHECK_EQ_INT(0, cluster_bus_handle_frame(&dc, frame.data, frame.len,
                                                &reply, T0 + 20));
    DD_CHECK_EQ_INT(0,
                    cluster_report_count(&dc, cluster_node_find(&dc, ME),
                                         T0 + 20));

    resp_buf_free(&reply);
    resp_buf_free(&frame);
    db_destroy(&dc);
    db_destroy(&db_);
    db_destroy(&da);
}

/* ------------------------------------------------------------------ */
/* a direct frame from a node clears PFAIL/FAIL/DISCONNECTED on it    */
/* ------------------------------------------------------------------ */
static void test_direct_frame_clears_flags(void)
{
    db da, db_;
    resp_buf frame, reply;
    cluster_node *a_in_b;
    db_init(&da);
    db_init(&db_);
    cluster_nodes_init(&da);
    cluster_nodes_init(&db_);
    resp_buf_init(&frame);
    resp_buf_init(&reply);

    mknode(&da, ME, CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    mknode(&db_, NB, CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    mknode(&db_, ME,
           CLUSTER_NODE_MASTER | CLUSTER_NODE_PFAIL | CLUSTER_NODE_FAIL |
               CLUSTER_NODE_DISCONNECTED);

    cluster_bus_build_frame(&da, CLUSTER_MSG_PING, &frame);
    DD_CHECK_EQ_INT(0, cluster_bus_handle_frame(&db_, frame.data, frame.len,
                                                &reply, T0));
    a_in_b = cluster_node_find(&db_, ME);
    DD_CHECK(a_in_b != NULL);
    DD_CHECK((a_in_b->flags &
              (CLUSTER_NODE_PFAIL | CLUSTER_NODE_FAIL |
               CLUSTER_NODE_DISCONNECTED)) == 0);

    resp_buf_free(&reply);
    resp_buf_free(&frame);
    db_destroy(&db_);
    db_destroy(&da);
}

/* ------------------------------------------------------------------ */
/* cluster_state: PFAIL alone keeps state ok; FAIL fails it           */
/* ------------------------------------------------------------------ */
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

static void test_state_pfail_vs_fail(void)
{
    db d;
    session *s;
    resp_buf out;
    cluster_node *me, *x;
    uint32_t sl;

    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);
    d.cluster_enabled = 1;
    me = mknode(&d, ME, CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    x = mknode(&d, NB, CLUSTER_NODE_MASTER);
    for (sl = 0; sl < 100; sl++)
        cluster_slots_set(me->slots, sl, 1);
    for (sl = 100; sl < 16384; sl++)
        cluster_slots_set(x->slots, sl, 1);
    d.slot_owner_dirty = 1;

    exec_sess(s, T0, &out, 2, "CLUSTER", "INFO");
    DD_CHECK(strstr(out.data, "cluster_state:ok\r\n") != NULL);

    /* suspicion (even with the link-state bit) does not fail the state */
    x->flags |= CLUSTER_NODE_PFAIL | CLUSTER_NODE_DISCONNECTED;
    exec_sess(s, T0, &out, 2, "CLUSTER", "INFO");
    DD_CHECK(strstr(out.data, "cluster_state:ok\r\n") != NULL);

    /* objective FAIL on a slot holder does */
    x->flags |= CLUSTER_NODE_FAIL;
    exec_sess(s, T0, &out, 2, "CLUSTER", "INFO");
    DD_CHECK(strstr(out.data, "cluster_state:fail\r\n") != NULL);

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

/* ------------------------------------------------------------------ */
/* quorum promotion: PFAIL -> FAIL on a majority of slot-serving      */
/* masters (reports + myself), only while we suspect the node locally */
/* ------------------------------------------------------------------ */
static void test_quorum_promotion(void)
{
    db d;
    cluster_node *me, *b, *c, *x;

    db_init(&d);
    cluster_nodes_init(&d);
    d.cluster_node_timeout_ms = 1000;
    me = mknode(&d, ME, CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    b = mknode(&d, NB, CLUSTER_NODE_MASTER);
    c = mknode(&d, NC, CLUSTER_NODE_MASTER);
    x = mknode(&d, NX, CLUSTER_NODE_MASTER);
    cluster_slots_set(me->slots, 0, 1);
    cluster_slots_set(b->slots, 1, 1);
    cluster_slots_set(c->slots, 2, 1);
    cluster_slots_set(x->slots, 3, 1);
    /* 4 slot-serving masters -> quorum 3 */

    /* reports alone do not promote: local suspicion is required */
    cluster_report_failure(&d, x, NB, T0);
    cluster_report_failure(&d, x, NC, T0);
    DD_CHECK_EQ_INT(0, cluster_mark_fail_if_quorum(&d, x, T0));
    DD_CHECK(!(x->flags & CLUSTER_NODE_FAIL));

    /* local PFAIL + 2 reports + myself = 3 >= 3 -> FAIL, broadcast due */
    x->flags |= CLUSTER_NODE_PFAIL;
    DD_CHECK_EQ_INT(1, cluster_mark_fail_if_quorum(&d, x, T0));
    DD_CHECK(x->flags & CLUSTER_NODE_FAIL);
    DD_CHECK(!(x->flags & CLUSTER_NODE_PFAIL));
    DD_CHECK_STR(NX, d.fail_broadcast_id);

    /* already FAIL: no re-trigger */
    d.fail_broadcast_id[0] = '\0';
    DD_CHECK_EQ_INT(0, cluster_mark_fail_if_quorum(&d, x, T0));
    DD_CHECK(d.fail_broadcast_id[0] == '\0');

    db_destroy(&d);

    /* 2-master cluster: quorum 2, our own suspicion alone is a deadlock
     * (documented; Redis needs >= 3 masters for automatic FAIL) */
    db_init(&d);
    cluster_nodes_init(&d);
    d.cluster_node_timeout_ms = 1000;
    me = mknode(&d, ME, CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    x = mknode(&d, NX, CLUSTER_NODE_MASTER);
    cluster_slots_set(me->slots, 0, 1);
    cluster_slots_set(x->slots, 1, 1);
    x->flags |= CLUSTER_NODE_PFAIL;
    DD_CHECK_EQ_INT(0, cluster_mark_fail_if_quorum(&d, x, T0));
    /* one report from the other master completes the majority */
    cluster_report_failure(&d, x, NX, T0);
    DD_CHECK_EQ_INT(1, cluster_mark_fail_if_quorum(&d, x, T0));
    DD_CHECK(x->flags & CLUSTER_NODE_FAIL);

    db_destroy(&d);
}

/* ------------------------------------------------------------------ */
/* FAIL frames force the objective state on receivers (both codecs)   */
/* ------------------------------------------------------------------ */
static void test_fail_frames(void)
{
    db da, db_;
    resp_buf frame, reply;
    cluster_node *x;

    /* RCM2: [RCM2][totlen][type=5][40-byte subject id] */
    db_init(&da);
    db_init(&db_);
    cluster_nodes_init(&da);
    cluster_nodes_init(&db_);
    resp_buf_init(&frame);
    resp_buf_init(&reply);
    mknode(&da, ME, CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    mknode(&db_, NB, CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    x = mknode(&db_, NX, CLUSTER_NODE_MASTER | CLUSTER_NODE_PFAIL);

    cluster_bus_build_fail(&da, NX, &frame);
    DD_CHECK(frame.len == 50 && memcmp(frame.data, "RCM2", 4) == 0);
    DD_CHECK_EQ_INT(0, cluster_bus_handle_frame(&db_, frame.data, frame.len,
                                                &reply, T0));
    DD_CHECK(x->flags & CLUSTER_NODE_FAIL);
    DD_CHECK(!(x->flags & CLUSTER_NODE_PFAIL));
    DD_CHECK(reply.len == 0); /* no PONG for a FAIL frame */

    /* unknown subject: tolerated; myself: never marked */
    frame.len = 0;
    cluster_bus_build_fail(&da, NC, &frame);
    DD_CHECK_EQ_INT(0, cluster_bus_handle_frame(&db_, frame.data, frame.len,
                                                &reply, T0));
    DD_CHECK(cluster_node_find(&db_, NC) == NULL);
    frame.len = 0;
    cluster_bus_build_fail(&da, NB, &frame);
    DD_CHECK_EQ_INT(0, cluster_bus_handle_frame(&db_, frame.data, frame.len,
                                                &reply, T0));
    DD_CHECK(!(cluster_node_find(&db_, NB)->flags & CLUSTER_NODE_FAIL));

    resp_buf_free(&reply);
    resp_buf_free(&frame);
    db_destroy(&db_);
    db_destroy(&da);

    /* redbus: full header + nodename payload, same receiver semantics */
    db_init(&da);
    db_init(&db_);
    cluster_nodes_init(&da);
    cluster_nodes_init(&db_);
    resp_buf_init(&frame);
    resp_buf_init(&reply);
    mknode(&da, ME, CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    mknode(&db_, NB, CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    x = mknode(&db_, NX, CLUSTER_NODE_MASTER | CLUSTER_NODE_PFAIL);

    redbus_build_fail(&da, NX, &frame);
    DD_CHECK(frame.len == REDBUS_HDR_LEN + 40);
    DD_CHECK_EQ_INT(0, redbus_handle_frame(&db_, frame.data, frame.len,
                                           &reply, T0, NULL));
    DD_CHECK(x->flags & CLUSTER_NODE_FAIL);
    DD_CHECK(!(x->flags & CLUSTER_NODE_PFAIL));

    resp_buf_free(&reply);
    resp_buf_free(&frame);
    db_destroy(&db_);
    db_destroy(&da);
}

/* ------------------------------------------------------------------ */
/* end to end: 3 masters, full mesh; one goes silent -> PFAIL ->      */
/* quorum FAIL -> FAIL frame -> all agree; heal on return             */
/* ------------------------------------------------------------------ */
#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "server/server.h"

static pal_socket_t e2e_cli(uint16_t port)
{
    pal_socket_t c = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(c != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));
    return c;
}

static void pump3(server *x, server *y, server *z)
{
    server_run_once(x, 5);
    if (y != NULL)
        server_run_once(y, 5);
    if (z != NULL)
        server_run_once(z, 5);
}

/* send req, pump the trio, read until want appears (bounded) */
static int ask3(server *x, server *y, server *z, pal_socket_t c,
                const char *req, const char *want, char *buf, size_t cap)
{
    size_t got = 0;
    uint64_t dl = pal_now_ms() + 15000;
    DD_CHECK_EQ_INT((long long)strlen(req),
                    (long long)pal_send(c, req, strlen(req)));
    while (pal_now_ms() < dl) {
        ptrdiff_t n;
        pump3(x, y, z);
        n = pal_recv(c, buf + got, cap - got - 1);
        if (n > 0) {
            got += (size_t)n;
            buf[got] = '\0';
            if (strstr(buf, want) != NULL)
                return 1;
        }
    }
    buf[got] = '\0';
    fprintf(stderr, "ask3 timeout: want [%s] got [%.600s]\n", want, buf);
    return 0;
}

static int wait_nodes3(server *x, server *y, server *z, pal_socket_t c,
                       const char *needle, char *buf, size_t cap)
{
    int i;
    for (i = 0; i < 800; i++) {
        pump3(x, y, z);
        if (i % 40 == 0) {
            if (ask3(x, y, z, c, "*2\r\n$7\r\nCLUSTER\r\n$5\r\nNODES\r\n",
                     needle, buf, cap))
                return 1;
        }
    }
    return 0;
}

static int wait_state3(server *x, server *y, server *z, pal_socket_t c,
                       const char *want, char *buf, size_t cap)
{
    int i;
    for (i = 0; i < 800; i++) {
        pump3(x, y, z);
        if (i % 40 == 0) {
            if (ask3(x, y, z, c, "*2\r\n$7\r\nCLUSTER\r\n$4\r\nINFO\r\n",
                     want, buf, cap))
                return 1;
        }
    }
    return 0;
}

static void meet3(server *x, server *y, server *z, pal_socket_t cx,
                  uint16_t port, char *buf, size_t cap)
{
    char req[256], ps[16];
    snprintf(ps, sizeof(ps), "%u", (unsigned)port);
    snprintf(req, sizeof(req),
             "*4\r\n$7\r\nCLUSTER\r\n$4\r\nMEET\r\n$9\r\n127.0.0.1\r\n"
             "$%zu\r\n%s\r\n",
             strlen(ps), ps);
    DD_CHECK(ask3(x, y, z, cx, req, "+OK\r\n", buf, cap));
}

/* chunked ADDSLOTS for [base, base+count) on one server */
static void addslots_span(server *x, server *y, server *z, pal_socket_t c,
                          int base, int count, char *buf, size_t cap)
{
    int off;
    for (off = 0; off < count; off += 1000) {
        int chunk = count - off < 1000 ? count - off : 1000;
        char *big = malloc(32768);
        size_t bl = 0, woff = 0;
        int sl;
        DD_CHECK(big != NULL);
        bl += (size_t)snprintf(big + bl, 32768 - bl, "*%d\r\n$7\r\nCLUSTER\r\n"
                               "$8\r\nADDSLOTS\r\n", chunk + 2);
        for (sl = base + off; sl < base + off + chunk; sl++) {
            char num[8];
            int nl = snprintf(num, sizeof(num), "%d", sl);
            bl += (size_t)snprintf(big + bl, 32768 - bl, "$%d\r\n%s\r\n", nl,
                                   num);
        }
        while (woff < bl) {
            ptrdiff_t w = pal_send(c, big + woff, bl - woff);
            if (w > 0)
                woff += (size_t)w;
            else
                pump3(x, y, z);
        }
        DD_CHECK(ask3(x, y, z, c, "", "+OK\r\n", buf, cap));
        free(big);
    }
}

static void test_quorum_fail_e2e(void)
{
    server *a, *b, *c;
    pal_socket_t ca, cb, cc;
    char buf[8192];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    c = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL && b != NULL && c != NULL);
    server_enable_cluster(a, ME);
    server_enable_cluster(b, NB);
    server_enable_cluster(c, NC);
    server_set_node_timeout(a, 2000);
    server_set_node_timeout(b, 2000);
    server_set_node_timeout(c, 2000);
    ca = e2e_cli(server_port(a));
    cb = e2e_cli(server_port(b));
    cc = e2e_cli(server_port(c));

    /* full mesh: a->b, a->c, b->c */
    meet3(a, b, c, ca, server_port(b), buf, sizeof(buf));
    meet3(a, b, c, ca, server_port(c), buf, sizeof(buf));
    meet3(a, b, c, cb, server_port(c), buf, sizeof(buf));

    /* slots split three ways */
    addslots_span(a, b, c, ca, 0, 5461, buf, sizeof(buf));
    addslots_span(a, b, c, cb, 5461, 5462, buf, sizeof(buf));
    addslots_span(a, b, c, cc, 10923, 5461, buf, sizeof(buf));
    DD_CHECK(wait_state3(a, b, c, ca, "cluster_state:ok\r\n", buf,
                         sizeof(buf)));

    /* a goes silent: b and c must agree on FAIL (quorum of masters);
     * flags render as "master,disconnected,fail" (trailing comma is
     * stripped), so the needle carries the following space */
    DD_CHECK(wait_nodes3(b, c, NULL, cb, ",fail ", buf, sizeof(buf)));
    DD_CHECK(wait_nodes3(b, c, NULL, cc, ",fail ", buf, sizeof(buf)));
    DD_CHECK(wait_state3(b, c, NULL, cb, "cluster_state:fail\r\n", buf,
                         sizeof(buf)));

    /* a returns: fresh direct frames clear the FAIL mark, state heals */
    DD_CHECK(wait_state3(a, b, c, cb, "cluster_state:ok\r\n", buf,
                         sizeof(buf)));
    DD_CHECK(wait_state3(a, b, c, cc, "cluster_state:ok\r\n", buf,
                         sizeof(buf)));

    pal_close(cc);
    pal_close(cb);
    pal_close(ca);
    server_destroy(c);
    server_destroy(b);
    server_destroy(a);
    pal_socket_cleanup();
}

/* suspicion without quorum must NOT trigger auto-failover either: a
 * lone slave of a silent master stays a slave (2-party deadlock,
 * documented; Redis likewise needs a master majority) */
static void test_pfail_only_no_failover(void)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char buf[8192], req[128];
    uint64_t dl;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL && b != NULL);
    server_enable_cluster(a, ME);
    server_enable_cluster(b, NB);
    server_set_node_timeout(a, 1000);
    server_set_node_timeout(b, 1000);
    ca = e2e_cli(server_port(a));
    cb = e2e_cli(server_port(b));

    meet3(a, b, NULL, ca, server_port(b), buf, sizeof(buf));
    addslots_span(a, b, NULL, ca, 0, 16384, buf, sizeof(buf));
    snprintf(req, sizeof(req),
             "*3\r\n$7\r\nCLUSTER\r\n$9\r\nREPLICATE\r\n$40\r\n%s\r\n", ME);
    DD_CHECK(ask3(a, b, NULL, cb, req, "+OK\r\n", buf, sizeof(buf)));
    /* let b learn a's claims before a goes silent */
    DD_CHECK(wait_nodes3(a, b, NULL, cb, "0-16383", buf, sizeof(buf)));

    /* a goes silent: b suspects (fail?) but can never reach a master
     * quorum alone, so no FAIL and no promotion */
    DD_CHECK(wait_nodes3(b, NULL, NULL, cb, "fail?", buf, sizeof(buf)));
    dl = pal_now_ms() + 5000;
    while (pal_now_ms() < dl)
        pump3(b, NULL, NULL);
    DD_CHECK(ask3(b, NULL, NULL, cb, "*2\r\n$7\r\nCLUSTER\r\n$5\r\nNODES\r\n",
                  "fail?", buf, sizeof(buf)));
    DD_CHECK(strstr(buf, ",fail ") == NULL);
    DD_CHECK(strstr(buf, "myself,slave") != NULL);

    pal_close(cb);
    pal_close(ca);
    server_destroy(b);
    server_destroy(a);
    pal_socket_cleanup();
}

static void test_two_master_deadlock(void)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char buf[8192];
    uint64_t dl;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL && b != NULL);
    server_enable_cluster(a, ME);
    server_enable_cluster(b, NB);
    server_set_node_timeout(a, 1000);
    server_set_node_timeout(b, 1000);
    ca = e2e_cli(server_port(a));
    cb = e2e_cli(server_port(b));

    meet3(a, b, NULL, ca, server_port(b), buf, sizeof(buf));
    addslots_span(a, b, NULL, ca, 0, 8192, buf, sizeof(buf));
    addslots_span(a, b, NULL, cb, 8192, 8192, buf, sizeof(buf));
    /* claims ride the 1s-cadence gossip PINGs: wait until b actually
     * sees a's slots before silencing a (else b's view of a is the
     * slotless MEET-time record and the quorum math degenerates) */
    DD_CHECK(wait_nodes3(a, b, NULL, cb, "0-8191", buf, sizeof(buf)));

    /* a goes silent: with only 2 masters the majority is 2, and the dead
     * one cannot report -- b suspects (fail?) but never confirms (fail,)
     * and keeps the state ok on a's covered slots */
    DD_CHECK(wait_nodes3(b, NULL, NULL, cb, "fail?", buf, sizeof(buf)));
    dl = pal_now_ms() + 5000;
    while (pal_now_ms() < dl)
        pump3(b, NULL, NULL);
    DD_CHECK(ask3(b, NULL, NULL, cb, "*2\r\n$7\r\nCLUSTER\r\n$5\r\nNODES\r\n",
                  "fail?", buf, sizeof(buf)));
    DD_CHECK(strstr(buf, ",fail ") == NULL);
    DD_CHECK(ask3(b, NULL, NULL, cb, "*2\r\n$7\r\nCLUSTER\r\n$4\r\nINFO\r\n",
                  "cluster_state:ok\r\n", buf, sizeof(buf)));

    pal_close(cb);
    pal_close(ca);
    server_destroy(b);
    server_destroy(a);
    pal_socket_cleanup();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    DD_RUN(test_report_window);
    DD_RUN(test_gossip_pfail_report);
    DD_RUN(test_direct_frame_clears_flags);
    DD_RUN(test_state_pfail_vs_fail);
    DD_RUN(test_quorum_promotion);
    DD_RUN(test_fail_frames);
    DD_RUN(test_quorum_fail_e2e);
    DD_RUN(test_pfail_only_no_failover);
    DD_RUN(test_two_master_deadlock);
    return DD_TEST_SUMMARY();
}