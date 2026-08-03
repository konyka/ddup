/* pal_event.c - readiness event loop backends; see pal_event.h.
 *
 * Platform selection (allowed only inside src/pal/):
 *   DDUP_OS_LINUX                -> epoll
 *   DDUP_OS_MACOS / DDUP_OS_FREEBSD -> kqueue
 *   everything else (incl. Windows) -> select()
 */
#include "pal/pal_event.h"

#include <stdlib.h>
#include <string.h>

#include "pal/pal_platform.h"

#if DDUP_OS_LINUX
/* ==================================================================== */
/* epoll backend                                                        */
/* ==================================================================== */
#include <sys/epoll.h>
#include <unistd.h>

typedef struct ep_reg {
    struct ep_reg *next;
    pal_socket_t fd;
    void *userdata;
} ep_reg;

struct pal_loop {
    int epfd;
    ep_reg *regs; /* live registrations, for pal_loop_free */
};

pal_loop *pal_loop_create(void)
{
    pal_loop *l = (pal_loop *)calloc(1, sizeof(*l));
    if (l == NULL)
        return NULL;
    l->epfd = epoll_create1(0);
    if (l->epfd < 0) {
        free(l);
        return NULL;
    }
    return l;
}

void pal_loop_free(pal_loop *l)
{
    if (l == NULL)
        return;
    while (l->regs != NULL) {
        ep_reg *r = l->regs;
        l->regs = r->next;
        free(r);
    }
    close(l->epfd);
    free(l);
}

static ep_reg *ep_find(pal_loop *l, pal_socket_t fd)
{
    ep_reg *r;
    for (r = l->regs; r != NULL; r = r->next)
        if (r->fd == fd)
            return r;
    return NULL;
}

static int ep_ctl(pal_loop *l, int op, ep_reg *r, int want_read,
                  int want_write)
{
    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));
    ee.events = (want_read ? EPOLLIN : 0u) | (want_write ? EPOLLOUT : 0u);
    ee.data.ptr = r;
    return epoll_ctl(l->epfd, op, r->fd, &ee);
}

int pal_loop_add(pal_loop *l, pal_socket_t fd, int want_read, int want_write,
                 void *userdata)
{
    ep_reg *r = (ep_reg *)calloc(1, sizeof(*r));
    if (r == NULL)
        return -1;
    r->fd = fd;
    r->userdata = userdata;
    if (ep_ctl(l, EPOLL_CTL_ADD, r, want_read, want_write) != 0) {
        free(r);
        return -1;
    }
    r->next = l->regs;
    l->regs = r;
    return 0;
}

int pal_loop_mod(pal_loop *l, pal_socket_t fd, int want_read, int want_write,
                 void *userdata)
{
    ep_reg *r = ep_find(l, fd);
    if (r == NULL)
        return -1;
    r->userdata = userdata;
    return ep_ctl(l, EPOLL_CTL_MOD, r, want_read, want_write);
}

int pal_loop_del(pal_loop *l, pal_socket_t fd)
{
    ep_reg **prev = &l->regs;
    while (*prev != NULL && (*prev)->fd != fd)
        prev = &(*prev)->next;
    if (*prev == NULL)
        return -1;
    if (epoll_ctl(l->epfd, EPOLL_CTL_DEL, fd, NULL) != 0)
        return -1;
    {
        ep_reg *r = *prev;
        *prev = r->next;
        free(r);
    }
    return 0;
}

int pal_loop_wait(pal_loop *l, pal_event *events, int max, int timeout_ms)
{
    struct epoll_event evs[128];
    int nreq = max < 128 ? max : 128;
    int n = epoll_wait(l->epfd, evs, nreq, timeout_ms);
    if (n <= 0)
        return n; /* 0 timeout, -1 error */
    for (int i = 0; i < n; i++) {
        ep_reg *r = (ep_reg *)evs[i].data.ptr;
        events[i].fd = r->fd;
        events[i].userdata = r->userdata;
        events[i].readable = (evs[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR))
                                 ? 1
                                 : 0;
        events[i].writable = (evs[i].events & EPOLLOUT) ? 1 : 0;
    }
    return n;
}

#elif DDUP_OS_MACOS || DDUP_OS_FREEBSD
/* ==================================================================== */
/* kqueue backend                                                       */
/* ==================================================================== */
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

struct pal_loop {
    int kq;
};

pal_loop *pal_loop_create(void)
{
    pal_loop *l = (pal_loop *)calloc(1, sizeof(*l));
    if (l == NULL)
        return NULL;
    l->kq = kqueue();
    if (l->kq < 0) {
        free(l);
        return NULL;
    }
    return l;
}

void pal_loop_free(pal_loop *l)
{
    if (l == NULL)
        return;
    close(l->kq);
    free(l);
}

static int kq_apply(pal_loop *l, pal_socket_t fd, int want_read,
                    int want_write, void *userdata)
{
    struct kevent kev[2];
    /* Both filters are always registered with EV_ADD (which also replaces
     * udata/flags on re-add, so this works for add AND mod); unwanted
     * filters are EV_DISABLEd rather than deleted. A plain EV_ADD +
     * EV_DELETE mix fails with ENOENT when the deleted filter was never
     * registered, and kevent() then applies NOTHING. */
    EV_SET(&kev[0], (uintptr_t)fd, EVFILT_READ,
           EV_ADD | (want_read ? EV_ENABLE : EV_DISABLE), 0, 0, userdata);
    EV_SET(&kev[1], (uintptr_t)fd, EVFILT_WRITE,
           EV_ADD | (want_write ? EV_ENABLE : EV_DISABLE), 0, 0, userdata);
    return kevent(l->kq, kev, 2, NULL, 0, NULL);
}

int pal_loop_add(pal_loop *l, pal_socket_t fd, int want_read, int want_write,
                 void *userdata)
{
    return kq_apply(l, fd, want_read, want_write, userdata);
}

int pal_loop_mod(pal_loop *l, pal_socket_t fd, int want_read, int want_write,
                 void *userdata)
{
    return kq_apply(l, fd, want_read, want_write, userdata);
}

int pal_loop_del(pal_loop *l, pal_socket_t fd)
{
    struct kevent kev[2];
    EV_SET(&kev[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&kev[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    return kevent(l->kq, kev, 2, NULL, 0, NULL);
}

int pal_loop_wait(pal_loop *l, pal_event *events, int max, int timeout_ms)
{
    struct kevent evs[128];
    struct timespec ts;
    struct timespec *tsp = NULL;
    int nreq = max < 128 ? max : 128;
    int n;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }
    n = kevent(l->kq, NULL, 0, evs, nreq, tsp);
    if (n <= 0)
        return n;
    for (int i = 0; i < n; i++) {
        events[i].fd = (pal_socket_t)evs[i].ident;
        events[i].userdata = evs[i].udata;
        events[i].readable = (evs[i].filter == EVFILT_READ) ? 1 : 0;
        events[i].writable = (evs[i].filter == EVFILT_WRITE) ? 1 : 0;
        if (evs[i].flags & EV_EOF)
            events[i].readable = 1; /* peer closed: surface as readable */
    }
    return n;
}

#else
/* ==================================================================== */
/* select() backend (Windows + generic fallback)                        */
/*                                                                      */
/* Linear registration table; wait() rebuilds fd_sets each round.       */
/* On Windows FD_SETSIZE is raised to 1024 before winsock2.h.           */
/* ==================================================================== */
#  if DDUP_OS_WINDOWS
#    define FD_SETSIZE 1024
#    include <winsock2.h>
#    include <windows.h>
#  else
#    include <sys/select.h>
#    include <unistd.h>
#  endif

typedef struct sel_reg {
    pal_socket_t fd;
    void *userdata;
    int want_read;
    int want_write;
} sel_reg;

struct pal_loop {
    sel_reg *regs;
    size_t count;
    size_t cap;
};

pal_loop *pal_loop_create(void)
{
    return (pal_loop *)calloc(1, sizeof(pal_loop));
}

void pal_loop_free(pal_loop *l)
{
    if (l == NULL)
        return;
    free(l->regs);
    free(l);
}

static sel_reg *sel_find(pal_loop *l, pal_socket_t fd)
{
    size_t i;
    for (i = 0; i < l->count; i++)
        if (l->regs[i].fd == fd)
            return &l->regs[i];
    return NULL;
}

int pal_loop_add(pal_loop *l, pal_socket_t fd, int want_read, int want_write,
                 void *userdata)
{
    if (sel_find(l, fd) != NULL)
        return -1;
    if (l->count == l->cap) {
        size_t ncap = l->cap == 0 ? 16 : l->cap * 2;
        sel_reg *nr = (sel_reg *)realloc(l->regs, ncap * sizeof(*nr));
        if (nr == NULL)
            return -1;
        l->regs = nr;
        l->cap = ncap;
    }
    l->regs[l->count].fd = fd;
    l->regs[l->count].userdata = userdata;
    l->regs[l->count].want_read = want_read;
    l->regs[l->count].want_write = want_write;
    l->count++;
    return 0;
}

int pal_loop_mod(pal_loop *l, pal_socket_t fd, int want_read, int want_write,
                 void *userdata)
{
    sel_reg *r = sel_find(l, fd);
    if (r == NULL)
        return -1;
    r->want_read = want_read;
    r->want_write = want_write;
    r->userdata = userdata;
    return 0;
}

int pal_loop_del(pal_loop *l, pal_socket_t fd)
{
    size_t i;
    for (i = 0; i < l->count; i++) {
        if (l->regs[i].fd == fd) {
            l->regs[i] = l->regs[l->count - 1];
            l->count--;
            return 0;
        }
    }
    return -1;
}

int pal_loop_wait(pal_loop *l, pal_event *events, int max, int timeout_ms)
{
    fd_set rfds, wfds;
    struct timeval tv;
    struct timeval *tvp = NULL;
    size_t i;
    int nev = 0;
    int rc;

    if (l->count == 0) {
        /* select() with no fds is a portable-ish sleep; on Windows it
         * fails, so sleep explicitly there. */
        if (timeout_ms > 0) {
#  if DDUP_OS_WINDOWS
            Sleep((DWORD)timeout_ms);
#  else
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (long)(timeout_ms % 1000) * 1000L;
            select(0, NULL, NULL, NULL, &tv);
#  endif
        }
        return 0;
    }

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    for (i = 0; i < l->count; i++) {
        if (l->regs[i].want_read)
            FD_SET(l->regs[i].fd, &rfds);
        if (l->regs[i].want_write)
            FD_SET(l->regs[i].fd, &wfds);
    }
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (long)(timeout_ms % 1000) * 1000L;
        tvp = &tv;
    }
    /* nfds is ignored on Windows; on POSIX it is max fd + 1. */
    {
        int nfds = 0;
#  if !DDUP_OS_WINDOWS
        for (i = 0; i < l->count; i++)
            if (l->regs[i].fd + 1 > nfds)
                nfds = l->regs[i].fd + 1;
#  endif
        rc = select(nfds, &rfds, &wfds, NULL, tvp);
    }
    if (rc <= 0)
        return rc; /* 0 timeout, -1 error */

    for (i = 0; i < l->count && nev < max; i++) {
        int rd = l->regs[i].want_read && FD_ISSET(l->regs[i].fd, &rfds);
        int wr = l->regs[i].want_write && FD_ISSET(l->regs[i].fd, &wfds);
        if (rd || wr) {
            events[nev].fd = l->regs[i].fd;
            events[nev].userdata = l->regs[i].userdata;
            events[nev].readable = rd;
            events[nev].writable = wr;
            nev++;
        }
    }
    return nev;
}

#endif
