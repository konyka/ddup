/* test_tls.c - TLS tests (registered in CTest only when DDUP_HAS_TLS=1). */
#include <limits.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "core/cluster.h"
#include "pal/pal_tls.h"
#include "pal/pal_time.h"
#include "pal/pal_thread.h"
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

static void test_tls_new_rejects_invalid_fd(void)
{
    pal_tls_ctx *ctx = pal_tls_ctx_new(DDUP_TEST_CERT_DIR "/cert.pem",
                                       DDUP_TEST_CERT_DIR "/key.pem");
    DD_CHECK(ctx != NULL);
    if (ctx != NULL) {
        DD_CHECK(pal_tls_new(ctx, PAL_SOCKET_INVALID) == NULL);
        pal_tls_ctx_free(ctx);
    }
}

static void test_tls_rejects_lengths_over_int_max(void)
{
    pal_tls_ctx *ctx;
    pal_tls *tls;
    pal_socket_t fd;
    char byte = 0;
    size_t too_big = (size_t)INT_MAX + 1;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ctx = pal_tls_ctx_new(DDUP_TEST_CERT_DIR "/cert.pem",
                          DDUP_TEST_CERT_DIR "/key.pem");
    DD_CHECK(ctx != NULL);
    fd = pal_tcp_listen("127.0.0.1", 0, 1, NULL);
    DD_CHECK(fd != PAL_SOCKET_INVALID);
    tls = ctx != NULL && fd != PAL_SOCKET_INVALID ? pal_tls_new(ctx, fd) : NULL;
    DD_CHECK(tls != NULL);
    if (tls != NULL) {
        DD_CHECK_EQ_INT(-1, pal_tls_read(tls, &byte, too_big));
        DD_CHECK_EQ_INT(-1, pal_tls_write(tls, &byte, too_big));
        pal_tls_free(tls);
    }
    if (fd != PAL_SOCKET_INVALID)
        pal_close(fd);
    pal_tls_ctx_free(ctx);
    pal_socket_cleanup();
}

typedef struct tls_init_worker_arg {
    int ok;
} tls_init_worker_arg;

static void *tls_init_worker(void *arg)
{
    tls_init_worker_arg *a = (tls_init_worker_arg *)arg;
    pal_tls_ctx *ctx = pal_tls_ctx_new(DDUP_TEST_CERT_DIR "/cert.pem",
                                       DDUP_TEST_CERT_DIR "/key.pem");
    a->ok = ctx != NULL;
    pal_tls_ctx_free(ctx);
    return NULL;
}

static void test_tls_init_is_thread_safe(void)
{
    pal_thread threads[8];
    tls_init_worker_arg args[8];
    int i;

    memset(threads, 0, sizeof(threads));
    memset(args, 0, sizeof(args));
    for (i = 0; i < 8; i++)
        DD_CHECK_EQ_INT(0, pal_thread_create(&threads[i], tls_init_worker,
                                             &args[i]));
    for (i = 0; i < 8; i++)
        DD_CHECK_EQ_INT(0, pal_thread_join(&threads[i], NULL));
    for (i = 0; i < 8; i++)
        DD_CHECK(args[i].ok);
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
    uint64_t deadline = pal_now_ms() + 60000;
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
    uint64_t deadline = pal_now_ms() + 60000;
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
    deadline = pal_now_ms() + 60000;
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
        uint64_t deadline = pal_now_ms() + 60000;
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

static void test_tls_replication_master_link(void)
{
    server *master;
    server *replica;
    pal_socket_t client = PAL_SOCKET_INVALID;
    uint16_t tls_port;
    char buf[128];
    ptrdiff_t n;
    int i;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    master = server_create("127.0.0.1", 0);
    replica = server_create("127.0.0.1", 0);
    DD_CHECK(master != NULL && replica != NULL);
    if (master == NULL || replica == NULL)
        goto cleanup;
    DD_CHECK_EQ_INT(0, server_enable_tls(master, "127.0.0.1", 0,
                                         DDUP_TEST_CERT_DIR "/cert.pem",
                                         DDUP_TEST_CERT_DIR "/key.pem"));
    tls_port = server_tls_port(master);
    DD_CHECK(tls_port != 0);
    DD_CHECK_EQ_INT(0, server_set_replica_tls(replica, 1,
                                               DDUP_TEST_CERT_DIR "/cert.pem"));
    DD_CHECK_EQ_INT(0, server_replicaof(replica, "127.0.0.1", tls_port));

    for (i = 0; i < 1000; i++) {
        server_run_once(master, 1);
        server_run_once(replica, 1);
        if (server_repl_info(replica)->link_up)
            break;
    }
    DD_CHECK(server_repl_info(replica)->link_up != 0);
    client = pal_tcp_connect("127.0.0.1", server_port(master));
    DD_CHECK(client != PAL_SOCKET_INVALID);
    if (client == PAL_SOCKET_INVALID)
        goto cleanup;
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(client, 1));
    DD_CHECK_EQ_INT(27, pal_send(client,
                                 "*3\r\n$3\r\nSET\r\n$1\r\nt\r\n$1\r\n1\r\n",
                                 27));
    for (i = 0; i < 1000; i++) {
        server_run_once(master, 1);
        server_run_once(replica, 1);
        if (server_repl_info(replica)->master_replid[0] != '\0')
            break;
    }
    for (i = 0; i < 100; i++) {
        n = pal_recv(client, buf, sizeof(buf));
        if (n > 0 || (n < 0 && !pal_would_block(pal_socket_error())))
            break;
        server_run_once(master, 1);
    }
    client = PAL_SOCKET_INVALID;
    DD_CHECK(server_repl_info(replica)->link_up != 0);

cleanup:
    if (client != PAL_SOCKET_INVALID)
        pal_close(client);
    if (replica != NULL)
        server_destroy(replica);
    if (master != NULL)
        server_destroy(master);
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
    uint64_t deadline = pal_now_ms() + 60000;
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
    deadline = pal_now_ms() + 60000;
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
        uint64_t deadline = pal_now_ms() + 60000;
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
        uint64_t deadline = pal_now_ms() + 60000;
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

static void test_cluster_tls_configuration(void)
{
    server *s;
    DD_CHECK_EQ_INT(0, pal_socket_init());
    s = server_create("127.0.0.1", 0);
    DD_CHECK(s != NULL);
    if (s != NULL) {
        DD_CHECK_EQ_INT(0, server_set_cluster_tls(
                               s, 1, DDUP_TEST_CERT_DIR "/cert.pem",
                               DDUP_TEST_CERT_DIR "/key.pem", NULL));
        server_enable_cluster(s,
                              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        DD_CHECK(server_db(s) != NULL);
        server_destroy(s);
    }
    s = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_IOURING_OP);
    if (s != NULL) {
        /* A proactor must reject bus TLS rather than silently enabling a
         * plaintext bus or leaving an unbound cluster control plane. */
        if (server_is_proactor(s))
            DD_CHECK_EQ_INT(-1, server_set_cluster_tls(
                                   s, 1, DDUP_TEST_CERT_DIR "/cert.pem",
                                   DDUP_TEST_CERT_DIR "/key.pem", NULL));
        server_destroy(s);
    }
    pal_socket_cleanup();
}

static void test_cluster_bus_tls_roundtrip(void)
{
    static const char id[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    server *s;
    SSL_CTX *cctx;
    SSL *ssl;
    pal_socket_t fd;
    resp_buf frame;
    char reply[16384];
    int rc, iter;
    uint64_t deadline;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    s = server_create("127.0.0.1", 0);
    DD_CHECK(s != NULL);
    if (s == NULL) {
        pal_socket_cleanup();
        return;
    }
    DD_CHECK_EQ_INT(0, server_set_cluster_tls(
                           s, 1, DDUP_TEST_CERT_DIR "/cert.pem",
                           DDUP_TEST_CERT_DIR "/key.pem", NULL));
    server_enable_cluster(s, id);

    /* Plaintext bytes on the TLS bus must be rejected before gossip parsing. */
    fd = pal_tcp_connect("127.0.0.1", (uint16_t)(server_port(s) + 10000));
    DD_CHECK(fd != PAL_SOCKET_INVALID);
    if (fd != PAL_SOCKET_INVALID) {
        DD_CHECK_EQ_INT(0, pal_set_nonblocking(fd, 1));
        (void)pal_send(fd, "RCM2", 4);
        for (iter = 0; iter < 50; iter++)
            server_run_once(s, 1);
        DD_CHECK(server_db(s)->nnodes == 1);
        pal_close(fd);
    }

    cctx = SSL_CTX_new(TLS_client_method());
    DD_CHECK(cctx != NULL);
    if (cctx == NULL) {
        server_destroy(s);
        pal_socket_cleanup();
        return;
    }
    SSL_CTX_set_verify(cctx, SSL_VERIFY_NONE, NULL);
    fd = pal_tcp_connect("127.0.0.1", (uint16_t)(server_port(s) + 10000));
    DD_CHECK(fd != PAL_SOCKET_INVALID);
    if (fd == PAL_SOCKET_INVALID) {
        SSL_CTX_free(cctx);
        server_destroy(s);
        pal_socket_cleanup();
        return;
    }
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(fd, 1));
    ssl = SSL_new(cctx);
    DD_CHECK(ssl != NULL);
    SSL_set_fd(ssl, (int)fd);
    deadline = pal_now_ms() + 10000;
    for (;;) {
        rc = SSL_connect(ssl);
        if (rc == 1)
            break;
        rc = SSL_get_error(ssl, rc);
        DD_CHECK(rc == SSL_ERROR_WANT_READ || rc == SSL_ERROR_WANT_WRITE);
        server_run_once(s, 1);
        if (pal_now_ms() >= deadline)
            break;
    }
    DD_CHECK(pal_now_ms() < deadline);

    resp_buf_init(&frame);
    DD_CHECK_EQ_INT(0, cluster_bus_build_frame(server_db(s),
                                               CLUSTER_MSG_PING, &frame));
    {
        size_t off = 0;
        deadline = pal_now_ms() + 10000;
        while (off < frame.len && pal_now_ms() < deadline) {
            rc = SSL_write(ssl, frame.data + off, (int)(frame.len - off));
            if (rc > 0)
                off += (size_t)rc;
            else {
                int er = SSL_get_error(ssl, rc);
                DD_CHECK(er == SSL_ERROR_WANT_READ ||
                         er == SSL_ERROR_WANT_WRITE);
                server_run_once(s, 1);
            }
        }
        DD_CHECK_EQ_INT((long long)frame.len, (long long)off);
    }
    {
        size_t got = 0;
        deadline = pal_now_ms() + 10000;
        while (got < 10 && pal_now_ms() < deadline) {
            rc = SSL_read(ssl, reply + got, (int)(sizeof(reply) - got));
            if (rc > 0)
                got += (size_t)rc;
            else {
                int er = SSL_get_error(ssl, rc);
                DD_CHECK(er == SSL_ERROR_WANT_READ ||
                         er == SSL_ERROR_WANT_WRITE);
                server_run_once(s, 1);
            }
        }
        DD_CHECK(got >= 10 && memcmp(reply, "RCM2", 4) == 0);
    }
    resp_buf_free(&frame);
    (void)SSL_shutdown(ssl);
    SSL_free(ssl);
    pal_close(fd);
    SSL_CTX_free(cctx);
    server_destroy(s);
    pal_socket_cleanup();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0); /* progress visible under timeout */
    DD_RUN(test_ctx_load);
    DD_RUN(test_ctx_bad_files);
    DD_RUN(test_tls_new_rejects_invalid_fd);
    DD_RUN(test_tls_rejects_lengths_over_int_max);
    DD_RUN(test_tls_init_is_thread_safe);
    DD_RUN(test_tls_server_roundtrip);
    DD_RUN(test_tls_replication_master_link);
    DD_RUN(test_mt_tls_roundtrip);
    DD_RUN(test_cluster_tls_configuration);
    DD_RUN(test_cluster_bus_tls_roundtrip);
    return DD_TEST_SUMMARY();
}
