/* test_cluster_migrate.c - slot migration states: SETSLOT MIGRATING TO /
 * IMPORTING FROM, -ASK redirects and the ASKING one-shot bypass, plus a
 * full two-server migration flow (unit + wire). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/hashslot.h"
#include "core/session.h"
#include "pal/pal_time.h"
#include "test.h"

#define T0 1000000ULL
#define TEST_ID "0123456789abcdef0123456789abcdef01234567"
#define OTHER_ID "ffffffffffffffffffffffffffffffffffffffff"
#define UNKNOWN_ID "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"

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
    if (me == NULL)
        return s;
    snprintf(me->ip, sizeof(me->ip), "127.0.0.1");
    me->port = 7777;
    me->bus_port = 17777;
    me->flags = CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER;
    return s; /* myself owns NOTHING */
}

static cluster_node *add_other(db *d)
{
    cluster_node *n = cluster_node_add(d, OTHER_ID);
    if (n == NULL)
        return NULL;
    snprintf(n->ip, sizeof(n->ip), "10.0.0.2");
    n->port = 7002;
    n->bus_port = 17002;
    n->flags = CLUSTER_NODE_MASTER;
    return n;
}

static uint16_t node_index(db *d, const char *id)
{
    return (uint16_t)(cluster_node_find(d, id) - d->nodes);
}

/* find a key hashing to the wanted slot (distinct from "foo") */
static void key_in_slot(uint32_t slot, char *out)
{
    int i;
    for (i = 0; i < 100000; i++) {
        snprintf(out, 16, "mkey%d", i);
        if (hash_slot(out, strlen(out)) == slot)
            return;
    }
    out[0] = '\0';
}

/* Parse a "$<len>\r\n<bytes>\r\n" reply; returns bulk length or -1 ($-1). */
static long long bulk_payload(const resp_buf *out, const char **p)
{
    long long n = 0;
    const char *s;
    if (out->len < 4 || out->data[0] != '$')
        return -2;
    s = out->data + 1;
    if (*s == '-')
        return -1;
    while (*s != '\r') {
        n = n * 10 + (*s - '0');
        s++;
    }
    *p = s + 2;
    return n;
}

static void test_setslot_migrating(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);
    add_other(&d);

    /* not the owner -> rejected */
    exec_sess(s, T0, &out, 6, "CLUSTER", "SETSLOT", "5", "MIGRATING", "TO",
              OTHER_ID);
    EXPECT(out,
           "-ERR Can't migrate slot: hash slot is not served by this "
           "node\r\n");

    exec_sess(s, T0, &out, 3, "CLUSTER", "ADDSLOTS", "5");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 6, "CLUSTER", "SETSLOT", "5", "MIGRATING", "TO",
              UNKNOWN_ID);
    EXPECT(out,
           "-ERR Unknown node "
           "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\r\n");
    exec_sess(s, T0, &out, 6, "CLUSTER", "SETSLOT", "5", "MIGRATING", "TO",
              TEST_ID);
    EXPECT(out, "-ERR Can't migrate slot to myself\r\n");

    exec_sess(s, T0, &out, 6, "CLUSTER", "SETSLOT", "5", "MIGRATING", "TO",
              OTHER_ID);
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT(node_index(&d, OTHER_ID), d.slot_migrating[5]);

    /* SETSLOT NODE clears the state */
    exec_sess(s, T0, &out, 5, "CLUSTER", "SETSLOT", "5", "NODE", TEST_ID);
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT(0xFFFF, d.slot_migrating[5]);

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_setslot_importing(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);
    add_other(&d);

    /* unassigned slot: importing is fine */
    exec_sess(s, T0, &out, 6, "CLUSTER", "SETSLOT", "6", "IMPORTING", "FROM",
              OTHER_ID);
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT(node_index(&d, OTHER_ID), d.slot_importing[6]);

    /* slot owned by myself: rejected */
    exec_sess(s, T0, &out, 3, "CLUSTER", "ADDSLOTS", "6");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 6, "CLUSTER", "SETSLOT", "6", "IMPORTING", "FROM",
              OTHER_ID);
    EXPECT(out,
           "-ERR Can't import slot: hash slot is already served by this "
           "node\r\n");
    exec_sess(s, T0, &out, 6, "CLUSTER", "SETSLOT", "6", "IMPORTING", "FROM",
              TEST_ID);
    EXPECT(out, "-ERR Can't import slot from myself\r\n");

    /* DELSLOTS clears the importing state too */
    exec_sess(s, T0, &out, 3, "CLUSTER", "DELSLOTS", "6");
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT(0xFFFF, d.slot_importing[6]);

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

/* slot of "foo" (fixed by the CRC16 table) */
#define FOO_SLOT 12182

static void test_ask_redirect(void)
{
    db d;
    session *s;
    resp_buf out;
    char other[16];
    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);
    add_other(&d);
    key_in_slot(FOO_SLOT, other);
    DD_CHECK(other[0] != '\0');
    DD_CHECK(hash_slot("foo", 3) == FOO_SLOT);

    exec_sess(s, T0, &out, 3, "CLUSTER", "ADDSLOTS", "12182");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "SET", "foo", "v");
    EXPECT(out, "+OK\r\n");

    exec_sess(s, T0, &out, 6, "CLUSTER", "SETSLOT", "12182", "MIGRATING",
              "TO", OTHER_ID);
    EXPECT(out, "+OK\r\n");

    /* existing keys are still served */
    exec_sess(s, T0, &out, 2, "GET", "foo");
    EXPECT(out, "$1\r\nv\r\n");
    /* absent keys redirect with -ASK */
    exec_sess(s, T0, &out, 2, "GET", other);
    EXPECT(out, "-ASK 12182 10.0.0.2:7002\r\n");
    exec_sess(s, T0, &out, 3, "SET", other, "v");
    EXPECT(out, "-ASK 12182 10.0.0.2:7002\r\n");

    /* clearing the state (owner re-takes the slot) restores serving */
    exec_sess(s, T0, &out, 5, "CLUSTER", "SETSLOT", "12182", "NODE",
              TEST_ID);
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "SET", other, "v");
    EXPECT(out, "+OK\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_asking_bypass(void)
{
    db d;
    session *s;
    resp_buf out;
    cluster_node *o;
    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);
    o = add_other(&d);
    cluster_slots_set(o->slots, FOO_SLOT, 1);
    d.slot_owner_dirty = 1;

    exec_sess(s, T0, &out, 6, "CLUSTER", "SETSLOT", "12182", "IMPORTING",
              "FROM", OTHER_ID);
    EXPECT(out, "+OK\r\n");

    /* without ASKING the peer-owned slot redirects with -MOVED */
    exec_sess(s, T0, &out, 2, "GET", "foo");
    EXPECT(out, "-MOVED 12182 10.0.0.2:7002\r\n");

    /* ASKING allows exactly one command */
    exec_sess(s, T0, &out, 1, "ASKING");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "GET", "foo");
    EXPECT(out, "$-1\r\n");
    exec_sess(s, T0, &out, 2, "GET", "foo");
    EXPECT(out, "-MOVED 12182 10.0.0.2:7002\r\n");

    /* writes too */
    exec_sess(s, T0, &out, 1, "ASKING");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "SET", "foo", "v");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "GET", "foo");
    EXPECT(out, "-MOVED 12182 10.0.0.2:7002\r\n");
    exec_sess(s, T0, &out, 1, "ASKING");
    exec_sess(s, T0, &out, 2, "GET", "foo");
    EXPECT(out, "$1\r\nv\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_asking_invalid_import_owner(void)
{
    db d;
    session *s;
    resp_buf out;

    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);

    /* Corrupt metadata must fail closed instead of indexing past nodes[]. */
    d.slot_importing[FOO_SLOT] = (uint16_t)CLUSTER_MAX_NODES;
    exec_sess(s, T0, &out, 1, "ASKING");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "GET", "foo");
    EXPECT(out, "-CLUSTERDOWN Hash slot not served\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_restore_asking_cluster(void)
{
    db d;
    session *s;
    resp_buf out;
    cluster_node *o;
    char payload[256], dst[16];
    const char *p;
    long long plen;
    resp_value argv[4];

    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);
    o = add_other(&d);
    cluster_slots_set(o->slots, FOO_SLOT, 1);
    d.slot_owner_dirty = 1;

    exec_sess(s, T0, &out, 6, "CLUSTER", "SETSLOT", "12182", "IMPORTING",
              "FROM", OTHER_ID);
    EXPECT(out, "+OK\r\n");

    exec_sess(s, T0, &out, 1, "ASKING");
    exec_sess(s, T0, &out, 3, "SET", "foo", "v");
    EXPECT(out, "+OK\r\n");

    exec_sess(s, T0, &out, 1, "ASKING");
    exec_sess(s, T0, &out, 2, "DUMP", "foo");
    plen = bulk_payload(&out, &p);
    DD_CHECK(plen > 0 && plen < (long long)sizeof(payload));
    memcpy(payload, p, (size_t)plen);

    key_in_slot(FOO_SLOT, dst);
    DD_CHECK(dst[0] != '\0');
    memset(argv, 0, sizeof(argv));
    argv[0].type = RESP_BULK_STRING;
    argv[0].str = "RESTORE-ASKING";
    argv[0].len = strlen(argv[0].str);
    argv[1].type = RESP_BULK_STRING;
    argv[1].str = dst;
    argv[1].len = strlen(dst);
    argv[2].type = RESP_BULK_STRING;
    argv[2].str = "0";
    argv[2].len = 1;
    argv[3].type = RESP_BULK_STRING;
    argv[3].str = payload;
    argv[3].len = (size_t)plen;
    out.len = 0;
    session_execute_at(s, argv, 4, &out, T0);
    EXPECT(out, "+OK\r\n");

    exec_sess(s, T0, &out, 1, "ASKING");
    exec_sess(s, T0, &out, 2, "GET", dst);
    EXPECT(out, "$1\r\nv\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

/* ------------------------------------------------------------------ */
/* wire: full two-server migration flow                               */
/* ------------------------------------------------------------------ */
#include <stdlib.h>

#include "core/migrate.h"
#include "pal/pal_socket.h"
#include "server/server.h"

#define IDA "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define IDB "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static void pump_target(void *ctx)
{
    server_run_once((server *)ctx, 1);
}

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

static void fmt_cmd(char *buf, size_t cap, int argc, ...)
{
    va_list ap;
    size_t off = 0;
    int i;
    off += (size_t)snprintf(buf + off, cap - off, "*%d\r\n", argc);
    va_start(ap, argc);
    for (i = 0; i < argc; i++) {
        const char *a = va_arg(ap, const char *);
        off += (size_t)snprintf(buf + off, cap - off, "$%zu\r\n%s\r\n",
                                strlen(a), a);
    }
    va_end(ap);
}

#define EXPECTW(buf, s) DD_CHECK_MEM((s), strlen(s), (buf), strlen(buf))

static void test_wire_full_flow(void)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char req[512], buf[4096], portb[16], ask[128], mkey[16];
    int i = 0, learned = 0;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    key_in_slot(FOO_SLOT, mkey);
    DD_CHECK(mkey[0] != '\0');
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL && b != NULL);
    server_enable_cluster(a, IDA);
    server_enable_cluster(b, IDB);
    migrate_set_pump_hook(pump_target, b);
    ca = cli(server_port(a));
    cb = cli(server_port(b));
    snprintf(portb, sizeof(portb), "%u", (unsigned)server_port(b));

    /* meet + a takes foo's slot; wait until gossip reaches b (wall-clock
     * bounded: iteration counts lie when the loop wakes instantly) */
    fmt_cmd(req, sizeof(req), 4, "CLUSTER", "MEET", "127.0.0.1", portb);
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECTW(buf, "+OK\r\n");
    fmt_cmd(req, sizeof(req), 3, "CLUSTER", "ADDSLOTS", "12182");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECTW(buf, "+OK\r\n");
    {
        uint64_t dl = pal_now_ms() + 12000;
        while (!learned && pal_now_ms() < dl) {
            pump2(a, b);
            if (i++ % 40 == 0) {
                fmt_cmd(req, sizeof(req), 2, "CLUSTER", "NODES");
                ask2(a, b, cb, req, buf, sizeof(buf));
                if (strstr(buf, "12182") != NULL && strstr(buf, IDA) != NULL)
                    learned = 1;
            }
        }
    }
    DD_CHECK_EQ_INT(1, learned);

    fmt_cmd(req, sizeof(req), 3, "SET", "foo", "hello");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECTW(buf, "+OK\r\n");

    /* a: migrating to b; b: importing from a */
    fmt_cmd(req, sizeof(req), 6, "CLUSTER", "SETSLOT", "12182", "MIGRATING",
            "TO", IDB);
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECTW(buf, "+OK\r\n");
    fmt_cmd(req, sizeof(req), 6, "CLUSTER", "SETSLOT", "12182", "IMPORTING",
            "FROM", IDA);
    ask2(a, b, cb, req, buf, sizeof(buf));
    EXPECTW(buf, "+OK\r\n");

    /* a still serves the existing key, absent keys -ASK to b */
    fmt_cmd(req, sizeof(req), 2, "GET", "foo");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECTW(buf, "$5\r\nhello\r\n");
    snprintf(ask, sizeof(ask), "-ASK 12182 127.0.0.1:%s\r\n", portb);
    fmt_cmd(req, sizeof(req), 2, "GET", mkey);
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECTW(buf, ask);

    /* b redirects with -MOVED unless the client said ASKING */
    fmt_cmd(req, sizeof(req), 2, "GET", "foo");
    ask2(a, b, cb, req, buf, sizeof(buf));
    DD_CHECK(buf[0] == '-' && strstr(buf, "MOVED 12182") != NULL);
    fmt_cmd(req, sizeof(req), 1, "ASKING");
    ask2(a, b, cb, req, buf, sizeof(buf));
    EXPECTW(buf, "+OK\r\n");
    fmt_cmd(req, sizeof(req), 2, "GET", "foo");
    ask2(a, b, cb, req, buf, sizeof(buf));
    EXPECTW(buf, "$-1\r\n");

    /* migrate the key; the internal ASKING+RESTORE pipeline passes b's
     * importing check */
    fmt_cmd(req, sizeof(req), 6, "MIGRATE", "127.0.0.1", portb, "foo", "0",
            "1000");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECTW(buf, "+OK\r\n");

    /* a no longer has it -> -ASK; b serves it under ASKING */
    fmt_cmd(req, sizeof(req), 2, "GET", "foo");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECTW(buf, ask);
    fmt_cmd(req, sizeof(req), 1, "ASKING");
    ask2(a, b, cb, req, buf, sizeof(buf));
    fmt_cmd(req, sizeof(req), 2, "GET", "foo");
    ask2(a, b, cb, req, buf, sizeof(buf));
    EXPECTW(buf, "$5\r\nhello\r\n");

    /* finalize: b becomes the owner on both ends */
    fmt_cmd(req, sizeof(req), 5, "CLUSTER", "SETSLOT", "12182", "NODE", IDB);
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECTW(buf, "+OK\r\n");
    ask2(a, b, cb, req, buf, sizeof(buf));
    EXPECTW(buf, "+OK\r\n");

    fmt_cmd(req, sizeof(req), 2, "GET", "foo");
    ask2(a, b, cb, req, buf, sizeof(buf));
    EXPECTW(buf, "$5\r\nhello\r\n");
    /* a redirects to the new owner (poll: propagation/load jitter) */
    {
        int moved = 0;
        uint64_t dl = pal_now_ms() + 12000;
        while (!moved && pal_now_ms() < dl) {
            fmt_cmd(req, sizeof(req), 2, "GET", "foo");
            ask2(a, b, ca, req, buf, sizeof(buf));
            if (buf[0] == '-' && strstr(buf, "MOVED 12182") != NULL)
                moved = 1;
        }
        DD_CHECK_EQ_INT(1, moved);
    }

    pal_close(ca);
    pal_close(cb);
    migrate_set_pump_hook(NULL, NULL);
    server_destroy(a);
    server_destroy(b);
}

int main(void)
{
    DD_RUN(test_setslot_migrating);
    DD_RUN(test_setslot_importing);
    DD_RUN(test_ask_redirect);
    DD_RUN(test_asking_bypass);
    DD_RUN(test_asking_invalid_import_owner);
    DD_RUN(test_restore_asking_cluster);
    DD_RUN(test_wire_full_flow);
    return DD_TEST_SUMMARY();
}
