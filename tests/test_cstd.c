/* test_cstd.c - tests for the C-standard capability wrapper layer. */
#include "test.h"
#include "pal/pal_cstd.h"

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

int main(void)
{
    DD_RUN(test_capability_macros_are_boolean);
    return DD_TEST_SUMMARY();
}
