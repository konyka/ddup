/* pal_tls.h - optional TLS wrapper over OpenSSL (server side).
 *
 * Feature-gated by DDUP_HAS_TLS (CMake sets it when OpenSSL is found and
 * DDUP_TLS=ON). When DDUP_HAS_TLS=0 the same API exists as stubs that fail
 * cleanly, so callers compile and run identically.
 *
 * OpenSSL headers are included ONLY in pal_tls.c. The accept handshake is
 * blocking (documented simplification for this phase).
 */
#ifndef DDUP_PAL_TLS_H
#define DDUP_PAL_TLS_H

#include <stddef.h>

#include "pal/pal_socket.h"

#ifndef DDUP_HAS_TLS
#define DDUP_HAS_TLS 0
#endif

typedef struct pal_tls_ctx pal_tls_ctx;
typedef struct pal_tls pal_tls;

/* Load cert/key (PEM), TLS 1.2+. NULL on error (and always when
 * DDUP_HAS_TLS=0). */
pal_tls_ctx *pal_tls_ctx_new(const char *cert_file, const char *key_file);
void pal_tls_ctx_free(pal_tls_ctx *ctx);

pal_tls *pal_tls_new(pal_tls_ctx *ctx, pal_socket_t fd);
/* Blocking server-side handshake. 0 on success, -1 on error. */
int pal_tls_accept_handshake(pal_tls *t);
/* Non-blocking handshake step (fd must be non-blocking):
 * 1 = done, 0 = want-read, 2 = want-write, -1 = error. */
int pal_tls_handshake_nb(pal_tls *t);
/* > 0 bytes read, 0 clean close, -1 error (incl. unclean close),
 * -2 would-block (WANT_READ/WANT_WRITE on a non-blocking fd). */
ptrdiff_t pal_tls_read(pal_tls *t, void *buf, size_t n);
/* > 0 bytes written, -1 error, -2 would-block. */
ptrdiff_t pal_tls_write(pal_tls *t, const void *buf, size_t n);
/* Best-effort close_notify. */
void pal_tls_shutdown(pal_tls *t);
void pal_tls_free(pal_tls *t);

#endif /* DDUP_PAL_TLS_H */
