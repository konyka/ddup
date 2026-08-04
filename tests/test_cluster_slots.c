/* test_cluster_slots.c - CLUSTER ADDSLOTS/DELSLOTS/SETSLOT commands. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/hashslot.h"
#include "core/session.h"
#include "test.h"

#define T0 1000000ULL
#define TEST_ID "0123456789abcdef0123456789abcdef01234567"
#define OTHER_ID "ffffffffffffffffffffffffffffffffffffffff"

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
    return s; /* myself owns NOTHING (fresh boot semantics) */
}

static void info_state(session *s, resp_buf *out, char *buf, size_t cap)
{
    exec_sess(s, T0, out, 2, "CLUSTER", "INFO");
    DD_CHECK(out->len < cap);
    memcpy(buf, out->data, out->len);
    buf[out->len] = '\0';
}

static void test_addslots(void)
{
    db d;
    session *s;
    resp_buf out;
    char buf[512];
    cluster_node *me;
    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);

    /* fresh boot: nothing assigned -> state fail */
    info_state(s, &out, buf, sizeof(buf));
    DD_CHECK(strstr(buf, "cluster_state:fail\r\n") != NULL);

    exec_sess(s, T0, &out, 4, "CLUSTER", "ADDSLOTS", "1", "2");
    EXPECT(out, "+OK\r\n");
    me = cluster_node_find(&d, TEST_ID);
    DD_CHECK_EQ_INT(1, cluster_slots_get(me->slots, 1));
    DD_CHECK_EQ_INT(1, cluster_slots_get(me->slots, 2));
    DD_CHECK_EQ_INT(0, cluster_slots_get(me->slots, 3));

    /* out of range and non-numeric */
    exec_sess(s, T0, &out, 3, "CLUSTER", "ADDSLOTS", "16384");
    EXPECT(out, "-ERR Invalid slot\r\n");
    exec_sess(s, T0, &out, 3, "CLUSTER", "ADDSLOTS", "abc");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");

    /* busy */
    exec_sess(s, T0, &out, 3, "CLUSTER", "ADDSLOTS", "1");
    EXPECT(out, "-ERR Slot 1 is already busy\r\n");

    /* still incomplete coverage -> fail */
    info_state(s, &out, buf, sizeof(buf));
    DD_CHECK(strstr(buf, "cluster_state:fail\r\n") != NULL);

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_delslots(void)
{
    db d;
    session *s;
    resp_buf out;
    cluster_node *me;
    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);

    exec_sess(s, T0, &out, 4, "CLUSTER", "ADDSLOTS", "1", "2");
    EXPECT(out, "+OK\r\n");
    me = cluster_node_find(&d, TEST_ID);

    exec_sess(s, T0, &out, 3, "CLUSTER", "DELSLOTS", "1");
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT(0, cluster_slots_get(me->slots, 1));
    DD_CHECK_EQ_INT(1, cluster_slots_get(me->slots, 2));

    exec_sess(s, T0, &out, 3, "CLUSTER", "DELSLOTS", "1");
    EXPECT(out, "-ERR Slot 1 is already unassigned\r\n");
    exec_sess(s, T0, &out, 3, "CLUSTER", "DELSLOTS", "16384");
    EXPECT(out, "-ERR Invalid slot\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_setslot(void)
{
    db d;
    session *s;
    resp_buf out;
    cluster_node *me, *other;
    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);

    exec_sess(s, T0, &out, 4, "CLUSTER", "ADDSLOTS", "5", "6");
    EXPECT(out, "+OK\r\n");
    me = cluster_node_find(&d, TEST_ID);

    /* unknown node id */
    exec_sess(s, T0, &out, 5, "CLUSTER", "SETSLOT", "5", "NODE", OTHER_ID);
    EXPECT(out, "-ERR Unknown node ffffffffffffffffffffffffffffffffffffffff\r\n");

    /* register the other node (as gossip would), then move slot 5 to it */
    other = cluster_node_add(&d, OTHER_ID);
    snprintf(other->ip, sizeof(other->ip), "10.0.0.2");
    other->port = 7002;
    other->bus_port = 17002;
    other->flags = CLUSTER_NODE_MASTER;
    exec_sess(s, T0, &out, 5, "CLUSTER", "SETSLOT", "5", "NODE", OTHER_ID);
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT(0, cluster_slots_get(me->slots, 5));
    DD_CHECK_EQ_INT(1, cluster_slots_get(other->slots, 5));
    DD_CHECK_EQ_INT(1, cluster_slots_get(me->slots, 6));

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_busy_across_nodes(void)
{
    db d;
    session *s;
    resp_buf out;
    cluster_node *other;
    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);

    /* another node owns slot 7: myself cannot add it */
    other = cluster_node_add(&d, OTHER_ID);
    other->flags = CLUSTER_NODE_MASTER;
    cluster_slots_set(other->slots, 7, 1);
    exec_sess(s, T0, &out, 3, "CLUSTER", "ADDSLOTS", "7");
    EXPECT(out, "-ERR Slot 7 is already busy\r\n");

    /* but myself can still add a free slot */
    exec_sess(s, T0, &out, 3, "CLUSTER", "ADDSLOTS", "8");
    EXPECT(out, "+OK\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_moved_basic(void);
static void test_moved_exec_partial(void);
static void test_moved_disabled_unaffected(void);

int main(void)
{
    DD_RUN(test_addslots);
    DD_RUN(test_delslots);
    DD_RUN(test_setslot);
    DD_RUN(test_busy_across_nodes);
    DD_RUN(test_moved_basic);
    DD_RUN(test_moved_exec_partial);
    DD_RUN(test_moved_disabled_unaffected);
    return DD_TEST_SUMMARY();
}

/* ---------------- -MOVED / -CLUSTERDOWN enforcement ---------------- */

static uint32_t pick_slot_key(char *out, int above, int start)
{
    int i;
    for (i = start; i < start + 100000; i++) {
        uint32_t sl;
        snprintf(out, 16, "key%d", i);
        sl = hash_slot(out, strlen(out));
        if (above ? sl >= 8192u : sl < 8192u)
            return sl;
    }
    return 0;
}

static void test_moved_basic(void)
{
    db d;
    session *s;
    resp_buf out;
    cluster_node *me, *other;
    char ownk[16];
    int i;
    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);

    /* unassigned slot -> CLUSTERDOWN */
    exec_sess(s, T0, &out, 3, "SET", "foo", "v");
    EXPECT(out, "-CLUSTERDOWN Hash slot not served\r\n");
    exec_sess(s, T0, &out, 2, "GET", "foo");
    EXPECT(out, "-CLUSTERDOWN Hash slot not served\r\n");

    /* myself: 0..8191, other: 8192..16383 (test-only direct bitmap setup) */
    me = cluster_node_find(&d, TEST_ID);
    for (i = 0; i < 8192; i++)
        cluster_slots_set(me->slots, (uint32_t)i, 1);
    other = cluster_node_add(&d, OTHER_ID);
    snprintf(other->ip, sizeof(other->ip), "10.0.0.2");
    other->port = 7002;
    other->flags = CLUSTER_NODE_MASTER;
    for (i = 8192; i < 16384; i++)
        cluster_slots_set(other->slots, (uint32_t)i, 1);
    d.slot_owner_dirty = 1;

    /* key "foo" (slot 12182) belongs to the other node -> MOVED */
    exec_sess(s, T0, &out, 3, "SET", "foo", "v");
    EXPECT(out, "-MOVED 12182 10.0.0.2:7002\r\n");
    exec_sess(s, T0, &out, 2, "GET", "foo");
    EXPECT(out, "-MOVED 12182 10.0.0.2:7002\r\n");

    /* own slot works */
    (void)pick_slot_key(ownk, 0, 1);
    exec_sess(s, T0, &out, 3, "SET", ownk, "v");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "GET", ownk);
    EXPECT(out, "$1\r\nv\r\n");

    /* MGET spanning both -> MOVED for the first offending key */
    exec_sess(s, T0, &out, 3, "MGET", ownk, "foo");
    EXPECT(out, "-MOVED 12182 10.0.0.2:7002\r\n");

    /* DEL on other's key -> MOVED */
    exec_sess(s, T0, &out, 2, "DEL", "foo");
    EXPECT(out, "-MOVED 12182 10.0.0.2:7002\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_moved_exec_partial(void)
{
    db d;
    session *s;
    resp_buf out;
    cluster_node *other;
    char tag[16], cmd[24], exp[160];
    uint32_t sl;
    int i;
    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);

    /* myself owns nothing; other owns everything */
    other = cluster_node_add(&d, OTHER_ID);
    snprintf(other->ip, sizeof(other->ip), "10.0.0.2");
    other->port = 7002;
    other->flags = CLUSTER_NODE_MASTER;
    for (i = 0; i < 16384; i++)
        cluster_slots_set(other->slots, (uint32_t)i, 1);
    d.slot_owner_dirty = 1;

    /* pick a hashtag (same slot for both keys) that is NOT mine */
    for (i = 1; i < 100; i++) {
        snprintf(tag, sizeof(tag), "z%d", i);
        if (hash_slot(tag, strlen(tag)) > 0) {
            sl = hash_slot(tag, strlen(tag));
            break;
        }
    }
    (void)pick_slot_key(tag, 1, 1);
    sl = hash_slot(tag, strlen(tag));
    snprintf(cmd, sizeof(cmd), "{%s}.a", tag);

    exec_sess(s, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "SET", cmd, "1");
    EXPECT(out, "+QUEUED\r\n");
    {
        char cmd2[24];
        snprintf(cmd2, sizeof(cmd2), "{%s}.b", tag);
        exec_sess(s, T0, &out, 3, "SET", cmd2, "2");
        EXPECT(out, "+QUEUED\r\n");
    }
    /* each queued element replies MOVED; others still execute (here: both) */
    snprintf(exp, sizeof(exp), "*2\r\n-MOVED %u 10.0.0.2:7002\r\n-MOVED %u 10.0.0.2:7002\r\n",
             sl, sl);
    exec_sess(s, T0, &out, 1, "EXEC");
    EXPECT(out, exp);

    /* and nothing was written locally */
    {
        char getexp[96];
        snprintf(getexp, sizeof(getexp), "-MOVED %u 10.0.0.2:7002\r\n", sl);
        exec_sess(s, T0, &out, 2, "GET", cmd);
        EXPECT(out, getexp);
    }

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_moved_disabled_unaffected(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d); /* cluster disabled */

    exec_sess(s, T0, &out, 3, "SET", "foo", "v");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "GET", "foo");
    EXPECT(out, "$1\r\nv\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}
