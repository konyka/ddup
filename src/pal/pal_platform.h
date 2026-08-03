/* pal_platform.h - platform / compiler / C-standard feature detection.
 *
 * Exactly one DDUP_OS_* macro is defined to 1; the others are defined to 0 so
 * they can be used in both #if and ordinary expressions.
 */
#ifndef DDUP_PAL_PLATFORM_H
#define DDUP_PAL_PLATFORM_H

#if defined(_WIN32)
#  define DDUP_OS_WINDOWS 1
#  define DDUP_OS_NAME "windows"
#else
#  define DDUP_OS_WINDOWS 0
#endif

#if defined(__linux__)
#  define DDUP_OS_LINUX 1
#  define DDUP_OS_NAME "linux"
#else
#  define DDUP_OS_LINUX 0
#endif

#if defined(__APPLE__)
#  define DDUP_OS_MACOS 1
#  define DDUP_OS_NAME "macos"
#else
#  define DDUP_OS_MACOS 0
#endif

#if defined(__FreeBSD__)
#  define DDUP_OS_FREEBSD 1
#  define DDUP_OS_NAME "freebsd"
#else
#  define DDUP_OS_FREEBSD 0
#endif

#if !DDUP_OS_WINDOWS && !DDUP_OS_LINUX && !DDUP_OS_MACOS && !DDUP_OS_FREEBSD
#  define DDUP_OS_UNIX 1
#  define DDUP_OS_NAME "unix"
#else
#  define DDUP_OS_UNIX 0
#endif

/* POSIX-like systems share the sockets/pthread code paths. */
#define DDUP_OS_POSIX (!DDUP_OS_WINDOWS)

/* C11 atomics available (and not opted out by the implementation). */
#if defined(DDUP_C_STD) && DDUP_C_STD >= 11 && \
    defined(__has_include)
#  if __has_include(<stdatomic.h>) && !defined(__STDC_NO_ATOMICS__)
#    define DDUP_HAS_C_ATOMICS 1
#  endif
#endif
#ifndef DDUP_HAS_C_ATOMICS
#  define DDUP_HAS_C_ATOMICS 0
#endif

#endif /* DDUP_PAL_PLATFORM_H */
