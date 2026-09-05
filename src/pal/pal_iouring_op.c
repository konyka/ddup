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
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>

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
#ifndef IORING_UNREGISTER_PBUF_RING
#define IORING_UNREGISTER_PBUF_RING 23
#endif
#ifndef IORING_OP_ASYNC_CANCEL
#define IORING_OP_ASYNC_CANCEL 14
#endif
#ifndef IORING_ASYNC_CANCEL_ALL
#define IORING_ASYNC_CANCEL_ALL (1U << 0)
#endif
#ifndef IORING_ASYNC_CANCEL_ANY
#define IORING_ASYNC_CANCEL_ANY (1U << 2)
#endif
#ifndef IORING_OP_SEND_ZC
#define IORING_OP_SEND_ZC 47
#endif
#ifndef IORING_RECVSEND_FIXED_BUF
#define IORING_RECVSEND_FIXED_BUF (1U << 2)
#endif
#ifndef IORING_SEND_ZC_REPORT_USAGE
#define IORING_SEND_ZC_REPORT_USAGE (1U << 3)
#endif
#ifndef IORING_CQE_F_NOTIF
#define IORING_CQE_F_NOTIF (1U << 3)
#endif
#ifndef IORING_UNREGISTER_BUFFERS
#define IORING_UNREGISTER_BUFFERS 1
#endif
/* CMake probes the complete registered-pbuf UAPI. Default off for builds
 * that compile this source outside the project configuration. */
#ifndef DDUP_HAVE_IOURING_PBUF_UAPI
#define DDUP_HAVE_IOURING_PBUF_UAPI 0
#endif
#define PAL_IOU_HAVE_PBUF DDUP_HAVE_IOURING_PBUF_UAPI

/* 6.0+ setup hints (macro-guarded like the rest) */
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif

#define PAL_IOU_SQ_ENTRIES 1024 /* CQ is 2x; overflow is kernel-buffered */

#define UD_TAG_MASK 0x7ull /* low 3 bits carry the op kind */
#define UD_TAG_TIMEOUT 0x5ull
#define UD_TAG_CANCEL 0x6ull

static struct io_uring_sqe *iou_reserve_sqe(pal_iouring *r,
                                             unsigned *tail_out);
static int iou_publish_sqe(pal_iouring *r, unsigned tail);
static int iou_flush(pal_iouring *r);
static int iou_cancel_all_locked(pal_iouring *r);
static void iou_dispose_teardown_cqe(const struct io_uring_cqe *cqe);

typedef struct iou_timeout {
    struct __kernel_timespec ts;
    struct iou_timeout *next;
} iou_timeout;

struct pal_iouring {
    int ring_fd;
    void *ring;
    size_t ring_sz;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    unsigned *sq_array; /* SQ indirection array */
    unsigned *sq_head;
    unsigned *sq_tail;
    unsigned *sq_flags;
    unsigned *sq_ring_mask;
    unsigned sq_entries;
    unsigned *cq_head;
    unsigned *cq_tail;
    unsigned *cq_ring_mask;
    unsigned sq_submit; /* tail accepted by non-SQPOLL io_uring_enter */
    int sqpoll;
    int accept_multishot; /* cleared on the first -EINVAL from accept */
    int accept_armed;     /* an accept is live (multi- or single-shot) */
    int defer_taskrun;    /* DEFER_TASKRUN active: pump via enter always */
    pal_mutex lock;       /* SQ is single-producer: serialize submitters */
    /* provided-buffer ring (Phase 33 multishot recv); inactive when NULL */
    struct io_uring_buf_ring *pbuf_ring;
    char *pbuf_data;      /* count slabs of pbuf_size bytes */
    unsigned pbuf_count;  /* power of two */
    unsigned pbuf_mask;
    size_t pbuf_size;
    iou_timeout *timeout_pool;
    iou_timeout *timeout_free;
    int pbuf_registered;
    void *sbuf_data;
    struct iovec *sbuf_iov;
    unsigned char *sbuf_busy;
    unsigned sbuf_count;
    unsigned sbuf_hint;  /* next slot to probe, reducing repeated busy scans */
    size_t sbuf_size;
    int sbuf_registered;
    int test_fail_wake_once;
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

pal_iouring *pal_iouring_create_ex(unsigned flags)
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
    if (flags & PAL_IOURING_F_SQPOLL) {
        p.flags |= IORING_SETUP_SQPOLL;
        p.sq_thread_idle = 1000; /* park the SQ thread after 1s idle */
    }
    if (flags & PAL_IOURING_F_DEFER)
        p.flags |= IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SINGLE_ISSUER;
    r->ring_fd = iou_sys_setup(PAL_IOU_SQ_ENTRIES, &p);
    if (r->ring_fd < 0 && p.flags != 0) {
        /* privilege/kernel too old for the hints: retry plain (silent) */
        memset(&p, 0, sizeof(p));
        r->ring_fd = iou_sys_setup(PAL_IOU_SQ_ENTRIES, &p);
    }
    if (r->ring_fd < 0) {
        pal_mutex_destroy(&r->lock);
        free(r);
        return NULL;
    }
    r->defer_taskrun = (p.flags & IORING_SETUP_DEFER_TASKRUN) != 0;
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
    r->sq_head = (unsigned *)(base + p.sq_off.head);
    r->sq_tail = (unsigned *)(base + p.sq_off.tail);
    r->sq_flags = (unsigned *)(base + p.sq_off.flags);
    r->sq_ring_mask = (unsigned *)(base + p.sq_off.ring_mask);
    r->cq_head = (unsigned *)(base + p.cq_off.head);
    r->cq_tail = (unsigned *)(base + p.cq_off.tail);
    r->cq_ring_mask = (unsigned *)(base + p.cq_off.ring_mask);
    r->sq_submit = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    r->sqpoll = (p.flags & IORING_SETUP_SQPOLL) != 0;
    r->accept_multishot = 1; /* probed live: first -EINVAL clears it */
    r->timeout_pool = (iou_timeout *)calloc(r->sq_entries,
                                            sizeof(*r->timeout_pool));
    if (r->timeout_pool == NULL) {
        munmap(r->sqes, sqes_sz);
        munmap(r->ring, r->ring_sz);
        goto fail;
    }
    {
        unsigned i;
        for (i = 0; i < r->sq_entries; i++) {
            r->timeout_pool[i].next = r->timeout_free;
            r->timeout_free = &r->timeout_pool[i];
        }
    }
    return r;
fail:
    close(r->ring_fd);
    pal_mutex_destroy(&r->lock);
    free(r);
    return NULL;
}

pal_iouring *pal_iouring_create(void)
{
    return pal_iouring_create_ex(0);
}

void pal_iouring_free(pal_iouring *r)
{
    int keep_pbuf = 0;
    int keep_sbuf = 0;
    if (r == NULL)
        return;
    /* The owner must have stopped submitters/reapers before teardown. Keep
     * this lock held while unregistering and closing so no local PAL caller
     * can publish work against mappings being released. */
    pal_mutex_lock(&r->lock);
#if PAL_IOU_HAVE_PBUF
    if (r->pbuf_registered) {
        struct io_uring_buf_reg reg;
        if (iou_cancel_all_locked(r) != 0) {
            keep_pbuf = 1;
        } else {
            /* The unregister UAPI consumes only bgid. Passing registration
             * fields here is rejected by newer kernels. */
            memset(&reg, 0, sizeof(reg));
            reg.bgid = 0;
            if (syscall(__NR_io_uring_register, r->ring_fd,
                        IORING_UNREGISTER_PBUF_RING, &reg, 1) != 0)
                keep_pbuf = 1;
            else
                r->pbuf_registered = 0;
        }
    }
#endif
    if (r->sbuf_registered) {
        if (iou_cancel_all_locked(r) != 0 ||
            syscall(__NR_io_uring_register, r->ring_fd,
                    IORING_UNREGISTER_BUFFERS, NULL, 0) != 0)
            keep_sbuf = 1;
        else
            r->sbuf_registered = 0;
    }
    /* The cancel completion is observed before the ring and its registered
     * memory are released. This is required for multishot pbuf receives. */
    close(r->ring_fd);
    munmap(r->sqes, r->sq_entries * sizeof(struct io_uring_sqe));
    munmap(r->ring, r->ring_sz);
    if (!keep_pbuf) {
        free(r->pbuf_ring);
        free(r->pbuf_data);
    }
    if (!keep_sbuf) {
        free(r->sbuf_iov);
        free(r->sbuf_busy);
        free(r->sbuf_data);
    }
    free(r->timeout_pool);
    pal_mutex_unlock(&r->lock);
    pal_mutex_destroy(&r->lock);
    free(r);
}

/* Cancel every request currently owned by the ring and wait for the cancel
 * request's CQE. The kernel posts that CQE only after the targeted requests
 * have been quiesced, so registered pbuf memory is no longer referenced. */
static int iou_cancel_all_locked(pal_iouring *r)
{
    struct io_uring_sqe *sqe;
    unsigned tail;
    unsigned head;
    int rc;
    int seen = 0;
    int cancel_res = 0;

    sqe = iou_reserve_sqe(r, &tail);
    if (sqe == NULL)
        return -1;
    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->addr = 0;
    sqe->cancel_flags = IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL;
    sqe->user_data = UD_TAG_CANCEL;
    (void)iou_publish_sqe(r, tail);
    if (iou_flush(r) < 0)
        return -1;

    do {
        do {
            rc = iou_sys_enter(r->ring_fd, 0, seen ? 0 : 1,
                               IORING_ENTER_GETEVENTS);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0)
            return -1;
        head = __atomic_load_n(r->cq_head, __ATOMIC_RELAXED);
        while (head != __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE)) {
            struct io_uring_cqe *cqe = &r->cqes[head & *r->cq_ring_mask];
            if (cqe->user_data == UD_TAG_CANCEL) {
                cancel_res = cqe->res;
                seen = 1;
            } else
                iou_dispose_teardown_cqe(cqe);
            head++;
        }
        __atomic_store_n(r->cq_head, head, __ATOMIC_RELEASE);
    } while (!seen ||
             (__atomic_load_n(r->sq_flags, __ATOMIC_ACQUIRE) &
              IORING_SQ_CQ_OVERFLOW) != 0 ||
             __atomic_load_n(r->cq_head, __ATOMIC_RELAXED) !=
                 __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE));
    if (cancel_res < 0 && cancel_res != -ENOENT) {
        errno = -cancel_res;
        return -1;
    }
    return 0;
}

static void iou_dispose_teardown_cqe(const struct io_uring_cqe *cqe)
{
    unsigned long long ud = cqe->user_data;

    if ((ud & UD_TAG_MASK) == PAL_IOURING_ACCEPT && cqe->res >= 0)
        (void)close(cqe->res);
}

/* Submit all published normal-mode entries. Caller holds r->lock. */
static int iou_submit_locked(pal_iouring *r)
{
    unsigned tail;
    if (r->sqpoll)
        return 0;
    tail = __atomic_load_n(r->sq_tail, __ATOMIC_RELAXED);
    while (r->sq_submit != tail) {
        unsigned n = tail - r->sq_submit;
        int rc;
        do {
            rc = iou_sys_enter(r->ring_fd, n, 0, 0);
        } while (rc < 0 && errno == EINTR);
        if (rc <= 0)
            return -1;
        if ((unsigned)rc > n) {
            errno = EIO;
            return -1;
        }
        r->sq_submit += (unsigned)rc;
    }
    return 0;
}

/* Wake a parked SQPOLL thread after publication. Caller holds r->lock. */
static int iou_wake_sqpoll_locked(pal_iouring *r)
{
    int rc;
    if (r->sqpoll && r->test_fail_wake_once) {
        r->test_fail_wake_once = 0;
        errno = EIO;
        return -1;
    }
    if (!r->sqpoll ||
        (__atomic_load_n(r->sq_flags, __ATOMIC_ACQUIRE) &
         IORING_SQ_NEED_WAKEUP) == 0)
        return 0;
    do {
        rc = iou_sys_enter(r->ring_fd, 0, 0, IORING_ENTER_SQ_WAKEUP);
    } while (rc < 0 && errno == EINTR);
    return rc < 0 ? -1 : 0;
}

/* A full SQPOLL ring needs the kernel consumer to make room. */
static int iou_wait_sqpoll_space_locked(pal_iouring *r)
{
    unsigned flags = IORING_ENTER_SQ_WAIT;
    int rc;
    if (__atomic_load_n(r->sq_flags, __ATOMIC_ACQUIRE) &
        IORING_SQ_NEED_WAKEUP)
        flags |= IORING_ENTER_SQ_WAKEUP;
    do {
        rc = iou_sys_enter(r->ring_fd, 0, 0, flags);
    } while (rc < 0 && errno == EINTR);
    return rc < 0 ? -1 : 0;
}

/* Reserve one SQ slot without exposing it to the kernel. Caller holds lock. */
static struct io_uring_sqe *iou_reserve_sqe(pal_iouring *r, unsigned *tail_out)
{
    unsigned tail = __atomic_load_n(r->sq_tail, __ATOMIC_RELAXED);
    unsigned head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    unsigned idx;
    struct io_uring_sqe *sqe;
    if (tail - head >= r->sq_entries) {
        if (r->sqpoll) {
            if (iou_wait_sqpoll_space_locked(r) != 0)
                return NULL;
        }
        else if (iou_submit_locked(r) != 0)
            return NULL;
        head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
        if (tail - head >= r->sq_entries) {
            errno = EAGAIN;
            return NULL;
        }
    }
    idx = tail & *r->sq_ring_mask;
    sqe = &r->sqes[idx];
    memset(sqe, 0, sizeof(*sqe));
    /* the kernel dereferences sqes through the SQ indirection array */
    r->sq_array[idx] = idx;
    *tail_out = tail;
    return sqe;
}

/* Publish only after the reserved SQE and its array entry are complete. */
static int iou_publish_sqe(pal_iouring *r, unsigned tail)
{
    __atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
    /* The kernel requires a full barrier before observing SQPOLL flags. */
    __sync_synchronize();
    return iou_wake_sqpoll_locked(r) == 0
               ? PAL_IOURING_PUBLISHED
               : PAL_IOURING_PUBLISHED_RETRY;
}

/* Submit everything queued so far. Caller must hold r->lock. */
static int iou_flush(pal_iouring *r)
{
    if (r->sqpoll) {
        return iou_wake_sqpoll_locked(r) == 0
                   ? PAL_IOURING_PUBLISHED
                   : PAL_IOURING_PUBLISHED_RETRY;
    }
    return iou_submit_locked(r);
}

pal_socket_t pal_iouring_listen(pal_iouring *r, const char *host,
                                uint16_t port, uint16_t *bound_port,
                                void *userdata)
{
    if (r == NULL)
        return PAL_SOCKET_INVALID;
    pal_socket_t fd = pal_tcp_listen(host, port, 511, bound_port);
    if (fd == PAL_SOCKET_INVALID)
        return PAL_SOCKET_INVALID;
    if (pal_iouring_accept_post(r, fd, userdata) < 0) {
        pal_close(fd);
        return PAL_SOCKET_INVALID;
    }
    return fd;
}

int pal_iouring_accept_post(pal_iouring *r, pal_socket_t listen_fd,
                            void *userdata)
{
    struct io_uring_sqe *sqe;
    unsigned tail;
    int rc;
    if (r == NULL || listen_fd == PAL_SOCKET_INVALID)
        return -1;
    pal_mutex_lock(&r->lock);
    if (r->accept_armed) {
        /* multishot accept still live: it keeps producing completions */
        pal_mutex_unlock(&r->lock);
        return 0;
    }
    sqe = iou_reserve_sqe(r, &tail);
    if (sqe == NULL) {
        pal_mutex_unlock(&r->lock);
        return -1;
    }
    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = (int)listen_fd;
    /* addr/addrlen NULL: peer address not needed */
    sqe->accept_flags = SOCK_NONBLOCK |
                        (r->accept_multishot ? IORING_ACCEPT_MULTISHOT : 0);
    sqe->user_data = (unsigned long long)(uintptr_t)userdata |
                     (unsigned long long)PAL_IOURING_ACCEPT;
    rc = iou_publish_sqe(r, tail);
    r->accept_armed = 1;
    pal_mutex_unlock(&r->lock);
    return rc;
}

int pal_iouring_recv(pal_iouring *r, pal_socket_t fd, void *buf, size_t cap,
                     void *userdata)
{
    struct io_uring_sqe *sqe;
    unsigned tail;
    int rc;
    if (r == NULL || fd == PAL_SOCKET_INVALID ||
        (buf == NULL && cap != 0) || cap > UINT32_MAX)
        return -1;
    pal_mutex_lock(&r->lock);
    sqe = iou_reserve_sqe(r, &tail);
    if (sqe == NULL) {
        pal_mutex_unlock(&r->lock);
        return -1;
    }
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = (int)fd;
    sqe->addr = (unsigned long long)(uintptr_t)buf;
    sqe->len = (unsigned)cap;
    sqe->msg_flags = 0;
    sqe->user_data = (unsigned long long)(uintptr_t)userdata |
                     (unsigned long long)PAL_IOURING_RECV;
    rc = iou_publish_sqe(r, tail);
    pal_mutex_unlock(&r->lock);
    return rc;
}

int pal_iouring_send(pal_iouring *r, pal_socket_t fd, const void *buf,
                     size_t n, void *userdata)
{
    struct io_uring_sqe *sqe;
    unsigned tail;
    int rc;
    if (r == NULL || fd == PAL_SOCKET_INVALID ||
        (buf == NULL && n != 0) || n > UINT32_MAX)
        return -1;
    pal_mutex_lock(&r->lock);
    sqe = iou_reserve_sqe(r, &tail);
    if (sqe == NULL) {
        pal_mutex_unlock(&r->lock);
        return -1;
    }
    sqe->opcode = IORING_OP_SEND;
    sqe->fd = (int)fd;
    sqe->addr = (unsigned long long)(uintptr_t)buf;
    sqe->len = (unsigned)n;
    sqe->msg_flags = MSG_NOSIGNAL; /* a closed peer must not kill the loop */
    sqe->user_data = (unsigned long long)(uintptr_t)userdata |
                     (unsigned long long)PAL_IOURING_SEND;
    rc = iou_publish_sqe(r, tail);
    pal_mutex_unlock(&r->lock);
    return rc;
}

static int iou_send_fixed_common(pal_iouring *r, pal_socket_t fd, int bid,
                                 size_t offset, size_t n, void *userdata,
                                 int zc)
{
    struct io_uring_sqe *sqe;
    unsigned tail;
    int rc;
    if (r == NULL || fd == PAL_SOCKET_INVALID || n > UINT32_MAX)
        return -1;
    pal_mutex_lock(&r->lock);
    if (!r->sbuf_registered || bid < 0 || (unsigned)bid >= r->sbuf_count ||
        offset > r->sbuf_size || n > r->sbuf_size - offset) {
        pal_mutex_unlock(&r->lock);
        return -1;
    }
    sqe = iou_reserve_sqe(r, &tail);
    if (sqe == NULL) {
        pal_mutex_unlock(&r->lock);
        return -1;
    }
    sqe->opcode = zc ? IORING_OP_SEND_ZC : IORING_OP_SEND;
    sqe->fd = (int)fd;
    sqe->addr = (unsigned long long)(uintptr_t)(r->sbuf_iov[bid].iov_base) +
                (unsigned long long)offset;
    sqe->len = (unsigned)n;
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->ioprio = IORING_RECVSEND_FIXED_BUF;
    if (zc)
        sqe->ioprio |= IORING_SEND_ZC_REPORT_USAGE;
    sqe->buf_index = (unsigned short)bid;
    sqe->user_data = (unsigned long long)(uintptr_t)userdata |
                     (unsigned long long)PAL_IOURING_SEND;
    rc = iou_publish_sqe(r, tail);
    pal_mutex_unlock(&r->lock);
    return rc;
}

int pal_iouring_send_fixed(pal_iouring *r, pal_socket_t fd, int bid,
                           size_t offset, size_t n, void *userdata)
{
    return iou_send_fixed_common(r, fd, bid, offset, n, userdata, 0);
}

int pal_iouring_send_zc_fixed(pal_iouring *r, pal_socket_t fd, int bid,
                              size_t offset, size_t n, void *userdata)
{
    return iou_send_fixed_common(r, fd, bid, offset, n, userdata, 1);
}

int pal_iouring_enable_sbuf(pal_iouring *r, unsigned count, size_t size)
{
    struct iovec *iov = NULL;
    unsigned char *busy = NULL;
    char *data = NULL;
    unsigned i;
    struct iovec *reg_iov;

    if (r == NULL || count == 0 || count > UINT16_MAX || size == 0 ||
        size > UINT32_MAX || (size_t)count > SIZE_MAX / size)
        return -1;
    pal_mutex_lock(&r->lock);
    if (r->sbuf_registered) {
        pal_mutex_unlock(&r->lock);
        return 0;
    }
    iov = (struct iovec *)calloc(count, sizeof(*iov));
    busy = (unsigned char *)calloc(count, sizeof(*busy));
    data = (char *)malloc((size_t)count * size);
    if (iov == NULL || busy == NULL || data == NULL)
        goto fail;
    for (i = 0; i < count; i++) {
        iov[i].iov_base = data + (size_t)i * size;
        iov[i].iov_len = size;
    }
    reg_iov = iov;
    if (syscall(__NR_io_uring_register, r->ring_fd,
                IORING_REGISTER_BUFFERS, reg_iov, count) != 0)
        goto fail;
    r->sbuf_iov = iov;
    r->sbuf_busy = busy;
    r->sbuf_data = data;
    r->sbuf_count = count;
    r->sbuf_size = size;
    r->sbuf_registered = 1;
    pal_mutex_unlock(&r->lock);
    return 0;
fail:
    free(iov);
    free(busy);
    free(data);
    pal_mutex_unlock(&r->lock);
    return -1;
}

int pal_iouring_sbuf_active(const pal_iouring *r)
{
    int active;
    if (r == NULL)
        return 0;
    pal_mutex_lock((pal_mutex *)&r->lock);
    active = r->sbuf_registered;
    pal_mutex_unlock((pal_mutex *)&r->lock);
    return active;
}

void *pal_iouring_sbuf_acquire(pal_iouring *r, int *bid)
{
    unsigned i;
    unsigned start;
    void *buf = NULL;
    if (bid != NULL)
        *bid = -1;
    if (r == NULL || bid == NULL)
        return NULL;
    pal_mutex_lock(&r->lock);
    if (!r->sbuf_registered)
        goto done;
    start = r->sbuf_hint;
    if (start >= r->sbuf_count)
        start = 0;
    for (i = 0; i < r->sbuf_count; i++) {
        unsigned slot = start + i;
        if (slot >= r->sbuf_count)
            slot -= r->sbuf_count;
        if (r->sbuf_busy[slot] != 0)
            continue;
        r->sbuf_busy[slot] = 1;
        *bid = (int)slot;
        buf = r->sbuf_iov[slot].iov_base;
        r->sbuf_hint = (slot + 1u == r->sbuf_count) ? 0u : slot + 1u;
        break;
    }
done:
    pal_mutex_unlock(&r->lock);
    return buf;
}

void pal_iouring_sbuf_release(pal_iouring *r, int bid)
{
    if (r == NULL)
        return;
    pal_mutex_lock(&r->lock);
    if (r->sbuf_registered && bid >= 0 &&
        (unsigned)bid < r->sbuf_count)
        r->sbuf_busy[bid] = 0;
    pal_mutex_unlock(&r->lock);
}

int pal_iouring_enable_pbuf(pal_iouring *r, unsigned count, size_t size)
{
#if PAL_IOU_HAVE_PBUF
    struct io_uring_buf_reg reg;
    void *ring_mem = NULL;
    struct io_uring_buf_ring *ring;
    char *data;
    size_t ring_bytes;
    size_t alloc_bytes;
    long page_size;
    unsigned i;

    if (r == NULL)
        return -1;
    pal_mutex_lock(&r->lock);
    if (r->pbuf_ring != NULL) {
        pal_mutex_unlock(&r->lock);
        return 0; /* already enabled */
    }
    /* ring->tail is 16-bit; 65536 would wrap to zero when published. */
    if (size == 0 || size > UINT_MAX || count >= 65536U)
        goto invalid;
    if (count == 0 || (count & (count - 1)) != 0)
        goto invalid; /* ring entries must be a power of two */
    if (size > SIZE_MAX / (size_t)count)
        goto invalid;
    {
        volatile size_t count_size = (size_t)count;
        if (count_size > SIZE_MAX / sizeof(struct io_uring_buf))
            goto invalid;
    }
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        goto invalid;
    ring_bytes = sizeof(struct io_uring_buf_ring) +
                 (size_t)count * sizeof(struct io_uring_buf);
    if (ring_bytes > SIZE_MAX - ((size_t)page_size - 1))
        goto invalid;
    alloc_bytes = (ring_bytes + (size_t)page_size - 1) /
                  (size_t)page_size * (size_t)page_size;
    if (posix_memalign(&ring_mem, (size_t)page_size, alloc_bytes) != 0)
        goto invalid;
    memset(ring_mem, 0, alloc_bytes);
    ring = (struct io_uring_buf_ring *)ring_mem;
    data = (char *)malloc((size_t)count * size);
    if (data == NULL)
        goto alloc_fail;
    for (i = 0; i < count; i++) {
        struct io_uring_buf *b = &ring->bufs[i];
        b->addr = (unsigned long long)(uintptr_t)(data + (size_t)i * size);
        b->len = (unsigned int)size;
        b->bid = (unsigned short)i;
        b->resv = 0;
    }
    memset(&reg, 0, sizeof(reg));
    reg.ring_addr = (unsigned long long)(uintptr_t)ring;
    reg.ring_entries = count;
    reg.bgid = 0;
    __atomic_store_n(&ring->tail, (unsigned short)count, __ATOMIC_RELEASE);
    if ((int)syscall(__NR_io_uring_register, r->ring_fd,
                     IORING_REGISTER_PBUF_RING, &reg, 1) != 0)
        goto alloc_fail;
    r->pbuf_ring = ring;
    r->pbuf_data = data;
    r->pbuf_count = count;
    r->pbuf_mask = count - 1;
    r->pbuf_size = size;
    r->pbuf_registered = 1;
    pal_mutex_unlock(&r->lock);
    return 0;
alloc_fail:
    free(data);
    free(ring_mem);
invalid:
    pal_mutex_unlock(&r->lock);
    return -1;
#else
    (void)r; (void)count; (void)size;
    return -1; /* headers predate pbuf support */
#endif
}

int pal_iouring_pbuf_active(const pal_iouring *r)
{
    int active;
    if (r == NULL)
        return 0;
    pal_mutex_lock((pal_mutex *)&r->lock);
    active = r->pbuf_ring != NULL;
    pal_mutex_unlock((pal_mutex *)&r->lock);
    return active;
}

int pal_iouring_sqpoll_active(const pal_iouring *r)
{
    if (r == NULL)
        return 0;
    return r->sqpoll;
}

void pal_iouring_test_fail_next_sqpoll_wake(pal_iouring *r, int err)
{
    if (r == NULL)
        return;
    pal_mutex_lock(&r->lock);
    r->test_fail_wake_once = err == 0 ? EIO : err;
    pal_mutex_unlock(&r->lock);
}

int pal_iouring_recv_ms(pal_iouring *r, pal_socket_t fd, void *userdata)
{
#if PAL_IOU_HAVE_PBUF
    struct io_uring_sqe *sqe;
    unsigned tail;
    int rc;
    if (r == NULL || fd == PAL_SOCKET_INVALID)
        return -1;
    pal_mutex_lock(&r->lock);
    if (r->pbuf_ring == NULL) {
        pal_mutex_unlock(&r->lock);
        return -1;
    }
    sqe = iou_reserve_sqe(r, &tail);
    if (sqe == NULL) {
        pal_mutex_unlock(&r->lock);
        return -1;
    }
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = (int)fd;
    /* Buffer selection requires zero len; the kernel supplies slot capacity. */
    sqe->flags = IOSQE_BUFFER_SELECT;
    sqe->len = 0;
    sqe->ioprio = IORING_RECV_MULTISHOT;
    sqe->buf_group = 0; /* matches the registered bgid */
    sqe->user_data = (unsigned long long)(uintptr_t)userdata |
                     (unsigned long long)PAL_IOURING_RECV;
    rc = iou_publish_sqe(r, tail);
    pal_mutex_unlock(&r->lock);
    return rc;
#else
    (void)r; (void)fd; (void)userdata;
    return -1;
#endif
}

const void *pal_iouring_buf(const pal_iouring *r, int bid)
{
    const void *buf;
    if (r == NULL)
        return NULL;
    pal_mutex_lock((pal_mutex *)&r->lock);
    if (r->pbuf_ring == NULL || bid < 0 || (unsigned)bid >= r->pbuf_count) {
        pal_mutex_unlock((pal_mutex *)&r->lock);
        return NULL;
    }
    buf = r->pbuf_data + (size_t)bid * r->pbuf_size;
    pal_mutex_unlock((pal_mutex *)&r->lock);
    return buf;
}

void pal_iouring_recycle(pal_iouring *r, int bid)
{
#if PAL_IOU_HAVE_PBUF
    /* single consumer (the reap thread): read tail, fill, release-store */
    unsigned short tail;
    struct io_uring_buf *b;
    if (r == NULL || r->pbuf_ring == NULL || bid < 0 ||
        (unsigned)bid >= r->pbuf_count)
        return;
    pal_mutex_lock(&r->lock);
    if (r->pbuf_ring == NULL || (unsigned)bid >= r->pbuf_count) {
        pal_mutex_unlock(&r->lock);
        return;
    }
    tail = __atomic_load_n(&r->pbuf_ring->tail,
                                           __ATOMIC_RELAXED);
    b = &r->pbuf_ring->bufs[tail & r->pbuf_mask];
    b->addr = (unsigned long long)(uintptr_t)(r->pbuf_data +
                                               (size_t)bid * r->pbuf_size);
    b->len = (unsigned int)r->pbuf_size;
    b->bid = (unsigned short)bid;
    /* bufs[0].resv aliases the shared ring tail; never write resv. */
    __atomic_store_n(&r->pbuf_ring->tail, (unsigned short)(tail + 1),
                     __ATOMIC_RELEASE);
    pal_mutex_unlock(&r->lock);
#else
    (void)r; (void)bid;
#endif
}

int pal_iouring_post(pal_iouring *r, void *userdata)
{
    struct io_uring_sqe *sqe;
    unsigned tail;
    int rc;
    if (r == NULL)
        return -1;
    pal_mutex_lock(&r->lock);
    sqe = iou_reserve_sqe(r, &tail);
    if (sqe == NULL) {
        pal_mutex_unlock(&r->lock);
        return -1;
    }
    sqe->opcode = IORING_OP_NOP;
    sqe->fd = -1;
    sqe->user_data = (unsigned long long)(uintptr_t)userdata |
                     (unsigned long long)PAL_IOURING_WAKEUP;
    rc = iou_publish_sqe(r, tail);
    if (rc == PAL_IOURING_PUBLISHED)
        rc = iou_flush(r);
    if (rc < 0) {
        pal_mutex_unlock(&r->lock);
        return PAL_IOURING_PUBLISHED_RETRY;
    }
    pal_mutex_unlock(&r->lock);
    return rc;
}

int pal_iouring_wait(pal_iouring *r, pal_iouring_event *evs, int max,
                     int timeout_ms)
{
    unsigned head;
    unsigned min_complete = timeout_ms == 0 ? 0 : 1;
    int nev = 0;
    int rc = 0;

    if (r == NULL || evs == NULL || max <= 0 || timeout_ms < 0)
        return -1;
    pal_mutex_lock(&r->lock);
    if (timeout_ms > 0) {
        /* relative timeout so a fully idle loop still wakes up; count=1
         * retires it early once any other completion arrives */
        struct io_uring_sqe *sqe;
        iou_timeout *timeout;
        unsigned tail;
        timeout = r->timeout_free;
        if (timeout == NULL) {
            pal_mutex_unlock(&r->lock);
            errno = EAGAIN;
            return -1;
        }
        sqe = iou_reserve_sqe(r, &tail);
        if (sqe == NULL) {
            pal_mutex_unlock(&r->lock);
            return -1;
        }
        r->timeout_free = timeout->next;
        timeout->next = NULL;
        timeout->ts.tv_sec = timeout_ms / 1000;
        timeout->ts.tv_nsec = (long long)(timeout_ms % 1000) * 1000000LL;
        sqe->opcode = IORING_OP_TIMEOUT;
        sqe->fd = -1;
        sqe->addr = (unsigned long long)(uintptr_t)&timeout->ts;
        sqe->len = 1;
        sqe->user_data = (unsigned long long)(uintptr_t)timeout |
                         UD_TAG_TIMEOUT;
        (void)iou_publish_sqe(r, tail);
    }
    if (iou_flush(r) < 0) {
        pal_mutex_unlock(&r->lock);
        return -1;
    }
    /* All normal SQEs were accepted above. Unlock before the blocking enter
     * so a cross-thread post can submit its wakeup NOP with to_submit=0. */
    pal_mutex_unlock(&r->lock);
    if (min_complete > 0 || r->defer_taskrun) {
        do {
            rc = iou_sys_enter(r->ring_fd, 0, min_complete,
                               IORING_ENTER_GETEVENTS);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0)
            return -1;
    }

    head = __atomic_load_n(r->cq_head, __ATOMIC_RELAXED);
    while (head != __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE) &&
           nev < max) {
        struct io_uring_cqe *cqe = &r->cqes[head & *r->cq_ring_mask];
        unsigned long long ud = cqe->user_data;
        if ((ud & UD_TAG_MASK) == UD_TAG_TIMEOUT) {
            iou_timeout *timeout =
                (iou_timeout *)(uintptr_t)(ud & ~UD_TAG_MASK);
            pal_mutex_lock(&r->lock);
            timeout->next = r->timeout_free;
            r->timeout_free = timeout;
            pal_mutex_unlock(&r->lock);
        } else if (ud != 0) {
            pal_iouring_event *ev = &evs[nev];
            int op = (int)(ud & UD_TAG_MASK);
            ev->userdata = (void *)(uintptr_t)(ud & ~UD_TAG_MASK);
            ev->op = (pal_iouring_ev)op;
            ev->fd = PAL_SOCKET_INVALID;
            ev->bytes = cqe->res >= 0 ? (ptrdiff_t)cqe->res : -1;
            ev->err = cqe->res < 0 ? -cqe->res : 0;
            /* F_MORE: the underlying multishot request is still armed */
            ev->op_done = (cqe->flags & IORING_CQE_F_MORE) ? 0 : 1;
            ev->notif = (cqe->flags & IORING_CQE_F_NOTIF) != 0;
#if PAL_IOU_HAVE_PBUF
            ev->buf_id = (cqe->flags & IORING_CQE_F_BUFFER)
                             ? (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT)
                             : -1;
#else
            ev->buf_id = -1;
#endif
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
pal_iouring *pal_iouring_create_ex(unsigned flags)
{
    (void)flags;
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
int pal_iouring_send_fixed(pal_iouring *p, pal_socket_t fd, int bid,
                           size_t offset, size_t n, void *userdata)
{
    (void)p; (void)fd; (void)bid; (void)offset; (void)n; (void)userdata;
    return -1;
}
int pal_iouring_send_zc_fixed(pal_iouring *p, pal_socket_t fd, int bid,
                              size_t offset, size_t n, void *userdata)
{
    (void)p; (void)fd; (void)bid; (void)offset; (void)n; (void)userdata;
    return -1;
}
int pal_iouring_enable_sbuf(pal_iouring *p, unsigned count, size_t size)
{
    (void)p; (void)count; (void)size;
    return -1;
}
int pal_iouring_sbuf_active(const pal_iouring *p)
{
    (void)p;
    return 0;
}
void *pal_iouring_sbuf_acquire(pal_iouring *p, int *bid)
{
    (void)p;
    if (bid != NULL)
        *bid = -1;
    return NULL;
}
void pal_iouring_sbuf_release(pal_iouring *p, int bid)
{
    (void)p; (void)bid;
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
int pal_iouring_sqpoll_active(const pal_iouring *p)
{
    (void)p;
    return 0;
}
void pal_iouring_test_fail_next_sqpoll_wake(pal_iouring *p, int err)
{
    (void)p; (void)err;
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
