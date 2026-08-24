cmake_minimum_required(VERSION 3.20)

project(ddup VERSION 0.1.0 LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(DetectCStandard)
include(DdupOptions)
include(CheckCSourceCompiles)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Registered provided-buffer rings arrived as one Linux UAPI feature, but
# some header combinations expose the CQE flag without the ring structs or
# registration enum. Probe every declaration used by pal_iouring_op.c.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    check_c_source_compiles("#include <linux/io_uring.h>
int main(void) {
    struct io_uring_buf buf;
    struct io_uring_buf_ring ring;
    struct io_uring_buf_reg reg;
    struct io_uring_buf *slot = ring.bufs;
    buf.addr = 0; buf.len = 0; buf.bid = 0; buf.resv = 0;
    slot->addr = buf.addr;
    ring.tail = 0;
    reg.ring_addr = 0; reg.ring_entries = 0; reg.bgid = 0;
    return IORING_REGISTER_PBUF_RING + IORING_CQE_F_BUFFER +
           IORING_CQE_BUFFER_SHIFT + IOSQE_BUFFER_SELECT +
           (int)sizeof(buf) + (int)sizeof(ring) + (int)sizeof(reg);
}" _DDUP_HAVE_IOURING_PBUF_UAPI)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND _DDUP_HAVE_IOURING_PBUF_UAPI)
    set(DDUP_HAVE_IOURING_PBUF_UAPI 1)
else()
    set(DDUP_HAVE_IOURING_PBUF_UAPI 0)
endif()

# ---------------------------------------------------------------------------
# Vendored Lua 5.1.5 (deps/lua, MIT; see deps/lua/PATCHES.md). Third-party
# code: NO project warning flags, relaxed C standard.
# ---------------------------------------------------------------------------
file(GLOB DDUP_LUA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/deps/lua/*.c")
add_library(ddup_lua STATIC ${DDUP_LUA_SOURCES})
target_include_directories(ddup_lua PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/deps/lua")
target_compile_definitions(ddup_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
set_target_properties(ddup_lua PROPERTIES C_STANDARD 99)
if(MSVC)
    target_compile_options(ddup_lua PRIVATE /W0)
else()
    target_compile_options(ddup_lua PRIVATE -w)
endif()
if(NOT WIN32)
    target_link_libraries(ddup_lua PUBLIC m) # Lua's math lib needs -lm
endif()

# ---------------------------------------------------------------------------
# Vendored xxHash 0.8.3 (deps/xxhash, BSD-2-Clause; see deps/xxhash/PATCHES.md).
# Third-party code: NO project warning flags, relaxed C standard.
# ---------------------------------------------------------------------------
add_library(ddup_xxhash STATIC deps/xxhash/xxhash.c)
target_include_directories(ddup_xxhash PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/deps/xxhash")
set_target_properties(ddup_xxhash PROPERTIES C_STANDARD 99)
if(MSVC)
    target_compile_options(ddup_xxhash PRIVATE /W0)
else()
    target_compile_options(ddup_xxhash PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# Core library: everything except the server entry point.
# ---------------------------------------------------------------------------
set(DDUP_CORE_SOURCES
    src/pal/pal_time.c
    src/pal/pal_socket.c
    src/pal/pal_event.c
    src/pal/pal_file.c
    src/pal/pal_tls.c
    src/pal/pal_iocp.c
    src/pal/pal_iouring_op.c
    src/pal/pal_thread.c
    src/pal/pal_wakeup.c
    src/core/arena.c
    src/core/rhtable.c
    src/core/command.c
    src/core/buf_pool.c
    src/core/tier.c
    src/ds/obj.c
    src/ds/glob.c
    src/ds/listpack.c
    src/ds/quicklist.c
    src/ds/skiplist.c
    src/ds/stream.c
    src/core/session.c
    src/core/config.c
    src/core/snapshot.c
    src/core/crc64.c
    src/core/migrate.c
    src/core/sha1.c
    src/core/script.c
    src/core/redbus.c
    src/core/hashslot.c
    src/core/cluster.c
    src/resp/resp_parser.c
    src/resp/resp_writer.c
    src/server/server.c
    src/server/mt_server.c
    src/server/aof.c
    src/server/repl.c
)

function(ddup_configure_core target)
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/wyhash")
    ddup_apply_options(${target})
    target_link_libraries(${target} PUBLIC ddup_lua ddup_xxhash)
    target_compile_definitions(${target} PUBLIC
        DDUP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        DDUP_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        DDUP_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        DDUP_HAVE_IOURING_PBUF_UAPI=${DDUP_HAVE_IOURING_PBUF_UAPI}
    )
endfunction()

add_library(ddup_core STATIC ${DDUP_CORE_SOURCES})
ddup_configure_core(ddup_core)

if(WIN32)
    target_link_libraries(ddup_core PUBLIC ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(ddup_core PUBLIC Threads::Threads)
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
# Cluster resharding tool (redis-cli --cluster reshard style)
# ---------------------------------------------------------------------------
add_executable(ddup-reshard
    tools/ddup-reshard.c
    tools/reshard_args.c
    tools/reshard_client.c
)
target_link_libraries(ddup-reshard PRIVATE ddup_core)
target_include_directories(ddup-reshard PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tools")
ddup_apply_options(ddup-reshard)

# ---------------------------------------------------------------------------
# Tests (TDD: every module has a test target; `ctest` or `cmake --build . --target check`)
# ---------------------------------------------------------------------------
enable_testing()
find_package(Python3 COMPONENTS Interpreter QUIET)

function(ddup_add_test name source)
    if(source MATCHES "\\.py$")
        if(NOT Python3_Interpreter_FOUND)
            message(FATAL_ERROR "Python3 is required for ${name}")
        endif()
        add_test(NAME ${name} COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/${source} ${ARGN})
        set_tests_properties(${name} PROPERTIES TIMEOUT 180)
        return()
    endif()
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ddup_core_test)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(${name} PRIVATE DDUP_TESTING=1)
    ddup_apply_options(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 180)
endfunction()

# Test-only PAL seams must not add mutable state to the production library.
add_library(ddup_core_test STATIC ${DDUP_CORE_SOURCES})
ddup_configure_core(ddup_core_test)
target_compile_definitions(ddup_core_test PRIVATE DDUP_TESTING=1)
target_compile_definitions(ddup_core_test PUBLIC DDUP_HAS_TLS=${DDUP_HAS_TLS})
if(DDUP_HAS_TLS)
    target_link_libraries(ddup_core_test PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
if(WIN32)
    target_link_libraries(ddup_core_test PUBLIC ws2_32)
else()
    target_link_libraries(ddup_core_test PUBLIC Threads::Threads)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(ddup_core_test PUBLIC _GNU_SOURCE)
endif()

ddup_add_test(test_pal tests/test_pal.c)
ddup_add_test(test_pal_thread tests/test_pal_thread.c)
ddup_add_test(test_pal_wakeup tests/test_pal_wakeup.c)
ddup_add_test(test_spsc tests/test_spsc.c)
ddup_add_test(test_mt_server tests/test_mt_server.c)
ddup_add_test(test_cstd tests/test_cstd.c)
ddup_add_test(test_cmdid tests/test_cmdid.c)
ddup_add_test(test_buf_pool tests/test_buf_pool.c)
ddup_add_test(test_socket tests/test_socket.c)
ddup_add_test(test_event tests/test_event.c)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_event PRIVATE DDUP_TEST_IOURING=1)
endif()
ddup_add_test(test_server tests/test_server.c)
ddup_add_test(test_arena tests/test_arena.c)
ddup_add_test(test_resp_parser tests/test_resp_parser.c)
ddup_add_test(test_resp_writer tests/test_resp_writer.c)
ddup_add_test(test_resp3 tests/test_resp3.c)
ddup_add_test(test_rhtable tests/test_rhtable.c)
ddup_add_test(test_command tests/test_command.c)
ddup_add_test(test_tier tests/test_tier.c)
ddup_add_test(test_tier_db tests/test_tier_db.c)
ddup_add_test(test_glob tests/test_glob.c)
ddup_add_test(test_keys tests/test_keys.c)
ddup_add_test(test_stringx tests/test_stringx.c)
ddup_add_test(test_sort tests/test_sort.c)
ddup_add_test(test_hll tests/test_hll.c)
ddup_add_test(test_geo tests/test_geo.c)
ddup_add_test(test_admin tests/test_admin.c)
ddup_add_test(test_bitmap tests/test_bitmap.c)
ddup_add_test(test_expire tests/test_expire.c)
ddup_add_test(test_eviction tests/test_eviction.c)
ddup_add_test(test_hash tests/test_hash.c)
ddup_add_test(test_list tests/test_list.c)
ddup_add_test(test_listpack tests/test_listpack.c)
ddup_add_test(test_quicklist tests/test_quicklist.c)
ddup_add_test(test_skiplist tests/test_skiplist.c)
ddup_add_test(test_set tests/test_set.c)
ddup_add_test(test_zset tests/test_zset.c)
ddup_add_test(test_stream tests/test_stream.c)
ddup_add_test(test_array tests/test_array.c)
ddup_add_test(test_session tests/test_session.c)
ddup_add_test(test_multidb tests/test_multidb.c)
ddup_add_test(test_multi tests/test_multi.c)
ddup_add_test(test_pubsub tests/test_pubsub.c)
ddup_add_test(test_pubsub_shard tests/test_pubsub_shard.c)
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
ddup_add_test(test_cluster_pfail tests/test_cluster_pfail.c)
ddup_add_test(test_reshard tests/test_reshard.c)
ddup_add_test(test_lua_smoke tests/test_lua_smoke.c)
ddup_add_test(test_script tests/test_script.c)
ddup_add_test(test_eval tests/test_eval.c)
ddup_add_test(test_redbus tests/test_redbus.c)
ddup_add_test(test_redbus_bus tests/test_redbus_bus.c)
ddup_add_test(test_redbus_failover tests/test_redbus_failover.c)
ddup_add_test(test_redbus_elect tests/test_redbus_elect.c)
target_sources(test_reshard PRIVATE tools/reshard_args.c tools/reshard_client.c)
target_include_directories(test_reshard PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tools")

if(WIN32)
    ddup_add_test(test_iocp tests/test_iocp.c)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    ddup_add_test(test_iouring_op tests/test_iouring_op.c)
endif()

if(DDUP_HAS_TLS)
    ddup_add_test(test_tls tests/test_tls.c)
    target_compile_definitions(test_tls PRIVATE
        DDUP_TEST_CERT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/certs")
    # loaded CI runners occasionally stall the RSA handshake well past
    # 15s; the deadline budgets guard hangs, they are not assertions
    set_tests_properties(test_tls PROPERTIES TIMEOUT 600)
endif()

set(DDUP_CHECK_DEPS test_pal test_pal_thread test_pal_wakeup test_spsc test_mt_server test_cstd test_cmdid test_buf_pool test_socket test_event test_server test_arena test_resp_parser test_resp_writer test_resp3 test_rhtable test_command test_glob test_keys test_stringx test_sort test_admin test_bitmap test_expire test_eviction test_hash test_list test_listpack test_quicklist test_skiplist test_set test_zset test_stream test_session test_multidb test_multi test_pubsub test_pubsub_shard test_config test_aof test_snapshot test_repl test_hashslot test_cluster_id test_cluster_cmds test_cluster_nodes test_cluster_bus test_cluster_slots test_dump test_migrate test_cluster_migrate test_cluster_replica test_cluster_epoch test_cluster_failover test_cluster_pfail test_reshard test_lua_smoke test_script test_eval test_redbus test_redbus_bus test_redbus_failover test_redbus_elect ddup-reshard)
if(DDUP_HAS_TLS)
    list(APPEND DDUP_CHECK_DEPS test_tls)
endif()
if(WIN32)
    list(APPEND DDUP_CHECK_DEPS test_iocp)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND DDUP_CHECK_DEPS test_iouring_op)
endif()

add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS ${DDUP_CHECK_DEPS}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# Redis 7.2.15 command-compat audit (read-only; zero runtime cost).
# Runs when python3 and a Redis src/commands JSON tree are available, so
# plain `ctest` stays green on machines without a Redis checkout.
# ---------------------------------------------------------------------------
if(Python3_Interpreter_FOUND)
    ddup_add_test(test_redis_compat_tool tools/test_audit_redis_compat.py)
endif()

find_program(DDUP_NODE node)
if(DDUP_NODE)
    add_test(NAME test_cooldown_button
             COMMAND ${DDUP_NODE} ${CMAKE_SOURCE_DIR}/ui/test_cooldown_button.js)
    list(APPEND DDUP_CHECK_DEPS test_cooldown_button)
endif()
if(Python3_Interpreter_FOUND AND DEFINED REDIS_COMMANDS_DIR)
    ddup_add_test(test_redis_compat tools/audit_redis_compat.py
                  --redis-json ${REDIS_COMMANDS_DIR}
                  --repo ${CMAKE_CURRENT_SOURCE_DIR}
                  --check)
endif()
