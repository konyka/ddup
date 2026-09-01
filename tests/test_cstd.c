/* test_cstd.c - tests for the C-standard capability wrapper layer. */
#include "test.h"

#include "pal/pal_cstd.h"
#include "pal/pal_platform.h"
#include "pal/pal_thread.h"
#include <stdlib.h>

#ifndef DDUP_HAS_WYHASH
#error "pal_platform.h must define DDUP_HAS_WYHASH"
#endif

static void test_capability_macros_are_boolean(void)
{
    DD_CHECK(DDUP_HAS_C_ATOMICS == 0 || DDUP_HAS_C_ATOMICS == 1);
    DD_CHECK(DDUP_HAS_C_THREADS == 0 || DDUP_HAS_C_THREADS == 1);
    DD_CHECK(DDUP_HAS_C_ALIGNAS == 0 || DDUP_HAS_C_ALIGNAS == 1);
    DD_CHECK(DDUP_HAS_C_STATIC_ASSERT == 0 || DDUP_HAS_C_STATIC_ASSERT == 1);
    DD_CHECK(DDUP_HAS_C_NORETURN == 0 || DDUP_HAS_C_NORETURN == 1);
    DD_CHECK(DDUP_HAS_C_THREAD_LOCAL == 0 || DDUP_HAS_C_THREAD_LOCAL == 1);
    DD_CHECK(DDUP_HAS_C_TYPEOF == 0 || DDUP_HAS_C_TYPEOF == 1);
    DD_CHECK(DDUP_HAS_C_CONSTEXPR == 0 || DDUP_HAS_C_CONSTEXPR == 1);
    DD_CHECK(DDUP_HAS_C_STDCKDINT == 0 || DDUP_HAS_C_STDCKDINT == 1);
    DD_CHECK(DDUP_HAS_C_BITINT == 0 || DDUP_HAS_C_BITINT == 1);
    DD_CHECK(DDUP_HAS_WYHASH == 0 || DDUP_HAS_WYHASH == 1);
}

/* Compile-time assertions at file scope: some compilers warn about an unused
 * local typedef created by the C99 fallback, so keep them at file scope. */
ddup_static_assert(1 == 1, "trivially true");
ddup_static_assert(sizeof(int) == 4, "int size assumption");

static void test_static_assert_compiles(void)
{
    /* Static assertions are already verified at compile time above. */
}

ddup_alignas(64) static char aligned_buf[128];

static void test_alignas_works(void)
{
    DD_CHECK(((uintptr_t)(void *)aligned_buf & 63) == 0);
}

DDUP_NORETURN static void test_noreturn_fn(void)
{
    abort(); /* noreturn; never actually invoked by tests */
}

static void test_noreturn_compiles(void)
{
    /* Reference the function so the attribute is materialized; do not call it. */
    (void)test_noreturn_fn;
}

static ddup_thread_local int tls_counter = 0;

static void test_thread_local_basic(void)
{
    tls_counter = 0;
    tls_counter++;
    DD_CHECK_EQ_INT(1, tls_counter);
    tls_counter++;
    DD_CHECK_EQ_INT(2, tls_counter);
}

static void test_typeof(void)
{
#if DDUP_HAS_C_TYPEOF
    int x = 42;
    ddup_typeof(x) y = x;
    DD_CHECK_EQ_INT(42, y);
#endif
}

static void test_constexpr(void)
{
#if DDUP_HAS_C_CONSTEXPR
    ddup_constexpr int n = 4;
    char arr[n]; /* constexpr makes n a constant expression (C23) */
    arr[0] = 'a';
    arr[1] = '\0';
    DD_CHECK(arr[0] == 'a');
#else
    /* const fallback is not a constant expression in C (no VLA on MSVC) */
    ddup_constexpr int n = 4;
    DD_CHECK_EQ_INT(4, n);
#endif
}

static void test_checked_arithmetic(void)
{
    int r;

    DD_CHECK(!ddup_add_overflow(10, 20, &r));
    DD_CHECK_EQ_INT(30, r);

    DD_CHECK(ddup_add_overflow(INT_MAX, 1, &r));
    DD_CHECK(ddup_add_overflow(INT_MIN, -1, &r));

    DD_CHECK(!ddup_sub_overflow(20, 10, &r));
    DD_CHECK_EQ_INT(10, r);
    DD_CHECK(ddup_sub_overflow(INT_MIN, 1, &r));
    DD_CHECK(ddup_sub_overflow(INT_MAX, -1, &r));

    DD_CHECK(!ddup_mul_overflow(6, 7, &r));
    DD_CHECK_EQ_INT(42, r);
    DD_CHECK(ddup_mul_overflow(INT_MAX, 2, &r));
    DD_CHECK(ddup_mul_overflow(INT_MIN, 2, &r));
}

static ddup_atomic_int atomic_counter;

static void test_atomic_basic(void)
{
    ddup_atomic_init(&atomic_counter, 10);
    DD_CHECK_EQ_INT(10, ddup_atomic_load(&atomic_counter, ddup_memory_order_relaxed));

    ddup_atomic_store(&atomic_counter, 20, ddup_memory_order_relaxed);
    DD_CHECK_EQ_INT(20, ddup_atomic_load(&atomic_counter, ddup_memory_order_relaxed));

    int prev = ddup_atomic_fetch_add(&atomic_counter, 5, ddup_memory_order_relaxed);
    DD_CHECK_EQ_INT(20, prev);
    DD_CHECK_EQ_INT(25, ddup_atomic_load(&atomic_counter, ddup_memory_order_relaxed));

    prev = ddup_atomic_fetch_sub(&atomic_counter, 7, ddup_memory_order_relaxed);
    DD_CHECK_EQ_INT(25, prev);
    DD_CHECK_EQ_INT(18, ddup_atomic_load(&atomic_counter, ddup_memory_order_relaxed));
}

typedef struct atomic_worker_ctx {
    ddup_atomic_int *value;
    int iterations;
} atomic_worker_ctx;

static void *atomic_worker(void *arg)
{
    atomic_worker_ctx *ctx = (atomic_worker_ctx *)arg;
    int i;
    for (i = 0; i < ctx->iterations; i++)
        (void)ddup_atomic_fetch_add(ctx->value, 1,
                                    ddup_memory_order_relaxed);
    return NULL;
}

static void test_atomic_concurrent(void)
{
    enum { THREADS = 4, ITERATIONS = 250000 };
    pal_thread threads[THREADS];
    atomic_worker_ctx ctx;
    int i;

    ddup_atomic_init(&atomic_counter, 0);
    ctx.value = &atomic_counter;
    ctx.iterations = ITERATIONS;
    for (i = 0; i < THREADS; i++) {
        DD_CHECK_EQ_INT(0, pal_thread_create(&threads[i], atomic_worker,
                                             &ctx));
    }
    for (i = 0; i < THREADS; i++)
        DD_CHECK_EQ_INT(0, pal_thread_join(&threads[i], NULL));
    DD_CHECK_EQ_INT(THREADS * ITERATIONS,
                    ddup_atomic_load(&atomic_counter,
                                     ddup_memory_order_relaxed));
}

int main(void)
{
    DD_RUN(test_capability_macros_are_boolean);
    DD_RUN(test_static_assert_compiles);
    DD_RUN(test_alignas_works);
    DD_RUN(test_noreturn_compiles);
    DD_RUN(test_thread_local_basic);
    DD_RUN(test_typeof);
    DD_RUN(test_constexpr);
    DD_RUN(test_checked_arithmetic);
    DD_RUN(test_atomic_basic);
    DD_RUN(test_atomic_concurrent);
    return DD_TEST_SUMMARY();
}
