/* test_tls.c - TLS tests (registered in CTest only when DDUP_HAS_TLS=1). */
#include <string.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "pal/pal_tls.h"
#include "pal/pal_time.h"
#include "server/mt_server.h"
#include "server/server.h"
#include "test.h"

#ifndef DDUP_TEST_CERT_DIR
#define DDUP_TEST_CERT_DIR "tests/certs"
#endif

static void test_ctx_load(void)
{
    pal_tls_ctx *ctx = pal_tls_ctx_new(DDUP_TEST_CERT_DIR "/cert.pem",
                                       DDUP_TEST_CERT_DIR "/key.pem");
    DD_CHECK(ctx != NULL);
    pal_tls_ctx_free(ctx);
}

static void test_ctx_bad_files(void)
{
    DD_CHECK(pal_tls_ctx_new("no-such-cert.pem", "no-such-key.pem") == NULL);
}

/* ------------------------------------------------------------------ */
/* client-side helpers (OpenSSL, non-blocking fd + run_once pumping)  */
/* ------------------------------------------------------------------ */

typedef struct tls_client {
    pal_socket_t fd;
    SSL *ssl;
} tls_client;

static void tls_client_open(server *s, SSL_CTX *cctx, tls_client *c)
{
    int rc;
    int iter = 0;
    uint64_t deadline = pal_now_ms() + 15000;
    c->fd = pal_tcp_connect("127.0.0.1", server_tls_port(s));
    DD_CHECK(c->fd != PAL_SOCKET_INVALID);
    if (c->fd == PAL_SOCKET_INVALID) {
        c->ssl = NULL;
        return;
    }
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c->fd, 1));
    c->ssl = SSL_new(cctx);
    DD_CHECK(c->ssl != NULL);
    SSL_set_fd(c->ssl, (int)c->fd);
    for (;;) {
        rc = SSL_connect(c->ssl);
        if (rc == 1)
            break;
        rc = SSL_get_error(c->ssl, rc);
        DD_CHECK(rc == SSL_ERROR_WANT_READ || rc == SSL_ERROR_WANT_WRITE);
        server_run_once(s, 10);
        if (++iter > 2000 || pal_now_ms() > deadline)
            break; /* fail via the check below */
    }
    DD_CHECK(iter <= 2000 && pal_now_ms() <= deadline);
}

static void tls_client_close(tls_client *c)
{
    (void)SSL_shutdown(c->ssl);
    SSL_free(c->ssl);
    pal_close(c->fd);
}

/* request/response over TLS, interleaving server_run_once */
static void tls_rt(server *s, tls_client *c, const char *req,
                   const char *expected)
{
    size_t elen = strlen(expected);
    char buf[4096];
    size_t got = 0;
    int iter = 0;
    int rc;
    uint64_t deadline = pal_now_ms() + 15000;
    DD_CHECK(elen <= sizeof(buf));
    if (c->ssl == NULL)
        return;
    for (size_t off = 0; off < strlen(req);) {
        rc = SSL_write(c->ssl, req + off, (int)(strlen(req) - off));
        if (rc > 0) {
            off += (size_t)rc;
            continue;
        }
        rc = SSL_get_error(c->ssl, rc);
        DD_CHECK(rc == SSL_ERROR_WANT_READ || rc == SSL_ERROR_WANT_WRITE);
        server_run_once(s, 10);
        if (++iter > 2000 || pal_now_ms() > deadline)
            break;
    }
    DD_CHECK(iter <= 2000 && pal_now_ms() <= deadline);
    iter = 0;
    deadline = pal_now_ms() + 15000;
    while (got < elen && iter < 2000 && pal_now_ms() <= deadline) {
        iter++;
        server_run_once(s, 10);
        rc = SSL_read(c->ssl, buf + got, (int)(sizeof(buf) - got));
        if (rc > 0)
            got += (size_t)rc;
        else {
            rc = SSL_get_error(c->ssl, rc);
            DD_CHECK(rc == SSL_ERROR_WANT_READ || rc == SSL_ERROR_WANT_WRITE);
        }
    }
    DD_CHECK_EQ_INT((long long)elen, (long long)got);
    DD_CHECK_MEM(expected, elen, buf, got);
}

static void test_tls_server_roundtrip(void)
{
    server *s;
    SSL_CTX *cctx;
    tls_client c;
    pal_socket_t plain;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    SSL_library_init();

    s = server_create("127.0.0.1", 0);
    DD_CHECK(s != NULL);
    if (server_enable_tls(s, "127.0.0.1", 0, DDUP_TEST_CERT_DIR "/cert.pem",
                          DDUP_TEST_CERT_DIR "/key.pem") != 0) {
        DD_CHECK(0); /* enable failed: report, don't grind the loops */
        server_destroy(s);
        pal_socket_cleanup();
        return;
    }
    DD_CHECK(server_tls_port(s) != 0);

    cctx = SSL_CTX_new(TLS_client_method());
    DD_CHECK(cctx != NULL);
    SSL_CTX_set_verify(cctx, SSL_VERIFY_NONE, NULL);

    /* handshake + PING */
    tls_client_open(s, cctx, &c);
    tls_rt(s, &c, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");

    /* SET/GET over TLS */
    tls_rt(s, &c, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n", "+OK\r\n");
    tls_rt(s, &c, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", "$1\r\nv\r\n");

    /* pipelined: 3 commands in one write, 3 replies */
    tls_rt(s, &c,
           "*1\r\n$4\r\nPING\r\n*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\nb\r\n"
           "*2\r\n$3\r\nGET\r\n$1\r\na\r\n",
           "+PONG\r\n+OK\r\n$1\r\nb\r\n");

    /* plain port serves in parallel */
    plain = pal_tcp_connect("127.0.0.1", server_port(s));
    DD_CHECK(plain != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(plain, 1));
    DD_CHECK_EQ_INT(14, pal_send(plain, "*1\r\n$4\r\nPING\r\n", 14));
    {
        char pb[8];
        ptrdiff_t n = -1;
        int iter = 0;
        uint64_t deadline = pal_now_ms() + 15000;
        while (n < 0 && iter < 2000 && pal_now_ms() <= deadline) {
            iter++;
            server_run_once(s, 10);
            n = pal_recv(plain, pb, sizeof(pb));
        }
        DD_CHECK_EQ_INT(7, n);
        DD_CHECK_MEM("+PONG\r\n", 7, pb, 7);
    }
    pal_close(plain);

    /* clean close + reconnect */
    tls_client_close(&c);
    server_run_once(s, 10);
    tls_client_open(s, cctx, &c);
    tls_rt(s, &c, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", "$1\r\nv\r\n");
    tls_client_close(&c);

    SSL_CTX_free(cctx);
    server_destroy(s);
    pal_socket_cleanup();
}

/* ------------------------------------------------------------------ */
/* mt (thread-per-core) TLS: acceptor owns the TLS listener, workers own  */
/* per-worker contexts and drive the handshake in their own event loop.   */
/* The server runs on background threads, so the client just pumps SSL    */
/* against the live server.                                               */
/* ------------------------------------------------------------------ */

static void mt_ssl_rt(SSL *ssl, const char *req, const char *expected)
{
    size_t elen = strlen(expected);
    char buf[4096];
    size_t got = 0, off = 0;
    int iter = 0;
    int rc;
    uint64_t deadline = pal_now_ms() + 15000;
    DD_CHECK(elen <= sizeof(buf));
    while (off < strlen(req)) {
        rc = SSL_write(ssl, req + off, (int)(strlen(req) - off));
        if (rc > 0) {
            off += (size_t)rc;
            continue;
        }
        rc = SSL_get_error(ssl, rc);
        DD_CHECK(rc == SSL_ERROR_WANT_READ || rc == SSL_ERROR_WANT_WRITE);
        pal_sleep_ms(1);
        if (++iter > 5000 || pal_now_ms() > deadline)
            break;
    }
    DD_CHECK(iter <= 5000 && pal_now_ms() <= deadline);
    iter = 0;
    deadline = pal_now_ms() + 15000;
    while (got < elen && iter < 5000 && pal_now_ms() <= deadline) {
        iter++;
        rc = SSL_read(ssl, buf + got, (int)(sizeof(buf) - got));
        if (rc > 0)
            got += (size_t)rc;
        else {
            rc = SSL_get_error(ssl, rc);
            DD_CHECK(rc == SSL_ERROR_WANT_READ || rc == SSL_ERROR_WANT_WRITE);
            pal_sleep_ms(1);
        }
    }
    DD_CHECK_EQ_INT((long long)elen, (long long)got);
    DD_CHECK_MEM(expected, elen, buf, got);
}

static void test_mt_tls_roundtrip(void)
{
    mt_server *ms;
    SSL_CTX *cctx;
    pal_socket_t fd;
    SSL *ssl;
    int rc;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    SSL_library_init();

    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    if (mt_server_enable_tls(ms, "127.0.0.1", 0,
                             DDUP_TEST_CERT_DIR "/cert.pem",
                             DDUP_TEST_CERT_DIR "/key.pem") != 0) {
        DD_CHECK(0); /* enable failed: report, don't grind the loops */
        mt_server_destroy(ms);
        pal_socket_cleanup();
        return;
    }
    DD_CHECK(mt_server_tls_port(ms) != 0);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));

    cctx = SSL_CTX_new(TLS_client_method());
    DD_CHECK(cctx != NULL);
    SSL_CTX_set_verify(cctx, SSL_VERIFY_NONE, NULL);

    fd = pal_tcp_connect("127.0.0.1", mt_server_tls_port(ms));
    DD_CHECK(fd != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(fd, 1));
    ssl = SSL_new(cctx);
    DD_CHECK(ssl != NULL);
    SSL_set_fd(ssl, (int)fd);
    {
        int iter = 0;
        uint64_t deadline = pal_now_ms() + 15000;
        for (;;) {
            rc = SSL_connect(ssl);
            if (rc == 1)
                break;
            rc = SSL_get_error(ssl, rc);
            DD_CHECK(rc == SSL_ERROR_WANT_READ ||
                     rc == SSL_ERROR_WANT_WRITE);
            pal_sleep_ms(1);
            if (++iter > 5000 || pal_now_ms() > deadline)
                break;
        }
        DD_CHECK(iter <= 5000 && pal_now_ms() <= deadline);
    }

    /* PING + SET/GET over the TLS tunnel */
    mt_ssl_rt(ssl, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");
    mt_ssl_rt(ssl, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n", "+OK\r\n");
    mt_ssl_rt(ssl, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", "$1\r\nv\r\n");

    /* plain port still serves in parallel */
    {
        pal_socket_t plain = pal_tcp_connect("127.0.0.1",
                                             mt_server_port(ms));
        char pb[8];
        ptrdiff_t n = -1;
        int iter = 0;
        uint64_t deadline = pal_now_ms() + 15000;
        DD_CHECK(plain != PAL_SOCKET_INVALID);
        DD_CHECK_EQ_INT(0, pal_set_nonblocking(plain, 1));
        DD_CHECK_EQ_INT(14, pal_send(plain, "*1\r\n$4\r\nPING\r\n", 14));
        while (n < 0 && iter < 5000 && pal_now_ms() <= deadline) {
            iter++;
            pal_sleep_ms(1);
            n = pal_recv(plain, pb, sizeof(pb));
        }
        DD_CHECK_EQ_INT(7, n);
        DD_CHECK_MEM("+PONG\r\n", 7, pb, 7);
        pal_close(plain);
    }

    (void)SSL_shutdown(ssl);
    SSL_free(ssl);
    pal_close(fd);
    SSL_CTX_free(cctx);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0); /* progress visible under timeout */
    DD_RUN(test_ctx_load);
    DD_RUN(test_ctx_bad_files);
    DD_RUN(test_tls_server_roundtrip);
    DD_RUN(test_mt_tls_roundtrip);
    return DD_TEST_SUMMARY();
}

