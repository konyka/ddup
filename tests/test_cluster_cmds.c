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
    d->cluster_enabled = 1;
    snprintf(d->node_id, sizeof(d->node_id), "%s", TEST_ID);
    snprintf(d->cluster_ip, sizeof(d->cluster_ip), "127.0.0.1");
    d->cluster_port = 7777;
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
    char exp[128];
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

    /* NODES returns the myself line, nodes.conf style */
    {
        char line[128];
        int ll = snprintf(line, sizeof(line),
                          "%s :0@0 myself,master - 0 0 1 connected 0-16383\n",
                          TEST_ID);
        snprintf(exp, sizeof(exp), "$%d\r\n%s\r\n", ll, line);
    }
    exec_sess(s, T0, &out, 2, "CLUSTER", "NODES");
    EXPECT(out, exp);

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

int main(void)
{
    DD_RUN(test_cluster_disabled);
    DD_RUN(test_cluster_info_myid_nodes);
    DD_RUN(test_cluster_slots_and_keyslot);
    DD_RUN(test_count_and_getkeysinslot);
    return DD_TEST_SUMMARY();
}
