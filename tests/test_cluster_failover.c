/* test_cluster_failover.c - replica failover: promotion primitive,
 * CLUSTER FAILOVER command, automatic failover on master loss (wire,
 * 3 nodes) and manual TAKEOVER (wire). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/hashslot.h"
#include "core/session.h"
#include "test.h"

#define T0 1000000ULL
#define TEST_ID "0123456789abcdef0123456789abcdef01234567"
#define OTHER_ID "ffffffffffffffffffffffffffffffffffffffff"

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

static void test_promote_primitive(void)
{
    db d;
    session *s;
    resp_buf out;
    cluster_node *me, *other;
    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);
    other = add_node(&d, OTHER_ID, 7002);
    cluster_slots_set(other->slots, 0, 1);
    cluster_slots_set(other->slots, 1, 1);
    cluster_slots_set(other->slots, 2, 1);
    other->epoch = 4;
    d.cluster_current_epoch = 4;

    /* a master cannot promote */
    DD_CHECK_EQ_INT(0, cluster_failover_promote(&d));

    exec_sess(s, T0, &out, 3, "CLUSTER", "REPLICATE", OTHER_ID);
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT(1, cluster_failover_promote(&d));

    me = cluster_myself(&d);
    DD_CHECK(me->flags & CLUSTER_NODE_MASTER);
    DD_CHECK(!(me->flags & CLUSTER_NODE_SLAVE));
    DD_CHECK_STR("-", me->master_id);
    DD_CHECK_EQ_INT(5, me->epoch); /* bumped past the dead master's */
    DD_CHECK_EQ_INT(5, d.cluster_current_epoch);
    DD_CHECK_EQ_INT(1, cluster_slots_get(me->slots, 0));
    DD_CHECK_EQ_INT(1, cluster_slots_get(me->slots, 2));
    DD_CHECK_EQ_INT(0, cluster_slots_get(other->slots, 0));

    DD_CHECK_EQ_INT(0, cluster_failover_promote(&d)); /* already master */

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_failover_command(void)
{
    db d;
    session *s;
    resp_buf out;
    cluster_node *me;
    db_init(&d);
    resp_buf_init(&out);
    s = fresh_session(&d);
    add_node(&d, OTHER_ID, 7002);

    /* on a master: rejected */
    exec_sess(s, T0, &out, 2, "CLUSTER", "FAILOVER");
    EXPECT(out, "-ERR You should send CLUSTER FAILOVER to a replica\r\n");
    exec_sess(s, T0, &out, 3, "CLUSTER", "FAILOVER", "TAKEOVER");
    EXPECT(out, "-ERR You should send CLUSTER FAILOVER to a replica\r\n");

    /* bad option */
    exec_sess(s, T0, &out, 3, "CLUSTER", "REPLICATE", OTHER_ID);
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "CLUSTER", "FAILOVER", "BOGUS");
    EXPECT(out, "-ERR syntax error\r\n");

    /* on a replica: promotes (simplified: same as TAKEOVER) */
    exec_sess(s, T0, &out, 2, "CLUSTER", "FAILOVER");
    EXPECT(out, "+OK\r\n");
    me = cluster_myself(&d);
    DD_CHECK(me->flags & CLUSTER_NODE_MASTER);

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

/* ------------------------------------------------------------------ */
/* wire helpers                                                       */
/* ------------------------------------------------------------------ */
#include "pal/pal_socket.h"
#include "server/server.h"

#define IDA "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define IDB "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define IDC "cccccccccccccccccccccccccccccccccccccccc"

static void pump3(server *x, server *y, server *z)
{
    server_run_once(x, 5);
    server_run_once(y, 5);
    server_run_once(z, 5);
}

static pal_socket_t cli(uint16_t port)
{
    pal_socket_t c = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(c != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));
    return c;
}

/* send a command, pump the live servers, read the reply; pass the dead
 * server as z to skip it (z may be NULL) */
static size_t ask3(server *x, server *y, server *z, pal_socket_t c,
                   const char *req, char *buf, size_t cap)
{
    size_t got = 0;
    int iter = 0;
    DD_CHECK_EQ_INT((long long)strlen(req),
                    (long long)pal_send(c, req, strlen(req)));
    while (iter < 4000) {
        ptrdiff_t n;
        iter++;
        if (z != NULL)
            pump3(x, y, z);
        else {
            server_run_once(x, 5);
            server_run_once(y, 5);
        }
        n = pal_recv(c, buf + got, cap - got - 1);
        if (n > 0)
            got += (size_t)n;
        if (n > 0)
            break;
    }
    buf[got] = '\0';
    return got;
}

/* assign the full slot range to this server (chunked ADDSLOTS) */
static void addslots_all(server *x, server *y, server *z, pal_socket_t c,
                         char *buf, size_t cap)
{
    int base;
    for (base = 0; base < 16384; base += 2048) {
        char *big = malloc(32768);
        size_t bl = 0, off = 0, got = 0;
        int sl, iter = 0;
        DD_CHECK(big != NULL);
        bl += (size_t)snprintf(big + bl, 32768 - bl,
                               "*2050\r\n$7\r\nCLUSTER\r\n$8\r\nADDSLOTS\r\n");
        for (sl = base; sl < base + 2048; sl++) {
            char num[8];
            int nl = snprintf(num, sizeof(num), "%d", sl);
            bl += (size_t)snprintf(big + bl, 32768 - bl, "$%d\r\n%s\r\n", nl,
                                   num);
        }
        while (off < bl) {
            ptrdiff_t w = pal_send(c, big + off, bl - off);
            if (w > 0)
                off += (size_t)w;
            else if (z != NULL)
                pump3(x, y, z);
            else {
                server_run_once(x, 5);
                server_run_once(y, 5);
            }
        }
        while (got < 5 && iter < 4000) {
            ptrdiff_t n;
            iter++;
            if (z != NULL)
                pump3(x, y, z);
            else {
                server_run_once(x, 5);
                server_run_once(y, 5);
            }
            n = pal_recv(c, buf + got, cap - got);
            if (n > 0)
                got += (size_t)n;
        }
        DD_CHECK(got >= 5 && memcmp(buf, "+OK\r\n", 5) == 0);
        free(big);
    }
}

static void key_in_slot(uint32_t slot, char *out)
{
    int i;
    for (i = 0; i < 100000; i++) {
        snprintf(out, 16, "fkey%d", i);
        if (hash_slot(out, strlen(out)) == slot)
            return;
    }
    out[0] = '\0';
}

/* poll CLUSTER INFO on c until it reports the wanted state (bounded) */
static int wait_state(server *x, server *y, server *z, pal_socket_t c,
                      const char *want, char *buf, size_t cap)
{
    int i;
    for (i = 0; i < 800; i++) {
        if (z != NULL)
            pump3(x, y, z);
        else {
            server_run_once(x, 5);
            server_run_once(y, 5);
        }
        if (i % 40 == 0) {
            ask3(x, y, z, c, "*2\r\n$7\r\nCLUSTER\r\n$4\r\nINFO\r\n", buf,
                 cap);
            if (strstr(buf, want) != NULL)
                return 1;
        }
    }
    return 0;
}

/* poll CLUSTER NODES on c until it contains the needle (bounded) */
static int wait_nodes(server *x, server *y, server *z, pal_socket_t c,
                      const char *needle, char *buf, size_t cap)
{
    int i;
    for (i = 0; i < 800; i++) {
        if (z != NULL)
            pump3(x, y, z);
        else {
            server_run_once(x, 5);
            server_run_once(y, 5);
        }
        if (i % 40 == 0) {
            ask3(x, y, z, c, "*2\r\n$7\r\nCLUSTER\r\n$5\r\nNODES\r\n", buf,
                 cap);
            if (strstr(buf, needle) != NULL)
                return 1;
        }
    }
    return 0;
}

static void test_wire_auto_failover(void)
{
    server *a, *b, *c;
    pal_socket_t ca, cb, cc;
    char req[512], buf[8192], port[16], key[16];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    c = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL && b != NULL && c != NULL);
    server_enable_cluster(a, IDA);
    server_enable_cluster(b, IDB);
    server_enable_cluster(c, IDC);
    /* node timeout must exceed the 1s gossip cadence, else live masters
     * get false-positive failure marks between pings */
    server_set_node_timeout(b, 2000);
    server_set_node_timeout(c, 2000);
    ca = cli(server_port(a));
    cb = cli(server_port(b));
    cc = cli(server_port(c));

    /* topology: full mesh (a<->b, a<->c, b<->c) so every master can form
     * local suspicion of every other; a owns everything; b replicates a */
    snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
    snprintf(req, sizeof(req),
             "*4\r\n$7\r\nCLUSTER\r\n$4\r\nMEET\r\n$9\r\n127.0.0.1\r\n"
             "$%zu\r\n%s\r\n",
             strlen(port), port);
    ask3(a, b, c, ca, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);
    snprintf(port, sizeof(port), "%u", (unsigned)server_port(c));
    snprintf(req, sizeof(req),
             "*4\r\n$7\r\nCLUSTER\r\n$4\r\nMEET\r\n$9\r\n127.0.0.1\r\n"
             "$%zu\r\n%s\r\n",
             strlen(port), port);
    ask3(a, b, c, ca, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);
    ask3(a, b, c, cb, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);
    addslots_all(a, b, c, ca, buf, sizeof(buf));
    snprintf(req, sizeof(req), "*3\r\n$7\r\nCLUSTER\r\n$9\r\nREPLICATE\r\n"
                               "$40\r\n%s\r\n",
             IDA);
    ask3(a, b, c, cb, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);
    DD_CHECK(wait_state(a, b, c, ca, "cluster_state:ok\r\n", buf,
                        sizeof(buf)));
    /* b must have learned a's claims via gossip before a dies */
    DD_CHECK(wait_nodes(a, b, c, cb, "0-16383", buf, sizeof(buf)));

    /* a dies: stop pumping it entirely. The PFAIL window is not reliably
     * observable here (a pre-existing report from c completes the quorum
     * in the same cron tick that sets PFAIL -- covered deterministically
     * in test_cluster_pfail.c), so assert the order that matters: a is
     * marked FAIL first, and only then does b's promotion heal the
     * state */
    DD_CHECK(wait_nodes(b, c, NULL, cb, ",fail ", buf, sizeof(buf)));
    DD_CHECK(wait_nodes(b, c, NULL, cb, "myself,master", buf, sizeof(buf)));
    DD_CHECK(wait_state(b, c, NULL, cb, "cluster_state:ok\r\n", buf,
                        sizeof(buf)));
    DD_CHECK(wait_state(b, c, NULL, cc, "cluster_state:ok\r\n", buf,
                        sizeof(buf)));

    /* b is writable again (promotion stopped data replication) */
    key_in_slot(0, key);
    DD_CHECK(key[0] != '\0');
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nv\r\n",
             strlen(key), key);
    ask3(b, c, NULL, cb, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);

    /* a rejoins: it sees b's higher-epoch claims and yields */
    DD_CHECK(wait_state(b, c, a, ca, "cluster_state:ok\r\n", buf,
                        sizeof(buf)));
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(key), key);
    {
        int i, moved = 0;
        for (i = 0; i < 800 && !moved; i++) {
            pump3(b, c, a);
            if (i % 40 == 0) {
                ask3(b, c, a, ca, req, buf, sizeof(buf));
                if (strstr(buf, "-MOVED 0 ") != NULL)
                    moved = 1;
            }
        }
        DD_CHECK_EQ_INT(1, moved);
    }

    pal_close(cc);
    pal_close(cb);
    pal_close(ca);
    server_destroy(c);
    server_destroy(b);
    server_destroy(a);
    pal_socket_cleanup();
}

static void test_wire_manual_takeover(void)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char req[512], buf[8192], port[16], key[16];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL && b != NULL);
    server_enable_cluster(a, IDA);
    server_enable_cluster(b, IDB);
    ca = cli(server_port(a));
    cb = cli(server_port(b));

    snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
    snprintf(req, sizeof(req),
             "*4\r\n$7\r\nCLUSTER\r\n$4\r\nMEET\r\n$9\r\n127.0.0.1\r\n"
             "$%zu\r\n%s\r\n",
             strlen(port), port);
    ask3(a, b, NULL, ca, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);
    addslots_all(a, b, NULL, ca, buf, sizeof(buf));
    snprintf(req, sizeof(req), "*3\r\n$7\r\nCLUSTER\r\n$9\r\nREPLICATE\r\n"
                               "$40\r\n%s\r\n",
             IDA);
    ask3(a, b, NULL, cb, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);
    DD_CHECK(wait_state(a, b, NULL, ca, "cluster_state:ok\r\n", buf,
                        sizeof(buf)));
    /* b must have learned a's claims before taking over */
    DD_CHECK(wait_nodes(a, b, NULL, cb, "0-16383", buf, sizeof(buf)));

    /* FAILOVER on the master is rejected */
    ask3(a, b, NULL, ca, "*2\r\n$7\r\nCLUSTER\r\n$8\r\nFAILOVER\r\n", buf,
         sizeof(buf));
    DD_CHECK_STR("-ERR You should send CLUSTER FAILOVER to a replica\r\n",
                 buf);

    /* TAKEOVER on the replica: immediate promotion while a lives */
    ask3(a, b, NULL, cb, "*3\r\n$7\r\nCLUSTER\r\n$8\r\nFAILOVER\r\n"
                         "$8\r\nTAKEOVER\r\n",
         buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);
    DD_CHECK(wait_state(a, b, NULL, cb, "cluster_state:ok\r\n", buf,
                        sizeof(buf)));

    key_in_slot(0, key);
    snprintf(req, sizeof(req), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\nv\r\n",
             strlen(key), key);
    ask3(a, b, NULL, cb, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);

    /* a yields via gossip (higher epoch) */
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(key), key);
    {
        int i, moved = 0;
        for (i = 0; i < 800 && !moved; i++) {
            server_run_once(a, 5);
            server_run_once(b, 5);
            if (i % 40 == 0) {
                ask3(a, b, NULL, ca, req, buf, sizeof(buf));
                if (strstr(buf, "-MOVED 0 ") != NULL)
                    moved = 1;
            }
        }
        DD_CHECK_EQ_INT(1, moved);
    }

    pal_close(cb);
    pal_close(ca);
    server_destroy(b);
    server_destroy(a);
    pal_socket_cleanup();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    DD_RUN(test_promote_primitive);
    DD_RUN(test_failover_command);
    DD_RUN(test_wire_auto_failover);
    DD_RUN(test_wire_manual_takeover);
    return DD_TEST_SUMMARY();
}
