/* pal_iocp.c - Windows IOCP proactor; see pal_iocp.h.
 *
 * Platform note: this file is the ONLY place (besides pal internals) where
 * winsock/IOCP APIs appear. Non-Windows builds get an empty TU.
 */
#include "pal/pal_iocp.h"

#if DDUP_OS_WINDOWS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <ws2tcpip.h>

#include "pal/pal_time.h"

/* AcceptEx address buffer: local + remote, AF_INET sized. */
#define IOCP_ACC_BUFSIZE ((sizeof(SOCKADDR_IN) + 16) * 2)

typedef struct iocp_op {
    OVERLAPPED ov;
    pal_iocp_op op;
    void *userdata;
    pal_socket_t fd;
    pal_socket_t aux; /* ACCEPT: the listen socket (for UPDATE_ACCEPT_CONTEXT) */
    char *accbuf;     /* ACCEPT only: address buffer */
} iocp_op;

struct pal_iocp {
    HANDLE port;
    LPFN_ACCEPTEX acceptex;
};

pal_iocp *pal_iocp_create(void)
{
    pal_iocp *p = (pal_iocp *)calloc(1, sizeof(*p));
    if (p == NULL)
        return NULL;
    p->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (p->port == NULL) {
        free(p);
        return NULL;
    }
    return p;
}

void pal_iocp_free(pal_iocp *p)
{
    if (p == NULL)
        return;
    if (p->port != NULL)
        CloseHandle(p->port);
    free(p);
}

static iocp_op *op_new(pal_iocp_op op, pal_socket_t fd, void *userdata)
{
    iocp_op *o = (iocp_op *)calloc(1, sizeof(*o));
    if (o == NULL)
        return NULL;
    o->op = op;
    o->fd = fd;
    o->userdata = userdata;
    return o;
}

static int load_acceptex(pal_iocp *p, SOCKET fd)
{
    GUID guid = WSAID_ACCEPTEX;
    DWORD bytes = 0;
    return WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid,
                    sizeof(guid), &p->acceptex, sizeof(p->acceptex), &bytes,
                    NULL, NULL) == 0 &&
           p->acceptex != NULL;
}

pal_socket_t pal_iocp_listen(pal_iocp *p, const char *host, uint16_t port,
                             uint16_t *bound_port, void *userdata)
{
    SOCKET fd;
    struct sockaddr_in addr;
    (void)userdata;

    fd = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                    WSA_FLAG_OVERLAPPED);
    if (fd == INVALID_SOCKET)
        return PAL_SOCKET_INVALID;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host != NULL)
        (void)inet_pton(AF_INET, host, &addr.sin_addr);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, SOMAXCONN) != 0) {
        closesocket(fd);
        return PAL_SOCKET_INVALID;
    }
    if (!load_acceptex(p, fd)) {
        closesocket(fd);
        return PAL_SOCKET_INVALID;
    }
    if (CreateIoCompletionPort((HANDLE)fd, p->port, 0, 0) == NULL) {
        closesocket(fd);
        return PAL_SOCKET_INVALID;
    }
    if (bound_port != NULL) {
        struct sockaddr_in sa;
        int salen = (int)sizeof(sa);
        if (getsockname(fd, (struct sockaddr *)&sa, &salen) == 0)
            *bound_port = ntohs(sa.sin_port);
    }
    if (pal_iocp_accept_post(p, fd, userdata) != 0) {
        closesocket(fd);
        return PAL_SOCKET_INVALID;
    }
    return (pal_socket_t)fd;
}

int pal_iocp_accept_post(pal_iocp *p, pal_socket_t listen_fd, void *userdata)
{
    SOCKET acc;
    iocp_op *o;
    DWORD bytes = 0;

    acc = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                     WSA_FLAG_OVERLAPPED);
    if (acc == INVALID_SOCKET)
        return -1;
    o = op_new(PAL_IOCP_ACCEPT, (pal_socket_t)acc, userdata);
    if (o == NULL) {
        closesocket(acc);
        return -1;
    }
    o->aux = listen_fd;
    o->accbuf = (char *)calloc(1, IOCP_ACC_BUFSIZE);
    if (o->accbuf == NULL) {
        free(o);
        closesocket(acc);
        return -1;
    }
    if (!p->acceptex((SOCKET)listen_fd, acc, o->accbuf, 0, IOCP_ACC_BUFSIZE / 2,
                     IOCP_ACC_BUFSIZE / 2, &bytes, &o->ov) &&
        WSAGetLastError() != WSA_IO_PENDING) {
        free(o->accbuf);
        free(o);
        closesocket(acc);
        return -1;
    }
    return 0;
}

int pal_iocp_recv(pal_iocp *p, pal_socket_t fd, void *buf, size_t cap,
                  void *userdata)
{
    iocp_op *o;
    WSABUF wb;
    DWORD flags = 0;

    CreateIoCompletionPort((HANDLE)fd, p->port, 0, 0);
    o = op_new(PAL_IOCP_RECV, fd, userdata);
    if (o == NULL)
        return -1;
    wb.buf = (char *)buf;
    wb.len = (ULONG)cap;
    if (WSARecv((SOCKET)fd, &wb, 1, NULL, &flags, &o->ov, NULL) != 0 &&
        WSAGetLastError() != WSA_IO_PENDING) {
        free(o);
        return -1;
    }
    return 0;
}

int pal_iocp_send(pal_iocp *p, pal_socket_t fd, const void *buf, size_t n,
                  void *userdata)
{
    iocp_op *o;

    CreateIoCompletionPort((HANDLE)fd, p->port, 0, 0);
    o = op_new(PAL_IOCP_SEND, fd, userdata);
    if (o == NULL)
        return -1;
    {
        WSABUF wb;
        wb.buf = (char *)buf;
        wb.len = (ULONG)n;
        if (WSASend((SOCKET)fd, &wb, 1, NULL, 0, &o->ov, NULL) != 0 &&
            WSAGetLastError() != WSA_IO_PENDING) {
            free(o);
            return -1;
        }
    }
    return 0;
}

static int pump_once(pal_iocp *p, pal_iocp_event *ev, DWORD timeout_ms)
{
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED *ov = NULL;
    BOOL ok = GetQueuedCompletionStatus(p->port, &bytes, &key, &ov,
                                        timeout_ms);
    iocp_op *o;
    (void)key;
    if (ov == NULL)
        return 0; /* timeout */
    o = (iocp_op *)ov;
    ev->op = o->op;
    ev->fd = o->fd;
    ev->userdata = o->userdata;
    ev->bytes = ok ? (ptrdiff_t)bytes : (ptrdiff_t)-1;
    if (o->op == PAL_IOCP_ACCEPT) {
        SOCKET lfd = (SOCKET)o->aux;
        /* required before the accepted socket is fully usable */
        setsockopt((SOCKET)o->fd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                   (char *)&lfd, (int)sizeof(lfd));
        CreateIoCompletionPort((HANDLE)o->fd, p->port, 0, 0);
        ev->bytes = 0;
    }
    free(o->accbuf);
    free(o);
    return 1;
}

int pal_iocp_wait(pal_iocp *p, pal_iocp_event *evs, int max, int timeout_ms)
{
    int n = 0;
    uint64_t deadline = pal_now_ms() + (uint64_t)(timeout_ms < 0 ? 0 : timeout_ms);
    while (n < max) {
        DWORD wait = 0;
        if (timeout_ms >= 0 && n > 0) {
            uint64_t now = pal_now_ms();
            wait = now >= deadline ? 0 : (DWORD)(deadline - now);
        } else if (timeout_ms >= 0) {
            wait = (DWORD)timeout_ms;
        } else {
            wait = INFINITE;
        }
        if (pump_once(p, &evs[n], wait) == 0)
            break;
        n++;
        if (timeout_ms >= 0 && pal_now_ms() >= deadline)
            break;
    }
    return n;
}

void pal_iocp_close(pal_iocp *p, pal_socket_t fd)
{
    (void)p;
    CancelIoEx((HANDLE)fd, NULL);
    closesocket((SOCKET)fd);
}

#else /* !DDUP_OS_WINDOWS: empty translation unit */

typedef int pal_iocp_empty_tu;

#endif
