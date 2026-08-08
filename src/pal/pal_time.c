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

uint64_t pal_wall_ms(void)
{
    FILETIME ft;
    uint64_t ticks;
    GetSystemTimeAsFileTime(&ft);
    /* 100-ns ticks since 1601-01-01 -> ms since 1970-01-01. */
    ticks = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
    return ticks / 10000ULL - 11644473600000ULL;
}

void pal_sleep_ms(uint64_t ms)
{
    Sleep((DWORD)ms);
}

#else /* POSIX */

#include <errno.h>
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

uint64_t pal_wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void pal_sleep_ms(uint64_t ms)
{
    struct timespec ts;
    struct timespec remaining;
    ts.tv_sec = (time_t)(ms / 1000ULL);
    ts.tv_nsec = (long)((ms % 1000ULL) * 1000000ULL);
    while (nanosleep(&ts, &remaining) != 0 && errno == EINTR)
        ts = remaining;
}

#endif
