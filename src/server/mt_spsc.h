/* mt_spsc.h - single-producer/single-consumer ring buffer.
 *
 * Lock-free with C11 atomics (acquire/release on head/tail); falls back to
 * a mutex-protected ring when atomics are unavailable (forced C99 builds).
 * One slot is kept empty to distinguish full from empty.
 */
#ifndef DDUP_MT_SPSC_H
#define DDUP_MT_SPSC_H

#include <stddef.h>
#include <stdlib.h>

#include "pal/pal_cstd.h"
#include "pal/pal_thread.h"

#if DDUP_HAS_C_ATOMICS
#  include <stdatomic.h>
#endif

typedef struct mt_spsc {
    void **buf;
    size_t mask;
#if DDUP_HAS_C_ATOMICS
    _Atomic size_t head; /* consumer index */
    _Atomic size_t tail; /* producer index */
#else
    size_t head;
    size_t tail;
    pal_mutex mu;
#endif
} mt_spsc;

static inline int mt_spsc_init(mt_spsc *q, size_t min_cap)
{
    size_t cap = 16;
    while (cap < min_cap)
        cap *= 2;
    q->buf = (void **)calloc(cap, sizeof(void *));
    if (q->buf == NULL)
        return -1;
    q->mask = cap - 1;
#if DDUP_HAS_C_ATOMICS
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
#else
    q->head = 0;
    q->tail = 0;
    if (pal_mutex_init(&q->mu) != 0) {
        free(q->buf);
        q->buf = NULL;
        return -1;
    }
#endif
    return 0;
}

static inline void mt_spsc_destroy(mt_spsc *q)
{
#if !DDUP_HAS_C_ATOMICS
    pal_mutex_destroy(&q->mu);
#endif
    free(q->buf);
    q->buf = NULL;
}

/* Producer side. Returns 1 when the queue was empty (consumer may be
 * asleep: kick it), 0 when it was non-empty, -1 when full. */
static inline int mt_spsc_push(mt_spsc *q, void *ptr)
{
#if DDUP_HAS_C_ATOMICS
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    size_t next = (tail + 1) & q->mask;
    int was_empty;
    if (next == head)
        return -1; /* full */
    was_empty = tail == head;
    q->buf[tail] = ptr;
    atomic_store_explicit(&q->tail, next, memory_order_release);
    return was_empty ? 1 : 0;
#else
    int ret;
    size_t next;
    pal_mutex_lock(&q->mu);
    next = (q->tail + 1) & q->mask;
    if (next == q->head) {
        pal_mutex_unlock(&q->mu);
        return -1;
    }
    ret = q->tail == q->head ? 1 : 0;
    q->buf[q->tail] = ptr;
    q->tail = next;
    pal_mutex_unlock(&q->mu);
    return ret;
#endif
}

/* Consumer side. Returns the next pointer, or NULL when empty. */
static inline void *mt_spsc_pop(mt_spsc *q)
{
#if DDUP_HAS_C_ATOMICS
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    void *ptr;
    if (head == tail)
        return NULL;
    ptr = q->buf[head];
    atomic_store_explicit(&q->head, (head + 1) & q->mask,
                          memory_order_release);
    return ptr;
#else
    void *ptr;
    pal_mutex_lock(&q->mu);
    if (q->head == q->tail) {
        pal_mutex_unlock(&q->mu);
        return NULL;
    }
    ptr = q->buf[q->head];
    q->head = (q->head + 1) & q->mask;
    pal_mutex_unlock(&q->mu);
    return ptr;
#endif
}

#endif /* DDUP_MT_SPSC_H */
