/* pal_wakeup.c - cross-platform event-loop wakeup pipe; see pal_wakeup.h.
 *
 * Platform ifdefs are allowed only inside src/pal/.
 */
#include "pal/pal_wakeup.h"

#include "pal/pal_platform.h"

#if DDUP_OS_WINDOWS

int pal_wakeup_create(pal_wakeup *w)
{
    pal_socket_t listener;
    uint16_t port = 0;

    if (w == NULL)
        return -1;
    w->wait_fd = PAL_SOCKET_INVALID;
    w->kick_fd = PAL_SOCKET_INVALID;

    listener = pal_tcp_listen("127.0.0.1", 0, 1, &port);
    if (listener == PAL_SOCKET_INVALID)
        return -1;
    w->kick_fd = pal_tcp_connect("127.0.0.1", port);
    if (w->kick_fd == PAL_SOCKET_INVALID) {
        pal_close(listener);
        return -1;
    }
    w->wait_fd = pal_accept(listener);
    pal_close(listener);
    if (w->wait_fd == PAL_SOCKET_INVALID) {
        pal_close(w->kick_fd);
        w->kick_fd = PAL_SOCKET_INVALID;
        return -1;
    }
    if (pal_set_nonblocking(w->wait_fd, 1) != 0 ||
        pal_set_nonblocking(w->kick_fd, 1) != 0) {
        pal_close(w->wait_fd);
        pal_close(w->kick_fd);
        w->wait_fd = PAL_SOCKET_INVALID;
        w->kick_fd = PAL_SOCKET_INVALID;
        return -1;
    }
    return 0;
}

#else /* POSIX */

#include <errno.h>
#include <sys/socket.h>

int pal_wakeup_create(pal_wakeup *w)
{
    int fds[2];
    if (w == NULL)
        return -1;
    w->wait_fd = PAL_SOCKET_INVALID;
    w->kick_fd = PAL_SOCKET_INVALID;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        return -1;
    w->wait_fd = fds[0];
    w->kick_fd = fds[1];
    if (pal_set_nonblocking(w->wait_fd, 1) != 0 ||
        pal_set_nonblocking(w->kick_fd, 1) != 0) {
        pal_close(w->wait_fd);
        pal_close(w->kick_fd);
        w->wait_fd = PAL_SOCKET_INVALID;
        w->kick_fd = PAL_SOCKET_INVALID;
        return -1;
    }
    return 0;
}

#endif

int pal_wakeup_kick(pal_wakeup *w)
{
    static const char b = 1;
    ptrdiff_t n;
    if (w == NULL || w->kick_fd == PAL_SOCKET_INVALID)
        return -1;
    do {
        n = pal_send(w->kick_fd, &b, 1);
    } while (n < 0 && pal_interrupted(pal_socket_error()));
    if (n == 1)
        return 0;
    /* A full nonblocking queue already guarantees a pending wakeup. */
    return n < 0 && pal_would_block(pal_socket_error()) ? 0 : -1;
}

int pal_wakeup_drain(pal_wakeup *w)
{
    char buf[64];
    int total = 0;
    if (w == NULL || w->wait_fd == PAL_SOCKET_INVALID)
        return 0;
    for (;;) {
        ptrdiff_t n = pal_recv(w->wait_fd, buf, sizeof(buf));
        if (n > 0) {
            total += (int)n;
            continue;
        }
        if (n < 0 && pal_interrupted(pal_socket_error()))
            continue;
        break; /* would-block / close / error: nothing more to drain */
    }
    return total;
}

void pal_wakeup_destroy(pal_wakeup *w)
{
    if (w == NULL)
        return;
    pal_close(w->wait_fd);
    pal_close(w->kick_fd);
    w->wait_fd = PAL_SOCKET_INVALID;
    w->kick_fd = PAL_SOCKET_INVALID;
}
