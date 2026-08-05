# DetectCStandard.cmake
#
# Detect the newest C standard the compiler supports, probing from newest to
# oldest: C23 -> C17 -> C11 -> C99. The result is exposed as:
#   - cache variable DDUP_C_STD (one of 23, 17, 11, 99)
#   - compile definition DDUP_C_STD=<n> applied globally
#
# Optionally a specific standard can be forced with:
#   -DDDUP_C_STD_FORCE=99
#
# Fine-grained capability macros are also exposed:
#   - DDUP_HAS_C_ATOMICS, DDUP_HAS_C_THREADS, DDUP_HAS_C_ALIGNAS,
#     DDUP_HAS_C_STATIC_ASSERT, DDUP_HAS_C_NORETURN,
#     DDUP_HAS_C_THREAD_LOCAL, DDUP_HAS_C_TYPEOF, DDUP_HAS_C_CONSTEXPR,
#     DDUP_HAS_C_STDCKDINT, DDUP_HAS_C_BITINT

include(CheckCSourceCompiles)
include(CheckIncludeFile)

set(DDUP_C_STD_FORCE "0" CACHE STRING "Force a specific C standard (0=auto, 99/11/17/23)")

if(NOT DDUP_C_STD_FORCE STREQUAL "0")
    if(NOT DDUP_C_STD_FORCE MATCHES "^(99|11|17|23)$")
        message(FATAL_ERROR "DDUP_C_STD_FORCE must be one of: 99, 11, 17, 23 (got ${DDUP_C_STD_FORCE})")
    endif()
    set(DDUP_C_STD ${DDUP_C_STD_FORCE})
    set(_ddup_forced_std TRUE)
else()
    if("c_std_23" IN_LIST CMAKE_C_COMPILE_FEATURES)
        set(DDUP_C_STD 23)
    elseif("c_std_17" IN_LIST CMAKE_C_COMPILE_FEATURES)
        set(DDUP_C_STD 17)
    elseif("c_std_11" IN_LIST CMAKE_C_COMPILE_FEATURES)
        set(DDUP_C_STD 11)
    elseif("c_std_99" IN_LIST CMAKE_C_COMPILE_FEATURES)
        set(DDUP_C_STD 99)
    else()
        message(FATAL_ERROR "ddup requires at least a C99-capable compiler")
    endif()
    set(_ddup_forced_std FALSE)
endif()

set(DDUP_C_STD ${DDUP_C_STD} CACHE STRING "Detected C standard (23/17/11/99)" FORCE)

set(CMAKE_C_STANDARD ${DDUP_C_STD})
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_definitions(DDUP_C_STD=${DDUP_C_STD})

# ---------------------------------------------------------------------------
# Fine-grained capability detection. Each capability is confirmed by trying
# to compile a minimal snippet at the selected C standard.
# ---------------------------------------------------------------------------

function(_ddup_check_feature name min_std src)
    if(DDUP_C_STD GREATER_EQUAL ${min_std})
        check_c_source_compiles("${src}" _DDUP_HAS_C_${name})
        if(_DDUP_HAS_C_${name})
            set(DDUP_HAS_C_${name} 1 PARENT_SCOPE)
        else()
            set(DDUP_HAS_C_${name} 0 PARENT_SCOPE)
        endif()
    else()
        set(DDUP_HAS_C_${name} 0 PARENT_SCOPE)
    endif()
endfunction()

# C11 family
_ddup_check_feature(ATOMICS 11 "
#include <stdatomic.h>
int main(void) {
    atomic_int x;
    atomic_init(&x, 0);
    atomic_fetch_add_explicit(&x, 1, memory_order_relaxed);
    return atomic_load_explicit(&x, memory_order_relaxed);
}")

if(DDUP_C_STD GREATER_EQUAL 11)
    check_include_file(threads.h _DDUP_HAS_THREADS_H)
    if(_DDUP_HAS_THREADS_H)
        set(DDUP_HAS_C_THREADS 1)
    else()
        set(DDUP_HAS_C_THREADS 0)
    endif()
else()
    set(DDUP_HAS_C_THREADS 0)
endif()

_ddup_check_feature(ALIGNAS 11 "
int main(void) {
    _Alignas(64) char buf[64];
    return (int)((unsigned long long)(void *)buf & 63U);
}")

_ddup_check_feature(STATIC_ASSERT 11 "
_Static_assert(1 == 1, \"ok\");
int main(void) { return 0; }")

_ddup_check_feature(NORETURN 11 "
_Noreturn void f(void) { while (1) { } }
int main(void) { (void)f; return 0; }")

_ddup_check_feature(THREAD_LOCAL 11 "
static _Thread_local int x = 0;
int main(void) {
    x = 1;
    return x;
}")

# C23 family
_ddup_check_feature(TYPEOF 23 "
int main(void) {
    typeof(int) x = 0;
    return x;
}")

_ddup_check_feature(CONSTEXPR 23 "
int main(void) {
    constexpr int n = 4;
    char arr[n];
    arr[0] = 0;
    return arr[0];
}")

_ddup_check_feature(STDCKDINT 23 "
#include <stdckdint.h>
#include <stdbool.h>
int main(void) {
    int r;
    bool o = ckd_add(&r, 1, 2);
    (void)o;
    return r;
}")

_ddup_check_feature(BITINT 23 "
int main(void) {
    _BitInt(8) x = 0;
    return (int)x;
}")

foreach(_feat ATOMICS ALIGNAS STATIC_ASSERT NORETURN THREAD_LOCAL TYPEOF CONSTEXPR STDCKDINT BITINT)
    if(NOT DEFINED DDUP_HAS_C_${_feat})
        set(DDUP_HAS_C_${_feat} 0)
    endif()
    add_compile_definitions(DDUP_HAS_C_${_feat}=${DDUP_HAS_C_${_feat}})
endforeach()
# THREADS was handled separately because it only needs a header check.
add_compile_definitions(DDUP_HAS_C_THREADS=${DDUP_HAS_C_THREADS})

if(_ddup_forced_std)
    message(STATUS "ddup: C standard forced to C${DDUP_C_STD} (DDUP_C_STD_FORCE=${DDUP_C_STD_FORCE})")
else()
    message(STATUS "ddup: C standard auto-detected as C${DDUP_C_STD} (compiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION})")
endif()
message(STATUS "ddup: C capabilities ATOMICS=${DDUP_HAS_C_ATOMICS} THREADS=${DDUP_HAS_C_THREADS} ALIGNAS=${DDUP_HAS_C_ALIGNAS} STATIC_ASSERT=${DDUP_HAS_C_STATIC_ASSERT} NORETURN=${DDUP_HAS_C_NORETURN} THREAD_LOCAL=${DDUP_HAS_C_THREAD_LOCAL} TYPEOF=${DDUP_HAS_C_TYPEOF} CONSTEXPR=${DDUP_HAS_C_CONSTEXPR} STDCKDINT=${DDUP_HAS_C_STDCKDINT} BITINT=${DDUP_HAS_C_BITINT}")
