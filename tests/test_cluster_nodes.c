/* test_cluster_nodes.c - cluster node table: ops, bitmap, render/parse. */
#include <string.h>
#include <stdint.h>

#include "core/cluster.h"
#include "core/command.h"
#include "test.h"

#define ID1 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define ID2 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define ID3 "cccccccccccccccccccccccccccccccccccccccc"

static void test_node_add_find(void)
{
    db d;
    db_init(&d);
    cluster_nodes_init(&d);
    DD_CHECK_EQ_INT(0, d.nnodes);
    {
        cluster_node *n = cluster_node_add(&d, ID1);
        DD_CHECK(n != NULL);
        DD_CHECK_STR(ID1, n->id);
        DD_CHECK_EQ_INT(1, d.nnodes);
        DD_CHECK(cluster_node_add(&d, ID1) == n); /* find-or-add */
        DD_CHECK_EQ_INT(1, d.nnodes);
        DD_CHECK(cluster_node_find(&d, ID1) == n);
        DD_CHECK(cluster_node_find(&d, ID2) == NULL);
    }
    db_destroy(&d);
}

static void test_slot_bitmap(void)
{
    uint8_t bm[2048];
    memset(bm, 0, sizeof(bm));
    DD_CHECK_EQ_INT(0, cluster_slots_get(bm, 0));
    cluster_slots_set(bm, 0, 1);
    cluster_slots_set(bm, 5461, 1);
    cluster_slots_set(bm, 16383, 1);
    DD_CHECK_EQ_INT(1, cluster_slots_get(bm, 0));
    DD_CHECK_EQ_INT(1, cluster_slots_get(bm, 5461));
    DD_CHECK_EQ_INT(1, cluster_slots_get(bm, 16383));
    DD_CHECK_EQ_INT(0, cluster_slots_get(bm, 5462));
    cluster_slots_set(bm, 5461, 0);
    DD_CHECK_EQ_INT(0, cluster_slots_get(bm, 5461));
}

static void test_slots_render(void)
{
    uint8_t bm[2048];
    char out[128];
    memset(bm, 0, sizeof(bm));
    cluster_slots_set(bm, 0, 1);
    cluster_slots_set(bm, 1, 1);
    cluster_slots_set(bm, 2, 1);
    cluster_slots_set(bm, 100, 1);
    cluster_slots_set(bm, 101, 1);
    cluster_slots_set(bm, 300, 1);
    {
        int n = cluster_slots_render(bm, out, sizeof(out));
        DD_CHECK_STR("0-2 100-101 300", out);
        DD_CHECK_EQ_INT((long long)strlen(out), n);
    }
    /* full range */
    memset(bm, 0xFF, sizeof(bm));
    cluster_slots_render(bm, out, sizeof(out));
    DD_CHECK_STR("0-16383", out);
    /* empty */
    memset(bm, 0, sizeof(bm));
    cluster_slots_render(bm, out, sizeof(out));
    DD_CHECK_STR("", out);
}

static void test_render_parse_roundtrip(void)
{
    db d, d2;
    resp_buf buf;
    db_init(&d);
    db_init(&d2);
    cluster_nodes_init(&d);
    cluster_nodes_init(&d2);
    resp_buf_init(&buf);

    {
        cluster_node *n1 = cluster_node_add(&d, ID1);
        cluster_node *n2 = cluster_node_add(&d, ID2);
        memcpy(n1->ip, "127.0.0.1", sizeof("127.0.0.1"));
        n1->port = 7001;
        n1->bus_port = 17001;
        n1->flags = CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER;
        memset(n1->slots, 0xFF, sizeof(n1->slots));
        memcpy(n2->ip, "127.0.0.1", sizeof("127.0.0.1"));
        n2->port = 7002;
        n2->bus_port = 17002;
        n2->flags = CLUSTER_NODE_MASTER | CLUSTER_NODE_HANDSHAKE;
        cluster_slots_set(n2->slots, 0, 1);
        cluster_slots_set(n2->slots, 1, 1);
        cluster_slots_set(n2->slots, 500, 1);
    }

    DD_CHECK_EQ_INT(0, cluster_nodes_render(&d, &buf));
    DD_CHECK(buf.len > 0);

    /* parse every rendered line into d2 and compare */
    {
        size_t off = 0;
        while (off < buf.len) {
            size_t end = off;
            while (end < buf.len && buf.data[end] != '\n')
                end++;
            DD_CHECK_EQ_INT(0,
                            cluster_nodes_parse_line(&d2, buf.data + off,
                                                     end - off));
            off = end + 1;
        }
    }
    DD_CHECK_EQ_INT(2, d2.nnodes);
    {
        cluster_node *p1 = cluster_node_find(&d2, ID1);
        cluster_node *p2 = cluster_node_find(&d2, ID2);
        DD_CHECK(p1 != NULL && p2 != NULL);
        DD_CHECK_STR("127.0.0.1", p1->ip);
        DD_CHECK_EQ_INT(7001, p1->port);
        DD_CHECK_EQ_INT(17001, p1->bus_port);
        DD_CHECK(p1->flags & CLUSTER_NODE_MYSELF);
        DD_CHECK_EQ_INT(1, cluster_slots_get(p1->slots, 16383));
        DD_CHECK(p2->flags & CLUSTER_NODE_HANDSHAKE);
        DD_CHECK_EQ_INT(1, cluster_slots_get(p2->slots, 0));
        DD_CHECK_EQ_INT(1, cluster_slots_get(p2->slots, 500));
        DD_CHECK_EQ_INT(0, cluster_slots_get(p2->slots, 501));
    }

    /* ip with bus port renders as ip:port@busport */
    {
        cluster_node *n3 = cluster_node_add(&d, ID3);
        DD_CHECK(n3 != NULL);
        if (n3 == NULL) {
            resp_buf_free(&buf);
            db_destroy(&d2);
            db_destroy(&d);
            return;
        }
        memcpy(n3->ip, "10.0.0.9", sizeof("10.0.0.9"));
        n3->port = 7003;
        n3->bus_port = 17003;
        buf.len = 0;
        DD_CHECK_EQ_INT(0, cluster_nodes_render(&d, &buf));
        DD_CHECK(buf.len > 0);
        DD_CHECK(strstr((char *)buf.data, "10.0.0.9:7003@17003") != NULL);
    }

    resp_buf_free(&buf);
    db_destroy(&d2);
    db_destroy(&d);
}

static void test_render_reserve_failure_leaves_output_unchanged(void)
{
    db d;
    resp_buf out;
    char byte = 'x';

    db_init(&d);
    cluster_nodes_init(&d);
    (void)cluster_node_add(&d, ID1);
    resp_buf_init(&out);
    out.data = &byte;
    out.len = SIZE_MAX;
    out.cap = SIZE_MAX;

    DD_CHECK_EQ_INT(-1, cluster_nodes_render(&d, &out));
    DD_CHECK(out.len == SIZE_MAX);
    DD_CHECK_EQ_INT('x', byte);

    out.data = NULL;
    out.len = 0;
    out.cap = 0;
    db_destroy(&d);
}

static void test_parse_rejects_unrepresentable_address(void)
{
    db d;
    char line[256];
    const char suffix[] = " :7000@17000 master - 0 0 1 connected -";
    size_t i;

    db_init(&d);
    cluster_nodes_init(&d);
    for (i = 0; i < 70; i++)
        line[40 + i] = 'a';
    memcpy(line, ID1, 40);
    memcpy(line + 110, suffix, sizeof(suffix) - 1);
    DD_CHECK_EQ_INT(-1, cluster_nodes_parse_line(&d, line,
                                                 110 + sizeof(suffix) - 1));
    DD_CHECK_EQ_INT(0, d.nnodes);
    db_destroy(&d);
}

static void test_parse_rejects_out_of_range_ports(void)
{
    db d;
    const char line[] =
        "0123456789012345678901234567890123456789 "
        "127.0.0.1:70000@17000 master - 0 0 1 connected -";

    db_init(&d);
    cluster_nodes_init(&d);
    DD_CHECK_EQ_INT(-1, cluster_nodes_parse_line(&d, line, sizeof(line) - 1));
    DD_CHECK_EQ_INT(0, d.nnodes);
    db_destroy(&d);
}

static void test_node_api_rejects_null_inputs(void)
{
    db d;
    db_init(&d);
    cluster_nodes_init(&d);
    DD_CHECK(cluster_node_find(NULL, ID1) == NULL);
    DD_CHECK(cluster_node_find(&d, NULL) == NULL);
    DD_CHECK(cluster_node_add(NULL, ID1) == NULL);
    DD_CHECK(cluster_node_add(&d, NULL) == NULL);
    DD_CHECK(cluster_node_find(&d, "short") == NULL);
    DD_CHECK(cluster_node_add(&d, "short") == NULL);
    DD_CHECK(cluster_node_add(&d, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == NULL);
    DD_CHECK_EQ_INT(-1, cluster_node_id_load_or_create(NULL, NULL));
    DD_CHECK_EQ_INT(-1, cluster_node_id_load_or_create("/tmp/ddup-node-id-test", NULL));
    db_destroy(&d);
}

static void test_slot_api_rejects_null_inputs(void)
{
    uint8_t bm[2048];
    char out[16];
    memset(bm, 0, sizeof(bm));
    DD_CHECK_EQ_INT(0, cluster_slots_get(NULL, 0));
    cluster_slots_set(NULL, 0, 1);
    DD_CHECK_EQ_INT(-1, cluster_slots_render(NULL, out, sizeof(out)));
    DD_CHECK_EQ_INT(-1, cluster_slots_render(bm, NULL, sizeof(out)));
    cluster_slots_parse(NULL, "0", 1);
    cluster_slots_parse(bm, NULL, 1);
    DD_CHECK_EQ_INT(0, cluster_slots_get(bm, 0));
}

static void test_cluster_state_api_rejects_null_inputs(void)
{
    db d;
    uint8_t bm[2048];
    db_init(&d);
    cluster_nodes_init(&d);
    memset(bm, 0, sizeof(bm));
    DD_CHECK_EQ_INT(0, (long long)cluster_next_epoch(NULL));
    cluster_adopt_claims(NULL, NULL, NULL, 1);
    cluster_merge_claims(NULL, NULL, NULL, 1);
    DD_CHECK(cluster_myself(NULL) == NULL);
    DD_CHECK_EQ_INT(1, cluster_state_is_ok(NULL));
    DD_CHECK_EQ_INT(0, cluster_state_is_minority(NULL));
    cluster_adopt_claims(&d, NULL, bm, 1);
    cluster_merge_claims(&d, NULL, bm, 1);
    db_destroy(&d);
}

static void test_corrupt_node_count_fails_closed(void)
{
    db d;
    resp_buf out;

    db_init(&d);
    cluster_nodes_init(&d);
    resp_buf_init(&out);
    d.cluster_enabled = 1;
    d.nnodes = CLUSTER_MAX_NODES + 1;
    DD_CHECK(cluster_node_find(&d, ID1) == NULL);
    DD_CHECK(cluster_node_add(&d, ID1) == NULL);
    DD_CHECK_EQ_INT(-1, cluster_nodes_render(&d, &out));
    DD_CHECK_EQ_INT(-1, cluster_bus_build_frame(&d, CLUSTER_MSG_PING, &out));
    DD_CHECK_EQ_INT(0, cluster_state_is_ok(&d));
    DD_CHECK_EQ_INT(0, cluster_state_is_minority(&d));
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_corrupt_report_count_fails_closed(void)
{
    db d;
    cluster_node *n;

    db_init(&d);
    cluster_nodes_init(&d);
    n = cluster_node_add(&d, ID1);
    DD_CHECK(n != NULL);
    if (n != NULL) {
        n->nreports = CLUSTER_MAX_NODES + 1;
        cluster_report_failure(&d, n, ID2, 1);
        cluster_report_heal(&d, n, ID2);
        DD_CHECK_EQ_INT(0, cluster_report_count(&d, n, 1));
        DD_CHECK_EQ_INT(0, cluster_mark_fail_if_quorum(&d, n, 1));
    }
    db_destroy(&d);
}

static void test_state_restore_rejects_corrupt_node_count(void)
{
    db d;
    cluster_state state;

    db_init(&d);
    cluster_nodes_init(&d);
    memset(&state, 0, sizeof(state));
    state.nnodes = CLUSTER_MAX_NODES + 1;
    d.nnodes = 0;
    cluster_state_restore(&d, &state);
    DD_CHECK_EQ_INT(0, d.nnodes);
    db_destroy(&d);
}

static void test_nodes_persistence_api_rejects_null_inputs(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    cluster_nodes_init(&d);
    resp_buf_init(&out);
    DD_CHECK_EQ_INT(-1, cluster_nodes_render(NULL, &out));
    DD_CHECK_EQ_INT(-1, cluster_nodes_render(&d, NULL));
    DD_CHECK_EQ_INT(-1, cluster_nodes_parse_line(NULL, NULL, 40));
    DD_CHECK_EQ_INT(-1, cluster_nodes_parse_line(&d, NULL, 40));
    DD_CHECK_EQ_INT(-1, cluster_nodes_parse_line(&d, "", 0));
    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_node_add_find);
    DD_RUN(test_slot_bitmap);
    DD_RUN(test_slots_render);
    DD_RUN(test_render_parse_roundtrip);
    DD_RUN(test_render_reserve_failure_leaves_output_unchanged);
    DD_RUN(test_parse_rejects_unrepresentable_address);
    DD_RUN(test_parse_rejects_out_of_range_ports);
    DD_RUN(test_node_api_rejects_null_inputs);
    DD_RUN(test_slot_api_rejects_null_inputs);
    DD_RUN(test_cluster_state_api_rejects_null_inputs);
    DD_RUN(test_corrupt_node_count_fails_closed);
    DD_RUN(test_corrupt_report_count_fails_closed);
    DD_RUN(test_state_restore_rejects_corrupt_node_count);
    DD_RUN(test_nodes_persistence_api_rejects_null_inputs);
    return DD_TEST_SUMMARY();
}
