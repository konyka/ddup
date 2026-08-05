/* pal_cstd.h - C-standard capability wrappers.
 *
 * The build system (cmake/DetectCStandard.cmake) probes the compiler and exposes
 * DDUP_HAS_C_* macros. This header turns those capabilities into portable,
 * zero-overhead wrappers with C99 fallback paths.
 */
#ifndef DDUP_PAL_CSTD_H
#define DDUP_PAL_CSTD_H

#include "pal/pal_platform.h"
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Static assertion                                                             */
/* -------------------------------------------------------------------------- */
#if DDUP_HAS_C_STATIC_ASSERT
#  define ddup_static_assert(expr, msg) _Static_assert(expr, msg)
#else
#  define DDUP_GLUE(a, b) a ## b
#  define ddup_static_assert(expr, msg) \
     typedef char DDUP_GLUE(_ddup_static_assert_, __LINE__)[(expr) ? 1 : -1]
#endif

/* -------------------------------------------------------------------------- */
/* Alignment                                                                    */
/* -------------------------------------------------------------------------- */
#if DDUP_HAS_C_ALIGNAS
#  define ddup_alignas(n) _Alignas(n)
#elif defined(__GNUC__) || defined(__clang__)
#  define ddup_alignas(n) __attribute__((__aligned__(n)))
#elif defined(_MSC_VER)
#  define ddup_alignas(n) __declspec(align(n))
#else
#  define ddup_alignas(n) /* alignment not supported */
#endif

/* -------------------------------------------------------------------------- */
/* Noreturn attribute                                                           */
/* -------------------------------------------------------------------------- */
#if DDUP_HAS_C_NORETURN
#  define DDUP_NORETURN _Noreturn
#elif defined(__GNUC__) || defined(__clang__)
#  define DDUP_NORETURN __attribute__((__noreturn__))
#elif defined(_MSC_VER)
#  define DDUP_NORETURN __declspec(noreturn)
#else
#  define DDUP_NORETURN
#endif

/* -------------------------------------------------------------------------- */
/* Thread-local storage                                                         */
/* -------------------------------------------------------------------------- */
#if DDUP_HAS_C_THREAD_LOCAL
#  define ddup_thread_local _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#  define ddup_thread_local __thread
#elif defined(_MSC_VER)
#  define ddup_thread_local __declspec(thread)
#else
#  define ddup_thread_local /* thread-local storage not supported */
#endif

/* -------------------------------------------------------------------------- */
/* Typeof                                                                       */
/* -------------------------------------------------------------------------- */
#if DDUP_HAS_C_TYPEOF
#  define ddup_typeof(expr) typeof(expr)
#elif defined(__GNUC__) || defined(__clang__)
#  define ddup_typeof(expr) __typeof__(expr)
#else
/* No typeof available. Callers that need it should gate with DDUP_HAS_C_TYPEOF. */
#  define ddup_typeof(expr) int
#endif

/* -------------------------------------------------------------------------- */
/* Constexpr (C23) / const fallback                                             */
/* -------------------------------------------------------------------------- */
#if DDUP_HAS_C_CONSTEXPR
#  define ddup_constexpr constexpr
#else
#  define ddup_constexpr const
#endif

/* -------------------------------------------------------------------------- */
/* Checked integer arithmetic                                                   */
/* -------------------------------------------------------------------------- */
#include <limits.h>
#include <stdbool.h>

#if DDUP_HAS_C_STDCKDINT
#  include <stdckdint.h>
#  define ddup_add_overflow(a, b, r) ckd_add(r, a, b)
#  define ddup_sub_overflow(a, b, r) ckd_sub(r, a, b)
#  define ddup_mul_overflow(a, b, r) ckd_mul(r, a, b)
#elif defined(__has_builtin)
#  if __has_builtin(__builtin_add_overflow)
#    define ddup_add_overflow(a, b, r) __builtin_add_overflow(a, b, r)
#    define ddup_sub_overflow(a, b, r) __builtin_sub_overflow(a, b, r)
#    define ddup_mul_overflow(a, b, r) __builtin_mul_overflow(a, b, r)
#  endif
#elif defined(__GNUC__)
#  define ddup_add_overflow(a, b, r) __builtin_add_overflow(a, b, r)
#  define ddup_sub_overflow(a, b, r) __builtin_sub_overflow(a, b, r)
#  define ddup_mul_overflow(a, b, r) __builtin_mul_overflow(a, b, r)
#endif

#ifndef ddup_add_overflow
static inline bool ddup_add_overflow_int(int a, int b, int *r)
{
    long long v = (long long)a + (long long)b;
    if (v < INT_MIN || v > INT_MAX) return true;
    *r = (int)v;
    return false;
}
static inline bool ddup_sub_overflow_int(int a, int b, int *r)
{
    long long v = (long long)a - (long long)b;
    if (v < INT_MIN || v > INT_MAX) return true;
    *r = (int)v;
    return false;
}
static inline bool ddup_mul_overflow_int(int a, int b, int *r)
{
    long long v = (long long)a * (long long)b;
    if (v < INT_MIN || v > INT_MAX) return true;
    *r = (int)v;
    return false;
}
#  define ddup_add_overflow(a, b, r) ddup_add_overflow_int((a), (b), (r))
#  define ddup_sub_overflow(a, b, r) ddup_sub_overflow_int((a), (b), (r))
#  define ddup_mul_overflow(a, b, r) ddup_mul_overflow_int((a), (b), (r))
#endif

/* -------------------------------------------------------------------------- */
/* Atomic operations                                                            */
/* -------------------------------------------------------------------------- */
#if DDUP_HAS_C_ATOMICS
#  include <stdatomic.h>
typedef atomic_int ddup_atomic_int;
#  define ddup_memory_order_relaxed memory_order_relaxed
#  define ddup_memory_order_acquire memory_order_acquire
#  define ddup_memory_order_release memory_order_release
#  define ddup_memory_order_seq_cst memory_order_seq_cst
#  define ddup_atomic_init(p, v) atomic_init(p, v)
#  define ddup_atomic_load(p, mo) atomic_load_explicit(p, mo)
#  define ddup_atomic_store(p, v, mo) atomic_store_explicit(p, v, mo)
#  define ddup_atomic_fetch_add(p, v, mo) atomic_fetch_add_explicit(p, v, mo)
#  define ddup_atomic_fetch_sub(p, v, mo) atomic_fetch_sub_explicit(p, v, mo)
#else
typedef int ddup_atomic_int;
#  define ddup_memory_order_relaxed 0
#  define ddup_memory_order_acquire 1
#  define ddup_memory_order_release 2
#  define ddup_memory_order_seq_cst 3
/* C99 fallback: single-thread semantics. A future multi-threaded backend
 * will need to add locking here or use compiler intrinsics. */
#  define ddup_atomic_init(p, v) (*(p) = (v))
#  define ddup_atomic_load(p, mo) (*(p))
#  define ddup_atomic_store(p, v, mo) (*(p) = (v))
#  define ddup_atomic_fetch_add(p, v, mo) ((*(p) += (v)) - (v))
#  define ddup_atomic_fetch_sub(p, v, mo) ((*(p) -= (v)) + (v))
#endif

#endif /* DDUP_PAL_CSTD_H */
