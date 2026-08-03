/* test_server.c - end-to-end tests: real TCP loopback against server.c.
 *
 * Single-threaded: the test acts as the client and interleaves
 * server_run_once() calls with client send/recv.
 */
#include <stdlib.h>
#include <string.h>

#include "pal/pal_socket.h"
#include "server/server.h"
#include "test.h"

/* Send req, pump the server, and read exactly strlen(expected) reply bytes;
 * assert the reply matches expected. Client socket must be non-blocking. */
static void roundtrip(server *s, pal_socket_t c, const char *req,
                      const char *expected)
{
    size_t elen = strlen(expected);
    size_t rlen = strlen(req);
    size_t sent = 0, got = 0;
    char buf[1024];
    int iter = 0;

    DD_CHECK(elen <= sizeof(buf));
    while (sent < rlen) {
        ptrdiff_t n = pal_send(c, req + sent, rlen - sent);
        if (n > 0)
            sent += (size_t)n;
    }
    DD_CHECK_EQ_INT((long long)rlen, (long long)sent);

    while (got < elen && iter < 10000) {
        ptrdiff_t n;
        iter++;
        server_run_once(s, 50);
        n = pal_recv(c, buf + got, sizeof(buf) - got);
        if (n > 0)
            got += (size_t)n;
    }
    DD_CHECK_EQ_INT((long long)elen, (long long)got);
    DD_CHECK_MEM(expected, elen, buf, got);
}

static pal_socket_t connect_client(server *s)
{
    pal_socket_t c = pal_tcp_connect("127.0.0.1", server_port(s));
    DD_CHECK(c != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));
    return c;
}

static void test_ping_set_get(void)
{
    server *s = server_create("127.0.0.1", 0);
    pal_socket_t c;
    DD_CHECK(s != NULL);
    DD_CHECK(server_port(s) != 0);
    c = connect_client(s);

    roundtrip(s, c, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");
    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", "+OK\r\n");
    roundtrip(s, c, "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n", "$3\r\nbar\r\n");
    /* GET of a missing key -> null bulk */
    roundtrip(s, c, "*2\r\n$3\r\nGET\r\n$5\r\nnokey\r\n", "$-1\r\n");

    pal_close(c);
    server_destroy(s);
}

static void test_pipeline(void)
{
    server *s = server_create("127.0.0.1", 0);
    pal_socket_t c;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    /* three commands in one TCP send -> three replies */
    roundtrip(s, c,
              "*1\r\n$4\r\nPING\r\n"
              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
              "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n",
              "+PONG\r\n+OK\r\n$1\r\nv\r\n");

    pal_close(c);
    server_destroy(s);
}

static void test_mget_missing_key(void)
{
    server *s = server_create("127.0.0.1", 0);
    pal_socket_t c;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n", "+OK\r\n");
    roundtrip(s, c, "*3\r\n$4\r\nMGET\r\n$1\r\na\r\n$7\r\nmissing\r\n",
              "*2\r\n$1\r\n1\r\n$-1\r\n");

    pal_close(c);
    server_destroy(s);
}

static void test_unknown_command(void)
{
    server *s = server_create("127.0.0.1", 0);
    pal_socket_t c;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    roundtrip(s, c, "*1\r\n$6\r\nFOOCMD\r\n",
              "-ERR unknown command 'FOOCMD'\r\n");

    pal_close(c);
    server_destroy(s);
}

static void test_split_delivery(void)
{
    server *s = server_create("127.0.0.1", 0);
    pal_socket_t c;
    char buf[64];
    ptrdiff_t n = -1;
    int iter = 0;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", "+OK\r\n");

    /* first half of a GET: server must not reply yet */
    DD_CHECK_EQ_INT(10, pal_send(c, "*2\r\n$3\r\nGE", 10));
    server_run_once(s, 50);
    n = pal_recv(c, buf, sizeof(buf));
    DD_CHECK_EQ_INT(-1, n);
    DD_CHECK(pal_would_block(pal_socket_error()));

    /* second half completes the command */
    DD_CHECK_EQ_INT(12, pal_send(c, "T\r\n$3\r\nfoo\r\n", 12));
    n = -1;
    while (n <= 0 && iter < 10000) {
        iter++;
        server_run_once(s, 50);
        n = pal_recv(c, buf, sizeof(buf));
    }
    DD_CHECK_EQ_INT(9, n);
    DD_CHECK_MEM("$3\r\nbar\r\n", 9, buf, 9);

    pal_close(c);
    server_destroy(s);
}

static void test_many_connections(void)
{
    server *s = server_create("127.0.0.1", 0);
    int i;
    DD_CHECK(s != NULL);

    for (i = 0; i < 32; i++) {
        pal_socket_t c = connect_client(s);
        roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$1\r\nx\r\n$1\r\ny\r\n", "+OK\r\n");
        roundtrip(s, c, "*2\r\n$3\r\nGET\r\n$1\r\nx\r\n", "$1\r\ny\r\n");
        pal_close(c);
        /* let the server observe the close */
        server_run_once(s, 5);
    }

    server_destroy(s);
}

static void test_protocol_error_closes_conn(void)
{
    server *s = server_create("127.0.0.1", 0);
    pal_socket_t c;
    char buf[64];
    ptrdiff_t n = -1;
    int iter = 0;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    /* inline (non-array) command -> protocol error, then server closes */
    DD_CHECK_EQ_INT(6, pal_send(c, "PING\r\n", 6));
    while (n <= 0 && iter < 10000) {
        iter++;
        server_run_once(s, 50);
        n = pal_recv(c, buf, sizeof(buf));
    }
    DD_CHECK_EQ_INT(21, n);
    DD_CHECK_MEM("-ERR Protocol error\r\n", 21, buf, 21);

    /* connection closed by the server: next recv reports orderly close */
    n = -1;
    iter = 0;
    while (n < 0 && iter < 10000) {
        iter++;
        server_run_once(s, 10);
        n = pal_recv(c, buf, sizeof(buf));
    }
    DD_CHECK_EQ_INT(0, n);

    pal_close(c);
    server_destroy(s);
}

static void test_pubsub_over_socket(void)
{
    server *s = server_create("127.0.0.1", 0);
    pal_socket_t a, b;
    char buf[256];
    const char *want = "*3\r\n$7\r\nmessage\r\n$2\r\nch\r\n$5\r\nhello\r\n";
    size_t wlen = strlen(want), got = 0;
    ptrdiff_t n;
    int iter = 0;
    DD_CHECK(s != NULL);
    a = connect_client(s);
    b = connect_client(s);

    /* A subscribes; B publishes; A gets the push without sending */
    roundtrip(s, a, "*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n",
              "*3\r\n$9\r\nsubscribe\r\n$2\r\nch\r\n:1\r\n");
    roundtrip(s, b, "*3\r\n$7\r\nPUBLISH\r\n$2\r\nch\r\n$5\r\nhello\r\n",
              ":1\r\n");
    while (got < wlen && iter < 10000) {
        iter++;
        server_run_once(s, 50);
        n = pal_recv(a, buf + got, sizeof(buf) - got);
        if (n > 0)
            got += (size_t)n;
    }
    DD_CHECK_EQ_INT((long long)wlen, (long long)got);
    DD_CHECK_MEM(want, wlen, buf, got);

    /* closing A unsubscribes it: nobody receives anymore */
    pal_close(a);
    server_run_once(s, 50);
    roundtrip(s, b, "*3\r\n$7\r\nPUBLISH\r\n$2\r\nch\r\n$5\r\nhello\r\n",
              ":0\r\n");

    pal_close(b);
    server_destroy(s);
}

static void test_shutdown_command(void)
{
    server *s = server_create("127.0.0.1", 0);
    pal_socket_t c;
    int iter = 0;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    DD_CHECK_EQ_INT(0, server_shutdown_requested(s));
    DD_CHECK_EQ_INT(18, pal_send(c, "*1\r\n$8\r\nSHUTDOWN\r\n", 18));
    while (!server_shutdown_requested(s) && iter < 10000) {
        iter++;
        server_run_once(s, 50);
    }
    DD_CHECK_EQ_INT(1, server_shutdown_requested(s));

    pal_close(c);
    server_destroy(s);
}

/* send everything, pumping the server between partial sends */
static void send_all(server *s, pal_socket_t c, const char *buf, size_t len)
{
    size_t sent = 0;
    int iter = 0;
    while (sent < len && iter < 100000) {
        ptrdiff_t n = pal_send(c, buf + sent, len - sent);
        iter++;
        if (n > 0)
            sent += (size_t)n;
        else
            server_run_once(s, 5);
    }
    DD_CHECK_EQ_INT((long long)len, (long long)sent);
}

/* read exactly len bytes, pumping the server between reads */
static size_t read_all(server *s, pal_socket_t c, char *buf, size_t len,
                       size_t chunk)
{
    size_t got = 0;
    int iter = 0;
    while (got < len && iter < 200000) {
        size_t want = len - got < chunk ? len - got : chunk;
        ptrdiff_t n;
        iter++;
        server_run_once(s, 5);
        n = pal_recv(c, buf + got, want);
        if (n > 0)
            got += (size_t)n;
        if (n == 0)
            break;
    }
    return got;
}

static void test_pipeline_2000(void)
{
    enum { NCMD = 2000, CMDLEN = 14, REPLYLEN = 7 };
    server *s = server_create("127.0.0.1", 0);
    pal_socket_t c;
    char *req, *rep;
    int i, ok = 1;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    req = (char *)malloc((size_t)NCMD * CMDLEN);
    rep = (char *)malloc((size_t)NCMD * REPLYLEN);
    DD_CHECK(req != NULL && rep != NULL);
    for (i = 0; i < NCMD; i++)
        memcpy(req + (size_t)i * CMDLEN, "*1\r\n$4\r\nPING\r\n", CMDLEN);

    send_all(s, c, req, (size_t)NCMD * CMDLEN);
    DD_CHECK_EQ_INT((long long)((size_t)NCMD * REPLYLEN),
                    (long long)read_all(s, c, rep,
                                        (size_t)NCMD * REPLYLEN, 65536));
    for (i = 0; i < NCMD; i++)
        if (memcmp(rep + (size_t)i * REPLYLEN, "+PONG\r\n", REPLYLEN) != 0)
            ok = 0;
    DD_CHECK_EQ_INT(1, ok);

    free(rep);
    free(req);
    pal_close(c);
    server_destroy(s);
}

/* 256 KiB value: big enough to exercise the streaming reply path. */
#define BIGVAL_LEN (256 * 1024)

static char *make_bigval_cmd(const char *key, size_t *outlen)
{
    char hdr[64];
    int hl = snprintf(hdr, sizeof(hdr), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n"
                                       "$%d\r\n",
                      strlen(key), key, BIGVAL_LEN);
    size_t total = (size_t)hl + BIGVAL_LEN + 2;
    char *cmd = (char *)malloc(total);
    DD_CHECK(cmd != NULL);
    memcpy(cmd, hdr, (size_t)hl);
    memset(cmd + hl, 'x', BIGVAL_LEN);
    cmd[total - 2] = '\r';
    cmd[total - 1] = '\n';
    *outlen = total;
    return cmd;
}

static void test_slow_client_no_stall(void)
{
    server *s = server_create("127.0.0.1", 0);
    pal_socket_t a, b, c;
    char *cmd, *big;
    size_t cmdlen, got;
    char pb[16];
    int iter = 0;
    char exp_hdr[16];
    int ehl;
    size_t exp_total;

    DD_CHECK(s != NULL);
    a = connect_client(s);
    b = connect_client(s);
    c = connect_client(s);

    /* A writes a 4 MiB value */
    cmd = make_bigval_cmd("big", &cmdlen);
    send_all(s, a, cmd, cmdlen);
    free(cmd);
    DD_CHECK_EQ_INT(5, (long long)read_all(s, a, pb, 5, 16));
    DD_CHECK_MEM("+OK\r\n", 5, pb, 5);

    /* B asks for it and does NOT read (slow/stalled client) */
    DD_CHECK_EQ_INT(22, pal_send(b, "*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n", 22));

    /* C must still be served promptly (bounded poll) */
    DD_CHECK_EQ_INT(14, pal_send(c, "*1\r\n$4\r\nPING\r\n", 14));
    got = 0;
    while (got < 7 && iter < 1000) {
        ptrdiff_t n;
        iter++;
        server_run_once(s, 5);
        n = pal_recv(c, pb + got, sizeof(pb) - got);
        if (n > 0)
            got += (size_t)n;
    }
    DD_CHECK_EQ_INT(7, (long long)got);
    DD_CHECK_MEM("+PONG\r\n", 7, pb, 7);
    DD_CHECK(iter < 200); /* prompt: no head-of-line blocking */

    /* drain B: the whole reply arrives intact */
    ehl = snprintf(exp_hdr, sizeof(exp_hdr), "$%d\r\n", BIGVAL_LEN);
    exp_total = (size_t)ehl + BIGVAL_LEN + 2;
    big = (char *)malloc(exp_total);
    DD_CHECK(big != NULL);
    got = read_all(s, b, big, exp_total, 65536);
    DD_CHECK_EQ_INT((long long)exp_total, (long long)got);
    DD_CHECK_MEM(exp_hdr, (size_t)ehl, big, (size_t)ehl);
    DD_CHECK(big[ehl] == 'x' && big[exp_total - 3] == 'x');
    DD_CHECK(big[exp_total - 2] == '\r' && big[exp_total - 1] == '\n');

    free(big);
    pal_close(c);
    pal_close(b);
    pal_close(a);
    server_destroy(s);
}

static void test_partial_reads(void)
{
    server *s = server_create("127.0.0.1", 0);
    pal_socket_t a, b;
    char *cmd, *big;
    size_t cmdlen, got;
    char pb[16];
    char exp_hdr[16];
    int ehl;
    size_t exp_total;

    DD_CHECK(s != NULL);
    a = connect_client(s);
    b = connect_client(s);

    cmd = make_bigval_cmd("big", &cmdlen);
    send_all(s, a, cmd, cmdlen);
    free(cmd);
    DD_CHECK_EQ_INT(5, (long long)read_all(s, a, pb, 5, 16));

    DD_CHECK_EQ_INT(22, pal_send(b, "*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n", 22));

    /* read the 4 MiB reply in small chunks, interleaved with the loop */
    ehl = snprintf(exp_hdr, sizeof(exp_hdr), "$%d\r\n", BIGVAL_LEN);
    exp_total = (size_t)ehl + BIGVAL_LEN + 2;
    big = (char *)malloc(exp_total);
    DD_CHECK(big != NULL);
    got = read_all(s, b, big, exp_total, 4096);
    DD_CHECK_EQ_INT((long long)exp_total, (long long)got);
    DD_CHECK_MEM(exp_hdr, (size_t)ehl, big, (size_t)ehl);
    DD_CHECK(big[exp_total - 3] == 'x');

    free(big);
    pal_close(b);
    pal_close(a);
    server_destroy(s);
}

int main(void)
{
    DD_CHECK_EQ_INT(0, pal_socket_init());
    DD_RUN(test_ping_set_get);
    DD_RUN(test_pipeline);
    DD_RUN(test_mget_missing_key);
    DD_RUN(test_unknown_command);
    DD_RUN(test_split_delivery);
    DD_RUN(test_many_connections);
    DD_RUN(test_protocol_error_closes_conn);
    DD_RUN(test_pubsub_over_socket);
    DD_RUN(test_shutdown_command);
    DD_RUN(test_pipeline_2000);
    DD_RUN(test_slow_client_no_stall);
    DD_RUN(test_partial_reads);
    pal_socket_cleanup();
    return DD_TEST_SUMMARY();
}
