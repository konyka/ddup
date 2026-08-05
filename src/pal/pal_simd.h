/* pal_simd.h - optional SIMD helpers with scalar fallback.
 *
 * Only used on hot paths. Each helper must behave identically to the scalar
 * reference implementation; SIMD is a transparent acceleration.
 */
#ifndef DDUP_PAL_SIMD_H
#define DDUP_PAL_SIMD_H

#include <stddef.h>
#include <string.h>

#if defined(__SSE2__) || (defined(_M_X64) && !defined(_M_ARM64) && \
                          !defined(_M_ARM64EC))
#  define DDUP_SIMD_SSE2 1
#else
#  define DDUP_SIMD_SSE2 0
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || \
    (defined(_M_ARM64) && !defined(_M_ARM64EC))
#  define DDUP_SIMD_NEON 1
#else
#  define DDUP_SIMD_NEON 0
#endif

#if DDUP_SIMD_SSE2
#  include <emmintrin.h>
#endif
#if DDUP_SIMD_NEON
#  include <arm_neon.h>
#endif
#if defined(_MSC_VER) && (DDUP_SIMD_SSE2 || DDUP_SIMD_NEON)
#  include <intrin.h>
#endif

static inline unsigned ddup_ctz32(unsigned v)
{
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctz(v);
#elif defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward(&idx, v);
    return (unsigned)idx;
#else
    unsigned n = 0;
    while ((v & 1U) == 0U) {
        v >>= 1;
        n++;
    }
    return n;
#endif
}

/* Find the next CRLF sequence in [p, end). Returns pointer to the '\r' or
 * NULL when no complete CRLF exists before end. */
static inline const char *ddup_find_crlf(const char *p, const char *end)
{
#if DDUP_SIMD_SSE2
    while (p + 16 <= end) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)p);
        __m128i cr = _mm_cmpeq_epi8(chunk, _mm_set1_epi8('\r'));
        unsigned mask = (unsigned)_mm_movemask_epi8(cr);
        while (mask != 0U) {
            unsigned idx = ddup_ctz32(mask);
            const char *candidate = p + idx;
            if (candidate + 1 >= end)
                return NULL;
            if (candidate[1] == '\n')
                return candidate;
            mask &= mask - 1U;
        }
        p += 16;
    }
#elif DDUP_SIMD_NEON
    while (p + 16 <= end) {
        uint8x16_t chunk = vld1q_u8((const uint8_t *)p);
        uint8x16_t cr = vceqq_u8(chunk, vdupq_n_u8((uint8_t)'\r'));
        /* movemask equivalent: keep the low bit of each byte in a u32 pair */
        uint16x8_t mask16 = vreinterpretq_u16_u8(vshrq_n_u8(cr, 7));
        uint32x4_t mask32 = vreinterpretq_u32_u16(mask16);
        uint64x2_t mask64 = vreinterpretq_u64_u32(mask32);
        uint64_t lo = vgetq_lane_u64(mask64, 0);
        uint64_t hi = vgetq_lane_u64(mask64, 1);
        int i;
        for (i = 0; i < 8; i++) {
            if (lo & (1ULL << (i * 8))) {
                const char *candidate = p + i;
                if (candidate + 1 >= end)
                    return NULL;
                if (candidate[1] == '\n')
                    return candidate;
            }
        }
        for (i = 0; i < 8; i++) {
            if (hi & (1ULL << (i * 8))) {
                const char *candidate = p + 8 + i;
                if (candidate + 1 >= end)
                    return NULL;
                if (candidate[1] == '\n')
                    return candidate;
            }
        }
        p += 16;
    }
#endif
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

#endif /* DDUP_PAL_SIMD_H */
