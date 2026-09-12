# DdupOptions.cmake
#
# Shared compile options for all ddup targets: warnings, optimization, LTO.

include(CheckIPOSupported)

set(DDUP_SANITIZE "" CACHE STRING
    "Comma-separated sanitizers for first-party targets (address,undefined)")
set_property(CACHE DDUP_SANITIZE PROPERTY STRINGS "" address undefined address,undefined)

function(ddup_apply_sanitizers target)
    if(NOT DDUP_SANITIZE)
        return()
    endif()
    if(MSVC)
        message(FATAL_ERROR "DDUP_SANITIZE is not supported by this MSVC configuration")
    endif()
    if(NOT DDUP_SANITIZE MATCHES "^(address|undefined|address,undefined)$")
        message(FATAL_ERROR "DDUP_SANITIZE must be address, undefined, or address,undefined")
    endif()
    target_compile_options(${target} PRIVATE "-fsanitize=${DDUP_SANITIZE}")
    target_link_options(${target} PRIVATE "-fsanitize=${DDUP_SANITIZE}")
    # Sanitizer instrumentation and LTO make diagnostics less reliable.
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
endfunction()

function(ddup_apply_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(WIN32)
        # Silence UCRT deprecation noise for standard C89 file/string APIs.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    ddup_apply_sanitizers(${target})

    # Link-time optimization for non-Debug builds when the toolchain supports it.
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
    if(_ipo_ok AND NOT DDUP_SANITIZE)
        set_target_properties(${target} PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
            INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON
        )
    endif()
endfunction()
