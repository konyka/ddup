/* test_pal.c - sanity tests for the platform abstraction layer. */
#include "test.h"

#include <string.h>

#include "pal/pal_platform.h"
#include "pal/pal_simd.h"
#include "pal/pal_time.h"

#if DDUP_OS_LINUX
#include <signal.h>
#include <sys/time.h>
#endif

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

#if DDUP_OS_LINUX
static volatile sig_atomic_t alarm_seen;

static void alarm_handler(int signo)
{
    if (signo == SIGALRM)
        alarm_seen = 1;
}

static void test_sleep_completes_after_signal(void)
{
    struct sigaction sa;
    struct sigaction old_sa;
    struct itimerval timer;
    struct itimerval old_timer;
    uint64_t start;
    uint64_t elapsed;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_handler;
    if (sigemptyset(&sa.sa_mask) != 0 ||
        sigaction(SIGALRM, &sa, &old_sa) != 0) {
        DD_CHECK(0);
        return;
    }
    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_usec = 10000;
    alarm_seen = 0;
    if (setitimer(ITIMER_REAL, &timer, &old_timer) != 0) {
        DD_CHECK(0);
        DD_CHECK_EQ_INT(0, sigaction(SIGALRM, &old_sa, NULL));
        return;
    }

    start = pal_now_ms();
    pal_sleep_ms(100);
    elapsed = pal_now_ms() - start;
    DD_CHECK(alarm_seen != 0);
    DD_CHECK(elapsed >= 80);

    DD_CHECK_EQ_INT(0, setitimer(ITIMER_REAL, &old_timer, NULL));
    DD_CHECK_EQ_INT(0, sigaction(SIGALRM, &old_sa, NULL));
}
#endif

static const char *find_crlf_ref(const char *p, const char *end)
{
    while (p < end) {
        const char *cr = (const char *)memchr(p, '\r', (size_t)(end - p));
        if (cr == NULL || cr + 1 >= end)
            return NULL;
        if (cr[1] == '\n')
            return cr;
        p = cr + 1;
    }
    return NULL;
}

static void test_ddup_find_crlf_basic(void)
{
    const char buf[] = "abc\r\nxyz";
    const char *r = ddup_find_crlf(buf, buf + sizeof(buf) - 1);
    DD_CHECK(r == buf + 3);
    DD_CHECK(ddup_find_crlf(buf, buf + 3) == NULL);
    DD_CHECK(ddup_find_crlf(buf + 4, buf + sizeof(buf) - 1) == NULL);
}

static void test_ddup_find_crlf_cr_not_lf(void)
{
    const char buf[] = "a\rb\r\nc";
    const char *r = ddup_find_crlf(buf, buf + sizeof(buf) - 1);
    DD_CHECK(r == buf + 3);
}

static void test_ddup_find_crlf_edge(void)
{
    const char a[] = "x\r";
    const char b[] = "\r\n";
    DD_CHECK(ddup_find_crlf(a, a + sizeof(a) - 1) == NULL);
    DD_CHECK(ddup_find_crlf(b, b + sizeof(b) - 1) == b);
}

static void test_ddup_find_crlf_random(void)
{
    char buf[4096];
    unsigned long long rng = 0x9E3779B97F4A7C15ULL;
    size_t i;
    const char *end = buf + sizeof(buf);
    const char *r;
    const char *ref;

    for (i = 0; i < sizeof(buf); i++) {
        unsigned char c;
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        c = (unsigned char)(rng >> 32);
        if (c == '\r' || c == '\n')
            c = 'x';
        buf[i] = (char)c;
    }
    for (i = 0; i + 1 < sizeof(buf); i += 97) {
        buf[i] = '\r';
        buf[i + 1] = '\n';
    }

    r = ddup_find_crlf(buf, end);
    ref = find_crlf_ref(buf, end);
    DD_CHECK(r == ref);
    if (r != NULL)
        DD_CHECK(r[1] == '\n');
}

int main(void)
{
    DD_RUN(test_exactly_one_os);
    DD_RUN(test_c_std_detected);
    DD_RUN(test_monotonic_non_decreasing);
    DD_RUN(test_us_at_least_ms_resolution);
    DD_RUN(test_wall_clock_sane);
#if DDUP_OS_LINUX
    DD_RUN(test_sleep_completes_after_signal);
#endif
    DD_RUN(test_ddup_find_crlf_basic);
    DD_RUN(test_ddup_find_crlf_cr_not_lf);
    DD_RUN(test_ddup_find_crlf_edge);
    DD_RUN(test_ddup_find_crlf_random);
    return DD_TEST_SUMMARY();
}
