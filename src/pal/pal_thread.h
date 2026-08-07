/* pal_thread.h - cross-platform thread, mutex and condition variable.
 *
 * Windows: _beginthreadex + CRITICAL_SECTION + CONDITION_VARIABLE.
 * POSIX: pthreads. All platform details live in pal_thread.c.
 *
 * Opaque-handle style: each object owns a heap-allocated impl freed by its
 * destroy/join call. Init returns -1 on allocation/creation failure.
 */
#ifndef DDUP_PAL_THREAD_H
#define DDUP_PAL_THREAD_H

typedef struct pal_thread {
    void *impl;
} pal_thread;

typedef struct pal_mutex {
    void *impl;
} pal_mutex;

typedef struct pal_cond {
    void *impl;
} pal_cond;

typedef void *(*pal_thread_fn)(void *arg);

/* Start a thread running fn(arg). Returns 0 on success. */
int pal_thread_create(pal_thread *t, pal_thread_fn fn, void *arg);
/* Wait for completion; optionally receive fn's return value. */
int pal_thread_join(pal_thread *t, void **ret);

int pal_mutex_init(pal_mutex *m);
void pal_mutex_lock(pal_mutex *m);
void pal_mutex_unlock(pal_mutex *m);
void pal_mutex_destroy(pal_mutex *m);

int pal_cond_init(pal_cond *c);
/* Caller must hold m; releases it while waiting and reacquires before
 * returning. */
void pal_cond_wait(pal_cond *c, pal_mutex *m);
void pal_cond_signal(pal_cond *c);
void pal_cond_broadcast(pal_cond *c);
void pal_cond_destroy(pal_cond *c);

/* Offer the rest of the time slice to a runnable peer (sched_yield /
 * SwitchToThread): cheaper than a 1ms sleep for backpressure handoff. */
void pal_thread_yield(void);

#endif /* DDUP_PAL_THREAD_H */
