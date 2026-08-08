/* pal_iouring_op.h - Linux io_uring proactor (completion model), pal-internal.
 *
 * This is NOT a readiness API: operations are submitted asynchronously and
 * their completions are pumped via pal_iouring_wait(). Buffer ownership
 * stays with the caller until the matching completion is delivered.
 *
 * The API deliberately mirrors pal_iocp.h (same op kinds, same event
 * semantics, identical op numeric values) so the server can share one
 * proactor code path across Windows IOCP and Linux io_uring.
 *
 * Rules (documented simplifications):
 *   - at most ONE outstanding RECV and ONE outstanding SEND per connection;
 *   - every ACCEPT completion yields a new connected socket in ev->fd; the
 *     caller should re-post pal_iouring_accept_post to keep accepting
 *     (internally a multishot accept is used when the kernel supports it:
 *     re-posts are no-ops while the multishot accept stays armed, and the
 *     backend self-heals to single-shot repost if the kernel rejects
 *     IORING_ACCEPT_MULTISHOT with -EINVAL);
 *   - after pal_iouring_close, already-submitted ops for that fd still
 *     complete (the fd is shutdown+closed, so a pending recv completes
 *     promptly with bytes == 0; errors arrive as bytes == -1) -- the
 *     caller must tolerate late completions (zombie drain, as with IOCP);
 *   - direct syscalls, no liburing dependency (same discipline as the
 *     readiness io_uring flavor in pal_event.c).
 *
 * Non-Linux builds compile an empty translation unit: pal_iouring_create
 * returns NULL and callers fall back to the readiness backend.
 */
#ifndef DDUP_PAL_IOURING_OP_H
#define DDUP_PAL_IOURING_OP_H

#include <stddef.h>

#include "pal/pal_socket.h"

/* op kinds: identical numeric values to pal_iocp_op (server.c asserts). */
typedef enum pal_iouring_ev {
    PAL_IOURING_ACCEPT = 1,
    PAL_IOURING_RECV = 2,
    PAL_IOURING_SEND = 3,
    PAL_IOURING_WAKEUP = 4 /* pal_iouring_post: no fd, bytes == 0 */
} pal_iouring_ev;

typedef struct pal_iouring_event {
    void *userdata;    /* cookie passed at post/listen time */
    pal_iouring_ev op;
    pal_socket_t fd;   /* ACCEPT: the NEW connection; RECV/SEND: invalid */
    ptrdiff_t bytes;   /* >= 0 bytes (RECV: 0 = orderly close), -1 = error */
} pal_iouring_event;

typedef struct pal_iouring pal_iouring;

pal_iouring *pal_iouring_create(void);
void pal_iouring_free(pal_iouring *p);

/* Listen on host:port (0 = ephemeral, read back via bound_port) and arm
 * the accept. PAL_SOCKET_INVALID on error. */
pal_socket_t pal_iouring_listen(pal_iouring *p, const char *host,
                                uint16_t port, uint16_t *bound_port,
                                void *userdata);
/* (Re-)arm the accept on a listener; a no-op while a multishot accept is
 * armed. 0 on success. */
int pal_iouring_accept_post(pal_iouring *p, pal_socket_t listen_fd,
                            void *userdata);

/* Submit one recv/send (buffers caller-owned until completion).
 * 0 on success (queued), -1 on immediate error. */
int pal_iouring_recv(pal_iouring *p, pal_socket_t fd, void *buf, size_t cap,
                     void *userdata);
int pal_iouring_send(pal_iouring *p, pal_socket_t fd, const void *buf,
                     size_t n, void *userdata);

/* Flush queued submissions, wait per timeout_ms (0 = poll), and reap
 * completions; returns count (>=1), 0 on timeout, -1 on error. */
int pal_iouring_wait(pal_iouring *p, pal_iouring_event *evs, int max,
                     int timeout_ms);

/* Submit a WAKEUP completion (cross-thread kick). 0 on success. */
int pal_iouring_post(pal_iouring *p, void *userdata);

/* Shutdown + close the socket; in-flight ops complete (0/-1) afterwards. */
void pal_iouring_close(pal_iouring *p, pal_socket_t fd);

#endif /* DDUP_PAL_IOURING_OP_H */
