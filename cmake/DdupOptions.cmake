# DdupOptions.cmake
#
# Shared compile options for all ddup targets: warnings, optimization, LTO.

include(CheckIPOSupported)

function(ddup_apply_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # Link-time optimization for non-Debug builds when the toolchain supports it.
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
    if(_ipo_ok)
        set_target_properties(${target} PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
            INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON
        )
    endif()
endfunction()
