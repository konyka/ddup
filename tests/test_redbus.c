/* test_redbus.c - Redis cluster bus wire codec: fixture decode, build/parse
 * roundtrip, UPDATE/FAIL handling, type tolerance. */
#include <stdio.h>
#include <string.h>

#include "core/cluster.h"
#include "core/redbus.h"
#include "test.h"

#define T0 1000000ULL
#define ID1 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define ID2 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define ID3 "cccccccccccccccccccccccccccccccccccccccc"

static void put16be(char *p, uint16_t v)
{
    p[0] = (char)((v >> 8) & 0xFFu);
    p[1] = (char)(v & 0xFFu);
}

static void put32be(char *p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++)
        p[i] = (char)((v >> (24 - 8 * i)) & 0xFFu);
}

static void put64be(char *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (char)((v >> (56 - 8 * i)) & 0xFFu);
}

/* canonical PING frame built byte-by-byte from the documented layout:
 * 2256-byte header + one 104-byte gossip entry */
static size_t build_fixture(char *f)
{
    memset(f, 0, REDBUS_HDR_LEN + REDBUS_GOSSIP_LEN);
    memcpy(f, "RCmb", 4);
    put32be(f + 4, REDBUS_HDR_LEN + REDBUS_GOSSIP_LEN);
    put16be(f + 8, 1);           /* ver */
    put16be(f + 10, 7001);       /* port */
    put16be(f + 12, 0);          /* PING */
    put16be(f + 14, 1);          /* count */
    put64be(f + 16, 42);         /* currentEpoch */
    put64be(f + 24, 7);          /* configEpoch */
    put64be(f + 32, 0);          /* offset */
    memset(f + 40, 'a', 40);     /* sender */
    f[80] = 0x03;                /* myslots: slots 0,1 */
    f[80 + 2047] = 0x80;         /* slot 16383 */
    /* slaveof @2128: zeros (master) */
    memcpy(f + 2168, "127.0.0.1", 9);
    put16be(f + 2248, 17001);    /* cport */
    put16be(f + 2250, 17);       /* MYSELF|MASTER */
    f[2252] = 0;                 /* CLUSTER_OK */

    /* gossip entry: node b, master, 10.0.0.2:7002@17002 */
    {
        char *g = f + REDBUS_HDR_LEN;
        memset(g, 'b', 40);
        put32be(g + 40, 1000);
        put32be(g + 44, 2000);
        memcpy(g + 48, "10.0.0.2", 8);
        put16be(g + 94, 7002);
        put16be(g + 96, 17002);
        put16be(g + 98, 1); /* MASTER */
    }
    return REDBUS_HDR_LEN + REDBUS_GOSSIP_LEN;
}

static void test_fixture_decode(void)
{
    db d;
    resp_buf reply;
    char frame[REDBUS_HDR_LEN + REDBUS_GOSSIP_LEN];
    size_t flen;
    cluster_node *n;

    db_init(&d);
    cluster_nodes_init(&d);
    resp_buf_init(&reply);
    flen = build_fixture(frame);

    DD_CHECK_EQ_INT(0, redbus_handle_frame(&d, frame, flen, &reply, T0, NULL));

    /* sender node */
    n = cluster_node_find(&d, ID1);
    DD_CHECK(n != NULL);
    DD_CHECK_STR("127.0.0.1", n->ip);
    DD_CHECK_EQ_INT(7001, n->port);
    DD_CHECK_EQ_INT(17001, n->bus_port);
    DD_CHECK(n->flags & CLUSTER_NODE_MASTER);
    DD_CHECK(!(n->flags & CLUSTER_NODE_MYSELF)); /* stripped on receive */
    DD_CHECK_STR("-", n->master_id);
    DD_CHECK_EQ_INT(7, n->epoch);
    DD_CHECK_EQ_INT(42, d.cluster_current_epoch);
    DD_CHECK_EQ_INT(1, cluster_slots_get(n->slots, 0));
    DD_CHECK_EQ_INT(1, cluster_slots_get(n->slots, 1));
    DD_CHECK_EQ_INT(1, cluster_slots_get(n->slots, 16383));
    DD_CHECK_EQ_INT(0, cluster_slots_get(n->slots, 2));
    DD_CHECK_EQ_INT((long long)T0, (long long)n->last_seen_ms);

    /* gossip entry registered (handshake) */
    n = cluster_node_find(&d, ID2);
    DD_CHECK(n != NULL);
    DD_CHECK(n->flags & CLUSTER_NODE_MASTER);
    DD_CHECK(n->flags & CLUSTER_NODE_HANDSHAKE);
    DD_CHECK_EQ_INT(7002, n->port);
    DD_CHECK_EQ_INT(17002, n->bus_port);

    /* a PONG reply was produced: Redis wire, type 1 */
    DD_CHECK(reply.len >= REDBUS_HDR_LEN);
    DD_CHECK(memcmp(reply.data, "RCmb", 4) == 0);
    DD_CHECK_EQ_INT(1, (reply.data[12] << 8) | (uint8_t)reply.data[13]);
    {
        uint32_t tot = 0;
        int i;
        for (i = 0; i < 4; i++)
            tot = (tot << 8) | (uint32_t)(uint8_t)reply.data[4 + i];
        DD_CHECK_EQ_INT((long long)reply.len, (long long)tot);
    }

    /* malformed frames rejected */
    DD_CHECK_EQ_INT(-1, redbus_handle_frame(&d, "garbage", 7, &reply, T0, NULL));
    DD_CHECK_EQ_INT(-1,
                    redbus_handle_frame(&d, frame, REDBUS_HDR_LEN - 1,
                                        &reply, T0, NULL));
    frame[4] = 0xFF; /* corrupt totlen */
    DD_CHECK_EQ_INT(-1, redbus_handle_frame(&d, frame, flen, &reply, T0, NULL));

    resp_buf_free(&reply);
    db_destroy(&d);
}

static cluster_node *mk_node(db *d, const char *id, const char *ip,
                             uint16_t port, uint32_t flags)
{
    cluster_node *n = cluster_node_add(d, id);
    snprintf(n->ip, sizeof(n->ip), "%s", ip);
    n->port = port;
    n->bus_port = (uint16_t)(port + 10000);
    n->flags = flags;
    return n;
}

static void test_roundtrip(void)
{
    db d1, d2;
    resp_buf frame, reply;
    cluster_node *n, *me;

    db_init(&d1);
    db_init(&d2);
    cluster_nodes_init(&d1);
    cluster_nodes_init(&d2);
    resp_buf_init(&frame);
    resp_buf_init(&reply);

    me = mk_node(&d1, ID1, "127.0.0.1", 7001,
                 CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    cluster_slots_set(me->slots, 0, 1);
    cluster_slots_set(me->slots, 1, 1);
    cluster_slots_set(me->slots, 2, 1);
    me->epoch = 3;
    d1.cluster_current_epoch = 5;
    n = mk_node(&d1, ID2, "10.0.0.2", 7002, CLUSTER_NODE_SLAVE);
    snprintf(n->master_id, sizeof(n->master_id), "%s", ID1);
    n->epoch = 2;
    mk_node(&d1, ID3, "10.0.0.3", 7003,
            CLUSTER_NODE_MASTER | CLUSTER_NODE_PFAIL |
                CLUSTER_NODE_DISCONNECTED);

    redbus_build_frame(&d1, REDBUS_TYPE_PING, &frame);
    DD_CHECK(frame.len >= REDBUS_HDR_LEN);
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d2, frame.data, frame.len, &reply,
                                        T0, NULL));

    n = cluster_node_find(&d2, ID1);
    DD_CHECK(n != NULL);
    DD_CHECK(n->flags & CLUSTER_NODE_MASTER);
    DD_CHECK_EQ_INT(3, n->epoch);
    DD_CHECK_EQ_INT(5, d2.cluster_current_epoch);
    DD_CHECK_EQ_INT(1, cluster_slots_get(n->slots, 2));
    DD_CHECK_EQ_INT(0, cluster_slots_get(n->slots, 3));

    n = cluster_node_find(&d2, ID2);
    DD_CHECK(n != NULL);
    DD_CHECK(n->flags & CLUSTER_NODE_SLAVE);
    DD_CHECK(!(n->flags & CLUSTER_NODE_MASTER));
    /* gossip entries carry no slaveof field: the master id is only
     * learned from a direct sender frame (Redis semantics) */
    DD_CHECK_STR("-", n->master_id);

    n = cluster_node_find(&d2, ID3);
    DD_CHECK(n != NULL);
    /* suspicion travels as PFAIL; the DISCONNECTED link state is local
     * only and never leaks onto the wire */
    DD_CHECK(n->flags & CLUSTER_NODE_PFAIL);
    DD_CHECK(!(n->flags & CLUSTER_NODE_DISCONNECTED));

    /* reply is a PONG frame that parses back */
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d1, reply.data, reply.len, &frame,
                                        T0 + 1, NULL));

    resp_buf_free(&frame);
    resp_buf_free(&reply);
    db_destroy(&d2);
    db_destroy(&d1);
}

static void test_update_fail_and_tolerance(void)
{
    db d;
    resp_buf reply;
    char frame[REDBUS_HDR_LEN + 2048 + 48];
    cluster_node *n;

    db_init(&d);
    cluster_nodes_init(&d);
    resp_buf_init(&reply);
    n = mk_node(&d, ID2, "10.0.0.2", 7002, CLUSTER_NODE_MASTER);

    /* UPDATE: ID2 claims slots 100-101 with config epoch 9 */
    memset(frame, 0, sizeof(frame));
    memcpy(frame, "RCmb", 4);
    put32be(frame + 4, REDBUS_HDR_LEN + 8 + 40 + 2048);
    put16be(frame + 12, REDBUS_TYPE_UPDATE);
    put64be(frame + REDBUS_HDR_LEN, 9);
    memcpy(frame + REDBUS_HDR_LEN + 8, ID2, 40);
    frame[REDBUS_HDR_LEN + 48 + 100 / 8] = 0x30; /* slots 100,101 */
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame,
                                        REDBUS_HDR_LEN + 8 + 40 + 2048,
                                        &reply, T0, NULL));
    DD_CHECK_EQ_INT(1, cluster_slots_get(n->slots, 100));
    DD_CHECK_EQ_INT(1, cluster_slots_get(n->slots, 101));
    DD_CHECK_EQ_INT(0, cluster_slots_get(n->slots, 102));
    DD_CHECK_EQ_INT(9, n->epoch);
    DD_CHECK_EQ_INT(9, d.cluster_current_epoch);

    /* FAIL: marks the node disconnected */
    memset(frame, 0, REDBUS_HDR_LEN + 40);
    memcpy(frame, "RCmb", 4);
    put32be(frame + 4, REDBUS_HDR_LEN + 40);
    put16be(frame + 12, REDBUS_TYPE_FAIL);
    memcpy(frame + REDBUS_HDR_LEN, ID2, 40);
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame, REDBUS_HDR_LEN + 40,
                                        &reply, T0, NULL));
    DD_CHECK(n->flags & CLUSTER_NODE_DISCONNECTED);

    /* PUBLISH and unknown types are tolerated (ignored, no reply) */
    put16be(frame + 12, REDBUS_TYPE_PUBLISH);
    reply.len = 0;
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame, REDBUS_HDR_LEN + 40,
                                        &reply, T0, NULL));
    put16be(frame + 12, 10);
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame, REDBUS_HDR_LEN + 40,
                                        &reply, T0, NULL));

    resp_buf_free(&reply);
    db_destroy(&d);
}

static void test_empty_myip_auto_discovery(void)
{
    db d;
    resp_buf reply;
    char frame[REDBUS_HDR_LEN + REDBUS_GOSSIP_LEN];
    size_t flen;
    cluster_node *n;

    db_init(&d);
    cluster_nodes_init(&d);
    resp_buf_init(&reply);
    flen = build_fixture(frame);
    /* redis without cluster-announce-ip sends an empty myip */
    memset(frame + 2168, 0, 46);

    DD_CHECK_EQ_INT(0, redbus_handle_frame(&d, frame, flen, &reply, T0,
                                           "10.9.8.7"));
    n = cluster_node_find(&d, ID1);
    DD_CHECK(n != NULL);
    DD_CHECK_STR("10.9.8.7", n->ip); /* fell back to the peer address */

    /* a later empty-myip frame must not blank the learned address */
    reply.len = 0;
    DD_CHECK_EQ_INT(0, redbus_handle_frame(&d, frame, flen, &reply, T0 + 1,
                                           NULL));
    DD_CHECK_STR("10.9.8.7", n->ip);

    resp_buf_free(&reply);
    db_destroy(&d);
}

static void test_update_to_self_adopts(void)
{
    db d;
    resp_buf reply;
    char frame[REDBUS_HDR_LEN + 2048 + 48];
    cluster_node *me, *other;

    db_init(&d);
    cluster_nodes_init(&d);
    resp_buf_init(&reply);
    me = mk_node(&d, ID1, "127.0.0.1", 7001,
                 CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    other = mk_node(&d, ID2, "10.0.0.2", 7002, CLUSTER_NODE_MASTER);
    cluster_slots_set(other->slots, 5, 1);
    cluster_slots_set(other->slots, 6, 1);
    other->epoch = 3;
    d.cluster_current_epoch = 3;

    /* UPDATE naming myself: adopt the slots, strip the previous holder */
    memset(frame, 0, sizeof(frame));
    memcpy(frame, "RCmb", 4);
    put32be(frame + 4, REDBUS_HDR_LEN + 8 + 40 + 2048);
    put16be(frame + 12, REDBUS_TYPE_UPDATE);
    put64be(frame + REDBUS_HDR_LEN, 8);
    memcpy(frame + REDBUS_HDR_LEN + 8, ID1, 40);
    frame[REDBUS_HDR_LEN + 48] = 0x60; /* slots 5,6 */
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame,
                                        REDBUS_HDR_LEN + 8 + 40 + 2048,
                                        &reply, T0, NULL));
    DD_CHECK_EQ_INT(1, cluster_slots_get(me->slots, 5));
    DD_CHECK_EQ_INT(1, cluster_slots_get(me->slots, 6));
    DD_CHECK_EQ_INT(0, cluster_slots_get(other->slots, 5));
    DD_CHECK_EQ_INT(0, cluster_slots_get(other->slots, 6));
    DD_CHECK_EQ_INT(8, me->epoch);
    DD_CHECK_EQ_INT(8, d.cluster_current_epoch);
    DD_CHECK_EQ_INT(1, d.slot_owner_dirty);

    resp_buf_free(&reply);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_fixture_decode);
    DD_RUN(test_roundtrip);
    DD_RUN(test_update_fail_and_tolerance);
    DD_RUN(test_update_to_self_adopts);
    DD_RUN(test_empty_myip_auto_discovery);
    return DD_TEST_SUMMARY();
}
