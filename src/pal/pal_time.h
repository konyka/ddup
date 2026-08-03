/* pal_time.h - monotonic clock, cross-platform. */
#ifndef DDUP_PAL_TIME_H
#define DDUP_PAL_TIME_H

#include <stdint.h>

/* Milliseconds from a monotonic clock (not affected by wall-clock changes).
 * Epoch is unspecified; only differences are meaningful. */
uint64_t pal_now_ms(void);

/* Microseconds from the same monotonic clock. */
uint64_t pal_now_us(void);

/* Milliseconds since the Unix epoch (wall clock, may jump on NTP changes). */
uint64_t pal_wall_ms(void);

#endif /* DDUP_PAL_TIME_H */
