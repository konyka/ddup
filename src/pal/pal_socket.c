/* pal_socket.c - cross-platform TCP socket wrapper; see pal_socket.h.
 *
 * This file is one of the few places where platform ifdefs are allowed
 * (project rule: platform code lives only under src/pal/).
 */
#include "pal/pal_socket.h"

#include <stdio.h>
#include <string.h>

#if DDUP_OS_WINDOWS

#  include <winsock2.h>
#  include <ws2tcpip.h>

typedef int pal_socklen_t;

#else /* POSIX */

#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>

typedef socklen_t pal_socklen_t;

#  ifdef MSG_NOSIGNAL
#    define PAL_SEND_FLAGS MSG_NOSIGNAL
#  else
#    define PAL_SEND_FLAGS 0
#  endif

#endif

/* Suppress SIGPIPE on platforms without MSG_NOSIGNAL (macOS): writing to a
 * peer-closed socket would otherwise kill the process. Must be applied to
 * every socket — it is not inherited across accept(). */
static void pal_no_sigpipe(pal_socket_t fd)
{
#if !DDUP_OS_WINDOWS && !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, (pal_socklen_t)sizeof(one));
#else
    (void)fd;
#endif
}

int pal_socket_init(void)
{
#if DDUP_OS_WINDOWS
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

void pal_socket_cleanup(void)
{
#if DDUP_OS_WINDOWS
    WSACleanup();
#endif
}

int pal_socket_error(void)
{
#if DDUP_OS_WINDOWS
    return WSAGetLastError();
#else
    return errno;
#endif
}

int pal_would_block(int err)
{
#if DDUP_OS_WINDOWS
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

int pal_interrupted(int err)
{
#if DDUP_OS_WINDOWS
    return err == WSAEINTR;
#else
    return err == EINTR;
#endif
}

/* Non-blocking connect start: *out_fd receives a NON-blocking socket.
 * Returns 1 = connected already, 0 = in progress (complete via writable
 * readiness + pal_connect_finish), -1 = failure. */
int pal_tcp_connect_start(const char *host, uint16_t port,
                          pal_socket_t *out_fd)
{
    char port_str[8];
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    pal_socket_t fd = PAL_SOCKET_INVALID;
    int rc = -1;

    if (host == NULL || out_fd == NULL)
        return -1;

    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return -1;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = (pal_socket_t)socket(ai->ai_family, ai->ai_socktype,
                                  ai->ai_protocol);
        if (fd == PAL_SOCKET_INVALID)
            continue;
        pal_no_sigpipe(fd);
        if (pal_set_nonblocking(fd, 1) != 0) {
            pal_close(fd);
            fd = PAL_SOCKET_INVALID;
            continue;
        }
        if (connect(fd, ai->ai_addr, (pal_socklen_t)ai->ai_addrlen) == 0) {
            rc = 1;
            break;
        }
#if DDUP_OS_WINDOWS
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
#else
        if (errno == EINPROGRESS || errno == EAGAIN) {
#endif
            rc = 0;
            break;
        }
        pal_close(fd);
        fd = PAL_SOCKET_INVALID;
    }
    freeaddrinfo(res);
    if (rc < 0)
        return -1;
    *out_fd = fd;
    return rc;
}

/* Outcome of a pending pal_tcp_connect_start: 0 connected, -1 failed. */
int pal_connect_finish(pal_socket_t fd)
{
    int err = 0;
    pal_socklen_t el = (pal_socklen_t)sizeof(err);
    if (fd == PAL_SOCKET_INVALID)
        return -1;
#if DDUP_OS_WINDOWS
    if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char *)&err, &el) != 0)
#else
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0)
#endif
        return -1;
    return err == 0 ? 0 : -1;
}

int pal_set_nonblocking(pal_socket_t fd, int on)
{
    if (fd == PAL_SOCKET_INVALID)
        return -1;
#if DDUP_OS_WINDOWS
    u_long mode = on ? 1u : 0u;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    if (on)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags) == 0 ? 0 : -1;
#endif
}

int pal_set_tcp_nodelay(pal_socket_t fd, int on)
{
    if (fd == PAL_SOCKET_INVALID)
        return -1;
    int v = on ? 1 : 0;
#if DDUP_OS_WINDOWS
    return setsockopt((SOCKET)fd, IPPROTO_TCP, TCP_NODELAY,
                      (const char *)&v, (pal_socklen_t)sizeof(v)) == 0
               ? 0
               : -1;
#else
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v,
                      (pal_socklen_t)sizeof(v)) == 0
               ? 0
               : -1;
#endif
}

/* Set the usual ddup socket options on a listener. */
static void pal_listen_opts(pal_socket_t fd)
{
#if DDUP_OS_WINDOWS
    /* SO_EXCLUSIVEADDRUSE prevents port hijacking; SO_REUSEADDR on Windows
     * has dangerously different semantics than on POSIX. */
    int one = 1;
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               (const char *)&one, (pal_socklen_t)sizeof(one));
#else
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, (pal_socklen_t)sizeof(one));
#endif
}

pal_socket_t pal_tcp_listen(const char *host, uint16_t port, int backlog,
                            uint16_t *bound_port)
{
    char port_str[8];
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    pal_socket_t fd = PAL_SOCKET_INVALID;

    if (backlog <= 0)
        return PAL_SOCKET_INVALID;
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (host == NULL)
        hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return PAL_SOCKET_INVALID;

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = (pal_socket_t)socket(ai->ai_family, ai->ai_socktype,
                                  ai->ai_protocol);
        if (fd == PAL_SOCKET_INVALID)
            continue;
        pal_listen_opts(fd);
        if (bind(fd, ai->ai_addr, (pal_socklen_t)ai->ai_addrlen) != 0 ||
            listen(fd, backlog) != 0) {
            pal_close(fd);
            fd = PAL_SOCKET_INVALID;
            continue;
        }
        break;
    }
    freeaddrinfo(res);

    if (fd != PAL_SOCKET_INVALID && bound_port != NULL) {
        struct sockaddr_storage ss;
        pal_socklen_t sl = (pal_socklen_t)sizeof(ss);
        if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0) {
            if (ss.ss_family == AF_INET)
                *bound_port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
            else if (ss.ss_family == AF_INET6)
                *bound_port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
        }
    }
    return fd;
}

/* Wait (bounded) for a nonblocking connect to complete; 0 = connected. */
int pal_connect_wait(pal_socket_t fd, int timeout_ms)
{
    fd_set wfds, efds;
    struct timeval tv;
    int rc, err = 0;
    pal_socklen_t el = (pal_socklen_t)sizeof(err);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
#if DDUP_OS_WINDOWS
    FD_SET((SOCKET)fd, &wfds);
    FD_SET((SOCKET)fd, &efds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (long)(timeout_ms % 1000) * 1000L;
    rc = select(0, NULL, &wfds, &efds, &tv);
    if (rc <= 0 || FD_ISSET(fd, &efds) || !FD_ISSET(fd, &wfds))
        return -1;
    if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char *)&err, &el) != 0)
        return -1;
#else
    FD_SET(fd, &wfds);
    FD_SET(fd, &efds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (long)(timeout_ms % 1000) * 1000L;
    rc = select((int)fd + 1, NULL, &wfds, &efds, &tv);
    if (rc <= 0)
        return -1;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0)
        return -1;
#endif
    return err == 0 ? 0 : -1;
}

/* Connect to host:port, bounded: healthy connects complete instantly,
 * refused ones fail in milliseconds, silently dropped ones in ~1s
 * (unbounded OS SYN retry stacks stall event loops for seconds — seen
 * on Windows where a refused loopback connect blocks ~2s). */
pal_socket_t pal_tcp_connect(const char *host, uint16_t port)
{
    char port_str[8];
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    pal_socket_t fd = PAL_SOCKET_INVALID;

    if (host == NULL)
        return PAL_SOCKET_INVALID;
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return PAL_SOCKET_INVALID;

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = (pal_socket_t)socket(ai->ai_family, ai->ai_socktype,
                                  ai->ai_protocol);
        if (fd == PAL_SOCKET_INVALID)
            continue;
        pal_no_sigpipe(fd);
        if (pal_set_nonblocking(fd, 1) != 0) {
            pal_close(fd);
            fd = PAL_SOCKET_INVALID;
            continue;
        }
        if (connect(fd, ai->ai_addr, (pal_socklen_t)ai->ai_addrlen) == 0 ||
            pal_connect_wait(fd, 1000) == 0) {
            (void)pal_set_nonblocking(fd, 0); /* legacy blocking semantics */
            break;
        }
        pal_close(fd);
        fd = PAL_SOCKET_INVALID;
    }
    freeaddrinfo(res);
    return fd;
}

pal_socket_t pal_accept(pal_socket_t listen_fd)
{
    if (listen_fd == PAL_SOCKET_INVALID)
        return PAL_SOCKET_INVALID;
    pal_socket_t fd = (pal_socket_t)accept(listen_fd, NULL, NULL);
    pal_no_sigpipe(fd);
    return fd;
}

ptrdiff_t pal_recv(pal_socket_t fd, void *buf, size_t n)
{
#if DDUP_OS_WINDOWS
    int rc = recv((SOCKET)fd, (char *)buf, (int)n, 0);
    if (rc == SOCKET_ERROR)
        return -1;
    return (ptrdiff_t)rc;
#else
    ssize_t rc = recv(fd, buf, n, 0);
    if (rc < 0)
        return -1;
    return (ptrdiff_t)rc;
#endif
}

ptrdiff_t pal_send(pal_socket_t fd, const void *buf, size_t n)
{
#if DDUP_OS_WINDOWS
    int rc = send((SOCKET)fd, (const char *)buf, (int)n, 0);
    if (rc == SOCKET_ERROR)
        return -1;
    return (ptrdiff_t)rc;
#else
    ssize_t rc = send(fd, buf, n, PAL_SEND_FLAGS);
    if (rc < 0)
        return -1;
    return (ptrdiff_t)rc;
#endif
}

void pal_close(pal_socket_t fd)
{
    if (fd == PAL_SOCKET_INVALID)
        return;
#if DDUP_OS_WINDOWS
    closesocket((SOCKET)fd);
#else
    close(fd);
#endif
}

int pal_get_peer_ip(pal_socket_t fd, char *out, size_t cap)
{
    if (fd == PAL_SOCKET_INVALID || out == NULL || cap == 0)
        return -1;
#if DDUP_OS_WINDOWS
    struct sockaddr_in sa;
    int salen = sizeof(sa);
    const char *r;
    if (getpeername((SOCKET)fd, (struct sockaddr *)&sa, &salen) != 0)
        return -1;
    r = inet_ntop(AF_INET, &sa.sin_addr, out, (int)cap);
#else
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    const char *r;
    if (getpeername(fd, (struct sockaddr *)&sa, &salen) != 0)
        return -1;
    r = inet_ntop(AF_INET, &sa.sin_addr, out, cap);
#endif
    return r != NULL ? 0 : -1;
}
