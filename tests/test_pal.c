/* test_pal.c - sanity tests for the platform abstraction layer. */
#include "test.h"

#include "pal/pal_platform.h"
#include "pal/pal_time.h"

static void test_exactly_one_os(void)
{
    int count = DDUP_OS_WINDOWS + DDUP_OS_LINUX + DDUP_OS_MACOS +
                DDUP_OS_FREEBSD + DDUP_OS_UNIX;
    DD_CHECK_EQ_INT(1, count);
}

static void test_c_std_detected(void)
{
    DD_CHECK(DDUP_C_STD == 99 || DDUP_C_STD == 11 ||
             DDUP_C_STD == 17 || DDUP_C_STD == 23);
}

static void test_monotonic_non_decreasing(void)
{
    uint64_t prev = pal_now_ms();
    for (int i = 0; i < 1000; i++) {
        uint64_t now = pal_now_ms();
        DD_CHECK(now >= prev);
        prev = now;
    }
}

static void test_us_at_least_ms_resolution(void)
{
    uint64_t ms = pal_now_ms();
    uint64_t us = pal_now_us();
    /* us reading must be consistent with the earlier ms reading. */
    DD_CHECK(us >= ms * 1000ULL);
    DD_CHECK(us < (ms + 1000ULL) * 1000ULL); /* generous 1s slack */
}

static void test_wall_clock_sane(void)
{
    /* Unix epoch ms: must be between 2020-01-01 and 2100-01-01. */
    const uint64_t epoch_2020 = 1577836800000ULL;
    const uint64_t epoch_2100 = 4102444800000ULL;
    uint64_t prev = pal_wall_ms();
    DD_CHECK(prev >= epoch_2020);
    DD_CHECK(prev < epoch_2100);
    for (int i = 0; i < 100; i++) {
        uint64_t now = pal_wall_ms();
        DD_CHECK(now >= prev);
        prev = now;
    }
}

int main(void)
{
    DD_RUN(test_exactly_one_os);
    DD_RUN(test_c_std_detected);
    DD_RUN(test_monotonic_non_decreasing);
    DD_RUN(test_us_at_least_ms_resolution);
    DD_RUN(test_wall_clock_sane);
    return DD_TEST_SUMMARY();
}
