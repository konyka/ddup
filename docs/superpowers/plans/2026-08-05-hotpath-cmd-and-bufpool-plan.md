# 热路径命令 ID 表与缓冲池实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用稳定命令 ID 表替换命令分发的 `if/else` 字符串比较，并用分级缓冲池减少连接接收/输出缓冲的 malloc，提升热路径性能。

**Architecture:** 在 `src/core/command.c` 内建统一 `cmd_entry` 表 + 开放寻址哈希表；新增 `src/core/buf_pool.c` 四级缓冲池；接入 `src/server/server.c` 的 `conn->rbuf` 与 `conn->out`。

**Tech Stack:** C99–C23, CMake, CTest, ddup 自研测试框架。

## Global Constraints

- 代码必须兼容 C99 语法下限；使用 C11+ 特性时经 `pal_cstd.h` 封装。
- 任何平台相关代码必须走 `src/pal/` 抽象；`src/pal/` 之外禁止 `#ifdef _WIN32` 等平台宏。
- `-Wall -Wextra -Wpedantic`（MSVC `/W4`）下不允许新增警告。
- 任何新功能/模块先写失败测试，再实现，测试全绿才算完成。
- 每小步验证通过后立即 `git commit` 并 `git push origin main`。
- 测试一律通过 `ddup_add_test()` 注册进 CTest。

## 文件结构

| 文件 | 职责 |
|---|---|
| `src/core/command.c` | `cmd_entry` 表、`cmd_resolve` 哈希、`switch` 分发、O(1) 辅助函数 |
| `src/core/command.h` | 暴露 `cmd_resolve`、`cmd_is_write`、ID 枚举/常量（如需） |
| `tests/test_cmdid.c` | 命令 ID 表 TDD 测试 |
| `src/core/buf_pool.h` | 缓冲池头文件 |
| `src/core/buf_pool.c` | 分级自由列表实现 |
| `tests/test_buf_pool.c` | 缓冲池 TDD 测试 |
| `src/server/server.c` | 连接创建/释放/读循环接入缓冲池 |
| `src/resp/resp_writer.h/c` | `resp_buf_pool_reserve()` 池化扩容路径 |
| `bench/bench_core.c` | 新增命令解析 + 缓冲池 micro-benchmark |
| `docs/architecture.md` | 命令 ID 与缓冲池架构说明 |
| `docs/performance.md` | benchmark 数字 |
| `docs/roadmap.md` | Phase 9 勾选 |

---

### Task 1: 命令表数据结构与 cmd_resolve

**Files:**
- Modify: `src/core/command.c`
- Create: `tests/test_cmdid.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:
  - `cmd_id` enum / `CMD_ID_UNKNOWN`
  - `cmd_entry` struct
  - `uint16_t cmd_resolve(const char *name, size_t len)` — 大小写不敏感
  - `int cmd_is_write(uint16_t cmd_id)`
  - `int cmd_min_argc`, `cmd_max_argc`, `cmd_parity`

- [ ] **Step 1: 写失败测试**

创建 `tests/test_cmdid.c`：

```c
#include "test.h"
#include "core/command.h"

static void test_resolve_known_command(void)
{
    DD_CHECK(cmd_resolve("GET", 3) != CMD_ID_UNKNOWN);
    DD_CHECK(cmd_resolve("get", 3) == cmd_resolve("GET", 3));
    DD_CHECK(cmd_resolve("Get", 3) == cmd_resolve("GET", 3));
}

static void test_resolve_unknown_command(void)
{
    DD_CHECK(cmd_resolve("NOTACMD", 7) == CMD_ID_UNKNOWN);
}

static void test_write_flag(void)
{
    DD_CHECK(cmd_is_write(cmd_resolve("SET", 3)));
    DD_CHECK(!cmd_is_write(cmd_resolve("GET", 3)));
}

int main(void)
{
    DD_RUN(test_resolve_known_command);
    DD_RUN(test_resolve_unknown_command);
    DD_RUN(test_write_flag);
    return DD_TEST_SUMMARY();
}
```

在 `CMakeLists.txt` 中加入 `ddup_add_test(test_cmdid tests/test_cmdid.c)` 和 `DDUP_CHECK_DEPS`。

Run:
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target test_cmdid
```
Expected: 编译失败，`cmd_resolve` 未定义。

- [ ] **Step 2: 实现 cmd_entry 表与 cmd_resolve**

在 `src/core/command.c` 中：

1. 定义标志：
```c
#define CMD_WRITE 0x01
```

2. 合并 `CMD_ARITY` / `WRITE_COMMANDS` 为统一 `cmd_entry` 表（放在原 `CMD_ARITY` 位置）。每个条目包含 `name`、`id`、`min_argc`、`max_argc`、`parity`、`flags`。

3. 定义枚举（紧邻表前或单独头文件）：
```c
enum {
    CMD_ID_UNKNOWN = 0,
    CMD_PING = 1,
    CMD_ECHO,
    CMD_GET,
    CMD_SET,
    /* ... 按表中顺序延续 ... */
};
```

4. 实现开放寻址哈希表：
```c
#define CMD_HASH_SIZE 256  /* 负载因子 < 0.5，2 的幂方便掩码 */

typedef struct cmd_hash_slot {
    const char *name;
    uint8_t nlen;
    uint16_t id;
} cmd_hash_slot;

static cmd_hash_slot cmd_hash[CMD_HASH_SIZE];
static uint8_t cmd_hash_inited = 0;
```

5. 哈希函数（大小写不敏感）：
```c
static uint32_t cmd_hash_fn(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
        h ^= c;
        h *= 16777619u;
    }
    return h;
}
```

6. 初始化函数：
```c
static void cmd_hash_init(void)
{
    size_t i;
    if (cmd_hash_inited) return;
    memset(cmd_hash, 0, sizeof(cmd_hash));
    for (i = 0; i < sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0]); i++) {
        const cmd_entry *e = &CMD_TABLE[i];
        uint32_t h = cmd_hash_fn(e->name, strlen(e->name));
        size_t idx = h & (CMD_HASH_SIZE - 1);
        while (cmd_hash[idx].id != 0) idx = (idx + 1) & (CMD_HASH_SIZE - 1);
        cmd_hash[idx].name = e->name;
        cmd_hash[idx].nlen = (uint8_t)strlen(e->name);
        cmd_hash[idx].id = e->id;
    }
    cmd_hash_inited = 1;
}
```

7. 解析函数：
```c
uint16_t cmd_resolve(const char *name, size_t len)
{
    uint32_t h;
    size_t idx;
    if (!cmd_hash_inited) cmd_hash_init();
    if (len == 0 || len > 255) return CMD_ID_UNKNOWN;
    h = cmd_hash_fn(name, len);
    idx = h & (CMD_HASH_SIZE - 1);
    while (cmd_hash[idx].id != 0) {
        if (cmd_hash[idx].nlen == len && ci_equal(name, len, cmd_hash[idx].name))
            return cmd_hash[idx].id;
        idx = (idx + 1) & (CMD_HASH_SIZE - 1);
    }
    return CMD_ID_UNKNOWN;
}
```

8. O(1) 辅助函数通过 ID 直接索引 `CMD_TABLE`（因为 IDs 是连续的 1..N，可用 `cmd_table_by_id[cmd_id]` 或 `CMD_TABLE[id-1]`）。

- [ ] **Step 3: 运行测试并确认通过**

```bash
cmake --build build --target test_cmdid
ctest --test-dir build -R test_cmdid --output-on-failure
```
Expected: PASS。

- [ ] **Step 4: Commit + push**

```bash
git add src/core/command.c src/core/command.h tests/test_cmdid.c CMakeLists.txt
git commit -m "feat(cmdid): unified cmd_entry table and cmd_resolve hash"
git push origin main
```

---

### Task 2: 用 switch(cmd_id) 替换 if/else 分发链

**Files:**
- Modify: `src/core/command.c`

**Interfaces:**
- Consumes: `cmd_resolve`, `cmd_is_write` from Task 1
- Produces: `command_dispatch()` 改为 `switch (cmd_id)`，移除 `is_write_command` 等旧线性扫描

- [ ] **Step 1: 写失败/回归测试**

扩展 `tests/test_cmdid.c`：

```c
static void test_dispatch_still_works(void)
{
    /* 通过 session/command_execute 跑几个基本命令验证行为未被破坏 */
    /* 这里先写空占位，等 Task 4 集成测试统一覆盖；
       本任务以全量 ctest 为回归证据。 */
}
```

Run `ctest --test-dir build -R test_command` 等现有测试确认 baseline 绿。

- [ ] **Step 2: 改写 command_dispatch**

在 `command_dispatch()` 顶部调用 `cmd_resolve` 得到 `cmd_id`；把后续所有 `if (ci_equal(name, nlen, "FOO"))` 改为 `case CMD_FOO:`。

示例：
```c
uint16_t cmd_id = cmd_resolve(name, nlen);
switch (cmd_id) {
case CMD_PING:  /* ... */ break;
case CMD_GET:   /* ... */ break;
case CMD_SET:   /* ... */ break;
/* ... */
case CMD_ID_UNKNOWN:
default:
    /* 原 unknown command 错误 */
    break;
}
```

- `is_write_command()` 改为 `cmd_is_write(cmd_id)`。
- `cluster_keyless()` 中若有命令名比较，改为 ID 比较（如 `CMD_CLUSTER`）。
- `queue_validate()` 改为通过 `cmd_id` 查 `CMD_TABLE` 做 arity 校验。
- 保留 `ci_equal` 仅作为哈希槽比较和兼容用途。

- [ ] **Step 3: 全量回归测试**

```bash
cmake --build build --target check
```
Expected: 38/38 通过，无新增警告。

- [ ] **Step 4: Commit + push**

```bash
git add src/core/command.c tests/test_cmdid.c
git commit -m "feat(cmdid): switch-based dispatch and O(1) write/arity helpers"
git push origin main
```

---

### Task 3: 分级缓冲池模块

**Files:**
- Create: `src/core/buf_pool.h`
- Create: `src/core/buf_pool.c`
- Create: `tests/test_buf_pool.c`
- Modify: `CMakeLists.txt`
- Modify: `src/core/CMakeLists`? (ddup_core 在 `CMakeLists.txt:16` 显式列出源文件，直接把 `buf_pool.c` 加到该列表)

**Interfaces:**
- Produces:
  - `buf_pool_init/destroy`
  - `buf_pool_get(pool, size, actual_size)`
  - `buf_pool_put(pool, ptr, size)`

- [ ] **Step 1: 写失败测试**

创建 `tests/test_buf_pool.c`：

```c
#include "test.h"
#include "core/buf_pool.h"

static void test_pool_basic(void)
{
    buf_pool pool;
    size_t sz;
    DD_CHECK(buf_pool_init(&pool) == 0);
    char *p = (char *)buf_pool_get(&pool, 64 * 1024, &sz);
    DD_CHECK(p != NULL);
    DD_CHECK(sz >= 64 * 1024);
    p[0] = 'x';
    DD_CHECK(p[0] == 'x');
    buf_pool_put(&pool, p, sz);
    buf_pool_destroy(&pool);
}

static void test_oversized_fallback(void)
{
    buf_pool pool;
    size_t sz;
    DD_CHECK(buf_pool_init(&pool) == 0);
    char *p = (char *)buf_pool_get(&pool, 10 * 1024 * 1024, &sz);
    DD_CHECK(p != NULL);
    DD_CHECK(sz >= 10 * 1024 * 1024);
    buf_pool_put(&pool, p, sz);
    buf_pool_destroy(&pool);
}

int main(void)
{
    DD_RUN(test_pool_basic);
    DD_RUN(test_oversized_fallback);
    return DD_TEST_SUMMARY();
}
```

加入 `CMakeLists.txt` 作为 `test_buf_pool`。

Run `cmake --build build --target test_buf_pool`。
Expected: 编译失败，头文件不存在。

- [ ] **Step 2: 实现 buf_pool**

`src/core/buf_pool.h`：

```c
#ifndef DDUP_BUF_POOL_H
#define DDUP_BUF_POOL_H

#include <stddef.h>

#define BUF_POOL_TIERS 4

typedef struct buf_pool_free_node {
    struct buf_pool_free_node *next;
} buf_pool_free_node;

typedef struct buf_pool {
    size_t sizes[BUF_POOL_TIERS];
    buf_pool_free_node *lists[BUF_POOL_TIERS];
    size_t allocs;      /* fallback allocations not from pool */
    size_t hits;        /* successful borrows */
} buf_pool;

int buf_pool_init(buf_pool *pool);
void buf_pool_destroy(buf_pool *pool);
void *buf_pool_get(buf_pool *pool, size_t size, size_t *actual_size);
void buf_pool_put(buf_pool *pool, void *ptr, size_t size);

#endif
```

`src/core/buf_pool.c`：

```c
#include "core/buf_pool.h"
#include <stdlib.h>
#include <string.h>
#include "pal/pal_cstd.h"

int buf_pool_init(buf_pool *pool)
{
    ddup_static_assert(BUF_POOL_TIERS == 4, "tiers");
    pool->sizes[0] = 4 * 1024;
    pool->sizes[1] = 16 * 1024;
    pool->sizes[2] = 64 * 1024;
    pool->sizes[3] = 256 * 1024;
    memset(pool->lists, 0, sizeof(pool->lists));
    pool->allocs = 0;
    pool->hits = 0;
    return 0;
}

static int buf_pool_tier(buf_pool *pool, size_t size)
{
    int i;
    for (i = 0; i < BUF_POOL_TIERS; i++) {
        if (size <= pool->sizes[i]) return i;
    }
    return -1;
}

void *buf_pool_get(buf_pool *pool, size_t size, size_t *actual_size)
{
    int tier = buf_pool_tier(pool, size);
    if (tier >= 0) {
        buf_pool_free_node *n = pool->lists[tier];
        if (n) {
            pool->lists[tier] = n->next;
            pool->hits++;
            *actual_size = pool->sizes[tier];
            return n;
        }
        void *p = malloc(pool->sizes[tier]);
        if (!p) return NULL;
        pool->allocs++;
        *actual_size = pool->sizes[tier];
        return p;
    }
    /* oversized: fallback */
    void *p = malloc(size);
    if (p) pool->allocs++;
    *actual_size = size;
    return p;
}

void buf_pool_put(buf_pool *pool, void *ptr, size_t size)
{
    int tier = buf_pool_tier(pool, size);
    if (tier >= 0 && size == pool->sizes[tier]) {
        buf_pool_free_node *n = (buf_pool_free_node *)ptr;
        n->next = pool->lists[tier];
        pool->lists[tier] = n;
    } else {
        free(ptr);
    }
}

void buf_pool_destroy(buf_pool *pool)
{
    int i;
    for (i = 0; i < BUF_POOL_TIERS; i++) {
        buf_pool_free_node *n = pool->lists[i];
        while (n) {
            buf_pool_free_node *next = n->next;
            free(n);
            n = next;
        }
        pool->lists[i] = NULL;
    }
}
```

- [ ] **Step 3: 运行测试**

```bash
cmake --build build --target test_buf_pool
ctest --test-dir build -R test_buf_pool --output-on-failure
```
Expected: PASS。

- [ ] **Step 4: Commit + push**

```bash
git add src/core/buf_pool.h src/core/buf_pool.c tests/test_buf_pool.c CMakeLists.txt
git commit -m "feat(bufpool): tiered fixed-size buffer pool"
git push origin main
```

---

### Task 4: 接入 conn->rbuf / conn->out

**Files:**
- Modify: `src/server/server.c`
- Modify: `src/resp/resp_writer.h/c`
- Modify: `src/core/buf_pool.h`（如需统计计数器）

**Interfaces:**
- Consumes: `buf_pool` from Task 3
- Produces: `resp_buf_pool_reserve()`、`server` 全局 `buf_pool`

- [ ] **Step 1: 写失败/回归测试**

扩展 `tests/test_buf_pool.c` 或新增 `tests/test_server_buf_pool.c`：

```c
/* 先占位；真正回归靠 test_server / bench_core。 */
```

Run 现有 `test_server` 确认 baseline 绿。

- [ ] **Step 2: 在 server.c 中创建全局 buf_pool**

在 `server` 结构体中加入：
```c
buf_pool pool;
```

在 `server_start()` 中调用 `buf_pool_init(&srv->pool)`，在 `server_stop()` 中 `buf_pool_destroy(&srv->pool)`。

- [ ] **Step 3: conn->rbuf 池化**

修改 `conn_create()`：
```c
c->rbuf = (char *)buf_pool_get(&srv->pool, SERVER_RECV_CHUNK, &c->rcap);
c->rlen = 0;
```

修改 `conn_free()`：
```c
buf_pool_put(&srv->pool, c->rbuf, c->rcap);
```

修改 `server_run_once()` 中 rbuf 增长分支（`server.c:1571`）：
```c
size_t new_cap = c->rcap * 2;
char *new_rbuf = (char *)buf_pool_get(&srv->pool, new_cap, &new_cap);
if (new_rbuf) {
    memcpy(new_rbuf, c->rbuf, c->rlen);
    buf_pool_put(&srv->pool, c->rbuf, c->rcap);
    c->rbuf = new_rbuf;
    c->rcap = new_cap;
}
```

IOCP 路径（`server_run_once_iocp`）同样处理。

- [ ] **Step 4: conn->out 池化**

在 `resp_writer.h/c` 新增：
```c
void resp_buf_pool_reserve(resp_buf *b, size_t n, buf_pool *pool);
```

实现：
```c
void resp_buf_pool_reserve(resp_buf *b, size_t n, buf_pool *pool)
{
    if (b->cap >= n) return;
    size_t new_cap;
    char *new_data = (char *)buf_pool_get(pool, n, &new_cap);
    if (!new_data) return; /* or abort */
    if (b->data) {
        memcpy(new_data, b->data, b->len);
        buf_pool_put(pool, b->data, b->cap);
    }
    b->data = new_data;
    b->cap = new_cap;
}
```

在 `resp_buf_reserve()` 中保留原 `realloc` 路径；新增一个“属于连接”的输出缓冲标记，或用 `resp_buf_pool_reserve` 显式调用。为最小改动，在 `server.c` 中创建连接后把 `out` 初始化为空，并在 `conn_flush()` / 写出前调用 `resp_buf_pool_reserve(&c->out, needed, &srv->pool)`。更简单的做法：在 `resp_buf_reserve()` 无法判断归属，所以让 `server.c` 在准备写响应前调用 `resp_buf_pool_reserve`。

实现细节：
- `resp_buf_init(&c->out)` 仍设 `data=NULL, len=cap=0`。
- 在 `conn_process_input()` 执行命令前，确保 `resp_buf_pool_reserve(&c->out, 4096, &srv->pool)` 预分配一个 4 KiB 缓冲（避免首条响应 realloc）。
- `conn_free()` 中 `resp_buf_free(&c->out)` 改为：若 `c->out.data` 非空，`buf_pool_put(&srv->pool, c->out.data, c->out.cap)`，然后 `c->out.data = NULL`。
- 非连接使用的 `resp_buf`（如 bench、AOF 临时缓冲）继续使用 `resp_buf_reserve()` / `resp_buf_free()`，不走池。

- [ ] **Step 5: 全量回归测试**

```bash
cmake --build build --target check
```
Expected: 38/38 通过，无新增警告。

- [ ] **Step 6: Commit + push**

```bash
git add src/server/server.c src/resp/resp_writer.h src/resp/resp_writer.c src/core/buf_pool.h src/core/buf_pool.c
git commit -m "feat(bufpool): wire pool into conn rbuf and out buffer"
git push origin main
```

---

### Task 5: Benchmark 与文档更新

**Files:**
- Modify: `bench/bench_core.c`
- Modify: `docs/performance.md`
- Modify: `docs/architecture.md`
- Modify: `docs/roadmap.md`

**Interfaces:**
- Consumes: `cmd_resolve`, `buf_pool`
- Produces: benchmark 数字、文档更新

- [ ] **Step 1: 新增 micro-benchmark**

在 `bench/bench_core.c` 中新增两个 micro-benchmark（独立可开关，不破坏现有主流程）：

1. `bench_cmd_resolve()`：随机生成命令名列表，循环 `cmd_resolve()`，计时并输出 ops/sec。
2. `bench_buf_pool()`：循环 `buf_pool_get`/`put` 64 KiB，对比直接 `malloc`/`free`。

示例骨架：
```c
static void bench_cmd_resolve(void)
{
    const char *names[] = {"GET","SET","HSET","LPUSH","ZADD","CLUSTER","NOTACMD"};
    size_t n = sizeof(names)/sizeof(names[0]);
    uint64_t start = pal_now_us();
    size_t iters = 10000000;
    size_t i;
    volatile uint16_t sink = 0;
    for (i = 0; i < iters; i++) {
        const char *name = names[i % n];
        sink = cmd_resolve(name, strlen(name));
    }
    uint64_t us = pal_now_us() - start;
    printf("cmd_resolve: %zu iters in %.3f ms -> %.1f Mops/sec\n",
           iters, us / 1000.0, (double)iters / us);
    (void)sink;
}
```

- [ ] **Step 2: 编译并运行 bench_core**

```bash
cmake --build build --target bench_core
./build/bench_core --cmd-resolve --buf-pool
```
Expected: 运行成功，数字合理。

- [ ] **Step 3: 更新文档**

- `docs/architecture.md`：在“命令层”与“内存管理”小节后补充命令 ID 表与缓冲池说明。
- `docs/performance.md`：新增“Phase 9 Hot-path micro-benchmarks”一节，记录 `cmd_resolve` 与 `buf_pool` 数字。
- `docs/roadmap.md`：新增 Phase 9 并勾选。

- [ ] **Step 4: 最终全量验证**

默认构建 + 强制 C99：
```bash
cmake --build build --target check
cmake -S . -B build-c99 -G Ninja -DCMAKE_BUILD_TYPE=Release -DDDUP_C_STD_FORCE=99
cmake --build build-c99 --target check
```
Expected: 各 38/38 通过，无新增警告。

- [ ] **Step 5: Commit + push**

```bash
git add bench/bench_core.c docs/architecture.md docs/performance.md docs/roadmap.md
git commit -m "perf(hotpath): micro-benchmarks and docs for cmdid/bufpool"
git push origin main
```

---

## Self-Review

- [x] Spec coverage：命令 ID 表、缓冲池、接入 server、benchmark、文档均有任务。
- [x] Placeholder scan：无 TBD；每个宏/函数给出实现示例。
- [x] Type consistency：`cmd_id` 为 `uint16_t`，`CMD_ID_UNKNOWN=0`；缓冲池 API 尺寸参数一致。
- [x] 跨平台：缓冲池纯标准 C；server 中无新增平台宏。
