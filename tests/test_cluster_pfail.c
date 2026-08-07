/* test_cluster_pfail.c - PFAIL suspicion state, failure reports with a
 * validity window, gossip report wiring, and PFAIL-vs-FAIL semantics in
 * cluster_state evaluation. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/cluster.h"
#include "core/command.h"
#include "core/session.h"
#include "test.h"

#define T0 1000000ULL
#define ME "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define NB "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define NC "cccccccccccccccccccccccccccccccccccccccc"

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

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    DD_RUN(test_report_window);
    DD_RUN(test_gossip_pfail_report);
    DD_RUN(test_direct_frame_clears_flags);
    DD_RUN(test_state_pfail_vs_fail);
    return DD_TEST_SUMMARY();
}
