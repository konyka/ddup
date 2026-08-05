/* test_pal_thread.c - tests for the pal thread/mutex/cond abstraction. */
#include "test.h"

#include <stddef.h>

#include "pal/pal_thread.h"

#define NTHREADS 4
#define NITER 25000

typedef struct counter_ctx {
    pal_mutex mu;
    long value;
} counter_ctx;

static void *counter_worker(void *arg)
{
    counter_ctx *ctx = (counter_ctx *)arg;
    int i;
    for (i = 0; i < NITER; i++) {
        pal_mutex_lock(&ctx->mu);
        ctx->value++;
        pal_mutex_unlock(&ctx->mu);
    }
    return NULL;
}

static void test_mutex_counter(void)
{
    counter_ctx ctx;
    pal_thread th[NTHREADS];
    int i;

    ctx.value = 0;
    DD_CHECK_EQ_INT(0, pal_mutex_init(&ctx.mu));

    for (i = 0; i < NTHREADS; i++)
        DD_CHECK_EQ_INT(0, pal_thread_create(&th[i], counter_worker, &ctx));
    for (i = 0; i < NTHREADS; i++)
        DD_CHECK_EQ_INT(0, pal_thread_join(&th[i], NULL));

    DD_CHECK_EQ_INT((long long)(NTHREADS * NITER), (long long)ctx.value);
    pal_mutex_destroy(&ctx.mu);
}

typedef struct cond_ctx {
    pal_mutex mu;
    pal_cond cv;
    int ready;
} cond_ctx;

static void *cond_waiter(void *arg)
{
    cond_ctx *ctx = (cond_ctx *)arg;
    pal_mutex_lock(&ctx->mu);
    while (!ctx->ready)
        pal_cond_wait(&ctx->cv, &ctx->mu);
    pal_mutex_unlock(&ctx->mu);
    return (void *)(ptrdiff_t)42;
}

static void test_cond_signal_and_return(void)
{
    cond_ctx ctx;
    pal_thread th;
    void *ret = NULL;

    ctx.ready = 0;
    DD_CHECK_EQ_INT(0, pal_mutex_init(&ctx.mu));
    DD_CHECK_EQ_INT(0, pal_cond_init(&ctx.cv));
    DD_CHECK_EQ_INT(0, pal_thread_create(&th, cond_waiter, &ctx));

    pal_mutex_lock(&ctx.mu);
    ctx.ready = 1;
    pal_cond_signal(&ctx.cv);
    pal_mutex_unlock(&ctx.mu);

    DD_CHECK_EQ_INT(0, pal_thread_join(&th, &ret));
    DD_CHECK_EQ_INT(42, (long long)(ptrdiff_t)ret);

    pal_cond_destroy(&ctx.cv);
    pal_mutex_destroy(&ctx.mu);
}

int main(void)
{
    DD_RUN(test_mutex_counter);
    DD_RUN(test_cond_signal_and_return);
    return DD_TEST_SUMMARY();
}
