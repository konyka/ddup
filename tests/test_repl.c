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

int main(void)
{
    DD_RUN(test_backlog_basic);
    DD_RUN(test_backlog_wrap);
    DD_RUN(test_sync_master);
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

    pal_close(b);
    pal_close(a);
    server_destroy(m);
    pal_socket_cleanup();
    resp_buf_free(&out);
}
