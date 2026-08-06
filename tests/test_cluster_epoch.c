/* test_cluster_epoch.c - config epochs: bump-on-claim, truthful CLUSTER
 * INFO epochs, gossip conflict resolution (higher epoch wins, ties go to
 * the larger node id), stale-gossip gating. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/session.h"
#include "test.h"

#define T0 1000000ULL
#define TEST_ID "0123456789abcdef0123456789abcdef01234567"
#define LOW_ID "00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff"
#define OTHER_ID "ffffffffffffffffffffffffffffffffffffffff"
#define THIRD_ID "1111111111111111111111111111111111111111"

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

static cluster_node *add_node(db *d, const char *id, uint16_t port)
{
    cluster_node *n = cluster_node_add(d, id);
    snprintf(n->ip, sizeof(n->ip), "10.0.0.2");
    n->port = port;
    n->bus_port = (uint16_t)(port + 10000);
    n->flags = CLUSTER_NODE_MASTER;
    return n;
}

/* build a PING frame whose sender is `id` claiming the given slots with
 * the given config epoch; gossip entries optional (one, for gate tests) */
static void sender_frame(db *src, const char *id, uint64_t epoch,
                         const uint32_t *slots, int nslots, resp_buf *out)
{
    cluster_node *n;
    int i;
    db_init(src);
    cluster_nodes_init(src);
    n = cluster_node_add(src, id);
    snprintf(n->ip, sizeof(n->ip), "10.9.9.9");
    n->port = 7099;
    n->bus_port = 17099;
    n->flags = CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER;
    n->epoch = epoch;
    for (i = 0; i < nslots; i++)
        cluster_slots_set(n->slots, slots[i], 1);
    out->len = 0;
    cluster_bus_build_frame(src, CLUSTER_MSG_PING, out);
}

static void test_epoch_bump_admin(void)
{
    db d;
    session *s;
    resp_buf out;
    cluster_node *me, *other;
    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);
    other = add_node(&d, OTHER_ID, 7002);

    DD_CHECK_EQ_INT(1, d.cluster_current_epoch);
    me = cluster_myself(&d);
    DD_CHECK_EQ_INT(0, me->epoch);

    exec_sess(s, T0, &out, 3, "CLUSTER", "ADDSLOTS", "5");
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT(2, d.cluster_current_epoch);
    DD_CHECK_EQ_INT(2, me->epoch);

    exec_sess(s, T0, &out, 4, "CLUSTER", "ADDSLOTS", "6", "7");
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT(3, me->epoch);

    exec_sess(s, T0, &out, 5, "CLUSTER", "SETSLOT", "5", "NODE", OTHER_ID);
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT(4, d.cluster_current_epoch);
    DD_CHECK_EQ_INT(4, other->epoch);

    exec_sess(s, T0, &out, 2, "CLUSTER", "INFO");
    DD_CHECK(out.len > 0);
    out.data[out.len] = '\0';
    DD_CHECK(strstr(out.data, "cluster_current_epoch:4\r\n") != NULL);
    DD_CHECK(strstr(out.data, "cluster_my_epoch:3\r\n") != NULL);

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_conflict_higher_epoch_wins(void)
{
    db d, src;
    resp_buf frame, reply;
    cluster_node *me, *other;
    uint32_t sl = 5;

    db_init(&d);
    cluster_nodes_init(&d);
    resp_buf_init(&frame);
    resp_buf_init(&reply);
    {
        session *s = fresh_session(&d);
        session_free(s);
    }
    me = cluster_myself(&d);
    cluster_slots_set(me->slots, 5, 1);
    me->epoch = 5;
    d.cluster_current_epoch = 5;
    other = add_node(&d, OTHER_ID, 7002);
    other->epoch = 1;

    sender_frame(&src, OTHER_ID, 7, &sl, 1, &frame);
    DD_CHECK_EQ_INT(0,
                    cluster_bus_handle_frame(&d, frame.data, frame.len,
                                             &reply, T0));
    DD_CHECK_EQ_INT(0, cluster_slots_get(me->slots, 5)); /* myself yields */
    DD_CHECK_EQ_INT(1, cluster_slots_get(other->slots, 5));
    DD_CHECK_EQ_INT(7, other->epoch);
    DD_CHECK_EQ_INT(7, d.cluster_current_epoch);

    db_destroy(&src);
    resp_buf_free(&reply);
    resp_buf_free(&frame);
    db_destroy(&d);
}

static void test_conflict_tie_break_by_id(void)
{
    db d, src;
    resp_buf frame, reply;
    cluster_node *me, *other;
    uint32_t sl6 = 6, sl8 = 8;

    db_init(&d);
    cluster_nodes_init(&d);
    resp_buf_init(&frame);
    resp_buf_init(&reply);
    { session *st = fresh_session(&d); session_free(st); }
    me = cluster_myself(&d);
    cluster_slots_set(me->slots, 6, 1);
    cluster_slots_set(me->slots, 8, 1);
    me->epoch = 5;
    d.cluster_current_epoch = 5;
    other = add_node(&d, OTHER_ID, 7002);
    other->epoch = 5;
    add_node(&d, LOW_ID, 7003)->epoch = 5;

    /* tie, claimant id (ffff...) > myself id (0123...): claimant wins */
    sender_frame(&src, OTHER_ID, 5, &sl6, 1, &frame);
    DD_CHECK_EQ_INT(0,
                    cluster_bus_handle_frame(&d, frame.data, frame.len,
                                             &reply, T0));
    DD_CHECK_EQ_INT(0, cluster_slots_get(me->slots, 6));
    DD_CHECK_EQ_INT(1, cluster_slots_get(other->slots, 6));
    db_destroy(&src);

    /* tie, claimant id (00ff...) < myself id: myself keeps the slot */
    reply.len = 0;
    sender_frame(&src, LOW_ID, 5, &sl8, 1, &frame);
    DD_CHECK_EQ_INT(0,
                    cluster_bus_handle_frame(&d, frame.data, frame.len,
                                             &reply, T0));
    DD_CHECK_EQ_INT(1, cluster_slots_get(me->slots, 8));
    DD_CHECK_EQ_INT(0,
                    cluster_slots_get(cluster_node_find(&d, LOW_ID)->slots,
                                      8));
    db_destroy(&src);

    resp_buf_free(&reply);
    resp_buf_free(&frame);
    db_destroy(&d);
}

static void test_conflict_lower_epoch_loses_and_retraction(void)
{
    db d, src;
    resp_buf frame, reply;
    cluster_node *me, *other;
    uint32_t sl9 = 9;
    uint32_t none = 0;

    db_init(&d);
    cluster_nodes_init(&d);
    resp_buf_init(&frame);
    resp_buf_init(&reply);
    { session *st = fresh_session(&d); session_free(st); }
    me = cluster_myself(&d);
    cluster_slots_set(me->slots, 9, 1);
    me->epoch = 5;
    d.cluster_current_epoch = 5;
    other = add_node(&d, OTHER_ID, 7002);
    other->epoch = 2;
    cluster_slots_set(other->slots, 10, 1); /* our table says it owns 10 */

    /* stale claim for slot 9 loses; the sender also stops claiming slot
     * 10, which our table retracts (sender is authoritative for itself) */
    sender_frame(&src, OTHER_ID, 3, &sl9, 1, &frame);
    DD_CHECK_EQ_INT(0,
                    cluster_bus_handle_frame(&d, frame.data, frame.len,
                                             &reply, T0));
    DD_CHECK_EQ_INT(1, cluster_slots_get(me->slots, 9));
    DD_CHECK_EQ_INT(0, cluster_slots_get(other->slots, 9));
    DD_CHECK_EQ_INT(0, cluster_slots_get(other->slots, 10)); /* retracted */
    DD_CHECK_EQ_INT(3, other->epoch);
    db_destroy(&src);

    /* a frame claiming nothing leaves my own claims alone */
    reply.len = 0;
    sender_frame(&src, OTHER_ID, 4, &none, 0, &frame);
    DD_CHECK_EQ_INT(0,
                    cluster_bus_handle_frame(&d, frame.data, frame.len,
                                             &reply, T0));
    DD_CHECK_EQ_INT(1, cluster_slots_get(me->slots, 9));
    db_destroy(&src);

    resp_buf_free(&reply);
    resp_buf_free(&frame);
    db_destroy(&d);
}

static void test_gossip_entry_merge_gated_by_epoch(void)
{
    db d, src;
    resp_buf frame, reply;
    cluster_node *other, *g;

    db_init(&d);
    cluster_nodes_init(&d);
    resp_buf_init(&frame);
    resp_buf_init(&reply);
    { session *st = fresh_session(&d); session_free(st); }
    other = add_node(&d, OTHER_ID, 7002);
    other->epoch = 6;
    cluster_slots_set(other->slots, 20, 1);

    /* stale gossip (epoch 2 < 6) about OTHER: ignored entirely */
    {
        cluster_node *gsrc;
        db_init(&src);
        cluster_nodes_init(&src);
        gsrc = cluster_node_add(&src, THIRD_ID);
        snprintf(gsrc->ip, sizeof(gsrc->ip), "10.9.9.8");
        gsrc->port = 7098;
        gsrc->bus_port = 17098;
        gsrc->flags = CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER;
        gsrc->epoch = 2;
        g = cluster_node_add(&src, OTHER_ID);
        snprintf(g->ip, sizeof(g->ip), "10.0.0.2");
        g->port = 7002;
        g->bus_port = 17002;
        g->flags = CLUSTER_NODE_MASTER;
        g->epoch = 2; /* stale: no slot 20 bit */
        frame.len = 0;
        cluster_bus_build_frame(&src, CLUSTER_MSG_PING, &frame);
        DD_CHECK_EQ_INT(0,
                        cluster_bus_handle_frame(&d, frame.data, frame.len,
                                                 &reply, T0));
        DD_CHECK_EQ_INT(1, cluster_slots_get(other->slots, 20));
        DD_CHECK_EQ_INT(6, other->epoch);
        db_destroy(&src);
    }

    /* fresh gossip (epoch 7 > 6) about OTHER claiming slot 21: merged */
    {
        cluster_node *gsrc;
        db_init(&src);
        cluster_nodes_init(&src);
        gsrc = cluster_node_add(&src, THIRD_ID);
        snprintf(gsrc->ip, sizeof(gsrc->ip), "10.9.9.8");
        gsrc->port = 7098;
        gsrc->bus_port = 17098;
        gsrc->flags = CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER;
        gsrc->epoch = 2;
        g = cluster_node_add(&src, OTHER_ID);
        snprintf(g->ip, sizeof(g->ip), "10.0.0.2");
        g->port = 7002;
        g->bus_port = 17002;
        g->flags = CLUSTER_NODE_MASTER;
        g->epoch = 7;
        cluster_slots_set(g->slots, 20, 1);
        cluster_slots_set(g->slots, 21, 1);
        reply.len = 0;
        frame.len = 0;
        cluster_bus_build_frame(&src, CLUSTER_MSG_PING, &frame);
        DD_CHECK_EQ_INT(0,
                        cluster_bus_handle_frame(&d, frame.data, frame.len,
                                                 &reply, T0));
        DD_CHECK_EQ_INT(1, cluster_slots_get(other->slots, 21));
        DD_CHECK_EQ_INT(7, other->epoch);
        DD_CHECK_EQ_INT(7, d.cluster_current_epoch);
        db_destroy(&src);
    }

    resp_buf_free(&reply);
    resp_buf_free(&frame);
    db_destroy(&d);
}

/* ------------------------------------------------------------------ */
/* wire: equal-epoch conflict converges to the larger id              */
/* ------------------------------------------------------------------ */
#include "core/hashslot.h"
#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "server/server.h"

#define IDA "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define IDB "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static void pump2(server *x, server *y)
{
    server_run_once(x, 5);
    server_run_once(y, 5);
}

static pal_socket_t cli(uint16_t port)
{
    pal_socket_t c = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(c != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));
    return c;
}

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
        if (n > 0)
            break;
    }
    buf[got] = '\0';
    return got;
}

static void key_in_slot(uint32_t slot, char *out)
{
    int i;
    for (i = 0; i < 100000; i++) {
        snprintf(out, 16, "ekey%d", i);
        if (hash_slot(out, strlen(out)) == slot)
            return;
    }
    out[0] = '\0';
}

static void test_wire_equal_epoch_converges(void)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char req[256], buf[2048], port[16], key[16];
    int i = 0, converged = 0;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    key_in_slot(100, key);
    DD_CHECK(key[0] != '\0');
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL && b != NULL);
    server_enable_cluster(a, IDA);
    server_enable_cluster(b, IDB);
    ca = cli(server_port(a));
    cb = cli(server_port(b));

    /* both claim slot 100 before ever meeting: both get config epoch 2,
     * so the tie goes to the larger id (IDB) */
    ask2(a, b, ca, "*3\r\n$7\r\nCLUSTER\r\n$8\r\nADDSLOTS\r\n$3\r\n100\r\n", buf,
         sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);
    ask2(a, b, cb, "*3\r\n$7\r\nCLUSTER\r\n$8\r\nADDSLOTS\r\n$3\r\n100\r\n", buf,
         sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);

    snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
    snprintf(req, sizeof(req),
             "*4\r\n$7\r\nCLUSTER\r\n$4\r\nMEET\r\n$9\r\n127.0.0.1\r\n"
             "$%zu\r\n%s\r\n",
             strlen(port), port);
    ask2(a, b, ca, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);

    /* after convergence a redirects slot 100 to b (-MOVED); wall-clock
     * bounded poll (iteration counts lie when the loop wakes instantly) */
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nv\r\n",
             strlen(key), key);
    {
        uint64_t dl = pal_now_ms() + 12000;
        while (!converged && pal_now_ms() < dl) {
            pump2(a, b);
            if (i++ % 40 == 0) {
                ask2(a, b, ca, req, buf, sizeof(buf));
                if (strstr(buf, "-MOVED 100 ") != NULL)
                    converged = 1;
            }
        }
    }
    DD_CHECK_EQ_INT(1, converged);

    /* and b serves it */
    ask2(a, b, cb, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);

    pal_close(cb);
    pal_close(ca);
    server_destroy(b);
    server_destroy(a);
    pal_socket_cleanup();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    DD_RUN(test_epoch_bump_admin);
    DD_RUN(test_conflict_higher_epoch_wins);
    DD_RUN(test_conflict_tie_break_by_id);
    DD_RUN(test_conflict_lower_epoch_loses_and_retraction);
    DD_RUN(test_gossip_entry_merge_gated_by_epoch);
    DD_RUN(test_wire_equal_epoch_converges);
    return DD_TEST_SUMMARY();
}
