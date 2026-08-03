# DetectCStandard.cmake
#
# Detect the newest C standard the compiler supports, probing from newest to
# oldest: C23 -> C17 -> C11 -> C99. The result is exposed as:
#   - cache variable DDUP_C_STD (one of 23, 17, 11, 99)
#   - compile definition DDUP_C_STD=<n> applied globally
#
# Detection relies on CMake's knowledge of compiler meta-features
# (CMAKE_C_COMPILE_FEATURES), which is reliable across MSVC, Clang and GCC.

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

set(DDUP_C_STD ${DDUP_C_STD} CACHE STRING "Detected C standard (23/17/11/99)")

set(CMAKE_C_STANDARD ${DDUP_C_STD})
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_definitions(DDUP_C_STD=${DDUP_C_STD})

message(STATUS "ddup: C standard set to C${DDUP_C_STD} (compiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION})")
