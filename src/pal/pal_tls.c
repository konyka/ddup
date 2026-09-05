/* pal_tls.c - optional TLS wrapper over OpenSSL; see pal_tls.h.
 *
 * OpenSSL headers are confined to this file (and tests/test_tls.c on the
 * client side). The stub half keeps every caller compiling when OpenSSL
 * is not available (DDUP_HAS_TLS=0).
 */
#include "pal/pal_tls.h"

#include <limits.h>
#include <stdlib.h>
#include "pal/pal_platform.h"

#if DDUP_HAS_TLS

#include <openssl/err.h>
#include <openssl/ssl.h>

#if DDUP_OS_WINDOWS
#include <windows.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <pthread.h>
#endif

#if DDUP_OS_WINDOWS
static BOOL CALLBACK pal_tls_lib_init_once(PINIT_ONCE once, PVOID param,
                                           PVOID *context)
{
    (void)once;
    (void)param;
    (void)context;
    SSL_library_init();
    SSL_load_error_strings();
    return TRUE;
}
#else
static void pal_tls_lib_init_once(void)
{
    SSL_library_init();
    SSL_load_error_strings();
}
#endif

struct pal_tls_ctx {
    SSL_CTX *ctx;
};

struct pal_tls {
    SSL *ssl;
};

static void pal_tls_lib_init(void)
{
#if DDUP_OS_WINDOWS
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    (void)InitOnceExecuteOnce(&once, pal_tls_lib_init_once, NULL, NULL);
#else
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    (void)pthread_once(&once, pal_tls_lib_init_once);
#endif
}

pal_tls_ctx *pal_tls_ctx_new(const char *cert_file, const char *key_file)
{
    pal_tls_ctx *c;
    SSL_CTX *ctx;
    if (cert_file == NULL || key_file == NULL || cert_file[0] == '\0' ||
        key_file[0] == '\0')
        return NULL;
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

pal_tls_ctx *pal_tls_ctx_new_client(const char *ca_file)
{
    pal_tls_ctx *c;
    SSL_CTX *ctx;
    pal_tls_lib_init();
    c = (pal_tls_ctx *)calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) {
        free(c);
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (ca_file != NULL && SSL_CTX_load_verify_locations(ctx, ca_file, NULL) != 1) {
        SSL_CTX_free(ctx);
        free(c);
        return NULL;
    }
    if (ca_file == NULL)
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    else
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
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
    if (ctx == NULL || ctx->ctx == NULL || fd == PAL_SOCKET_INVALID)
        return NULL;
    pal_tls *t = (pal_tls *)calloc(1, sizeof(*t));
    if (t == NULL)
        return NULL;
    t->ssl = SSL_new(ctx->ctx);
    if (t->ssl == NULL) {
        free(t);
        return NULL;
    }
    if ((uintmax_t)fd > (uintmax_t)INT_MAX ||
        SSL_set_fd(t->ssl, (int)fd) != 1) {
        SSL_free(t->ssl);
        free(t);
        return NULL;
    }
    return t;
}

int pal_tls_set_peer_name(pal_tls *t, const char *name)
{
    unsigned char addr[16];

    if (t == NULL || t->ssl == NULL || name == NULL || name[0] == '\0')
        return -1;
    if (inet_pton(AF_INET, name, addr) == 1 ||
        inet_pton(AF_INET6, name, addr) == 1) {
        X509_VERIFY_PARAM *param = SSL_get0_param(t->ssl);
        return param != NULL && X509_VERIFY_PARAM_set1_ip_asc(param, name) == 1
                   ? 0
                   : -1;
    }
    return SSL_set1_host(t->ssl, name) == 1 ? 0 : -1;
}

int pal_tls_accept_handshake(pal_tls *t)
{
    if (t == NULL || t->ssl == NULL)
        return -1;
    return SSL_accept(t->ssl) == 1 ? 0 : -1;
}

int pal_tls_handshake_nb(pal_tls *t)
{
    if (t == NULL || t->ssl == NULL)
        return -1;
    int rc = SSL_accept(t->ssl);
    int err;
    if (rc == 1)
        return 1;
    err = SSL_get_error(t->ssl, rc);
    if (err == SSL_ERROR_WANT_READ)
        return 0;
    if (err == SSL_ERROR_WANT_WRITE)
        return 2;
    return -1;
}

int pal_tls_connect_handshake_nb(pal_tls *t)
{
    if (t == NULL || t->ssl == NULL)
        return -1;
    int rc = SSL_connect(t->ssl);
    int err;
    if (rc == 1)
        return 1;
    err = SSL_get_error(t->ssl, rc);
    if (err == SSL_ERROR_WANT_READ)
        return 0;
    if (err == SSL_ERROR_WANT_WRITE)
        return 2;
    return -1;
}

ptrdiff_t pal_tls_read(pal_tls *t, void *buf, size_t n)
{
    if (t == NULL || t->ssl == NULL || (buf == NULL && n != 0) ||
        n > (size_t)INT_MAX)
        return -1;
    int rc = SSL_read(t->ssl, buf, (int)n);
    int err;
    if (rc > 0)
        return (ptrdiff_t)rc;
    err = SSL_get_error(t->ssl, rc);
    if (err == SSL_ERROR_ZERO_RETURN)
        return 0; /* clean close_notify */
    if (err == SSL_ERROR_SYSCALL && rc == 0)
        return 0; /* unclean close: treat as close */
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return -2;
    return -1;
}

ptrdiff_t pal_tls_write(pal_tls *t, const void *buf, size_t n)
{
    if (t == NULL || t->ssl == NULL || (buf == NULL && n != 0) ||
        n > (size_t)INT_MAX)
        return -1;
    int rc = SSL_write(t->ssl, buf, (int)n);
    int err;
    if (rc > 0)
        return (ptrdiff_t)rc;
    err = SSL_get_error(t->ssl, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return -2;
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

pal_tls_ctx *pal_tls_ctx_new_client(const char *ca_file)
{
    (void)ca_file;
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

int pal_tls_set_peer_name(pal_tls *t, const char *name)
{
    (void)t;
    (void)name;
    return -1;
}

int pal_tls_accept_handshake(pal_tls *t)
{
    (void)t;
    return -1;
}

int pal_tls_handshake_nb(pal_tls *t)
{
    (void)t;
    return -1;
}
int pal_tls_connect_handshake_nb(pal_tls *t)
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
