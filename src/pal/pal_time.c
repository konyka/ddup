/* pal_time.c - monotonic clock implementation. */
#include "pal/pal_time.h"
#include "pal/pal_platform.h"

#if DDUP_OS_WINDOWS

#include <windows.h>

static uint64_t pal_qpc_freq(void)
{
    static LARGE_INTEGER freq;
    static int initialized = 0;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    return (uint64_t)freq.QuadPart;
}

uint64_t pal_now_us(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    /* Multiply first to keep precision; QPC freq is 10 MHz or less on
     * modern Windows, so uint64 overflow is not a practical concern. */
    return (uint64_t)counter.QuadPart * 1000000ULL / pal_qpc_freq();
}

uint64_t pal_now_ms(void)
{
    return pal_now_us() / 1000ULL;
}

#else /* POSIX */

#include <time.h>

uint64_t pal_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

uint64_t pal_now_ms(void)
{
    return pal_now_us() / 1000ULL;
}

#endif
