/* main.c - ddup-server entry point. */
#include <stdio.h>

#include "pal/pal_platform.h"
#include "pal/pal_time.h"

int main(void)
{
    printf("ddup-server %d.%d.%d (platform: %s, C standard: C%d)\n",
           DDUP_VERSION_MAJOR, DDUP_VERSION_MINOR, DDUP_VERSION_PATCH,
           DDUP_OS_NAME, DDUP_C_STD);
    printf("monotonic clock: %llu ms\n", (unsigned long long)pal_now_ms());
    printf("ddup is under construction: network server arrives in Phase 3.\n");
    return 0;
}
