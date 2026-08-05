/* test_cstd.c - tests for the C-standard capability wrapper layer. */
#include "test.h"

#include "pal/pal_cstd.h"
#include <stdlib.h>

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
}

static void test_static_assert_compiles(void)
{
    ddup_static_assert(1 == 1, "trivially true");
    ddup_static_assert(sizeof(int) == 4, "int size assumption");
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
    ddup_constexpr int n = 4;
    char arr[n];
    arr[0] = 'a';
    arr[1] = '\0';
    DD_CHECK(arr[0] == 'a');
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
    return DD_TEST_SUMMARY();
}
