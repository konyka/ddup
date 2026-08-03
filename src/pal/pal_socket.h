/* pal_socket.h - cross-platform TCP socket wrapper.
 *
 * Windows: Winsock2 (ws2_32). POSIX: BSD sockets. All platform details are
 * confined to pal_socket.c; this header only exposes an opaque handle type.
 *
 * Error model: functions return PAL_SOCKET_INVALID / -1 on failure; call
 * pal_socket_error() for the platform error code and pal_would_block() to
 * test it for EAGAIN/EWOULDBLOCK-style transient errors.
 */
#ifndef DDUP_PAL_SOCKET_H
#define DDUP_PAL_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#include "pal/pal_platform.h"

#if DDUP_OS_WINDOWS
/* SOCKET is UINT_PTR; keep winsock2.h out of this public header. */
typedef uintptr_t pal_socket_t;
#define PAL_SOCKET_INVALID ((pal_socket_t)~0ull)
#else
typedef int pal_socket_t;
#define PAL_SOCKET_INVALID (-1)
#endif

/* Winsock startup/cleanup; no-op on POSIX. init returns 0 on success. */
int pal_socket_init(void);
void pal_socket_cleanup(void);

/* Bind + listen on host:port. Pass port 0 for an ephemeral port; the actual
 * port is returned via bound_port (may be NULL). Returns PAL_SOCKET_INVALID
 * on failure. */
pal_socket_t pal_tcp_listen(const char *host, uint16_t port, int backlog,
                            uint16_t *bound_port);

/* Blocking connect. Returns PAL_SOCKET_INVALID on failure. */
pal_socket_t pal_tcp_connect(const char *host, uint16_t port);

/* Accept one pending connection (blocking unless the listener is
 * non-blocking). Returns PAL_SOCKET_INVALID on failure. */
pal_socket_t pal_accept(pal_socket_t listen_fd);

/* on != 0: non-blocking; on == 0: blocking. Returns 0 on success. */
int pal_set_nonblocking(pal_socket_t fd, int on);

/* > 0 bytes read, 0 orderly close, -1 error (check pal_would_block). */
ptrdiff_t pal_recv(pal_socket_t fd, void *buf, size_t n);
/* > 0 bytes written, -1 error (check pal_would_block). */
ptrdiff_t pal_send(pal_socket_t fd, const void *buf, size_t n);

/* Last platform socket error code (WSAGetLastError / errno). */
int pal_socket_error(void);
/* Non-zero if err is a transient would-block error. */
int pal_would_block(int err);

void pal_close(pal_socket_t fd);

#endif /* DDUP_PAL_SOCKET_H */
