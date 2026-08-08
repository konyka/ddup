/* pal_wakeup.h - cross-platform event-loop wakeup pipe.
 *
 * A connected socket pair: one end (wait_fd) is registered with a pal_loop
 * for read readiness; writing a byte to the other end (kick_fd) wakes the
 * loop. POSIX uses socketpair(AF_UNIX); Windows uses a loopback TCP pair.
 */
#ifndef DDUP_PAL_WAKEUP_H
#define DDUP_PAL_WAKEUP_H

#include "pal/pal_socket.h"

typedef struct pal_wakeup {
    pal_socket_t wait_fd; /* read end: register with the event loop */
    pal_socket_t kick_fd; /* write end: pal_wakeup_kick */
} pal_wakeup;

/* Create the pair (both ends non-blocking). Returns 0 on success and leaves
 * both descriptors invalid on failure. */
int pal_wakeup_create(pal_wakeup *w);
/* Post one wakeup byte. Returns 0 when a wakeup is pending (including a
 * full nonblocking queue), -1 on error. */
int pal_wakeup_kick(pal_wakeup *w);
/* Drain pending wakeup bytes. Returns bytes drained (>= 0). */
int pal_wakeup_drain(pal_wakeup *w);
void pal_wakeup_destroy(pal_wakeup *w);

#endif /* DDUP_PAL_WAKEUP_H */
