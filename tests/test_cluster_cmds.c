/* test_cluster_cmds.c - CLUSTER command family (single-node mode). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/hashslot.h"
#include "core/session.h"
#include "test.h"

#define T0 1000000ULL
#define TEST_ID "0123456789abcdef0123456789abcdef01234567"

static void exec_sess(session *s, uint64_t now, resp_buf *out, int argc, ...)
{
    resp_value argv[8];
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

static session *cluster_session(db *d)
{
    session *s = session_create(d);
    cluster_node *me;
    d->cluster_enabled = 1;
    snprintf(d->node_id, sizeof(d->node_id), "%s", TEST_ID);
    snprintf(d->cluster_ip, sizeof(d->cluster_ip), "127.0.0.1");
    d->cluster_port = 7777;
    /* mirror server_enable_cluster: myself in the table with all slots */
    me = cluster_node_add(d, TEST_ID);
    snprintf(me->ip, sizeof(me->ip), "127.0.0.1");
    me->port = 7777;
    me->bus_port = 17777;
    me->flags = CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER;
    memset(me->slots, 0xFF, sizeof(me->slots));
    me->epoch = 1; /* pre-epochs ownership: config epoch 1 */
    d->slot_owner_dirty = 1;
    return s;
}

static void test_cluster_disabled(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 2, "CLUSTER", "INFO");
    EXPECT(out, "-ERR This instance has cluster support disabled\r\n");
    exec_sess(s, T0, &out, 2, "CLUSTER", "MYID");
    EXPECT(out, "-ERR This instance has cluster support disabled\r\n");
    exec_sess(s, T0, &out, 3, "CLUSTER", "KEYSLOT", "foo");
    EXPECT(out, "-ERR This instance has cluster support disabled\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_cluster_info_myid_nodes(void)
{
    db d;
    session *s;
    resp_buf out;
    char exp[256];
    db_init(&d);
    resp_buf_init(&out);
    s = cluster_session(&d);

    exec_sess(s, T0, &out, 2, "CLUSTER", "INFO");
    {
        const char *body =
            "cluster_enabled:1\r\ncluster_state:ok\r\n"
            "cluster_slots_assigned:16384\r\ncluster_slots_ok:16384\r\n"
            "cluster_known_nodes:1\r\ncluster_size:1\r\n"
            "cluster_current_epoch:1\r\ncluster_my_epoch:1\r\n";
        snprintf(exp, sizeof(exp), "$%zu\r\n%s\r\n", strlen(body), body);
        EXPECT(out, exp);
    }

    exec_sess(s, T0, &out, 2, "CLUSTER", "MYID");
    EXPECT(out, "$40\r\n" TEST_ID "\r\n");

    /* NODES renders the table: myself line with real addr/flags/slots */
    exec_sess(s, T0, &out, 2, "CLUSTER", "NODES");
    {
        char nul[512];
        DD_CHECK(out.len > 0 && out.len < sizeof(nul) - 1);
        memcpy(nul, out.data, out.len);
        nul[out.len] = '\0';
        DD_CHECK(strstr(nul, TEST_ID) != NULL);
        DD_CHECK(strstr(nul, "127.0.0.1:7777@17777") != NULL);
        DD_CHECK(strstr(nul, "myself,master") != NULL);
        DD_CHECK(strstr(nul, "connected 0-16383") != NULL);
    }

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_cluster_slots_and_keyslot(void)
{
    db d;
    session *s;
    resp_buf out;
    char exp[256];
    db_init(&d);
    resp_buf_init(&out);
    s = cluster_session(&d);

    /* SLOTS: one array covering 0-16383 with [ip, port, id] */
    snprintf(exp, sizeof(exp),
             "*1\r\n*3\r\n:0\r\n:16383\r\n*3\r\n$9\r\n127.0.0.1\r\n"
             ":7777\r\n$40\r\n%s\r\n",
             TEST_ID);
    exec_sess(s, T0, &out, 2, "CLUSTER", "SLOTS");
    EXPECT(out, exp);

    exec_sess(s, T0, &out, 3, "CLUSTER", "KEYSLOT", "foo");
    EXPECT(out, ":12182\r\n");
    exec_sess(s, T0, &out, 3, "CLUSTER", "KEYSLOT", "{u}.a");
    {
        char tag[8];
        size_t tl = hash_tag("{u}.a", 5, tag, sizeof(tag));
        char want[32];
        snprintf(want, sizeof(want), ":%u\r\n", hash_slot(tag, tl));
        EXPECT(out, want);
    }

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_cluster_slot_stats(void)
{
    db d;
    session *s;
    resp_buf out;
    uint32_t txn_slot;
    db_init(&d);
    resp_buf_init(&out);
    s = cluster_session(&d);

    txn_slot = hash_slot("txn", 3);
    exec_sess(s, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "SET", "txn", "1");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(s, T0, &out, 1, "EXEC");
    EXPECT(out, "*1\r\n+OK\r\n");
    DD_CHECK(d.slot_cpu_usecs[txn_slot] > 0);
    DD_CHECK(d.slot_net_bytes_in[txn_slot] > 0);
    DD_CHECK(d.slot_net_bytes_out[txn_slot] > 0);

    exec_sess(s, T0, &out, 5, "EVAL_RO", "return 1", "1", "foo", "x");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.slot_cpu_usecs[hash_slot("foo", 3)] > 0);
    exec_sess(s, T0, &out, 3, "FUNCTION", "LOAD",
              "#!lua name=slotlib\nreturn KEYS[1]");
    EXPECT(out, "$7\r\nslotlib\r\n");
    exec_sess(s, T0, &out, 4, "FCALL_RO", "slotlib", "1", "fkey");
    EXPECT(out, "$4\r\nfkey\r\n");
    DD_CHECK(d.slot_cpu_usecs[hash_slot("fkey", 4)] > 0);

    exec_sess(s, T0, &out, 3, "SET", "alpha", "1");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "CLUSTER", "SLOT-STATS", "SLOTSRANGE");
    EXPECT(out, "-ERR wrong number of arguments for 'cluster' command\r\n");

    exec_sess(s, T0, &out, 5, "CLUSTER", "SLOT-STATS", "SLOTSRANGE", "0", "16383");
    DD_CHECK(out.len > 0 && out.data[0] == '*');
    DD_CHECK(strstr(out.data, "key-count") != NULL);

    exec_sess(s, T0, &out, 5, "CLUSTER", "SLOT-STATS", "ORDERBY", "key-count", "LIMIT");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_sess(s, T0, &out, 6, "CLUSTER", "SLOT-STATS", "ORDERBY", "key-count", "LIMIT", "1");
    DD_CHECK(out.len > 0 && out.data[0] == '*');
    DD_CHECK(strstr(out.data, "key-count") != NULL);

    exec_sess(s, T0, &out, 5, "CLUSTER", "SLOT-STATS", "ORDERBY", "memory-bytes", "LIMIT");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_sess(s, T0, &out, 6, "CLUSTER", "SLOT-STATS", "ORDERBY", "memory-bytes", "LIMIT", "1");
    DD_CHECK(out.len > 0 && out.data[0] == '*');
    DD_CHECK(strstr(out.data, "memory-bytes") != NULL);

    exec_sess(s, T0, &out, 5, "CLUSTER", "SLOT-STATS", "ORDERBY", "cpu-usec", "LIMIT");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_sess(s, T0, &out, 6, "CLUSTER", "SLOT-STATS", "ORDERBY", "cpu-usec", "LIMIT", "1");
    DD_CHECK(out.len > 0 && out.data[0] == '*');
    DD_CHECK(strstr(out.data, "cpu-usec") != NULL);

    exec_sess(s, T0, &out, 6, "CLUSTER", "SLOT-STATS", "ORDERBY", "network-bytes-in", "LIMIT", "1");
    DD_CHECK(out.len > 0 && out.data[0] == '*');
    DD_CHECK(strstr(out.data, "network-bytes-in") != NULL);

    exec_sess(s, T0, &out, 6, "CLUSTER", "SLOT-STATS", "ORDERBY", "network-bytes-out", "LIMIT", "1");
    DD_CHECK(out.len > 0 && out.data[0] == '*');
    DD_CHECK(strstr(out.data, "network-bytes-out") != NULL);

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_cluster_internal_subcommands_are_protected(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = cluster_session(&d);

    exec_sess(s, T0, &out, 3, "CLUSTER", "SYNCSLOTS", "SYNC");
    EXPECT(out, "-ERR CLUSTER SYNCSLOTS subcommands are only allowed for internal clients\r\n");
    exec_sess(s, T0, &out, 3, "CLUSTER", "MIGRATION", "STATUS");
    EXPECT(out, "-ERR CLUSTER MIGRATION subcommands are only allowed for internal clients\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_count_and_getkeysinslot(void)
{
    db d;
    session *s;
    resp_buf out;
    uint32_t slot_foo, slot_u;
    char num[16];
    db_init(&d);
    resp_buf_init(&out);
    s = cluster_session(&d);

    exec_sess(s, T0, &out, 3, "SET", "foo", "1");
    exec_sess(s, T0, &out, 3, "SET", "{u}.a", "1");
    exec_sess(s, T0, &out, 3, "SET", "{u}.b", "1");

    slot_foo = hash_slot("foo", 3);
    {
        char tag[8];
        size_t tl = hash_tag("{u}.a", 5, tag, sizeof(tag));
        slot_u = hash_slot(tag, tl);
    }

    /* empty slot -> :0 */
    exec_sess(s, T0, &out, 3, "CLUSTER", "COUNTKEYSINSLOT", "0");
    EXPECT(out, ":0\r\n");

    snprintf(num, sizeof(num), "%u", slot_foo);
    out.len = 0;
    exec_sess(s, T0, &out, 3, "CLUSTER", "COUNTKEYSINSLOT", num);
    EXPECT(out, ":1\r\n");

    snprintf(num, sizeof(num), "%u", slot_u);
    out.len = 0;
    exec_sess(s, T0, &out, 3, "CLUSTER", "COUNTKEYSINSLOT", num);
    EXPECT(out, ":2\r\n");

    /* GETKEYSINSLOT returns the keys (unordered: check containment) */
    {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "%u", slot_u);
        out.len = 0;
        exec_sess(s, T0, &out, 4, "CLUSTER", "GETKEYSINSLOT", cmd, "10");
        DD_CHECK(out.len >= 4 && memcmp(out.data, "*2\r\n", 4) == 0);
        {
            char nul[256];
            DD_CHECK(out.len < sizeof(nul) - 1);
            memcpy(nul, out.data, out.len);
            nul[out.len] = '\0';
            DD_CHECK(strstr(nul, "$5\r\n{u}.a\r\n") != NULL);
            DD_CHECK(strstr(nul, "$5\r\n{u}.b\r\n") != NULL);
        }
        /* count limits */
        out.len = 0;
        exec_sess(s, T0, &out, 4, "CLUSTER", "GETKEYSINSLOT", cmd, "1");
        DD_CHECK(out.len >= 4 && memcmp(out.data, "*1\r\n", 4) == 0);
    }

    /* unknown subcommand and bad arity */
    exec_sess(s, T0, &out, 2, "CLUSTER", "BOGUS");
    EXPECT(out,
           "-ERR Unknown CLUSTER subcommand or wrong number of arguments "
           "for 'bogus'\r\n");
    exec_sess(s, T0, &out, 2, "CLUSTER", "KEYSLOT");
    EXPECT(out,
           "-ERR Unknown CLUSTER subcommand or wrong number of arguments "
           "for 'keyslot'\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_cluster_management_subcommands(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = cluster_session(&d);

    exec_sess(s, T0, &out, 2, "CLUSTER", "MYSHARDID");
    EXPECT(out, "$40\r\n" TEST_ID "\r\n");
    exec_sess(s, T0, &out, 2, "CLUSTER", "LINKS");
    EXPECT(out, "*0\r\n");
    exec_sess(s, T0, &out, 2, "CLUSTER", "SHARDS");
    EXPECT(out, "*0\r\n");
    exec_sess(s, T0, &out, 3, "CLUSTER", "REPLICAS", TEST_ID);
    EXPECT(out, "*0\r\n");
    exec_sess(s, T0, &out, 3, "CLUSTER", "SLAVES", TEST_ID);
    EXPECT(out, "*0\r\n");
    exec_sess(s, T0, &out, 2, "CLUSTER", "BUMPEPOCH");
    EXPECT(out, "+BUMPED 1\r\n");
    exec_sess(s, T0, &out, 3, "CLUSTER", "SET-CONFIG-EPOCH", "5");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "CLUSTER", "SET-CONFIG-EPOCH", "4");
    DD_CHECK(out.len > 0 && out.data[0] == '-');
    exec_sess(s, T0, &out, 3, "CLUSTER", "COUNT-FAILURE-REPORTS", TEST_ID);
    EXPECT(out, ":0\r\n");
    exec_sess(s, T0, &out, 3, "CLUSTER", "FORGET", TEST_ID);
    DD_CHECK(out.len > 0 && out.data[0] == '-');
    exec_sess(s, T0, &out, 2, "CLUSTER", "SAVECONFIG");
    EXPECT(out, "-ERR The server is running without a config file\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_crossslot_basic(void);
static void test_crossslot_setops_watch(void);
static void test_crossslot_exec(void);
static void test_crossslot_disabled_allows(void);
static void test_crossslot_copy(void);
static void test_cluster_variant_key_routing(void);

int main(void)
{
    DD_RUN(test_cluster_disabled);
    DD_RUN(test_cluster_info_myid_nodes);
    DD_RUN(test_cluster_slots_and_keyslot);
    DD_RUN(test_cluster_slot_stats);
    DD_RUN(test_cluster_internal_subcommands_are_protected);
    DD_RUN(test_count_and_getkeysinslot);
    DD_RUN(test_cluster_management_subcommands);
    DD_RUN(test_crossslot_basic);
    DD_RUN(test_crossslot_setops_watch);
    DD_RUN(test_crossslot_copy);
    DD_RUN(test_crossslot_exec);
    DD_RUN(test_crossslot_disabled_allows);
    DD_RUN(test_cluster_variant_key_routing);
    return DD_TEST_SUMMARY();
}

/* ---------------- CROSSSLOT enforcement ---------------- */

#define XS "-CROSSSLOT Keys in request don't hash to the same slot\r\n"

static void test_crossslot_basic(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = cluster_session(&d);

    exec_sess(s, T0, &out, 5, "MSET", "{u}.a", "1", "{u}.b", "2");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 5, "MSET", "{u}.a", "1", "x", "2");
    EXPECT(out, XS);
    exec_sess(s, T0, &out, 3, "MGET", "{u}.a", "{u}.b");
    EXPECT(out, "*2\r\n$1\r\n1\r\n$1\r\n2\r\n");
    exec_sess(s, T0, &out, 3, "MGET", "{u}.a", "x");
    EXPECT(out, XS);
    exec_sess(s, T0, &out, 3, "DEL", "{u}.a", "{u}.b");
    EXPECT(out, ":2\r\n");
    exec_sess(s, T0, &out, 3, "DEL", "a", "b");
    EXPECT(out, XS);
    exec_sess(s, T0, &out, 3, "EXISTS", "{u}.c", "{u}.d");
    EXPECT(out, ":0\r\n");
    exec_sess(s, T0, &out, 3, "EXISTS", "a", "b");
    EXPECT(out, XS);

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_crossslot_setops_watch(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = cluster_session(&d);

    exec_sess(s, T0, &out, 3, "SADD", "{u}.s1", "m");
    EXPECT(out, ":1\r\n");
    exec_sess(s, T0, &out, 3, "SADD", "{u}.s2", "m");
    EXPECT(out, ":1\r\n");
    exec_sess(s, T0, &out, 3, "SINTER", "{u}.s1", "{u}.s2");
    EXPECT(out, "*1\r\n$1\r\nm\r\n");
    exec_sess(s, T0, &out, 3, "SUNION", "a", "b");
    EXPECT(out, XS);

    exec_sess(s, T0, &out, 3, "WATCH", "a", "b");
    EXPECT(out, XS);
    exec_sess(s, T0, &out, 3, "WATCH", "{u}.a", "{u}.b");
    EXPECT(out, "+OK\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_crossslot_copy(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = cluster_session(&d);

    exec_sess(s, T0, &out, 3, "SET", "{u}.a", "1");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "COPY", "{u}.a", "{u}.b");
    EXPECT(out, ":1\r\n");
    exec_sess(s, T0, &out, 2, "GET", "{u}.b");
    EXPECT(out, "$1\r\n1\r\n");
    exec_sess(s, T0, &out, 3, "COPY", "{u}.a", "x");
    EXPECT(out, XS);

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_crossslot_exec(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = cluster_session(&d);

    /* mixed-slot transaction: aborted, no partial effects */
    exec_sess(s, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "SET", "a", "1");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(s, T0, &out, 3, "SET", "b", "2");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(s, T0, &out, 1, "EXEC");
    EXPECT(out, XS);
    exec_sess(s, T0, &out, 2, "GET", "a");
    EXPECT(out, "$-1\r\n");
    exec_sess(s, T0, &out, 2, "GET", "b");
    EXPECT(out, "$-1\r\n");

    /* same-slot transaction: executes */
    exec_sess(s, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "SET", "{u}.a", "1");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(s, T0, &out, 3, "SET", "{u}.b", "2");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(s, T0, &out, 1, "EXEC");
    EXPECT(out, "*2\r\n+OK\r\n+OK\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_crossslot_disabled_allows(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d); /* cluster disabled by default */

    exec_sess(s, T0, &out, 5, "MSET", "a", "1", "b", "2");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "MGET", "a", "b");
    EXPECT(out, "*2\r\n$1\r\n1\r\n$1\r\n2\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_cluster_variant_key_routing(void)
{
    db d;
    session *s;
    resp_buf out;
    cluster_node *me;
    cluster_node *other;
    uint32_t slot = hash_slot("{route}", 7);
    char moved[128];

    db_init(&d);
    resp_buf_init(&out);
    s = cluster_session(&d);
    me = cluster_myself(&d);
    other = cluster_node_add(&d, "abcdefabcdefabcdefabcdefabcdefabcdefabcd");
    DD_CHECK(me != NULL && other != NULL);
    if (me == NULL || other == NULL)
        goto done;
    snprintf(other->ip, sizeof(other->ip), "10.0.0.2");
    other->port = 7778;
    other->bus_port = 17778;
    other->flags = CLUSTER_NODE_MASTER;
    cluster_slots_set(me->slots, slot, 0);
    cluster_slots_set(other->slots, slot, 1);
    d.cluster_changes++;
    d.slot_owner_dirty = 1;
    snprintf(moved, sizeof(moved), "-MOVED %u 10.0.0.2:7778\r\n", slot);

    exec_sess(s, T0, &out, 5, "MSETNX", "{route}.a", "1", "{route}.b", "2");
    EXPECT(out, moved);
    exec_sess(s, T0, &out, 4, "HIMPORT", "PREPARE", "route", "field");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 5, "HIMPORT", "SET", "{route}", "route", "v");
    EXPECT(out, moved);
    exec_sess(s, T0, &out, 4, "SUNIONCARD", "2", "{route}.a", "{route}.b");
    EXPECT(out, moved);

done:
    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}
