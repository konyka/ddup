/* pal_tls.c - optional TLS wrapper over OpenSSL; see pal_tls.h.
 *
 * OpenSSL headers are confined to this file (and tests/test_tls.c on the
 * client side). The stub half keeps every caller compiling when OpenSSL
 * is not available (DDUP_HAS_TLS=0).
 */
#include "pal/pal_tls.h"

#include <stdlib.h>

#if DDUP_HAS_TLS

#include <openssl/err.h>
#include <openssl/ssl.h>

struct pal_tls_ctx {
    SSL_CTX *ctx;
};

struct pal_tls {
    SSL *ssl;
};

static void pal_tls_lib_init(void)
{
    static int done = 0;
    if (!done) {
        SSL_library_init();
        SSL_load_error_strings();
        done = 1;
    }
}

pal_tls_ctx *pal_tls_ctx_new(const char *cert_file, const char *key_file)
{
    pal_tls_ctx *c;
    SSL_CTX *ctx;
    pal_tls_lib_init();
    c = (pal_tls_ctx *)calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == NULL) {
        free(c);
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    /* use_certificate_file (single PEM cert) is the most portable variant
     * across OpenSSL header generations; our deployments use one cert. */
    if (SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        free(c);
        return NULL;
    }
    c->ctx = ctx;
    return c;
}

void pal_tls_ctx_free(pal_tls_ctx *ctx)
{
    if (ctx == NULL)
        return;
    SSL_CTX_free(ctx->ctx);
    free(ctx);
}

pal_tls *pal_tls_new(pal_tls_ctx *ctx, pal_socket_t fd)
{
    pal_tls *t = (pal_tls *)calloc(1, sizeof(*t));
    if (t == NULL)
        return NULL;
    t->ssl = SSL_new(ctx->ctx);
    if (t->ssl == NULL) {
        free(t);
        return NULL;
    }
    SSL_set_fd(t->ssl, (int)fd);
    return t;
}

int pal_tls_accept_handshake(pal_tls *t)
{
    return SSL_accept(t->ssl) == 1 ? 0 : -1;
}

ptrdiff_t pal_tls_read(pal_tls *t, void *buf, size_t n)
{
    int rc = SSL_read(t->ssl, buf, (int)n);
    int err;
    if (rc > 0)
        return (ptrdiff_t)rc;
    err = SSL_get_error(t->ssl, rc);
    if (err == SSL_ERROR_ZERO_RETURN)
        return 0; /* clean close_notify */
    if (err == SSL_ERROR_SYSCALL && rc == 0)
        return 0; /* unclean close: treat as close */
    return -1;
}

ptrdiff_t pal_tls_write(pal_tls *t, const void *buf, size_t n)
{
    int rc = SSL_write(t->ssl, buf, (int)n);
    if (rc > 0)
        return (ptrdiff_t)rc;
    return -1;
}

void pal_tls_shutdown(pal_tls *t)
{
    if (t != NULL)
        (void)SSL_shutdown(t->ssl);
}

void pal_tls_free(pal_tls *t)
{
    if (t == NULL)
        return;
    SSL_free(t->ssl);
    free(t);
}

#else /* !DDUP_HAS_TLS: stubs that fail cleanly */

pal_tls_ctx *pal_tls_ctx_new(const char *cert_file, const char *key_file)
{
    (void)cert_file;
    (void)key_file;
    return NULL;
}

void pal_tls_ctx_free(pal_tls_ctx *ctx)
{
    (void)ctx;
}

pal_tls *pal_tls_new(pal_tls_ctx *ctx, pal_socket_t fd)
{
    (void)ctx;
    (void)fd;
    return NULL;
}

int pal_tls_accept_handshake(pal_tls *t)
{
    (void)t;
    return -1;
}

ptrdiff_t pal_tls_read(pal_tls *t, void *buf, size_t n)
{
    (void)t;
    (void)buf;
    (void)n;
    return -1;
}

ptrdiff_t pal_tls_write(pal_tls *t, const void *buf, size_t n)
{
    (void)t;
    (void)buf;
    (void)n;
    return -1;
}

void pal_tls_shutdown(pal_tls *t)
{
    (void)t;
}

void pal_tls_free(pal_tls *t)
{
    (void)t;
}

#endif /* DDUP_HAS_TLS */
