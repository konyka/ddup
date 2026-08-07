/* pal_thread.c - cross-platform threads; see pal_thread.h.
 *
 * Platform ifdefs are allowed only inside src/pal/.
 */
#include "pal/pal_thread.h"

#include <stdlib.h>

#include "pal/pal_platform.h"

#if DDUP_OS_WINDOWS

#include <process.h>
#include <windows.h>

typedef struct win_thread {
    pal_thread_fn fn;
    void *arg;
    void *ret;
    HANDLE h;
} win_thread;

static unsigned __stdcall win_thread_main(void *arg)
{
    win_thread *wt = (win_thread *)arg;
    wt->ret = wt->fn(wt->arg);
    return 0;
}

int pal_thread_create(pal_thread *t, pal_thread_fn fn, void *arg)
{
    win_thread *wt = (win_thread *)calloc(1, sizeof(*wt));
    uintptr_t h;
    if (wt == NULL)
        return -1;
    wt->fn = fn;
    wt->arg = arg;
    h = _beginthreadex(NULL, 0, win_thread_main, wt, 0, NULL);
    if (h == 0) {
        free(wt);
        return -1;
    }
    wt->h = (HANDLE)h;
    t->impl = wt;
    return 0;
}

int pal_thread_join(pal_thread *t, void **ret)
{
    win_thread *wt = (win_thread *)t->impl;
    if (wt == NULL)
        return -1;
    if (WaitForSingleObject(wt->h, INFINITE) != WAIT_OBJECT_0)
        return -1;
    if (ret != NULL)
        *ret = wt->ret;
    CloseHandle(wt->h);
    free(wt);
    t->impl = NULL;
    return 0;
}

int pal_mutex_init(pal_mutex *m)
{
    CRITICAL_SECTION *cs =
        (CRITICAL_SECTION *)malloc(sizeof(CRITICAL_SECTION));
    if (cs == NULL)
        return -1;
    InitializeCriticalSection(cs);
    m->impl = cs;
    return 0;
}

void pal_mutex_lock(pal_mutex *m)
{
    EnterCriticalSection((CRITICAL_SECTION *)m->impl);
}

void pal_mutex_unlock(pal_mutex *m)
{
    LeaveCriticalSection((CRITICAL_SECTION *)m->impl);
}

void pal_mutex_destroy(pal_mutex *m)
{
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)m->impl;
    if (cs != NULL) {
        DeleteCriticalSection(cs);
        free(cs);
        m->impl = NULL;
    }
}

int pal_cond_init(pal_cond *c)
{
    CONDITION_VARIABLE *cv =
        (CONDITION_VARIABLE *)malloc(sizeof(CONDITION_VARIABLE));
    if (cv == NULL)
        return -1;
    InitializeConditionVariable(cv);
    c->impl = cv;
    return 0;
}

void pal_cond_wait(pal_cond *c, pal_mutex *m)
{
    SleepConditionVariableCS((CONDITION_VARIABLE *)c->impl,
                             (CRITICAL_SECTION *)m->impl, INFINITE);
}

void pal_cond_signal(pal_cond *c)
{
    WakeConditionVariable((CONDITION_VARIABLE *)c->impl);
}

void pal_cond_broadcast(pal_cond *c)
{
    WakeAllConditionVariable((CONDITION_VARIABLE *)c->impl);
}

void pal_cond_destroy(pal_cond *c)
{
    free(c->impl);
    c->impl = NULL;
}

void pal_thread_yield(void)
{
    SwitchToThread();
}

#else /* POSIX */

#include <pthread.h>
#include <sched.h>

typedef struct posix_thread {
    pthread_t tid;
} posix_thread;

int pal_thread_create(pal_thread *t, pal_thread_fn fn, void *arg)
{
    posix_thread *pt = (posix_thread *)malloc(sizeof(*pt));
    if (pt == NULL)
        return -1;
    if (pthread_create(&pt->tid, NULL, fn, arg) != 0) {
        free(pt);
        return -1;
    }
    t->impl = pt;
    return 0;
}

int pal_thread_join(pal_thread *t, void **ret)
{
    posix_thread *pt = (posix_thread *)t->impl;
    if (pt == NULL)
        return -1;
    if (pthread_join(pt->tid, ret) != 0)
        return -1;
    free(pt);
    t->impl = NULL;
    return 0;
}

int pal_mutex_init(pal_mutex *m)
{
    pthread_mutex_t *mu =
        (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (mu == NULL)
        return -1;
    if (pthread_mutex_init(mu, NULL) != 0) {
        free(mu);
        return -1;
    }
    m->impl = mu;
    return 0;
}

void pal_mutex_lock(pal_mutex *m)
{
    (void)pthread_mutex_lock((pthread_mutex_t *)m->impl);
}

void pal_mutex_unlock(pal_mutex *m)
{
    (void)pthread_mutex_unlock((pthread_mutex_t *)m->impl);
}

void pal_mutex_destroy(pal_mutex *m)
{
    pthread_mutex_t *mu = (pthread_mutex_t *)m->impl;
    if (mu != NULL) {
        (void)pthread_mutex_destroy(mu);
        free(mu);
        m->impl = NULL;
    }
}

int pal_cond_init(pal_cond *c)
{
    pthread_cond_t *cv = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
    if (cv == NULL)
        return -1;
    if (pthread_cond_init(cv, NULL) != 0) {
        free(cv);
        return -1;
    }
    c->impl = cv;
    return 0;
}

void pal_cond_wait(pal_cond *c, pal_mutex *m)
{
    (void)pthread_cond_wait((pthread_cond_t *)c->impl,
                            (pthread_mutex_t *)m->impl);
}

void pal_cond_signal(pal_cond *c)
{
    (void)pthread_cond_signal((pthread_cond_t *)c->impl);
}

void pal_cond_broadcast(pal_cond *c)
{
    (void)pthread_cond_broadcast((pthread_cond_t *)c->impl);
}

void pal_cond_destroy(pal_cond *c)
{
    pthread_cond_t *cv = (pthread_cond_t *)c->impl;
    if (cv != NULL) {
        (void)pthread_cond_destroy(cv);
        free(cv);
        c->impl = NULL;
    }
}

void pal_thread_yield(void)
{
    (void)sched_yield();
}

#endif
