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
    src/pal/pal_socket.c
    src/pal/pal_event.c
    src/pal/pal_file.c
    src/pal/pal_tls.c
    src/pal/pal_iocp.c
    src/core/arena.c
    src/core/rhtable.c
    src/core/command.c
    src/core/buf_pool.c
    src/ds/obj.c
    src/ds/skiplist.c
    src/core/session.c
    src/core/config.c
    src/core/snapshot.c
    src/core/crc64.c
    src/core/migrate.c
    src/core/hashslot.c
    src/core/cluster.c
    src/resp/resp_parser.c
    src/resp/resp_writer.c
    src/server/server.c
    src/server/aof.c
    src/server/repl.c
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
# Optional TLS (OpenSSL)
# ---------------------------------------------------------------------------
option(DDUP_TLS "Build with TLS support (requires OpenSSL)" ON)
find_package(OpenSSL QUIET)
if(DDUP_TLS AND OpenSSL_FOUND)
    set(DDUP_HAS_TLS 1)
    target_compile_definitions(ddup_core PUBLIC DDUP_HAS_TLS=1)
    target_link_libraries(ddup_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "ddup: TLS enabled (OpenSSL ${OPENSSL_VERSION})")
else()
    set(DDUP_HAS_TLS 0)
    target_compile_definitions(ddup_core PUBLIC DDUP_HAS_TLS=0)
    if(NOT DDUP_TLS)
        message(STATUS "ddup: TLS disabled (DDUP_TLS=OFF)")
    else()
        message(STATUS "ddup: TLS disabled (OpenSSL not found)")
    endif()
endif()

# glibc hides POSIX functions (clock_gettime, addrinfo, epoll...) under
# strict -std=cNN; _GNU_SOURCE exposes them. macOS/FreeBSD expose them
# by default, so this is Linux-only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(ddup_core PUBLIC _GNU_SOURCE)
endif()

# ---------------------------------------------------------------------------
# Server executable
# ---------------------------------------------------------------------------
add_executable(ddup-server src/main.c)
target_link_libraries(ddup-server PRIVATE ddup_core)
ddup_apply_options(ddup-server)

# ---------------------------------------------------------------------------
# Benchmark client (tool, not a ctest test)
# ---------------------------------------------------------------------------
add_executable(ddup-bench bench/ddup-bench.c)
target_link_libraries(ddup-bench PRIVATE ddup_core)
ddup_apply_options(ddup-bench)

# In-process hot-path benchmark (profiling harness, not a ctest test)
add_executable(bench_core bench/bench_core.c)
target_link_libraries(bench_core PRIVATE ddup_core)
ddup_apply_options(bench_core)

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
    set_tests_properties(${name} PROPERTIES TIMEOUT 180)
endfunction()

ddup_add_test(test_pal tests/test_pal.c)
ddup_add_test(test_cstd tests/test_cstd.c)
ddup_add_test(test_cmdid tests/test_cmdid.c)
ddup_add_test(test_buf_pool tests/test_buf_pool.c)
ddup_add_test(test_socket tests/test_socket.c)
ddup_add_test(test_event tests/test_event.c)
ddup_add_test(test_server tests/test_server.c)
ddup_add_test(test_arena tests/test_arena.c)
ddup_add_test(test_resp_parser tests/test_resp_parser.c)
ddup_add_test(test_resp_writer tests/test_resp_writer.c)
ddup_add_test(test_resp3 tests/test_resp3.c)
ddup_add_test(test_rhtable tests/test_rhtable.c)
ddup_add_test(test_command tests/test_command.c)
ddup_add_test(test_expire tests/test_expire.c)
ddup_add_test(test_eviction tests/test_eviction.c)
ddup_add_test(test_hash tests/test_hash.c)
ddup_add_test(test_list tests/test_list.c)
ddup_add_test(test_skiplist tests/test_skiplist.c)
ddup_add_test(test_set tests/test_set.c)
ddup_add_test(test_zset tests/test_zset.c)
ddup_add_test(test_session tests/test_session.c)
ddup_add_test(test_multi tests/test_multi.c)
ddup_add_test(test_pubsub tests/test_pubsub.c)
ddup_add_test(test_config tests/test_config.c)
ddup_add_test(test_aof tests/test_aof.c)
ddup_add_test(test_snapshot tests/test_snapshot.c)
ddup_add_test(test_repl tests/test_repl.c)
ddup_add_test(test_hashslot tests/test_hashslot.c)
ddup_add_test(test_cluster_id tests/test_cluster_id.c)
ddup_add_test(test_cluster_cmds tests/test_cluster_cmds.c)
ddup_add_test(test_cluster_nodes tests/test_cluster_nodes.c)
ddup_add_test(test_cluster_bus tests/test_cluster_bus.c)
ddup_add_test(test_cluster_slots tests/test_cluster_slots.c)
ddup_add_test(test_dump tests/test_dump.c)
ddup_add_test(test_migrate tests/test_migrate.c)
ddup_add_test(test_cluster_migrate tests/test_cluster_migrate.c)
ddup_add_test(test_cluster_replica tests/test_cluster_replica.c)
ddup_add_test(test_cluster_epoch tests/test_cluster_epoch.c)
ddup_add_test(test_cluster_failover tests/test_cluster_failover.c)

if(WIN32)
    ddup_add_test(test_iocp tests/test_iocp.c)
endif()

if(DDUP_HAS_TLS)
    if(NOT WIN32)
        ddup_add_test(test_tls tests/test_tls.c)
        target_compile_definitions(test_tls PRIVATE
            DDUP_TEST_CERT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/certs")
    else()
        # test_tls currently hangs on Windows CI (under investigation);
        # the TLS library and server integration still compile there.
        message(STATUS "ddup: test_tls skipped on Windows (hang under investigation)")
    endif()
endif()

set(DDUP_CHECK_DEPS test_pal test_cstd test_cmdid test_buf_pool test_socket test_event test_server test_arena test_resp_parser test_resp_writer test_resp3 test_rhtable test_command test_expire test_eviction test_hash test_list test_skiplist test_set test_zset test_session test_multi test_pubsub test_config test_aof test_snapshot test_repl test_hashslot test_cluster_id test_cluster_cmds test_cluster_nodes test_cluster_bus test_cluster_slots test_dump test_migrate test_cluster_migrate test_cluster_replica test_cluster_epoch test_cluster_failover)
if(DDUP_HAS_TLS AND NOT WIN32)
    list(APPEND DDUP_CHECK_DEPS test_tls)
endif()
if(WIN32)
    list(APPEND DDUP_CHECK_DEPS test_iocp)
endif()

add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS ${DDUP_CHECK_DEPS}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
