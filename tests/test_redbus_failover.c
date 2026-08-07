/* test_redbus_failover.c - FAILOVER_AUTH_REQUEST/ACK wire codec and the
 * vote-grant matrix (redis-mode failover building blocks). */
#include <stdio.h>
#include <string.h>

#include "core/cluster.h"
#include "core/redbus.h"
#include "test.h"

#define T0 1000000ULL
#define ME "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define DEAD "dddddddddddddddddddddddddddddddddddddddd"
#define SLV "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
#define OTHER "ffffffffffffffffffffffffffffffffffffffff"

static void test_vote_grant_matrix(void);
static void test_ack_counting(void);
static void test_auth_frames_are_header_only(void);

static uint16_t get16be_inline(const char *p)
{
    return (uint16_t)(((uint16_t)(uint8_t)p[0] << 8) |
                      (uint16_t)(uint8_t)p[1]);
}

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

static cluster_node *mk(db *d, const char *id, uint32_t flags, uint16_t port)
{
    cluster_node *n = cluster_node_add(d, id);
    snprintf(n->ip, sizeof(n->ip), "10.0.0.9");
    n->port = port;
    n->bus_port = (uint16_t)(port + 10000);
    n->flags = flags;
    return n;
}

/* a db where myself (ME) is a master owning slots 0-99, DEAD is a
 * quorum-failed (FAIL) master owning 100-199 (epoch 5), SLV its slave
 * (epoch 2), OTHER a master with 200-299 (epoch 4); current epoch 10 */
static void mk_cluster(db *d)
{
    cluster_node *me, *dead, *slv, *other;
    int i;
    cluster_nodes_init(d);
    me = mk(d, ME, CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER, 7000);
    for (i = 0; i < 100; i++)
        cluster_slots_set(me->slots, (uint32_t)i, 1);
    me->epoch = 3;
    dead = mk(d, DEAD, CLUSTER_NODE_MASTER | CLUSTER_NODE_FAIL, 7001);
    for (i = 100; i < 200; i++)
        cluster_slots_set(dead->slots, (uint32_t)i, 1);
    dead->epoch = 5;
    slv = mk(d, SLV, CLUSTER_NODE_SLAVE, 7002);
    snprintf(slv->master_id, sizeof(slv->master_id), "%s", DEAD);
    slv->epoch = 2;
    other = mk(d, OTHER, CLUSTER_NODE_MASTER, 7003);
    for (i = 200; i < 300; i++)
        cluster_slots_set(other->slots, (uint32_t)i, 1);
    other->epoch = 4;
    d->cluster_current_epoch = 10;
}

/* AUTH_REQUEST frame from SLV claiming DEAD's slots (100-199) with
 * config epoch 5, election epoch req_epoch */
static size_t mk_auth_request(char *f, uint64_t req_epoch,
                              uint64_t cfg_epoch)
{
    memset(f, 0, REDBUS_HDR_LEN);
    memcpy(f, "RCmb", 4);
    put32be(f + 4, REDBUS_HDR_LEN);
    put16be(f + 8, 1);
    put16be(f + 10, 7002);
    put16be(f + 12, REDBUS_TYPE_AUTH_REQUEST);
    put16be(f + 14, 0);
    put64be(f + 16, req_epoch);  /* currentEpoch = election epoch */
    put64be(f + 24, cfg_epoch);  /* configEpoch of the claim */
    memcpy(f + 40, SLV, 40);
    /* myslots @80: the dead master's slots (100-199) */
    {
        int i;
        for (i = 100; i < 200; i++)
            f[80 + i / 8] |= (char)(1u << (i % 8));
    }
    memcpy(f + 2128, DEAD, 40); /* slaveof */
    memcpy(f + 2168, "10.0.0.9", 8);
    put16be(f + 2248, 17002);
    put16be(f + 2250, REDBUS_NODE_SLAVE);
    return REDBUS_HDR_LEN;
}

static int reply_is_ack(const resp_buf *reply)
{
    return reply->len >= REDBUS_HDR_LEN &&
           memcmp(reply->data, "RCmb", 4) == 0 &&
           get16be_inline(reply->data + 12) == REDBUS_TYPE_AUTH_ACK;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    DD_RUN(test_vote_grant_matrix);
    DD_RUN(test_ack_counting);
    DD_RUN(test_auth_frames_are_header_only);
    return DD_TEST_SUMMARY();
}

static void test_vote_grant_matrix(void)
{
    db d;
    resp_buf reply;
    char frame[REDBUS_HDR_LEN + 8];
    size_t flen;

    /* baseline: valid request -> ACK */
    db_init(&d);
    resp_buf_init(&reply);
    mk_cluster(&d);
    flen = mk_auth_request(frame, 11, 5);
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame, flen, &reply, T0, NULL));
    DD_CHECK(reply_is_ack(&reply));
    DD_CHECK_EQ_INT(11, d.cluster_current_epoch);
    DD_CHECK_EQ_INT(11, d.last_vote_epoch);

    /* same epoch, second request: already voted -> no ACK */
    reply.len = 0;
    flen = mk_auth_request(frame, 11, 5);
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame, flen, &reply, T0 + 1,
                                        NULL));
    DD_CHECK(!reply_is_ack(&reply));
    db_destroy(&d);

    /* stale request epoch -> no ACK */
    db_init(&d);
    resp_buf_init(&reply);
    mk_cluster(&d);
    flen = mk_auth_request(frame, 9, 5); /* below current 10 */
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame, flen, &reply, T0, NULL));
    DD_CHECK(!reply_is_ack(&reply));
    db_destroy(&d);

    /* claim epoch below the current owner's -> no ACK */
    db_init(&d);
    resp_buf_init(&reply);
    mk_cluster(&d);
    flen = mk_auth_request(frame, 11, 4); /* DEAD owns 100-199 @ epoch 5 */
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame, flen, &reply, T0, NULL));
    DD_CHECK(!reply_is_ack(&reply));
    db_destroy(&d);

    /* master not marked FAIL (mere suspicion is not enough) -> no ACK */
    db_init(&d);
    resp_buf_init(&reply);
    mk_cluster(&d);
    cluster_node_find(&d, DEAD)->flags &= ~(uint32_t)CLUSTER_NODE_FAIL;
    flen = mk_auth_request(frame, 11, 5);
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame, flen, &reply, T0, NULL));
    DD_CHECK(!reply_is_ack(&reply));
    db_destroy(&d);

    /* requester is a master -> no ACK */
    db_init(&d);
    resp_buf_init(&reply);
    mk_cluster(&d);
    {
        cluster_node *slv = cluster_node_find(&d, SLV);
        slv->flags &= ~(uint32_t)CLUSTER_NODE_SLAVE;
        slv->flags |= CLUSTER_NODE_MASTER;
    }
    flen = mk_auth_request(frame, 11, 5);
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame, flen, &reply, T0, NULL));
    DD_CHECK(!reply_is_ack(&reply));
    db_destroy(&d);

    /* myself has no slots -> no vote right */
    db_init(&d);
    resp_buf_init(&reply);
    mk_cluster(&d);
    memset(cluster_myself(&d)->slots, 0, 2048);
    flen = mk_auth_request(frame, 11, 5);
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame, flen, &reply, T0, NULL));
    DD_CHECK(!reply_is_ack(&reply));
    db_destroy(&d);
}

static void test_ack_counting(void)
{
    db d;
    resp_buf reply;
    char frame[REDBUS_HDR_LEN + 8];
    cluster_node *voter;
    int i;

    db_init(&d);
    resp_buf_init(&reply);
    mk_cluster(&d);
    /* pretend we asked for votes at epoch 11 */
    d.failover_req_epoch = 11;
    d.failover_ack_mask = 0;
    d.failover_ack_count = 0;

    /* ACK frame from OTHER (master with slots, currentEpoch 11) */
    memset(frame, 0, REDBUS_HDR_LEN);
    memcpy(frame, "RCmb", 4);
    put32be(frame + 4, REDBUS_HDR_LEN);
    put16be(frame + 8, 1);
    put16be(frame + 10, 7003);
    put16be(frame + 12, REDBUS_TYPE_AUTH_ACK);
    put64be(frame + 16, 11);
    put64be(frame + 24, 4);
    memcpy(frame + 40, OTHER, 40);
    memcpy(frame + 2168, "10.0.0.9", 8);
    put16be(frame + 2248, 17003);
    put16be(frame + 2250, REDBUS_NODE_MASTER);
    /* OTHER claims 200-299 in the sender bitmap (it has slots) */
    for (i = 200; i < 300; i++)
        frame[80 + i / 8] |= (char)(1u << (i % 8));

    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame, REDBUS_HDR_LEN, &reply,
                                        T0, NULL));
    DD_CHECK_EQ_INT(1, d.failover_ack_count);

    /* same voter again: no double count */
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame, REDBUS_HDR_LEN, &reply,
                                        T0 + 1, NULL));
    DD_CHECK_EQ_INT(1, d.failover_ack_count);

    /* ACK with stale epoch (< requested): not counted */
    put64be(frame + 16, 7);
    voter = cluster_node_find(&d, OTHER);
    (void)voter;
    d.failover_req_epoch = 12;
    d.failover_ack_mask = 0;
    d.failover_ack_count = 0;
    DD_CHECK_EQ_INT(0,
                    redbus_handle_frame(&d, frame, REDBUS_HDR_LEN, &reply,
                                        T0 + 2, NULL));
    DD_CHECK_EQ_INT(0, d.failover_ack_count);

    resp_buf_free(&reply);
    db_destroy(&d);
}

static void test_auth_frames_are_header_only(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    mk_cluster(&d);
    /* become the dead master's slave and request votes */
    {
        cluster_node *me = cluster_myself(&d);
        me->flags &= ~(uint32_t)CLUSTER_NODE_MASTER;
        me->flags |= CLUSTER_NODE_SLAVE;
        snprintf(me->master_id, sizeof(me->master_id), "%s", DEAD);
    }
    d.failover_req_epoch = 11;
    redbus_build_auth_request(&d, 11, &out);
    DD_CHECK_EQ_INT(REDBUS_HDR_LEN, (long long)out.len);
    DD_CHECK_EQ_INT(0, (out.data[14] << 8) | (uint8_t)out.data[15]);
    DD_CHECK_EQ_INT(REDBUS_TYPE_AUTH_REQUEST,
                    (out.data[12] << 8) | (uint8_t)out.data[13]);
    /* the request aliases the dead master's slots (100-199) and epoch */
    DD_CHECK(out.data[80 + 100 / 8] & (1 << (100 % 8)));
    DD_CHECK(!(out.data[80] & 1)); /* slot 0 is NOT claimed */
    {
        uint64_t ce = 0;
        int i;
        for (i = 0; i < 8; i++)
            ce = (ce << 8) | (uint64_t)(uint8_t)out.data[24 + i];
        DD_CHECK_EQ_INT(5, (long long)ce); /* dead master's configEpoch */
    }
    resp_buf_free(&out);
    db_destroy(&d);
}
