cmake_minimum_required(VERSION 3.20)

project(ddup VERSION 0.1.0 LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(DetectCStandard)
include(DdupOptions)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Core library: everything except the server entry point.
# ---------------------------------------------------------------------------
add_library(ddup_core STATIC
    src/pal/pal_time.c
    src/core/arena.c
    src/core/rhtable.c
    src/core/command.c
    src/resp/resp_parser.c
    src/resp/resp_writer.c
)
target_include_directories(ddup_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
ddup_apply_options(ddup_core)
target_compile_definitions(ddup_core PUBLIC
    DDUP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    DDUP_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    DDUP_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

if(WIN32)
    target_link_libraries(ddup_core PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# Server executable
# ---------------------------------------------------------------------------
add_executable(ddup-server src/main.c)
target_link_libraries(ddup-server PRIVATE ddup_core)

# ---------------------------------------------------------------------------
# Tests (TDD: every module has a test target; `ctest` or `cmake --build . --target check`)
# ---------------------------------------------------------------------------
enable_testing()

function(ddup_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ddup_core)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    ddup_apply_options(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ddup_add_test(test_pal tests/test_pal.c)
ddup_add_test(test_arena tests/test_arena.c)
ddup_add_test(test_resp_parser tests/test_resp_parser.c)
ddup_add_test(test_resp_writer tests/test_resp_writer.c)
ddup_add_test(test_resp3 tests/test_resp3.c)
ddup_add_test(test_rhtable tests/test_rhtable.c)
ddup_add_test(test_command tests/test_command.c)

add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test_pal test_arena test_resp_parser test_resp_writer test_resp3 test_rhtable test_command
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
