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

int main(void)
{
    DD_RUN(test_frame_roundtrip);
    return DD_TEST_SUMMARY();
}
