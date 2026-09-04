/* test_repl.c - replication backlog ring buffer (sub-step 1). */
#include <stdlib.h>
#include <string.h>

#include "pal/pal_iocp.h"
#include "server/repl.h"
#include "server/server.h"
#include "test.h"

static void test_backlog_basic(void)
{
    repl_backlog b;
    char out[64];
    DD_CHECK_EQ_INT(0, repl_backlog_init(&b, 16));

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
    DD_CHECK_EQ_INT(0, repl_backlog_init(&b, 16));

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

static void test_backlog_empty_state(void)
{
    repl_backlog b;
    char out[8] = {'s', 'e', 'n', 't', 'i', 'n', 'e', 'l'};

    memset(&b, 0, sizeof(b));
    DD_CHECK_EQ_INT(-1, repl_backlog_init(&b, 0));
    DD_CHECK(b.buf == NULL);
    DD_CHECK_EQ_INT(0, (long long)b.cap);
    DD_CHECK_EQ_INT(0, (long long)b.start);
    DD_CHECK_EQ_INT(0, (long long)b.len);
    DD_CHECK_EQ_INT(0, (long long)b.offset);

    repl_backlog_append(&b, "abc", 3);
    DD_CHECK_EQ_INT(3, (long long)b.offset);
    DD_CHECK_EQ_INT(0, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_EQ_INT(0, (long long)repl_backlog_read_from(&b, 0, out,
                                                        sizeof(out)));
    DD_CHECK_MEM("sentinel", 8, out, 8);
    repl_backlog_free(&b);
}

static void test_backlog_null_and_offset_safety(void)
{
    repl_backlog b;
    char out[8] = {'s', 'e', 'n', 't', 'i', 'n', 'e', 'l'};

    repl_backlog_free(NULL);
    repl_backlog_append(NULL, "x", 1);
    DD_CHECK_EQ_INT(0, (long long)repl_backlog_read(NULL, out, sizeof(out)));
    DD_CHECK_EQ_INT(0, (long long)repl_backlog_read_from(NULL, 0, out,
                                                         sizeof(out)));

    memset(&b, 0, sizeof(b));
    DD_CHECK_EQ_INT(0, repl_backlog_init(&b, 8));
    repl_backlog_append(&b, NULL, 1);
    DD_CHECK_EQ_INT(0, (long long)b.offset);
    DD_CHECK_EQ_INT(0, (long long)repl_backlog_read(&b, NULL, 1));
    DD_CHECK_EQ_INT(0, (long long)repl_backlog_read_from(&b, 0, NULL, 1));
    repl_backlog_free(&b);

    DD_CHECK_EQ_INT(0, repl_backlog_init(&b, 8));
    b.offset = UINT64_MAX - 1;
    repl_backlog_append(&b, "abc", 3);
    DD_CHECK_EQ_INT(UINT64_MAX, (long long)b.offset);
    DD_CHECK_EQ_INT(3, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_MEM("abc", 3, out, 3);
    repl_backlog_free(&b);
}

static void test_backlog_corrupt_state_fails_closed(void)
{
    repl_backlog b;
    char out[8] = {'s', 'e', 'n', 't', 'i', 'n', 'e', 'l'};

    DD_CHECK_EQ_INT(0, repl_backlog_init(&b, 8));
    repl_backlog_append(&b, "safe", 4);
    b.len = 9;
    b.offset = 4;
    repl_backlog_append(&b, "x", 1);
    DD_CHECK_EQ_INT(4, (long long)b.offset);
    DD_CHECK_EQ_INT(0, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_EQ_INT(0, (long long)repl_backlog_read_from(&b, 0, out,
                                                        sizeof(out)));

    b.len = 0;
    b.start = 8;
    repl_backlog_append(&b, "x", 1);
    DD_CHECK_EQ_INT(4, (long long)b.offset);
    DD_CHECK_EQ_INT(0, (long long)repl_backlog_read(&b, out, sizeof(out)));
    repl_backlog_free(&b);
}

static void test_backlog_boundaries_and_offsets(void)
{
    repl_backlog b;
    char out[32];

    DD_CHECK_EQ_INT(0, repl_backlog_init(&b, 8));

    repl_backlog_append(&b, "abcdefgh", 8);
    DD_CHECK_EQ_INT(8, (long long)b.offset);
    DD_CHECK_EQ_INT(8, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_MEM("abcdefgh", 8, out, 8);

    repl_backlog_append(&b, "12", 2);
    DD_CHECK_EQ_INT(10, (long long)b.offset);
    DD_CHECK_EQ_INT(8, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_MEM("cdefgh12", 8, out, 8);
    DD_CHECK_EQ_INT(6, (long long)repl_backlog_read_from(&b, 4, out,
                                                        sizeof(out)));
    DD_CHECK_MEM("efgh12", 6, out, 6);
    DD_CHECK_EQ_INT(0, (long long)repl_backlog_read_from(&b, 10, out,
                                                        sizeof(out)));
    DD_CHECK_EQ_INT(0, (long long)repl_backlog_read_from(&b, UINT64_MAX, out,
                                                        sizeof(out)));

    repl_backlog_append(&b, "34567", 5);
    DD_CHECK_EQ_INT(15, (long long)b.offset);
    DD_CHECK_EQ_INT(8, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_MEM("h1234567", 8, out, 8);
    DD_CHECK_EQ_INT(8, (long long)repl_backlog_read_from(&b, 0, out,
                                                        sizeof(out)));
    DD_CHECK_MEM("h1234567", 8, out, 8);

    repl_backlog_append(&b, "0123456789ABC", 13);
    DD_CHECK_EQ_INT(28, (long long)b.offset);
    DD_CHECK_EQ_INT(8, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_MEM("56789ABC", 8, out, 8);
    DD_CHECK_EQ_INT(4, (long long)repl_backlog_read_from(&b, 24, out,
                                                        sizeof(out)));
    DD_CHECK_MEM("9ABC", 4, out, 4);

    repl_backlog_free(&b);
}

static void test_sync_master(void);
static void test_psync_fullresync(void);
static void test_psync_continue_partial(void);
static void test_psync_stale_offset_fullresync(void);
static void test_chained_replication(void);
static void test_replica_large_snapshot(void);
static void test_invalid_snapshot_preserves_db(void);
static void test_rejected_fullresync_does_not_cache_psync(void);
static void test_replica_full_cycle(void);
static void test_replica_full_cycle_iocp(void);
static void test_replica_reconnect_resync(void);
static void test_replica_reconnect_resync_iocp(void);
static void test_psync_continue_reserve_failure(void);

int main(void)
{
    DD_RUN(test_backlog_basic);
    DD_RUN(test_backlog_wrap);
    DD_RUN(test_backlog_empty_state);
    DD_RUN(test_backlog_null_and_offset_safety);
    DD_RUN(test_backlog_corrupt_state_fails_closed);
    DD_RUN(test_backlog_boundaries_and_offsets);
    DD_RUN(test_sync_master);
    DD_RUN(test_psync_fullresync);
    DD_RUN(test_psync_continue_partial);
    DD_RUN(test_psync_stale_offset_fullresync);
    DD_RUN(test_chained_replication);
    DD_RUN(test_replica_large_snapshot);
    DD_RUN(test_invalid_snapshot_preserves_db);
    DD_RUN(test_rejected_fullresync_does_not_cache_psync);
    DD_RUN(test_replica_full_cycle);
    DD_RUN(test_replica_full_cycle_iocp);
    DD_RUN(test_replica_reconnect_resync);
    DD_RUN(test_replica_reconnect_resync_iocp);
    DD_RUN(test_psync_continue_reserve_failure);
    return DD_TEST_SUMMARY();
}

static void test_psync_continue_reserve_failure(void)
{
    DD_CHECK_EQ_INT(0, server_test_psync_continue_reserve_failure());
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

/* single-db accessor for the multi snapshot loader used by SYNC frames */
static db *repl_snap_get(void *ctx, int idx)
{
    (void)idx;
    return (db *)ctx;
}

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
    pal_socket_t a, b, c;
    resp_buf out;
    char buf[8192];
    char replid[41];
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
        DD_CHECK_EQ_INT(0, snapshot_load_mem_multi(&d2, repl_snap_get, 1,
                                                   snap, snaplen, 1000000));
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

    /* A successful resize clears retained bytes but preserves the absolute
     * stream position for subsequent appends and INFO reporting. */
    DD_CHECK_EQ_INT(0, server_set_backlog_size(m, 64));
    DD_CHECK_EQ_INT(29, pal_send(a,
                                "*3\r\n$3\r\nSET\r\n$2\r\nk3\r\n$2\r\nv3\r\n",
                                29));
    got = pump_recv(m, a, buf, 0, 5);
    DD_CHECK(got >= 5);
    got = pump_recv(m, b, buf, 0, 29);
    DD_CHECK_EQ_INT(29, (long long)got);

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
    DD_CHECK(strstr(buf, "master_repl_offset:58\r\n") != NULL);
    /* Phase 12: every server has a 40-hex replication id */
    {
        const char *p = strstr(buf, "master_replid:");
        int i;
        int hex = 0;
        DD_CHECK(p != NULL);
        if (p != NULL) {
            p += strlen("master_replid:");
            memcpy(replid, p, 40);
            replid[40] = '\0';
            for (i = 0; i < 40; i++) {
                char ch = p[i];
                if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))
                    hex++;
            }
            DD_CHECK_EQ_INT(40, hex);
            DD_CHECK(p[40] == '\r' && p[41] == '\n');
        }
    }

    /* Offset zero belonged to the cleared pre-resize contents. */
    c = pal_tcp_connect("127.0.0.1", server_port(m));
    DD_CHECK(c != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));
    {
        char req[128];
        int req_len = snprintf(req, sizeof(req),
                               "*3\r\n$5\r\nPSYNC\r\n$40\r\n%s\r\n$1\r\n0\r\n",
                               replid);
        DD_CHECK(req_len > 0);
        DD_CHECK_EQ_INT(req_len, pal_send(c, req, (size_t)req_len));
    }
    got = pump_recv(m, c, buf, 0, 12);
    DD_CHECK(got >= 12);
    DD_CHECK_MEM("+FULLRESYNC ", 12, buf, 12);

    pal_close(c);
    pal_close(b);
    pal_close(a);
    server_destroy(m);
    pal_socket_cleanup();
    resp_buf_free(&out);
}

/* ------------------------------------------------------------------ */
/* PSYNC: unknown replid / stale offset falls back to +FULLRESYNC      */
/* ------------------------------------------------------------------ */

static void test_psync_fullresync(void)
{
    server *m;
    pal_socket_t a, b;
    resp_buf out;
    char buf[8192];
    size_t got;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    m = server_create("127.0.0.1", 0);
    DD_CHECK(m != NULL);
    resp_buf_init(&out);

    a = pal_tcp_connect("127.0.0.1", server_port(m));
    DD_CHECK(a != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(a, 1));
    DD_CHECK_EQ_INT(27, pal_send(a, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n", 27));
    got = pump_recv(m, a, buf, 0, 5);
    DD_CHECK(got >= 5);

    /* PSYNC with an unknown replid must fall back to FULLRESYNC */
    b = pal_tcp_connect("127.0.0.1", server_port(m));
    DD_CHECK(b != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(b, 1));
    DD_CHECK_EQ_INT(32,
                    pal_send(b, "*3\r\n$5\r\nPSYNC\r\n$3\r\n???\r\n$2\r\n-1\r\n",
                             32));

    /* expect +FULLRESYNC <40hex> <offset>\r\n then the $<len> frame */
    got = pump_recv(m, b, buf, 0, 64);
    DD_CHECK(got >= 56);
    DD_CHECK_MEM("+FULLRESYNC ", 12, buf, 12);
    {
        int i;
        int hex = 0;
        size_t pos;
        size_t snaplen = 0;
        size_t hdrlen;
        for (i = 0; i < 40; i++) {
            char ch = buf[12 + i];
            if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))
                hex++;
        }
        DD_CHECK_EQ_INT(40, hex);
        pos = 12 + 40;
        DD_CHECK(buf[pos] == ' ');
        pos++;
        /* offset digits then \r\n then $<len>\r\n frame */
        while (pos < got && buf[pos] >= '0' && buf[pos] <= '9')
            pos++;
        DD_CHECK(buf[pos] == '\r' && buf[pos + 1] == '\n');
        pos += 2;
        DD_CHECK(pos < got && buf[pos] == '$');
        hdrlen = pos;
        while (hdrlen < got && buf[hdrlen] != '\n')
            hdrlen++;
        DD_CHECK(hdrlen < got);
        for (size_t i = pos + 1; i < hdrlen && buf[i] >= '0' && buf[i] <= '9';
             i++)
            snaplen = snaplen * 10 + (size_t)(buf[i] - '0');
        hdrlen++;
        got = pump_recv(m, b, buf, got, hdrlen + snaplen);
        DD_CHECK_EQ_INT((long long)(hdrlen + snaplen), (long long)got);
        {
            db d2;
            session *r;
            db_init(&d2);
            DD_CHECK_EQ_INT(0,
                            snapshot_load_mem_multi(&d2, repl_snap_get, 1,
                                                    buf + hdrlen, snaplen,
                                                    1000000));
            r = session_create(&d2);
            sess_cmd(r, &out, 2, "GET", "k");
            EXPECT(out, "$1\r\nv\r\n");
            session_free(r);
            db_destroy(&d2);
        }
    }

    /* subsequent writes stream to the psync'd replica conn */
    DD_CHECK_EQ_INT(29, pal_send(a, "*3\r\n$3\r\nSET\r\n$2\r\nk2\r\n$2\r\nv2\r\n", 29));
    got = pump_recv(m, a, buf, 0, 5);
    DD_CHECK(got >= 5);
    got = pump_recv(m, b, buf, 0, 29);
    DD_CHECK_EQ_INT(29, (long long)got);
    DD_CHECK_MEM("*3\r\n$3\r\nSET\r\n$2\r\nk2\r\n$2\r\nv2\r\n", 29, buf, 29);

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
    if (got != elen || memcmp(expected, buf, elen) != 0) {
        fprintf(stderr, "rt2 mismatch:\n  req: %.*s\n  exp: %.*s\n  got(%zu): %.*s\n",
                (int)rlen, req, (int)elen, expected, got, (int)got, buf);
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

/* Poll GET key on a fresh throwaway connection per attempt (no reply drift
 * on the caller's client). Returns 1 when the expected reply arrives. */
static int wait_sync_get(server *x, server *y, uint16_t port, const char *key,
                         const char *expected, char *buf, size_t bufcap)
{
    size_t elen = strlen(expected);
    int i;
    for (i = 0; i < 500; i++) {
        pal_socket_t t = nb_client(port);
        char req[96];
        size_t got = 0;
        int iter = 0;
        int rl = snprintf(req, sizeof(req),
                          "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n", strlen(key),
                          key);
        DD_CHECK(elen <= bufcap);
        DD_CHECK(rl > 0 && (size_t)rl < sizeof(req));
        if (pal_send(t, req, (size_t)rl) != rl) {
            pal_close(t);
            continue;
        }
        {
            int idle = 0;
            while (got < elen && iter < 10000 && idle < 100) {
                ptrdiff_t n;
                iter++;
                pump2(x, y);
                n = pal_recv(t, buf + got, bufcap - got);
                if (n > 0) {
                    got += (size_t)n;
                    idle = 0;
                } else {
                    idle++;
                }
            }
        }
        pal_close(t);
        if (got == elen && memcmp(buf, expected, elen) == 0)
            return 1;
    }
    return 0;
}

/* Three-server variant for chained replication. */
static int wait_sync_get3(server *x, server *y, server *z, uint16_t port,
                          const char *key, const char *expected, char *buf,
                          size_t bufcap)
{
    size_t elen = strlen(expected);
    int i;
    for (i = 0; i < 500; i++) {
        pal_socket_t t = nb_client(port);
        char req[96];
        size_t got = 0;
        int iter = 0;
        int rl = snprintf(req, sizeof(req),
                          "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n", strlen(key),
                          key);
        DD_CHECK(elen <= bufcap);
        DD_CHECK(rl > 0 && (size_t)rl < sizeof(req));
        if (pal_send(t, req, (size_t)rl) != rl) {
            pal_close(t);
            continue;
        }
        {
            int idle = 0;
            while (got < elen && iter < 10000 && idle < 100) {
                ptrdiff_t n;
                iter++;
                server_run_once(x, 5);
                server_run_once(y, 5);
                server_run_once(z, 5);
                n = pal_recv(t, buf + got, bufcap - got);
                if (n > 0) {
                    got += (size_t)n;
                    idle = 0;
                } else {
                    idle++;
                }
            }
        }
        pal_close(t);
        if (got == elen && memcmp(buf, expected, elen) == 0)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* PSYNC fallback: an offset older than the backlog forces FULLRESYNC  */
/* ------------------------------------------------------------------ */

static void test_psync_stale_offset_fullresync(void)
{
    server *m, *r;
    pal_socket_t mc, rc;
    char buf[8192];
    char port_str[16];
    char req[192];
    uint16_t mport;
    int synced;
    int i;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    m = server_create("127.0.0.1", 0);
    r = server_create("127.0.0.1", 0);
    DD_CHECK(m != NULL && r != NULL);
    /* tiny backlog: a few dozen bytes of commands evict the resume point */
    DD_CHECK_EQ_INT(0, server_set_backlog_size(m, 64));
    DD_CHECK_EQ_INT(-1, server_set_backlog_size(m, 0));
    mport = server_port(m);
    mc = nb_client(mport);
    rc = nb_client(server_port(r));

    snprintf(port_str, sizeof(port_str), "%u", (unsigned)mport);
    {
        const char *p1 = "*3\r\n$9\r\nREPLICAOF\r\n$9\r\n127.0.0.1\r\n$";
        snprintf(req, sizeof(req), "%s%zu\r\n%s\r\n", p1, strlen(port_str),
                 port_str);
        rt2(m, r, rc, req, "+OK\r\n", buf, sizeof(buf));
    }
    rt2(m, r, mc, "*3\r\n$3\r\nSET\r\n$2\r\nk1\r\n$2\r\nv1\r\n", "+OK\r\n",
        buf, sizeof(buf));
    synced = wait_sync_get(m, r, server_port(r), "k1", "$2\r\nv1\r\n", buf,
                           sizeof(buf));
    DD_CHECK_EQ_INT(1, synced);

    /* detach, then overflow the tiny backlog on the master */
    rt2(m, r, rc, "*3\r\n$9\r\nREPLICAOF\r\n$2\r\nNO\r\n$3\r\nONE\r\n",
        "+OK\r\n", buf, sizeof(buf));
    rt2(m, r, rc, "*3\r\n$3\r\nSET\r\n$5\r\nlocal\r\n$4\r\nmine\r\n",
        "+OK\r\n", buf, sizeof(buf));
    for (i = 0; i < 20; i++) {
        char kbuf[8];
        snprintf(kbuf, sizeof(kbuf), "k%d", i);
        snprintf(req, sizeof(req),
                 "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$7\r\nvalue%02d\r\n",
                 strlen(kbuf), kbuf, i);
        rt2(m, r, mc, req, "+OK\r\n", buf, sizeof(buf));
    }

    /* reconnect: the resume offset is gone from the backlog -> FULLRESYNC
     * (the replica-local key is wiped, fresh master keys arrive) */
    {
        const char *p1 = "*3\r\n$9\r\nREPLICAOF\r\n$9\r\n127.0.0.1\r\n$";
        snprintf(req, sizeof(req), "%s%zu\r\n%s\r\n", p1, strlen(port_str),
                 port_str);
        rt2(m, r, rc, req, "+OK\r\n", buf, sizeof(buf));
    }
    synced = wait_sync_get(m, r, server_port(r), "k19", "$7\r\nvalue19\r\n",
                           buf, sizeof(buf));
    DD_CHECK_EQ_INT(1, synced);
    rt2(m, r, rc, "*2\r\n$3\r\nGET\r\n$5\r\nlocal\r\n", "$-1\r\n", buf,
        sizeof(buf));
    /* k1 was overwritten by the overflow loop and arrives via FULLRESYNC */
    rt2(m, r, rc, "*2\r\n$3\r\nGET\r\n$2\r\nk1\r\n", "$7\r\nvalue01\r\n",
        buf, sizeof(buf));

    pal_close(rc);
    pal_close(mc);
    server_destroy(r);
    server_destroy(m);
    pal_socket_cleanup();
}

/* ------------------------------------------------------------------ */
/* chained replication: A -> B -> C propagates through the middle link */
/* ------------------------------------------------------------------ */

static void test_chained_replication(void)
{
    server *a, *b, *c;
    pal_socket_t ac, bc, cc;
    char buf[8192];
    char port_str[16];
    char req[192];
    int synced;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    a = server_create("127.0.0.1", 0);
    b = server_create("127.0.0.1", 0);
    c = server_create("127.0.0.1", 0);
    DD_CHECK(a != NULL && b != NULL && c != NULL);
    ac = nb_client(server_port(a));
    bc = nb_client(server_port(b));
    cc = nb_client(server_port(c));

    /* B replicates A */
    fprintf(stderr, "[chain] created\n");
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)server_port(a));
    {
        const char *p1 = "*3\r\n$9\r\nREPLICAOF\r\n$9\r\n127.0.0.1\r\n$";
        snprintf(req, sizeof(req), "%s%zu\r\n%s\r\n", p1, strlen(port_str),
                 port_str);
        rt2(a, b, bc, req, "+OK\r\n", buf, sizeof(buf));
    }
    fprintf(stderr, "[chain] b->a linked\n");
    /* C replicates B */
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)server_port(b));
    {
        const char *p1 = "*3\r\n$9\r\nREPLICAOF\r\n$9\r\n127.0.0.1\r\n$";
        snprintf(req, sizeof(req), "%s%zu\r\n%s\r\n", p1, strlen(port_str),
                 port_str);
        rt2(b, c, cc, req, "+OK\r\n", buf, sizeof(buf));
    }
    fprintf(stderr, "[chain] c->b linked\n");

    /* write on A; it must reach C through B */
    rt2(a, b, ac, "*3\r\n$3\r\nSET\r\n$5\r\nchain\r\n$5\r\nworks\r\n",
        "+OK\r\n", buf, sizeof(buf));
    fprintf(stderr, "[chain] set done\n");
    synced = wait_sync_get3(a, b, c, server_port(c), "chain",
                            "$5\r\nworks\r\n", buf, sizeof(buf));
    DD_CHECK_EQ_INT(1, synced);

    /* C reports slave role with an up link to B */
    {
        size_t got = 0;
        int iter = 0;
        DD_CHECK_EQ_INT(14, pal_send(cc, "*1\r\n$4\r\nINFO\r\n", 14));
        while (got < sizeof(buf) - 1 && iter < 10000) {
            ptrdiff_t n;
            iter++;
            server_run_once(a, 5);
            server_run_once(b, 5);
            server_run_once(c, 5);
            n = pal_recv(cc, buf + got, sizeof(buf) - 1 - got);
            if (n > 0) {
                got += (size_t)n;
                buf[got] = '\0';
                if (strstr(buf, "master_link_status:up\r\n") != NULL)
                    break;
            }
        }
        buf[got] = '\0';
        DD_CHECK(strstr(buf, "role:slave\r\n") != NULL);
        DD_CHECK(strstr(buf, "master_link_status:up\r\n") != NULL);
    }

    pal_close(cc);
    pal_close(bc);
    pal_close(ac);
    server_destroy(c);
    server_destroy(b);
    server_destroy(a);
    pal_socket_cleanup();
}

/* ------------------------------------------------------------------ */
/* full resync with a snapshot larger than the 64 KiB receive chunk    */
/* ------------------------------------------------------------------ */

static void test_replica_large_snapshot(void)
{
    server *m, *r;
    pal_socket_t mc, rc;
    char buf[8192];
    char port_str[16];
    char req[256];
    int synced;
    int i;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    m = server_create("127.0.0.1", 0);
    r = server_create("127.0.0.1", 0);
    DD_CHECK(m != NULL && r != NULL);
    mc = nb_client(server_port(m));
    rc = nb_client(server_port(r));

    /* ~3000 keys x ~45 bytes > 64 KiB snapshot (pipelined seeding) */
    {
        char *big = (char *)malloc((size_t)3000 * 80);
        size_t blen = 0, bsent = 0, bgot = 0;
        int iter = 0;
        DD_CHECK(big != NULL);
        for (i = 0; i < 3000; i++) {
            char kbuf[8];
            int w;
            snprintf(kbuf, sizeof(kbuf), "k:%d", i);
            w = snprintf(big + blen, 80,
                         "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$32\r\n"
                         "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\r\n",
                         strlen(kbuf), kbuf);
            DD_CHECK(w > 0);
            blen += (size_t)w;
        }
        while ((bsent < blen || bgot < (size_t)3000 * 5) && iter < 200000) {
            ptrdiff_t n;
            iter++;
            pump2(m, r);
            if (bsent < blen) {
                n = pal_send(mc, big + bsent, blen - bsent);
                if (n > 0)
                    bsent += (size_t)n;
            }
            if (bgot < (size_t)3000 * 5) {
                n = pal_recv(mc, buf, sizeof(buf));
                if (n > 0)
                    bgot += (size_t)n;
            }
        }
        DD_CHECK_EQ_INT((long long)blen, (long long)bsent);
        DD_CHECK_EQ_INT((long long)(3000 * 5), (long long)bgot);
        free(big);
    }

    snprintf(port_str, sizeof(port_str), "%u", (unsigned)server_port(m));
    {
        const char *p1 = "*3\r\n$9\r\nREPLICAOF\r\n$9\r\n127.0.0.1\r\n$";
        snprintf(req, sizeof(req), "%s%zu\r\n%s\r\n", p1, strlen(port_str),
                 port_str);
        rt2(m, r, rc, req, "+OK\r\n", buf, sizeof(buf));
    }

    synced = wait_sync_get(m, r, server_port(r), "k:2999",
                           "$32\r\nxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\r\n",
                           buf, sizeof(buf));
    DD_CHECK_EQ_INT(1, synced);
    synced = wait_sync_get(m, r, server_port(r), "k:0",
                           "$32\r\nxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\r\n",
                           buf, sizeof(buf));
    DD_CHECK_EQ_INT(1, synced);

    pal_close(rc);
    pal_close(mc);
    server_destroy(r);
    server_destroy(m);
    pal_socket_cleanup();
}

static void test_invalid_snapshot_preserves_db(void)
{
    db d;
    session *s;
    resp_buf out;
    const char invalid[] = "DDUP0002\x01";

    db_init(&d);
    s = session_create(&d);
    resp_buf_init(&out);
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    sess_cmd(s, &out, 3, "SET", "survivor", "yes");

    /* Invalid and zero-length frames must not flush data.  The wire-side
     * replica path uses this same all-or-nothing loader before LINK_STREAMING. */
    DD_CHECK_EQ_INT(-1, snapshot_load_mem_multi(&d, repl_snap_get, 1,
                                                invalid, sizeof(invalid) - 1,
                                                1000000));
    DD_CHECK_EQ_INT(-1, snapshot_load_mem_multi(&d, repl_snap_get, 1,
                                                "", 0, 1000000));
    sess_cmd(s, &out, 2, "GET", "survivor");
    EXPECT(out, "$3\r\nyes\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_rejected_fullresync_does_not_cache_psync(void)
{
    server *r;
    pal_socket_t listener, client, master_link = PAL_SOCKET_INVALID;
    uint16_t port;
    char req[512];
    char buf[256];
    size_t got;
    int i;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    listener = pal_tcp_listen("127.0.0.1", 0, 4, &port);
    DD_CHECK(listener != PAL_SOCKET_INVALID);
    if (listener == PAL_SOCKET_INVALID)
        return;
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(listener, 1));
    r = server_create("127.0.0.1", 0);
    DD_CHECK(r != NULL);
    client = nb_client(server_port(r));

    /* Preserve a replica-local value across the rejected full resync. */
    rt2(r, r, client, "*3\r\n$3\r\nSET\r\n$9\r\npreserved\r\n$2\r\nok\r\n",
        "+OK\r\n", buf, sizeof(buf));
    {
        char cmd[128];
        char port_text[16];
        snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
        int n = snprintf(cmd, sizeof(cmd),
                         "*3\r\n$9\r\nREPLICAOF\r\n$9\r\n127.0.0.1\r\n$%u\r\n%u\r\n",
                         (unsigned)strlen(port_text), (unsigned)port);
        DD_CHECK(n > 0);
        rt2(r, r, client, cmd, "+OK\r\n", buf, sizeof(buf));
    }

    /* Observe the initial PSYNC, then send metadata with a zero snapshot. */
    for (i = 0; i < 10000 && master_link == PAL_SOCKET_INVALID; i++) {
        server_run_once(r, 1);
        master_link = pal_accept(listener);
    }
    DD_CHECK(master_link != PAL_SOCKET_INVALID);
    if (master_link == PAL_SOCKET_INVALID)
        goto cleanup;
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(master_link, 1));
    got = 0;
    for (i = 0; i < 10000 && got == 0; i++) {
        ptrdiff_t n = pal_recv(master_link, req, sizeof(req));
        if (n > 0)
            got = (size_t)n;
        server_run_once(r, 1);
    }
    DD_CHECK(got > 0 && strstr(req, "$1\r\n?\r\n$2\r\n-1\r\n") != NULL);
    {
        char frame[128];
        const char payload[] = "DDUP0002\x01\x00";
        int hdr = snprintf(frame, sizeof(frame),
                           "+FULLRESYNC abcdef0123456789abcdef0123456789abcdef01 42\r\n$10\r\n");
        DD_CHECK(hdr > 0);
        memcpy(frame + hdr, payload, sizeof(payload) - 1);
        DD_CHECK_EQ_INT((long long)hdr + (long long)(sizeof(payload) - 1),
                        (long long)pal_send(master_link, frame,
                                            (size_t)hdr + sizeof(payload) - 1));
    }
    for (i = 0; i < 10000; i++)
        server_run_once(r, 1);
    {
        const char command[] =
            "*3\r\n$3\r\nSET\r\n$9\r\npreserved\r\n$2\r\nok\r\n";
        DD_CHECK_EQ_INT((long long)strlen(command),
                        (long long)pal_send(master_link, command,
                                            strlen(command)));
        for (i = 0; i < 1000; i++)
            server_run_once(r, 1);
    }
    pal_close(master_link);
    master_link = PAL_SOCKET_INVALID;

    /* A transport reconnect must still use the metadata from the valid sync. */
    master_link = PAL_SOCKET_INVALID;
    got = 0;
    for (i = 0; i < 20000 && master_link == PAL_SOCKET_INVALID; i++) {
        server_run_once(r, 1);
        master_link = pal_accept(listener);
    }
    DD_CHECK(master_link != PAL_SOCKET_INVALID);
    if (master_link != PAL_SOCKET_INVALID) {
        DD_CHECK_EQ_INT(0, pal_set_nonblocking(master_link, 1));
        for (i = 0; i < 10000 && got == 0; i++) {
            ptrdiff_t n = pal_recv(master_link, req, sizeof(req));
            if (n > 0)
                got = (size_t)n;
            server_run_once(r, 1);
        }
        DD_CHECK(got > 0 && strstr(req, "$40\r\nabcdef0123456789abcdef0123456789abcdef01\r\n") != NULL);
        {
            const char invalid[] =
                "+FULLRESYNC 0123456789abcdef0123456789abcdef0123 99\r\n$10\r\nX";
            DD_CHECK_EQ_INT((long long)strlen(invalid),
                            (long long)pal_send(master_link, invalid,
                                                strlen(invalid)));
        }
        pal_close(master_link);
        master_link = PAL_SOCKET_INVALID;
    }

    /* The rejected FULLRESYNC must clear the old cache before reconnect. */
    got = 0;
    for (i = 0; i < 20000 && master_link == PAL_SOCKET_INVALID; i++) {
        server_run_once(r, 1);
        master_link = pal_accept(listener);
    }
    DD_CHECK(master_link != PAL_SOCKET_INVALID);
    if (master_link != PAL_SOCKET_INVALID) {
        DD_CHECK_EQ_INT(0, pal_set_nonblocking(master_link, 1));
        for (i = 0; i < 10000 && got == 0; i++) {
            ptrdiff_t n = pal_recv(master_link, req, sizeof(req));
            if (n > 0)
                got = (size_t)n;
            server_run_once(r, 1);
        }
        DD_CHECK(got > 0 && strstr(req, "$1\r\n?\r\n$2\r\n-1\r\n") != NULL);
    }
    rt2(r, r, client, "*2\r\n$3\r\nGET\r\n$9\r\npreserved\r\n",
        "$2\r\nok\r\n", buf, sizeof(buf));

cleanup:
    if (master_link != PAL_SOCKET_INVALID)
        pal_close(master_link);
    pal_close(client);
    pal_close(listener);
    server_destroy(r);
    pal_socket_cleanup();
}

static void full_cycle_at(int backend)
{
    server *m, *r;
    pal_socket_t mc, rc;
    char buf[8192];
    char port_str[16];
    int i;
    int synced;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    m = server_create_ex("127.0.0.1", 0, backend);
    r = server_create_ex("127.0.0.1", 0, backend);
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

static void test_replica_full_cycle(void)
{
    full_cycle_at(SERVER_BACKEND_SELECT);
}

static void test_replica_full_cycle_iocp(void)
{
    pal_iocp *probe = pal_iocp_create();
    if (probe == NULL)
        return; /* non-Windows platform: no IOCP backend */
    pal_iocp_free(probe);
    full_cycle_at(SERVER_BACKEND_IOCP);
}

/* ------------------------------------------------------------------ */
/* PSYNC +CONTINUE: reconnect replays only the backlog tail            */
/* ------------------------------------------------------------------ */

static void test_psync_continue_partial(void)
{
    server *m, *r;
    pal_socket_t mc, rc;
    char buf[8192];
    char port_str[16];
    uint16_t mport;
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
    rt2(m, r, mc, "*3\r\n$3\r\nSET\r\n$2\r\nk1\r\n$2\r\nv1\r\n", "+OK\r\n",
        buf, sizeof(buf));
    synced = wait_sync_get(m, r, server_port(r), "k1", "$2\r\nv1\r\n", buf,
                           sizeof(buf));
    DD_CHECK_EQ_INT(1, synced);

    /* detach, write a replica-local key, write more on the master */
    rt2(m, r, rc, "*3\r\n$9\r\nREPLICAOF\r\n$2\r\nNO\r\n$3\r\nONE\r\n",
        "+OK\r\n", buf, sizeof(buf));
    rt2(m, r, rc, "*3\r\n$3\r\nSET\r\n$5\r\nlocal\r\n$4\r\nmine\r\n",
        "+OK\r\n", buf, sizeof(buf));
    rt2(m, r, mc, "*3\r\n$3\r\nSET\r\n$2\r\nk2\r\n$2\r\nv2\r\n", "+OK\r\n",
        buf, sizeof(buf));
    DD_CHECK_EQ_INT(-1, server_set_backlog_size(m, 0));

    /* reconnect: PSYNC continues from the backlog (no full wipe) */
    {
        char req[128];
        const char *p1 = "*3\r\n$9\r\nREPLICAOF\r\n$9\r\n127.0.0.1\r\n$";
        snprintf(req, sizeof(req), "%s%zu\r\n%s\r\n", p1, strlen(port_str),
                 port_str);
        rt2(m, r, rc, req, "+OK\r\n", buf, sizeof(buf));
    }
    synced = wait_sync_get(m, r, server_port(r), "k2", "$2\r\nv2\r\n", buf,
                           sizeof(buf));
    DD_CHECK_EQ_INT(1, synced);

    /* partial resync: the replica-local key survived (full resync would
     * have wiped it); the earlier replicated key is intact */
    rt2(m, r, rc, "*2\r\n$3\r\nGET\r\n$5\r\nlocal\r\n", "$4\r\nmine\r\n",
        buf, sizeof(buf));
    rt2(m, r, rc, "*2\r\n$3\r\nGET\r\n$2\r\nk1\r\n", "$2\r\nv1\r\n", buf,
        sizeof(buf));

    pal_close(rc);
    pal_close(mc);
    server_destroy(r);
    server_destroy(m);
    pal_socket_cleanup();
}

static void reconnect_resync_at(int backend)
{
    server *m, *r;
    pal_socket_t mc, rc;
    char buf[8192];
    char port_str[16];
    uint16_t mport;
    int i;
    int synced;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    m = server_create_ex("127.0.0.1", 0, backend);
    r = server_create_ex("127.0.0.1", 0, backend);
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

static void test_replica_reconnect_resync(void)
{
    reconnect_resync_at(SERVER_BACKEND_SELECT);
}

static void test_replica_reconnect_resync_iocp(void)
{
    pal_iocp *probe = pal_iocp_create();
    if (probe == NULL)
        return; /* non-Windows platform: no IOCP backend */
    pal_iocp_free(probe);
    reconnect_resync_at(SERVER_BACKEND_IOCP);
}
