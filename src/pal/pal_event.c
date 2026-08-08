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
/* epoll (default) + io_uring (optional, direct syscalls, no liburing)  */
/* ==================================================================== */
#include <sys/epoll.h>
#include <unistd.h>

/* io_uring: raw syscalls + ring mmap (kernel >= 5.10, probed at runtime) */
#include <linux/io_uring.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>

typedef struct ep_reg {
    struct ep_reg *next;
    pal_socket_t fd;
    void *userdata;
    int want_read;  /* last requested interest set (io_uring re-arm) */
    int want_write;
    int active;     /* 0 after del: pending fired completions are skipped */
} ep_reg;

struct pal_loop {
    int epfd;
    ep_reg *regs; /* live registrations, for pal_loop_free */
    ep_reg *dead; /* io_uring: deactivated registrations (freed at loop_free) */
    /* io_uring flavor (use_iouring == 1) */
    int use_iouring;
    int uring_fd;
    void *ring_ptr;
    size_t ring_sz;
    struct io_uring_sqe *sqes;
    size_t sqes_sz;
    struct io_uring_cqe *cqes;
    unsigned *sq_array;  /* SQ indirection array (kernel reads sqes via it) */
    unsigned *sq_tail;
    unsigned *sq_ring_mask;
    unsigned sq_entries;
    unsigned *cq_head;
    unsigned *cq_tail;
    unsigned *cq_ring_mask;
    unsigned sq_submit; /* last tail the kernel was told about */
};

/* ------------------------------------------------------------------ */
/* epoll flavor                                                       */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* io_uring flavor                                                    */
/* ------------------------------------------------------------------ */

static int uring_sys_setup(unsigned entries, struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int uring_sys_enter(int fd, unsigned to_submit, unsigned min_complete,
                           unsigned flags)
{
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete,
                        flags, NULL, 0);
}

/* Probe support once (io_uring_setup works when the kernel allows it). */
static int uring_available(void)
{
    struct io_uring_params p;
    int fd;
    memset(&p, 0, sizeof(p));
    fd = uring_sys_setup(4, &p);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static int uring_init(pal_loop *l)
{
    struct io_uring_params p;
    size_t sq_ring_sz, cq_ring_sz, sqes_sz;
    char *base;

    memset(&p, 0, sizeof(p));
    l->uring_fd = uring_sys_setup(256, &p);
    if (l->uring_fd < 0)
        return -1;

    sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (sq_ring_sz < cq_ring_sz)
        sq_ring_sz = cq_ring_sz; /* SINGLE_MMAP is universal today */
    sqes_sz = p.sq_entries * sizeof(struct io_uring_sqe);

    l->ring_ptr = mmap(NULL, sq_ring_sz, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE, l->uring_fd,
                       IORING_OFF_SQ_RING);
    if (l->ring_ptr == MAP_FAILED) {
        close(l->uring_fd);
        return -1;
    }
    l->ring_sz = sq_ring_sz;
    base = (char *)l->ring_ptr;

    l->sqes = (struct io_uring_sqe *)mmap(
        NULL, sqes_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
        l->uring_fd, IORING_OFF_SQES);
    if (l->sqes == MAP_FAILED) {
        munmap(l->ring_ptr, l->ring_sz);
        close(l->uring_fd);
        return -1;
    }
    l->sqes_sz = sqes_sz;

    l->cqes = (struct io_uring_cqe *)(base + p.cq_off.cqes);
    l->sq_array = (unsigned *)(base + p.sq_off.array);
    l->sq_entries = p.sq_entries;
    l->sq_tail = (unsigned *)(base + p.sq_off.tail);
    l->sq_ring_mask = (unsigned *)(base + p.sq_off.ring_mask);
    l->cq_head = (unsigned *)(base + p.cq_off.head);
    l->cq_tail = (unsigned *)(base + p.cq_off.tail);
    l->cq_ring_mask = (unsigned *)(base + p.cq_off.ring_mask);
    l->sq_submit = 0;
    l->use_iouring = 1;
    return 0;
}

pal_loop *pal_loop_create_iouring(void)
{
    pal_loop *l;
    if (!uring_available())
        return NULL;
    l = pal_loop_create();
    if (l == NULL)
        return NULL;
    if (uring_init(l) != 0) {
        pal_loop_free(l);
        return NULL;
    }
    return l;
}

static struct io_uring_sqe *uring_get_sqe(pal_loop *l)
{
    unsigned tail = __atomic_load_n(l->sq_tail, __ATOMIC_RELAXED);
    unsigned idx = tail & *l->sq_ring_mask;
    struct io_uring_sqe *sqe;
    if (tail - l->sq_submit >= l->sq_entries) {
        /* SQ full (bursty mod/rearm storms at high fd counts): submit the
         * queue now; non-SQPOLL enter copies the sqes synchronously, so
         * the slots are reusable on return */
        unsigned n = tail - l->sq_submit;
        l->sq_submit = tail;
        (void)uring_sys_enter(l->uring_fd, n, 0, 0);
    }
    sqe = &l->sqes[idx];
    memset(sqe, 0, sizeof(*sqe));
    /* the kernel dereferences sqes through the SQ indirection array --
     * without this write every submission aliases sqe[0] */
    l->sq_array[idx] = idx;
    /* publish the sqe + array entry before the tail (ARM needs the
     * release store) */
    __atomic_store_n(l->sq_tail, tail + 1, __ATOMIC_RELEASE);
    return sqe;
}

static void uring_poll_add(pal_loop *l, ep_reg *r, int want_read,
                           int want_write)
{
    struct io_uring_sqe *sqe = uring_get_sqe(l);
    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = (int)r->fd;
    sqe->poll_events = (short)((want_read ? POLLIN : 0) |
                               (want_write ? POLLOUT : 0) | POLLERR |
                               POLLHUP);
    sqe->user_data = (unsigned long long)(uintptr_t)r;
}

static void uring_poll_remove(pal_loop *l, ep_reg *r)
{
    struct io_uring_sqe *sqe = uring_get_sqe(l);
    sqe->opcode = IORING_OP_POLL_REMOVE;
    sqe->fd = -1;
    sqe->user_data = (unsigned long long)(uintptr_t)r;
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
    while (l->dead != NULL) {
        ep_reg *r = l->dead;
        l->dead = r->next;
        free(r);
    }
    if (l->use_iouring) {
        munmap(l->sqes, l->sqes_sz);
        munmap(l->ring_ptr, l->ring_sz);
        close(l->uring_fd);
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
    r->want_read = want_read;
    r->want_write = want_write;
    r->active = 1;
    if (l->use_iouring) {
        uring_poll_add(l, r, want_read, want_write);
    } else if (ep_ctl(l, EPOLL_CTL_ADD, r, want_read, want_write) != 0) {
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
    if (l->use_iouring) {
        /* simplest correct mod: drop + re-add */
        r->want_read = want_read;
        r->want_write = want_write;
        uring_poll_remove(l, r);
        uring_poll_add(l, r, want_read, want_write);
        return 0;
    }
    return ep_ctl(l, EPOLL_CTL_MOD, r, want_read, want_write);
}

int pal_loop_del(pal_loop *l, pal_socket_t fd)
{
    ep_reg **prev = &l->regs;
    while (*prev != NULL && (*prev)->fd != fd)
        prev = &(*prev)->next;
    if (*prev == NULL)
        return -1;
    if (l->use_iouring) {
        /* deactivate + cancel; the reg is freed at loop_free (remove and
         * already-fired completions must never see freed memory) */
        ep_reg *r = *prev;
        r->active = 0;
        uring_poll_remove(l, r);
        *prev = r->next;
        r->next = l->dead;
        l->dead = r;
        return 0;
    }
    if (epoll_ctl(l->epfd, EPOLL_CTL_DEL, fd, NULL) != 0)
        return -1;
    {
        ep_reg *r = *prev;
        *prev = r->next;
        free(r);
    }
    return 0;
}

/* io_uring wait: submit queued sqes, block per timeout, reap cqes and
 * re-arm oneshot polls (level-triggered, epoll-compatible semantics). */
static int uring_wait(pal_loop *l, pal_event *events, int max, int timeout_ms)
{
    struct __kernel_timespec ts;
    unsigned to_submit = *l->sq_tail - l->sq_submit;
    unsigned min_complete = timeout_ms == 0 ? 0 : 1;
    unsigned head;
    int nev = 0;

    if (timeout_ms > 0) {
        struct io_uring_sqe *sqe = uring_get_sqe(l);
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long long)(timeout_ms % 1000) * 1000000LL;
        sqe->opcode = IORING_OP_TIMEOUT;
        sqe->fd = -1;
        sqe->addr = (unsigned long long)(uintptr_t)&ts;
        sqe->len = 1;
        sqe->user_data = 0; /* timeout completion marker */
        to_submit = *l->sq_tail - l->sq_submit;
    }

    l->sq_submit = *l->sq_tail;
    if (to_submit > 0 || min_complete > 0) {
        int rc = uring_sys_enter(l->uring_fd, to_submit, min_complete,
                                 IORING_ENTER_GETEVENTS);
        if (rc < 0)
            return -1;
    }

    head = __atomic_load_n(l->cq_head, __ATOMIC_RELAXED);
    while (head != __atomic_load_n(l->cq_tail, __ATOMIC_ACQUIRE) &&
           nev < max) {
        struct io_uring_cqe *cqe = &l->cqes[head & *l->cq_ring_mask];
        if (cqe->user_data != 0 && cqe->res > 0) {
            ep_reg *r = (ep_reg *)(uintptr_t)cqe->user_data;
            /* res > 0 is an event mask; res == 0 is a control completion
             * (POLL_REMOVE/POLL_UPDATE ack), res < 0 an error */
            if (r->active) {
                events[nev].fd = r->fd;
                events[nev].userdata = r->userdata;
                events[nev].readable =
                    (cqe->res & (POLLIN | POLLERR | POLLHUP)) ? 1 : 0;
                events[nev].writable = (cqe->res & POLLOUT) ? 1 : 0;
                nev++;
                /* oneshot poll completed: re-arm with the last requested
                 * interest set (level-triggered semantics) */
                uring_poll_add(l, r, r->want_read, r->want_write);
            }
        }
        head++;
    }
    __atomic_store_n(l->cq_head, head, __ATOMIC_RELEASE);
    return nev;
}

int pal_loop_wait(pal_loop *l, pal_event *events, int max, int timeout_ms)
{
    struct epoll_event evs[128];
    int nreq = max < 128 ? max : 128;
    int n;
    if (l->use_iouring)
        return uring_wait(l, events, max, timeout_ms);
    n = epoll_wait(l->epfd, evs, nreq, timeout_ms);
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

#if !DDUP_OS_LINUX
/* io_uring is Linux-only; other platforms probe as unavailable. */
pal_loop *pal_loop_create_iouring(void)
{
    return NULL;
}
#endif
