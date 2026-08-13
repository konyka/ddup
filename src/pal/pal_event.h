/* pal_event.h - readiness-based event loop, one per IO thread.
 *
 * Backend per platform (all inside pal_event.c):
 *   Linux            -> epoll (level-triggered)
 *   macOS / FreeBSD  -> kqueue
 *   Windows / others -> select()
 *
 * All fds registered with a loop must be non-blocking; the loop itself does
 * not change fd flags (pal_set_nonblocking).
 */
#ifndef DDUP_PAL_EVENT_H
#define DDUP_PAL_EVENT_H

#include "pal/pal_socket.h"

typedef struct pal_event {
    pal_socket_t fd;
    void *userdata;
    int readable;
    int writable;
} pal_event;

typedef struct pal_loop pal_loop;

pal_loop *pal_loop_create(void);
/* Create an io_uring-backed loop (Linux only). Returns NULL when the
 * kernel does not support io_uring — caller falls back to pal_loop_create. */
pal_loop *pal_loop_create_iouring(void);
void pal_loop_free(pal_loop *l);

/* Register fd for read and/or write readiness. Returns 0 on success.
 * userdata is returned verbatim in pal_event on every readiness report. */
int pal_loop_add(pal_loop *l, pal_socket_t fd, int want_read, int want_write,
                 void *userdata);
/* Change the interest set / userdata of an already-registered fd. */
int pal_loop_mod(pal_loop *l, pal_socket_t fd, int want_read, int want_write,
                 void *userdata);
int pal_loop_del(pal_loop *l, pal_socket_t fd);

/* Wait up to timeout_ms (< 0: block forever) for readiness; fills up to max
 * events and returns their count, 0 on timeout, -1 on error. */
int pal_loop_wait(pal_loop *l, pal_event *events, int max, int timeout_ms);

#if defined(DDUP_TESTING) && DDUP_OS_LINUX
typedef int (*pal_event_uring_enter_fn)(int fd, unsigned to_submit,
                                        unsigned min_complete,
                                        unsigned flags);
void pal_event_test_set_uring_enter(pal_event_uring_enter_fn fn);
#endif

#endif /* DDUP_PAL_EVENT_H */
