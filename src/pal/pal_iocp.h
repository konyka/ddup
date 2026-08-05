/* pal_iocp.h - Windows IOCP proactor (completion model), pal-internal.
 *
 * This is NOT a readiness API: operations are posted asynchronously and
 * their completions are pumped via pal_iocp_wait(). Buffer ownership stays
 * with the caller until the matching completion is delivered.
 *
 * Rules (documented simplifications):
 *   - at most ONE outstanding RECV and ONE outstanding SEND per connection;
 *   - IPv4 only for now (AcceptEx address buffers are sized for AF_INET);
 *   - every ACCEPT completion yields a new connected socket in ev->fd; the
 *     caller should re-post pal_iocp_accept_post to keep accepting;
 *   - after pal_iocp_close, already-queued completions for that fd may still
 *     be delivered with bytes == -1; the caller must tolerate them.
 *
 * Non-Windows builds compile an empty translation unit (this module is
 * Windows-only; other platforms keep the readiness backends).
 */
#ifndef DDUP_PAL_IOCP_H
#define DDUP_PAL_IOCP_H

#include <stddef.h>

#include "pal/pal_socket.h"

typedef enum pal_iocp_op {
    PAL_IOCP_ACCEPT = 1,
    PAL_IOCP_RECV = 2,
    PAL_IOCP_SEND = 3,
    PAL_IOCP_WAKEUP = 4 /* pal_iocp_post: no fd, bytes == 0 */
} pal_iocp_op;

typedef struct pal_iocp_event {
    void *userdata;    /* cookie passed at post/listen time */
    pal_iocp_op op;
    pal_socket_t fd;   /* ACCEPT: the NEW connection; RECV/SEND: the conn */
    ptrdiff_t bytes;   /* >= 0 bytes (RECV: 0 = orderly close), -1 = error */
} pal_iocp_event;

typedef struct pal_iocp pal_iocp;

pal_iocp *pal_iocp_create(void);
void pal_iocp_free(pal_iocp *p);

/* Overlapped listen on host:port (0 = ephemeral, read back via bound_port);
 * posts the first accept. PAL_SOCKET_INVALID on error. */
pal_socket_t pal_iocp_listen(pal_iocp *p, const char *host, uint16_t port,
                             uint16_t *bound_port, void *userdata);
/* Post one accept on a listener. 0 on success. */
int pal_iocp_accept_post(pal_iocp *p, pal_socket_t listen_fd, void *userdata);

/* Post one overlapped recv/send (buffers caller-owned until completion).
 * 0 on success (queued), -1 on immediate error. */
int pal_iocp_recv(pal_iocp *p, pal_socket_t fd, void *buf, size_t cap,
                  void *userdata);
int pal_iocp_send(pal_iocp *p, pal_socket_t fd, const void *buf, size_t n,
                  void *userdata);

/* Pump completions; returns count (>=1), 0 on timeout, -1 on error. */
int pal_iocp_wait(pal_iocp *p, pal_iocp_event *evs, int max, int timeout_ms);

/* Post a WAKEUP completion (cross-thread kick; e.g. a worker's task queue
 * became non-empty). 0 on success. */
int pal_iocp_post(pal_iocp *p, void *userdata);

/* Cancel outstanding ops and close the socket. */
void pal_iocp_close(pal_iocp *p, pal_socket_t fd);

#endif /* DDUP_PAL_IOCP_H */
