/* pal_iouring_op.c - Linux io_uring proactor backend; see pal_iouring_op.h.
 *
 * Direct syscalls + ring mmap (no liburing), same discipline as the
 * readiness io_uring flavor in pal_event.c. Completions are correlated to
 * operations by tagging user_data with the op kind in the low 3 bits
 * (conn/listen pointers are at least 8-aligned).
 */
#include "pal/pal_iouring_op.h"

#include "pal/pal_platform.h"

#if DDUP_OS_LINUX

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>

#include "pal/pal_thread.h"

/* older kernel headers may lack these (ubuntu CI has them; guards are for
 * portability of the source, the runtime still self-heals on -EINVAL) */
#ifndef IORING_ACCEPT_MULTISHOT
#define IORING_ACCEPT_MULTISHOT (1U << 1)
#endif
#ifndef IORING_CQE_F_MORE
#define IORING_CQE_F_MORE (1U << 1)
#endif
#ifndef IORING_RECV_MULTISHOT
#define IORING_RECV_MULTISHOT (1U << 1)
#endif
#ifndef IORING_CQE_F_BUFFER
#define IORING_CQE_F_BUFFER (1U << 0)
#endif
#ifndef IORING_CQE_BUFFER_SHIFT
#define IORING_CQE_BUFFER_SHIFT 16
#endif
#ifndef IORING_REGISTER_PBUF_RING
#define IORING_REGISTER_PBUF_RING 22
#define IORING_UNREGISTER_PBUF_RING 23
/* headers predate 5.19: provide the UAPI structs too */
struct io_uring_buf {
    unsigned long long addr;
    unsigned int len;
    unsigned short bid;
    unsigned short resv;
};
struct io_uring_buf_ring {
    union {
        struct {
            unsigned long long resv1;
            unsigned int resv2;
            unsigned short resv3;
            unsigned short tail;
        };
        struct io_uring_buf bufs[0];
    };
};
struct io_uring_buf_reg {
    unsigned long long ring_addr;
    unsigned int ring_entries;
    unsigned short bgid;
    unsigned short flags;
    unsigned long long resv[3];
};
#endif

#define PAL_IOU_SQ_ENTRIES 1024 /* CQ is 2x; overflow is kernel-buffered */

#define UD_TAG_MASK 0x7ull /* low 3 bits carry the op kind */

struct pal_iouring {
    int ring_fd;
    void *ring;
    size_t ring_sz;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    unsigned *sq_array; /* SQ indirection array */
    unsigned *sq_tail;
    unsigned *sq_ring_mask;
    unsigned sq_entries;
    unsigned *cq_head;
    unsigned *cq_tail;
    unsigned *cq_ring_mask;
    unsigned sq_submit; /* last tail the kernel was told about */
    int accept_multishot; /* cleared on the first -EINVAL from accept */
    int accept_armed;     /* an accept is live (multi- or single-shot) */
    pal_mutex lock;       /* SQ is single-producer: serialize submitters */
    /* provided-buffer ring (Phase 33 multishot recv); inactive when NULL */
    struct io_uring_buf_ring *pbuf_ring;
    char *pbuf_data;      /* count slabs of pbuf_size bytes */
    unsigned pbuf_count;  /* power of two */
    unsigned pbuf_mask;
    size_t pbuf_size;
};

static int iou_sys_setup(unsigned entries, struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int iou_sys_enter(int fd, unsigned to_submit, unsigned min_complete,
                         unsigned flags)
{
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete,
                        flags, NULL, 0);
}

pal_iouring *pal_iouring_create(void)
{
    struct io_uring_params p;
    size_t sq_ring_sz, cq_ring_sz, sqes_sz;
    char *base;
    pal_iouring *r = (pal_iouring *)calloc(1, sizeof(*r));
    if (r == NULL)
        return NULL;
    if (pal_mutex_init(&r->lock) != 0) {
        free(r);
        return NULL;
    }
    memset(&p, 0, sizeof(p));
    r->ring_fd = iou_sys_setup(PAL_IOU_SQ_ENTRIES, &p);
    if (r->ring_fd < 0) {
        pal_mutex_destroy(&r->lock);
        free(r);
        return NULL;
    }
    sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (sq_ring_sz < cq_ring_sz)
        sq_ring_sz = cq_ring_sz; /* SINGLE_MMAP is universal today */
    sqes_sz = p.sq_entries * sizeof(struct io_uring_sqe);
    r->ring = mmap(NULL, sq_ring_sz, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, r->ring_fd, IORING_OFF_SQ_RING);
    if (r->ring == MAP_FAILED)
        goto fail;
    r->ring_sz = sq_ring_sz;
    r->sqes = (struct io_uring_sqe *)mmap(
        NULL, sqes_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
        r->ring_fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) {
        munmap(r->ring, r->ring_sz);
        goto fail;
    }
    base = (char *)r->ring;
    r->cqes = (struct io_uring_cqe *)(base + p.cq_off.cqes);
    r->sq_array = (unsigned *)(base + p.sq_off.array);
    r->sq_entries = p.sq_entries;
    r->sq_tail = (unsigned *)(base + p.sq_off.tail);
    r->sq_ring_mask = (unsigned *)(base + p.sq_off.ring_mask);
    r->cq_head = (unsigned *)(base + p.cq_off.head);
    r->cq_tail = (unsigned *)(base + p.cq_off.tail);
    r->cq_ring_mask = (unsigned *)(base + p.cq_off.ring_mask);
    r->accept_multishot = 1; /* probed live: first -EINVAL clears it */
    return r;
fail:
    close(r->ring_fd);
    pal_mutex_destroy(&r->lock);
    free(r);
    return NULL;
}

void pal_iouring_free(pal_iouring *r)
{
    if (r == NULL)
        return;
    /* closing the ring fd first: the kernel cancels every in-flight
     * request synchronously on release, so no completion can touch
     * caller buffers after this returns; then drop the mappings */
    close(r->ring_fd);
    munmap(r->sqes, r->sq_entries * sizeof(struct io_uring_sqe));
    munmap(r->ring, r->ring_sz);
    free(r->pbuf_ring);
    free(r->pbuf_data);
    pal_mutex_destroy(&r->lock);
    free(r);
}

/* Caller must hold r->lock. */
static struct io_uring_sqe *iou_get_sqe(pal_iouring *r)
{
    unsigned tail = __atomic_load_n(r->sq_tail, __ATOMIC_RELAXED);
    unsigned idx = tail & *r->sq_ring_mask;
    struct io_uring_sqe *sqe;
    if (tail - r->sq_submit >= r->sq_entries) {
        /* SQ full: submit now; non-SQPOLL enter copies the sqes
         * synchronously, so the slots are reusable on return */
        unsigned n = tail - r->sq_submit;
        r->sq_submit = tail;
        (void)iou_sys_enter(r->ring_fd, n, 0, 0);
    }
    sqe = &r->sqes[idx];
    memset(sqe, 0, sizeof(*sqe));
    /* the kernel dereferences sqes through the SQ indirection array */
    r->sq_array[idx] = idx;
    __atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
    return sqe;
}

/* Submit everything queued so far. Caller must hold r->lock. */
static void iou_flush(pal_iouring *r)
{
    unsigned n = *r->sq_tail - r->sq_submit;
    if (n > 0) {
        int rc;
        r->sq_submit = *r->sq_tail;
        do {
            rc = iou_sys_enter(r->ring_fd, n, 0, 0);
        } while (rc < 0 && errno == EINTR);
    }
}

pal_socket_t pal_iouring_listen(pal_iouring *r, const char *host,
                                uint16_t port, uint16_t *bound_port,
                                void *userdata)
{
    pal_socket_t fd = pal_tcp_listen(host, port, 511, bound_port);
    if (fd == PAL_SOCKET_INVALID)
        return PAL_SOCKET_INVALID;
    if (pal_iouring_accept_post(r, fd, userdata) != 0) {
        pal_close(fd);
        return PAL_SOCKET_INVALID;
    }
    return fd;
}

int pal_iouring_accept_post(pal_iouring *r, pal_socket_t listen_fd,
                            void *userdata)
{
    struct io_uring_sqe *sqe;
    pal_mutex_lock(&r->lock);
    if (r->accept_armed) {
        /* multishot accept still live: it keeps producing completions */
        pal_mutex_unlock(&r->lock);
        return 0;
    }
    sqe = iou_get_sqe(r);
    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = (int)listen_fd;
    /* addr/addrlen NULL: peer address not needed */
    sqe->accept_flags = SOCK_NONBLOCK |
                        (r->accept_multishot ? IORING_ACCEPT_MULTISHOT : 0);
    sqe->user_data = (unsigned long long)(uintptr_t)userdata |
                     (unsigned long long)PAL_IOURING_ACCEPT;
    r->accept_armed = 1;
    pal_mutex_unlock(&r->lock);
    return 0;
}

int pal_iouring_recv(pal_iouring *r, pal_socket_t fd, void *buf, size_t cap,
                     void *userdata)
{
    struct io_uring_sqe *sqe;
    pal_mutex_lock(&r->lock);
    sqe = iou_get_sqe(r);
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = (int)fd;
    sqe->addr = (unsigned long long)(uintptr_t)buf;
    sqe->len = (unsigned)cap;
    sqe->msg_flags = 0;
    sqe->user_data = (unsigned long long)(uintptr_t)userdata |
                     (unsigned long long)PAL_IOURING_RECV;
    pal_mutex_unlock(&r->lock);
    return 0;
}

int pal_iouring_send(pal_iouring *r, pal_socket_t fd, const void *buf,
                     size_t n, void *userdata)
{
    struct io_uring_sqe *sqe;
    pal_mutex_lock(&r->lock);
    sqe = iou_get_sqe(r);
    sqe->opcode = IORING_OP_SEND;
    sqe->fd = (int)fd;
    sqe->addr = (unsigned long long)(uintptr_t)buf;
    sqe->len = (unsigned)n;
    sqe->msg_flags = MSG_NOSIGNAL; /* a closed peer must not kill the loop */
    sqe->user_data = (unsigned long long)(uintptr_t)userdata |
                     (unsigned long long)PAL_IOURING_SEND;
    pal_mutex_unlock(&r->lock);
    return 0;
}

int pal_iouring_enable_pbuf(pal_iouring *r, unsigned count, size_t size)
{
    struct io_uring_buf_reg reg;
    unsigned i;

    if (r->pbuf_ring != NULL)
        return 0; /* already enabled */
    if (count == 0 || (count & (count - 1)) != 0)
        return -1; /* ring entries must be a power of two */
    r->pbuf_ring = (struct io_uring_buf_ring *)calloc(
        count, sizeof(struct io_uring_buf));
    r->pbuf_data = (char *)malloc((size_t)count * size);
    if (r->pbuf_ring == NULL || r->pbuf_data == NULL) {
        free(r->pbuf_ring);
        free(r->pbuf_data);
        r->pbuf_ring = NULL;
        r->pbuf_data = NULL;
        return -1;
    }
    r->pbuf_count = count;
    r->pbuf_mask = count - 1;
    r->pbuf_size = size;
    /* the ring tail aliases bufs[0].resv (UAPI layout): slot array is
     * fully usable, only the reserved halfword is shared */
    for (i = 0; i < count; i++) {
        struct io_uring_buf *b = &r->pbuf_ring->bufs[i];
        b->addr = (unsigned long long)(uintptr_t)(r->pbuf_data +
                                                  (size_t)i * size);
        b->len = (unsigned int)size;
        b->bid = (unsigned short)i;
        b->resv = 0;
    }
    memset(&reg, 0, sizeof(reg));
    reg.ring_addr = (unsigned long long)(uintptr_t)r->pbuf_ring;
    reg.ring_entries = count;
    reg.bgid = 0;
    __atomic_store_n(&r->pbuf_ring->tail, (unsigned short)count,
                     __ATOMIC_RELEASE);
    if ((int)syscall(__NR_io_uring_register, r->ring_fd,
                     IORING_REGISTER_PBUF_RING, &reg, 1) != 0) {
        free(r->pbuf_ring);
        free(r->pbuf_data);
        r->pbuf_ring = NULL;
        r->pbuf_data = NULL;
        return -1; /* kernel too old: caller falls back to reposts */
    }
    return 0;
}

int pal_iouring_pbuf_active(const pal_iouring *r)
{
    return r->pbuf_ring != NULL;
}

int pal_iouring_recv_ms(pal_iouring *r, pal_socket_t fd, void *userdata)
{
    struct io_uring_sqe *sqe;
    if (r->pbuf_ring == NULL)
        return -1;
    pal_mutex_lock(&r->lock);
    sqe = iou_get_sqe(r);
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = (int)fd;
    /* addr/len unused with provided buffers: the kernel picks a slot */
    sqe->ioprio = IORING_RECV_MULTISHOT;
    sqe->buf_group = 0; /* matches the registered bgid */
    sqe->user_data = (unsigned long long)(uintptr_t)userdata |
                     (unsigned long long)PAL_IOURING_RECV;
    pal_mutex_unlock(&r->lock);
    return 0;
}

const void *pal_iouring_buf(const pal_iouring *r, int bid)
{
    if (bid < 0 || (unsigned)bid >= r->pbuf_count)
        return NULL;
    return r->pbuf_data + (size_t)bid * r->pbuf_size;
}

void pal_iouring_recycle(pal_iouring *r, int bid)
{
    /* single consumer (the reap thread): read tail, fill, release-store */
    unsigned short tail = __atomic_load_n(&r->pbuf_ring->tail,
                                          __ATOMIC_RELAXED);
    struct io_uring_buf *b = &r->pbuf_ring->bufs[tail & r->pbuf_mask];
    b->addr = (unsigned long long)(uintptr_t)(r->pbuf_data +
                                              (size_t)bid * r->pbuf_size);
    b->len = (unsigned int)r->pbuf_size;
    b->bid = (unsigned short)bid;
    b->resv = 0;
    __atomic_store_n(&r->pbuf_ring->tail, (unsigned short)(tail + 1),
                     __ATOMIC_RELEASE);
}

int pal_iouring_post(pal_iouring *r, void *userdata)
{
    struct io_uring_sqe *sqe;
    pal_mutex_lock(&r->lock);
    sqe = iou_get_sqe(r);
    sqe->opcode = IORING_OP_NOP;
    sqe->fd = -1;
    sqe->user_data = (unsigned long long)(uintptr_t)userdata |
                     (unsigned long long)PAL_IOURING_WAKEUP;
    iou_flush(r); /* cross-thread kick: make the waiter see it now */
    pal_mutex_unlock(&r->lock);
    return 0;
}

int pal_iouring_wait(pal_iouring *r, pal_iouring_event *evs, int max,
                     int timeout_ms)
{
    struct __kernel_timespec ts;
    unsigned head;
    unsigned min_complete = timeout_ms == 0 ? 0 : 1;
    int nev = 0;

    pal_mutex_lock(&r->lock);
    if (timeout_ms > 0) {
        /* relative timeout so a fully idle loop still wakes up; count=1
         * retires it early once any other completion arrives */
        struct io_uring_sqe *sqe = iou_get_sqe(r);
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long long)(timeout_ms % 1000) * 1000000LL;
        sqe->opcode = IORING_OP_TIMEOUT;
        sqe->fd = -1;
        sqe->addr = (unsigned long long)(uintptr_t)&ts;
        sqe->len = 1;
        sqe->user_data = 0; /* timeout completions carry no tag: skipped */
    }
    {
        unsigned to_submit = *r->sq_tail - r->sq_submit;
        int rc = 0;
        r->sq_submit = *r->sq_tail;
        /* the lock is NOT held across the blocking enter: a cross-thread
         * pal_iouring_post must be able to submit its NOP concurrently,
         * otherwise the kick could never wake this wait */
        pal_mutex_unlock(&r->lock);
        if (to_submit > 0 || min_complete > 0) {
            do {
                rc = iou_sys_enter(r->ring_fd, to_submit, min_complete,
                                   IORING_ENTER_GETEVENTS);
            } while (rc < 0 && errno == EINTR);
            if (rc < 0)
                return -1;
        }
    }

    head = __atomic_load_n(r->cq_head, __ATOMIC_RELAXED);
    while (head != __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE) &&
           nev < max) {
        struct io_uring_cqe *cqe = &r->cqes[head & *r->cq_ring_mask];
        unsigned long long ud = cqe->user_data;
        if (ud != 0) {
            pal_iouring_event *ev = &evs[nev];
            int op = (int)(ud & UD_TAG_MASK);
            ev->userdata = (void *)(uintptr_t)(ud & ~UD_TAG_MASK);
            ev->op = (pal_iouring_ev)op;
            ev->fd = PAL_SOCKET_INVALID;
            ev->bytes = cqe->res >= 0 ? (ptrdiff_t)cqe->res : -1;
            ev->err = cqe->res < 0 ? -cqe->res : 0;
            /* F_MORE: the underlying multishot request is still armed */
            ev->op_done = (cqe->flags & IORING_CQE_F_MORE) ? 0 : 1;
            ev->buf_id = (cqe->flags & IORING_CQE_F_BUFFER)
                             ? (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT)
                             : -1;
            if (op == PAL_IOURING_ACCEPT) {
                if (cqe->res < 0) {
                    r->accept_armed = 0;
                    if (cqe->res == -EINVAL && r->accept_multishot)
                        r->accept_multishot = 0; /* single-shot fallback */
                } else {
                    ev->fd = (pal_socket_t)cqe->res;
                    ev->bytes = 0;
                    /* F_MORE absent: the multishot accept terminated
                     * (or this was a single-shot); the caller's re-post
                     * re-arms it */
                    r->accept_armed =
                        (cqe->flags & IORING_CQE_F_MORE) != 0;
                }
            }
            nev++;
        }
        head++;
    }
    __atomic_store_n(r->cq_head, head, __ATOMIC_RELEASE);
    return nev;
}

void pal_iouring_close(pal_iouring *r, pal_socket_t fd)
{
    (void)r;
    /* in-flight requests hold their own file reference: shutdown makes a
     * pending recv complete promptly (0 = orderly close) and a pending
     * send fail (-EPIPE); close then releases the fd number. Completions
     * keep arriving keyed by user_data until everything has drained. */
    (void)shutdown((int)fd, SHUT_RDWR);
    (void)close((int)fd);
}

#else  /* !DDUP_OS_LINUX */

/* Non-Linux stub: the op backend is unavailable; callers probe
 * pal_iouring_create() and fall back to the readiness backend. */
pal_iouring *pal_iouring_create(void)
{
    return NULL;
}
void pal_iouring_free(pal_iouring *p)
{
    (void)p;
}
pal_socket_t pal_iouring_listen(pal_iouring *p, const char *host,
                                uint16_t port, uint16_t *bound_port,
                                void *userdata)
{
    (void)p; (void)host; (void)port; (void)bound_port; (void)userdata;
    return PAL_SOCKET_INVALID;
}
int pal_iouring_accept_post(pal_iouring *p, pal_socket_t listen_fd,
                            void *userdata)
{
    (void)p; (void)listen_fd; (void)userdata;
    return -1;
}
int pal_iouring_recv(pal_iouring *p, pal_socket_t fd, void *buf, size_t cap,
                     void *userdata)
{
    (void)p; (void)fd; (void)buf; (void)cap; (void)userdata;
    return -1;
}
int pal_iouring_send(pal_iouring *p, pal_socket_t fd, const void *buf,
                     size_t n, void *userdata)
{
    (void)p; (void)fd; (void)buf; (void)n; (void)userdata;
    return -1;
}
int pal_iouring_wait(pal_iouring *p, pal_iouring_event *evs, int max,
                     int timeout_ms)
{
    (void)p; (void)evs; (void)max; (void)timeout_ms;
    return -1;
}
int pal_iouring_enable_pbuf(pal_iouring *p, unsigned count, size_t size)
{
    (void)p; (void)count; (void)size;
    return -1;
}
int pal_iouring_pbuf_active(const pal_iouring *p)
{
    (void)p;
    return 0;
}
int pal_iouring_recv_ms(pal_iouring *p, pal_socket_t fd, void *userdata)
{
    (void)p; (void)fd; (void)userdata;
    return -1;
}
const void *pal_iouring_buf(const pal_iouring *p, int bid)
{
    (void)p; (void)bid;
    return NULL;
}
void pal_iouring_recycle(pal_iouring *p, int bid)
{
    (void)p; (void)bid;
}
int pal_iouring_post(pal_iouring *p, void *userdata)
{
    (void)p; (void)userdata;
    return -1;
}
void pal_iouring_close(pal_iouring *p, pal_socket_t fd)
{
    (void)p; (void)fd;
}

#endif /* DDUP_OS_LINUX */
