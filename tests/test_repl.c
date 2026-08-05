/* test_repl.c - replication backlog ring buffer (sub-step 1). */
#include <string.h>

#include "server/repl.h"
#include "test.h"

static void test_backlog_basic(void)
{
    repl_backlog b;
    char out[64];
    repl_backlog_init(&b, 16);

    repl_backlog_append(&b, "hello", 5);
    DD_CHECK_EQ_INT(5, (long long)b.offset);
    DD_CHECK_EQ_INT(5, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_MEM("hello", 5, out, 5);

    repl_backlog_append(&b, " world", 6);
    DD_CHECK_EQ_INT(11, (long long)b.offset);
    DD_CHECK_EQ_INT(11, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_MEM("hello world", 11, out, 11);

    repl_backlog_free(&b);
}

static void test_backlog_wrap(void)
{
    repl_backlog b;
    char out[64];
    repl_backlog_init(&b, 16);

    /* 10 + 10 bytes into a 16-byte ring: oldest 4 dropped */
    repl_backlog_append(&b, "0123456789", 10);
    repl_backlog_append(&b, "abcdefghij", 10);
    DD_CHECK_EQ_INT(20, (long long)b.offset); /* offset counts everything */
    DD_CHECK_EQ_INT(16, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_MEM("456789abcdefghij", 16, out, 16);

    /* a write larger than the ring keeps only its tail */
    repl_backlog_append(&b, "0123456789ABCDEFGHIJ", 20);
    DD_CHECK_EQ_INT(40, (long long)b.offset);
    DD_CHECK_EQ_INT(16, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_MEM("456789ABCDEFGHIJ", 16, out, 16);

    repl_backlog_free(&b);
}

static void test_sync_master(void);
static void test_replica_full_cycle(void);
static void test_replica_reconnect_resync(void);

int main(void)
{
    DD_RUN(test_backlog_basic);
    DD_RUN(test_backlog_wrap);
    DD_RUN(test_sync_master);
    DD_RUN(test_replica_full_cycle);
    DD_RUN(test_replica_reconnect_resync);
    return DD_TEST_SUMMARY();
}

/* ------------------------------------------------------------------ */
/* master side: SYNC full-resync + command streaming (sub-step 2)     */
/* ------------------------------------------------------------------ */
#include <stdarg.h>
#include <stdio.h>

#include "core/session.h"
#include "core/snapshot.h"
#include "pal/pal_socket.h"
#include "server/server.h"

static void sess_cmd(session *s, resp_buf *out, int argc, ...)
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
    session_execute(s, argv, (size_t)argc, out);
}

#define EXPECT(out, s) DD_CHECK_MEM((s), strlen(s), (out).data, (out).len)

/* pump the server until at least want bytes arrived on the client */
static size_t pump_recv(server *s, pal_socket_t c, char *buf, size_t got,
                        size_t want)
{
    int iter = 0;
    while (got < want && iter < 10000) {
        ptrdiff_t n;
        iter++;
        server_run_once(s, 50);
        n = pal_recv(c, buf + got, want - got);
        if (n > 0)
            got += (size_t)n;
    }
    return got;
}

static void test_sync_master(void)
{
    server *m;
    pal_socket_t a, b;
    resp_buf out;
    char buf[8192];
    size_t got;
    int iter;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    m = server_create("127.0.0.1", 0);
    DD_CHECK(m != NULL);
    resp_buf_init(&out);

    /* seed data via a normal client session path (session on server db
     * would bypass propagation; use a real client) */
    a = pal_tcp_connect("127.0.0.1", server_port(m));
    DD_CHECK(a != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(a, 1));
    DD_CHECK_EQ_INT(27, pal_send(a, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n", 27));
    got = pump_recv(m, a, buf, 0, 5);
    DD_CHECK(got >= 5 && memcmp(buf, "+OK\r\n", 5) == 0);

    /* raw SYNC client */
    b = pal_tcp_connect("127.0.0.1", server_port(m));
    DD_CHECK(b != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(b, 1));
    DD_CHECK_EQ_INT(14, pal_send(b, "*1\r\n$4\r\nSYNC\r\n", 14));

    /* read the $<len> header */
    got = pump_recv(m, b, buf, 0, 16);
    DD_CHECK(got >= 4 && buf[0] == '$');
    {
        size_t snaplen = 0;
        size_t hdrlen = 0;
        size_t total;
        char *snap;
        db d2;
        session *r;
        while (hdrlen < got && buf[hdrlen] != '\n')
            hdrlen++;
        DD_CHECK(hdrlen < got);
        for (size_t i = 1; i < hdrlen && buf[i] >= '0' && buf[i] <= '9'; i++)
            snaplen = snaplen * 10 + (size_t)(buf[i] - '0');
        hdrlen++; /* skip \n */
        total = hdrlen + snaplen;
        got = pump_recv(m, b, buf, got, total);
        DD_CHECK_EQ_INT((long long)total, (long long)got);

        /* frame is a valid snapshot containing k=v */
        snap = buf + hdrlen;
        db_init(&d2);
        DD_CHECK_EQ_INT(0, snapshot_load_mem(&d2, snap, snaplen, 1000000));
        r = session_create(&d2);
        sess_cmd(r, &out, 2, "GET", "k");
        EXPECT(out, "$1\r\nv\r\n");
        session_free(r);
        db_destroy(&d2);
    }

    /* subsequent writes on the master stream to the replica conn */
    DD_CHECK_EQ_INT(29, pal_send(a, "*3\r\n$3\r\nSET\r\n$2\r\nk2\r\n$2\r\nv2\r\n", 29));
    got = pump_recv(m, a, buf, 0, 5);
    DD_CHECK(got >= 5);
    got = pump_recv(m, b, buf, 0, 29);
    DD_CHECK_EQ_INT(29, (long long)got);
    DD_CHECK_MEM("*3\r\n$3\r\nSET\r\n$2\r\nk2\r\n$2\r\nv2\r\n", 29, buf, 29);

    /* INFO replication on the master */
    DD_CHECK_EQ_INT(14, pal_send(a, "*1\r\n$4\r\nINFO\r\n", 14));
    iter = 0;
    got = 0;
    while (iter < 10000) {
        ptrdiff_t n;
        iter++;
        server_run_once(m, 50);
        n = pal_recv(a, buf + got, sizeof(buf) - 1 - got);
        if (n > 0) {
            got += (size_t)n;
            buf[got] = '\0';
            if (strstr(buf, "# Replication\r\n") != NULL)
                break;
        }
    }
    DD_CHECK(got > 0);
    buf[got] = '\0';
    DD_CHECK(strstr(buf, "role:master\r\n") != NULL);
    DD_CHECK(strstr(buf, "connected_slaves:1\r\n") != NULL);
    DD_CHECK(strstr(buf, "master_repl_offset:") != NULL);
    /* Phase 12: every server has a 40-hex replication id */
    {
        const char *p = strstr(buf, "master_replid:");
        int i;
        int hex = 0;
        DD_CHECK(p != NULL);
        if (p != NULL) {
            p += strlen("master_replid:");
            for (i = 0; i < 40; i++) {
                char ch = p[i];
                if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))
                    hex++;
            }
            DD_CHECK_EQ_INT(40, hex);
            DD_CHECK(p[40] == '\r' && p[41] == '\n');
        }
    }

    pal_close(b);
    pal_close(a);
    server_destroy(m);
    pal_socket_cleanup();
    resp_buf_free(&out);
}

/* ------------------------------------------------------------------ */
/* replica side: REPLICAOF full cycle (sub-step 3)                    */
/* ------------------------------------------------------------------ */

/* pump two servers; bounded request/response against one of them */
static void pump2(server *x, server *y)
{
    server_run_once(x, 5);
    server_run_once(y, 5);
}

static void rt2(server *x, server *y, pal_socket_t c, const char *req,
                const char *expected, char *buf, size_t bufcap)
{
    size_t elen = strlen(expected);
    size_t rlen = strlen(req);
    size_t got = 0;
    int iter = 0;
    DD_CHECK(elen <= bufcap);
    DD_CHECK_EQ_INT((long long)rlen,
                    (long long)pal_send(c, req, rlen));
    while (got < elen && iter < 10000) {
        ptrdiff_t n;
        iter++;
        pump2(x, y);
        n = pal_recv(c, buf + got, bufcap - got);
        if (n > 0)
            got += (size_t)n;
    }
    DD_CHECK_EQ_INT((long long)elen, (long long)got);
    DD_CHECK_MEM(expected, elen, buf, got);
}

static pal_socket_t nb_client(uint16_t port)
{
    pal_socket_t c = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(c != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));
    return c;
}

static void test_replica_full_cycle(void)
{
    server *m, *r;
    pal_socket_t mc, rc;
    char buf[8192];
    char port_str[16];
    int i;
    int synced;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    m = server_create("127.0.0.1", 0);
    r = server_create("127.0.0.1", 0);
    DD_CHECK(m != NULL && r != NULL);
    mc = nb_client(server_port(m));
    rc = nb_client(server_port(r));

    /* key does not exist on the replica yet */
    rt2(m, r, rc, "*2\r\n$3\r\nGET\r\n$1\r\nx\r\n", "$-1\r\n", buf,
        sizeof(buf));

    /* link the replica */
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)server_port(m));
    {
        char req[128];
        const char *p1 = "*3\r\n$9\r\nREPLICAOF\r\n$9\r\n127.0.0.1\r\n$";
        snprintf(req, sizeof(req), "%s%zu\r\n%s\r\n", p1, strlen(port_str),
                 port_str);
        rt2(m, r, rc, req, "+OK\r\n", buf, sizeof(buf));
    }

    /* write on the master; the replica catches up (bounded poll) */
    rt2(m, r, mc, "*3\r\n$3\r\nSET\r\n$1\r\nx\r\n$5\r\nhello\r\n", "+OK\r\n",
        buf, sizeof(buf));
    synced = 0;
    for (i = 0; i < 500 && !synced; i++) {
        ptrdiff_t n;
        pump2(m, r);
        if (pal_send(rc, "*2\r\n$3\r\nGET\r\n$1\r\nx\r\n", 20) != 20)
            break;
        pump2(m, r);
        n = pal_recv(rc, buf, sizeof(buf));
        if (n > 0 && memcmp(buf, "$5\r\nhello\r\n", (size_t)n) == 0)
            synced = 1;
    }
    DD_CHECK_EQ_INT(1, synced);

    /* replica is read-only for client writes */
    rt2(m, r, rc, "*3\r\n$3\r\nSET\r\n$1\r\ny\r\n$1\r\n1\r\n",
        "-READONLY You can't write against a read only replica.\r\n", buf,
        sizeof(buf));
    rt2(m, r, rc, "*2\r\n$3\r\nDEL\r\n$1\r\nx\r\n",
        "-READONLY You can't write against a read only replica.\r\n", buf,
        sizeof(buf));

    /* INFO replication on the replica */
    {
        size_t got = 0;
        int iter = 0;
        DD_CHECK_EQ_INT(14, pal_send(rc, "*1\r\n$4\r\nINFO\r\n", 14));
        while (got < sizeof(buf) - 1 && iter < 10000) {
            ptrdiff_t n;
            iter++;
            pump2(m, r);
            n = pal_recv(rc, buf + got, sizeof(buf) - 1 - got);
            if (n > 0) {
                got += (size_t)n;
                buf[got] = '\0';
                if (strstr(buf, "master_link_status:up\r\n") != NULL)
                    break;
            }
        }
        buf[got] = '\0';
        DD_CHECK(strstr(buf, "role:slave\r\n") != NULL);
        DD_CHECK(strstr(buf, "master_host:127.0.0.1\r\n") != NULL);
        DD_CHECK(strstr(buf, "master_link_status:up\r\n") != NULL);
    }

    /* REPLICAOF NO ONE promotes to master and allows writes */
    rt2(m, r, rc,
        "*3\r\n$9\r\nREPLICAOF\r\n$2\r\nNO\r\n$3\r\nONE\r\n", "+OK\r\n", buf,
        sizeof(buf));
    rt2(m, r, rc, "*3\r\n$3\r\nSET\r\n$1\r\ny\r\n$1\r\n1\r\n", "+OK\r\n",
        buf, sizeof(buf));

    pal_close(rc);
    pal_close(mc);
    server_destroy(r);
    server_destroy(m);
    pal_socket_cleanup();
}

static void test_replica_reconnect_resync(void)
{
    server *m, *r;
    pal_socket_t mc, rc;
    char buf[8192];
    char port_str[16];
    uint16_t mport;
    int i;
    int synced;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    m = server_create("127.0.0.1", 0);
    r = server_create("127.0.0.1", 0);
    DD_CHECK(m != NULL && r != NULL);
    mport = server_port(m);
    mc = nb_client(mport);
    rc = nb_client(server_port(r));

    snprintf(port_str, sizeof(port_str), "%u", (unsigned)mport);
    {
        char req[128];
        const char *p1 = "*3\r\n$9\r\nREPLICAOF\r\n$9\r\n127.0.0.1\r\n$";
        snprintf(req, sizeof(req), "%s%zu\r\n%s\r\n", p1, strlen(port_str),
                 port_str);
        rt2(m, r, rc, req, "+OK\r\n", buf, sizeof(buf));
    }
    rt2(m, r, mc, "*3\r\n$3\r\nSET\r\n$3\r\nold\r\n$3\r\nval\r\n", "+OK\r\n",
        buf, sizeof(buf));

    /* wait for the initial sync */
    synced = 0;
    for (i = 0; i < 500 && !synced; i++) {
        ptrdiff_t n;
        pump2(m, r);
        if (pal_send(rc, "*2\r\n$3\r\nGET\r\n$3\r\nold\r\n", 22) != 22)
            break;
        pump2(m, r);
        n = pal_recv(rc, buf, sizeof(buf));
        if (n > 0 && memcmp(buf, "$3\r\nval\r\n", (size_t)n) == 0)
            synced = 1;
    }
    DD_CHECK_EQ_INT(1, synced);

    /* master "restarts": a fresh empty master on the same port */
    pal_close(mc);
    server_destroy(m);
    m = server_create("127.0.0.1", mport);
    DD_CHECK(m != NULL);
    mc = nb_client(mport);
    rt2(m, r, mc, "*3\r\n$3\r\nSET\r\n$5\r\nfresh\r\n$3\r\nnew\r\n",
        "+OK\r\n", buf, sizeof(buf));

    /* replica reconnects and full-resyncs: old key gone, fresh key there */
    synced = 0;
    for (i = 0; i < 1000 && !synced; i++) {
        ptrdiff_t n;
        pump2(m, r);
        if (pal_send(rc, "*2\r\n$3\r\nGET\r\n$5\r\nfresh\r\n", 24) != 24)
            break;
        pump2(m, r);
        n = pal_recv(rc, buf, sizeof(buf));
        if (n > 0 && memcmp(buf, "$3\r\nnew\r\n", (size_t)n) == 0)
            synced = 1;
    }
    DD_CHECK_EQ_INT(1, synced);
    /* the pre-restart key was wiped by the full resync */
    rt2(m, r, rc, "*2\r\n$3\r\nGET\r\n$3\r\nold\r\n", "$-1\r\n", buf,
        sizeof(buf));

    pal_close(rc);
    pal_close(mc);
    server_destroy(r);
    server_destroy(m);
    pal_socket_cleanup();
}
