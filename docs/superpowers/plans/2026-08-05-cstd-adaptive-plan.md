# C 标准自适应能力层实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 ddup 增加编译期 C 标准能力探测与统一封装层，让上层代码按“最优实现 + C99 降级”写出跨标准、零开销、可测试的代码。

**Architecture:** 扩展 `cmake/DetectCStandard.cmake` 输出 `DDUP_HAS_*` 编译定义；新增 `src/pal/pal_cstd.h` 统一封装所有特性；新增 `tests/test_cstd.c` 用 TDD 覆盖每个宏；最后更新架构/路线图文档。

**Tech Stack:** CMake ≥ 3.20, C99–C23, GCC/Clang/MSVC, CTest, ddup 自研测试框架。

## Global Constraints

- 代码必须兼容 C99 语法下限；使用 C11+ 特性时经 `pal_cstd.h` 的能力宏检测并提供降级路径。
- 任何平台相关代码必须走 `src/pal/` 抽象；`src/pal/` 之外禁止 `#ifdef _WIN32` 等平台宏。
- `-Wall -Wextra -Wpedantic`（MSVC `/W4`）下不允许新增警告。
- 任何新功能/模块先写失败测试，再实现，测试全绿才算完成。
- 每小步验证通过后立即 `git commit` 并 `git push origin main`。
- 测试一律通过 `ddup_add_test()` 注册进 CTest。

## 文件结构

| 文件 | 职责 |
|---|---|
| `cmake/DetectCStandard.cmake` | 探测 C 标准版本 + 探测/导出 10 项细粒度能力宏 + 支持 `DDUP_C_STD_FORCE` |
| `src/pal/pal_cstd.h` | 统一能力封装头：静态断言、对齐、noreturn、thread_local、typeof、constexpr、溢出算术、原子操作 |
| `tests/test_cstd.c` | TDD 测试：每个宏正向行为、降级语义、跨标准一致性 |
| `docs/architecture.md` | 补充 C 标准自适应能力矩阵 |
| `docs/roadmap.md` | 新增 Phase 8 并勾选 |

---

### Task 1: 扩展 CMake 特性探测

**Files:**
- Modify: `cmake/DetectCStandard.cmake`

**Interfaces:**
- Consumes: `CMAKE_C_COMPILE_FEATURES`, `CMAKE_C_COMPILER_ID`
- Produces: cache `DDUP_C_STD`, compile definitions `DDUP_C_STD=<n>` and `DDUP_HAS_C_*=0|1`, option `DDUP_C_STD_FORCE`

- [ ] **Step 1: 写失败测试预期**

在 `tests/test_cstd.c` 里（先创建空文件占位即可，真正测试在 Task 5），预期能读取 `DDUP_HAS_C_STATIC_ASSERT` 等宏。当前这些宏不存在，测试会编译失败。

- [ ] **Step 2: 修改 `cmake/DetectCStandard.cmake`**

顶部增加强制选项：

```cmake
option(DDUP_C_STD_FORCE "Force a specific C standard (99/11/17/23); 0=auto" 0)
if(DDUP_C_STD_FORCE)
    set(DDUP_C_STD ${DDUP_C_STD_FORCE})
    set(_ddup_forced_std TRUE)
else()
    # 原有探测逻辑 ...
endif()
```

保留原有 C23→C17→C11→C99 的自动探测，但在得到 `DDUP_C_STD` 后，追加以下探测（优先 `__has_include`，再回退版本号）：

```cmake
include(CheckIncludeFile)
include(CheckCSourceCompiles)

macro(ddup_detect_feature name cond)
    if(${cond})
        set(DDUP_HAS_C_${name} 1)
    else()
        set(DDUP_HAS_C_${name} 0)
    endif()
    add_compile_definitions(DDUP_HAS_C_${name}=${DDUP_HAS_C_${name}})
endmacro()

ddup_detect_feature(ATOMICS    DDUP_C_STD>=11 AND HAVE_STDATOMIC_H AND NOT DEFINED __STDC_NO_ATOMICS__)
ddup_detect_feature(THREADS    DDUP_C_STD>=11 AND HAVE_THREADS_H)
ddup_detect_feature(ALIGNAS    DDUP_C_STD>=11)
ddup_detect_feature(STATIC_ASSERT DDUP_C_STD>=11)
ddup_detect_feature(NORETURN   DDUP_C_STD>=11)
ddup_detect_feature(THREAD_LOCAL  DDUP_C_STD>=11)
ddup_detect_feature(TYPEOF     DDUP_C_STD>=23)
ddup_detect_feature(CONSTEXPR  DDUP_C_STD>=23)
ddup_detect_feature(STDCKDINT  DDUP_C_STD>=23 AND HAVE_STDCKDINT_H)
ddup_detect_feature(BITINT     DDUP_C_STD>=23)
```

对 `STDCKDINT` 和 `BITINT` 增加 `check_c_source_compiles` 片段验证，避免编译器虚标支持。

- [ ] **Step 3: 配置并验证 CMake 能生成定义**

Run:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
```
Expected: 配置成功，输出包含 `DDUP_C_STD=...` 和各项 `DDUP_HAS_C_*`。

- [ ] **Step 4: Commit + push**

```bash
git add cmake/DetectCStandard.cmake tests/test_cstd.c
git commit -m "build(cstd): probe C standard feature capabilities and DDUP_C_STD_FORCE"
git push origin main
```

---

### Task 2: 静态断言 / 对齐 / noreturn / thread_local 封装

**Files:**
- Create: `src/pal/pal_cstd.h`
- Modify: `tests/test_cstd.c`

**Interfaces:**
- Produces:
  - `DDUP_HAS_C_*` mirrors from CMake
  - `ddup_static_assert(expr, msg)`
  - `ddup_alignas(n)`
  - `DDUP_NORETURN` (attribute-like macro for function declaration)
  - `ddup_thread_local`

- [ ] **Step 1: 写失败测试**

```c
/* tests/test_cstd.c */
#include "test.h"
#include "pal/pal_cstd.h"

static void test_static_assert_compiles(void)
{
    ddup_static_assert(1 == 1, "trivially true");
}

static void test_alignas_works(void)
{
    ddup_alignas(64) char buf[64];
    DD_CHECK(((uintptr_t)&buf & 63) == 0);
}

static int tls_counter;
static void thread_inc(void *arg)
{
    (void)arg;
    ddup_thread_local int local = 0;
    local++;
    tls_counter += local;
}

int main(void)
{
    DD_RUN(test_static_assert_compiles);
    DD_RUN(test_alignas_works);
    return DD_TEST_SUMMARY();
}
```

Run:
```bash
cmake --build build --target test_cstd
```
Expected: 编译失败，`pal/pal_cstd.h` 不存在。

- [ ] **Step 2: 实现 `src/pal/pal_cstd.h`（第一部分）**

```c
#ifndef DDUP_PAL_CSTD_H
#define DDUP_PAL_CSTD_H

#include "pal/pal_platform.h"
#include <stdint.h>

/* --- static_assert --- */
#if DDUP_HAS_C_STATIC_ASSERT
#  include <assert.h>
#  define ddup_static_assert(expr, msg) static_assert(expr, msg)
#else
#  define ddup_static_assert(expr, msg) \
     typedef char DDUP_GLUE(_ddup_static_assert_, __LINE__)[(expr) ? 1 : -1]
#  define DDUP_GLUE(a, b) a ## b
#endif

/* --- alignas --- */
#if DDUP_HAS_C_ALIGNAS
#  include <stdalign.h>
#  define ddup_alignas(n) alignas(n)
#elif defined(__GNUC__) || defined(__clang__)
#  define ddup_alignas(n) __attribute__((__aligned__(n)))
#elif defined(_MSC_VER)
#  define ddup_alignas(n) __declspec(align(n))
#else
#  define ddup_alignas(n) /* no alignment support */
#endif

/* --- noreturn --- */
#if DDUP_HAS_C_NORETURN
#  include <stdnoreturn.h>
#  define DDUP_NORETURN noreturn
#elif defined(__GNUC__) || defined(__clang__)
#  define DDUP_NORETURN __attribute__((__noreturn__))
#elif defined(_MSC_VER)
#  define DDUP_NORETURN __declspec(noreturn)
#else
#  define DDUP_NORETURN
#endif

/* --- thread_local --- */
#if DDUP_HAS_C_THREAD_LOCAL
#  include <threads.h>
#  define ddup_thread_local thread_local
#elif defined(__GNUC__) || defined(__clang__)
#  define ddup_thread_local __thread
#elif defined(_MSC_VER)
#  define ddup_thread_local __declspec(thread)
#else
#  define ddup_thread_local /* no thread-local support */
#endif

#endif /* DDUP_PAL_CSTD_H */
```

- [ ] **Step 3: 运行测试并确认通过**

```bash
cmake --build build --target test_cstd
ctest --test-dir build -R test_cstd --output-on-failure
```
Expected: `test_cstd` 通过。

- [ ] **Step 4: Commit + push**

```bash
git add src/pal/pal_cstd.h tests/test_cstd.c CMakeLists.txt
git commit -m "feat(cstd): add pal_cstd.h with static_assert/alignas/noreturn/thread_local"
git push origin main
```

---

### Task 3: typeof / constexpr / stdckdint 溢出检测封装

**Files:**
- Modify: `src/pal/pal_cstd.h`
- Modify: `tests/test_cstd.c`

**Interfaces:**
- Produces:
  - `ddup_typeof(expr)` — C23/GNU typeof；无支持时展开为 `int` 并触发 static_assert？不，直接不可用，调用方需 `#if DDUP_HAS_C_TYPEOF`。
  - `ddup_constexpr` — C23 `constexpr`，否则退化为 `const`。
  - `ddup_add_overflow(a, b, res)`, `ddup_sub_overflow`, `ddup_mul_overflow` — 返回 `bool`。

- [ ] **Step 1: 写失败测试**

```c
static void test_typeof(void)
{
#if DDUP_HAS_C_TYPEOF
    int x = 5;
    ddup_typeof(x) y = x;
    DD_CHECK_EQ_INT(5, y);
#endif
}

static void test_constexpr(void)
{
    ddup_constexpr int arr_size = 4;
    char arr[arr_size];
    arr[0] = 'a';
    DD_CHECK(arr[0] == 'a');
}

static void test_add_overflow(void)
{
    int r;
    DD_CHECK(!ddup_add_overflow(10, 20, &r));
    DD_CHECK_EQ_INT(30, r);
    DD_CHECK(ddup_add_overflow(INT_MAX, 1, &r));
}
```

Run:
```bash
cmake --build build --target test_cstd
```
Expected: 编译失败，宏未定义。

- [ ] **Step 2: 实现封装**

在 `pal_cstd.h` 中追加：

```c
#include <limits.h>
#include <stdbool.h>

/* --- typeof --- */
#if DDUP_HAS_C_TYPEOF
#  define ddup_typeof(expr) typeof(expr)
#elif defined(__GNUC__) || defined(__clang__)
#  define ddup_typeof(expr) __typeof__(expr)
#else
#  define ddup_typeof(expr) int /* unsupported; caller should gate with DDUP_HAS_C_TYPEOF */
#endif

/* --- constexpr --- */
#if DDUP_HAS_C_CONSTEXPR
#  define ddup_constexpr constexpr
#else
#  define ddup_constexpr const
#endif

/* --- checked arithmetic --- */
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
    if (b > 0 && a > INT_MAX - b) return true;
    if (b < 0 && a < INT_MIN - b) return true;
    *r = a + b;
    return false;
}
#  define ddup_add_overflow(a, b, r) ddup_add_overflow_int((a), (b), (r))
#  define ddup_sub_overflow(a, b, r) /* ... similar ... */ ddup_sub_overflow_int((a), (b), (r))
#  define ddup_mul_overflow(a, b, r) ddup_mul_overflow_int((a), (b), (r))
#endif
```

补齐 `ddup_sub_overflow_int` / `ddup_mul_overflow_int` 的 C99 fallback 实现。

- [ ] **Step 3: 运行测试并确认通过**

```bash
cmake --build build --target test_cstd
ctest --test-dir build -R test_cstd --output-on-failure
```
Expected: 通过。

- [ ] **Step 4: Commit + push**

```bash
git add src/pal/pal_cstd.h tests/test_cstd.c
git commit -m "feat(cstd): add typeof/constexpr/checked-arithmetic wrappers"
git push origin main
```

---

### Task 4: 原子操作封装

**Files:**
- Modify: `src/pal/pal_cstd.h`
- Modify: `tests/test_cstd.c`

**Interfaces:**
- Produces:
  - `ddup_atomic_int`
  - `ddup_atomic_init(p, v)`, `ddup_atomic_load(p, mo)`, `ddup_atomic_store(p, v, mo)`, `ddup_atomic_fetch_add(p, v, mo)`, `ddup_atomic_fetch_sub(p, v, mo)`
  - `ddup_memory_order_*` 常量

- [ ] **Step 1: 写失败测试**

```c
static ddup_atomic_int atomic_counter;

static void atomic_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        ddup_atomic_fetch_add(&atomic_counter, 1, ddup_memory_order_relaxed);
    }
}

static void test_atomic_fetch_add(void)
{
    ddup_atomic_init(&atomic_counter, 0);
    /* spawn 4 threads each doing 1000 increments */
    pal_thread_t t[4];
    for (int i = 0; i < 4; i++) pal_thread_create(&t[i], atomic_worker, NULL);
    for (int i = 0; i < 4; i++) pal_thread_join(t[i]);
    DD_CHECK_EQ_INT(4000, ddup_atomic_load(&atomic_counter, ddup_memory_order_relaxed));
}
```

如果 `pal_thread_*` 还不存在，先用 `pthread_create`/`WaitForSingleObject` 等，但这段测试必须被 PAL 抽象；若当前没有 `pal_thread.h`，可以在 `tests/test_cstd.c` 里临时用 `#if DDUP_OS_POSIX` 走 pthread，`#elif DDUP_OS_WINDOWS` 走 Win32 thread，并记为后续应抽取到 `pal_thread.h` 的技术债。

Run:
```bash
cmake --build build --target test_cstd
```
Expected: 编译失败，原子宏未定义。

- [ ] **Step 2: 实现原子封装**

```c
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

/* C99 fallback：优先使用编译器原子内建；无内建的平台才保留单线程降级。
 * ddup 的 PAL 线程安全构建应在此类平台上显式禁用 mt。 */
#  define ddup_atomic_init(p, v) (*(p) = (v))
#  define ddup_atomic_load(p, mo) (*(p))
#  define ddup_atomic_store(p, v, mo) (*(p) = (v))
#  define ddup_atomic_fetch_add(p, v, mo) ((*(p) += (v)) - (v))
#  define ddup_atomic_fetch_sub(p, v, mo) ((*(p) -= (v)) + (v))

#endif
```

备注：GCC/Clang 和 MSVC 的 C99 构建分别使用 `__atomic`/Interlocked，保证
多线程 mt 状态安全；无原子内建的平台仍需显式限制为单线程。

- [ ] **Step 3: 运行测试并确认通过**

```bash
cmake --build build --target test_cstd
ctest --test-dir build -R test_cstd --output-on-failure
```
Expected: 通过。

- [ ] **Step 4: Commit + push**

```bash
git add src/pal/pal_cstd.h tests/test_cstd.c
git commit -m "feat(cstd): add atomic operation wrappers with C11 and C99 fallbacks"
git push origin main
```

---

### Task 5: 强制 C99 路径验证

**Files:**
- No new files; reuse existing ones.

**Interfaces:**
- Consumes: `DDUP_C_STD_FORCE` option from Task 1

- [ ] **Step 1: 使用 C99 重新配置构建**

```bash
cmake -S . -B build-c99 -DCMAKE_BUILD_TYPE=Release -G Ninja -DDDUP_C_STD_FORCE=99
cmake --build build-c99 --target test_cstd
ctest --test-dir build-c99 -R test_cstd --output-on-failure
```
Expected: `test_cstd` 在 C99 路径下仍能通过。

- [ ] **Step 2: 检查无新增警告**

```bash
cmake --build build-c99 --target test_cstd 2>&1 | grep -i warning || true
```
Expected: 无相关警告。

- [ ] **Step 3: Commit + push**

无需代码变更；若 CI 通过即可。如有问题回到 Task 2-4 修复。

---

### Task 6: 更新文档

**Files:**
- Modify: `docs/architecture.md`
- Modify: `docs/roadmap.md`

**Interfaces:**
- Produces: 更新的 C 标准自适应说明和 Phase 8 完成标记。

- [ ] **Step 1: 更新 `docs/architecture.md`**

在“C 标准自适应”小节后追加：

```markdown
### C 标准能力矩阵（Phase 8）

`cmake/DetectCStandard.cmake` 在构建期探测编译器支持的最高 C 标准，并导出细粒度能力宏：

| 能力 | 宏 | C 标准 | C99 降级 |
|---|---|---|---|
| 原子操作 | `DDUP_HAS_C_ATOMICS` | C11 `<stdatomic.h>` | GCC/Clang `__atomic` 或 MSVC Interlocked；无内建时单线程降级 |
| 线程 | `DDUP_HAS_C_THREADS` | C11 `<threads.h>` | 平台原生线程（PAL 封装） |
| 对齐 | `DDUP_HAS_C_ALIGNAS` | C11 | `__attribute__((aligned))` / `__declspec(align)` |
| 静态断言 | `DDUP_HAS_C_STATIC_ASSERT` | C11 | 数组大小技巧 |
| typeof | `DDUP_HAS_C_TYPEOF` | C23/GNU | 不可用 |
| constexpr | `DDUP_HAS_C_CONSTEXPR` | C23 | `const` |
| 溢出算术 | `DDUP_HAS_C_STDCKDINT` | C23 `<stdckdint.h>` | `__builtin_*_overflow` / 分支检测 |
| noreturn | `DDUP_HAS_C_NORETURN` | C11 | 编译器属性 |
| thread_local | `DDUP_HAS_C_THREAD_LOCAL` | C11 | `__thread` / `__declspec(thread)` |

上层代码统一包含 `src/pal/pal_cstd.h`，使用 `ddup_*` 前缀宏，不再直接依赖具体 C 标准。
```

- [ ] **Step 2: 更新 `docs/roadmap.md`**

在 Phase 7+ 长期项之前插入：

```markdown
- [x] **Phase 8 — C 标准自适应与 Garnet 化底座**
  细粒度 C 标准能力探测（atomics/threads/typeof/constexpr/stdckdint 等）、
  `src/pal/pal_cstd.h` 统一封装与 C99 降级路径、`test_cstd` 覆盖、
  支持 `DDUP_C_STD_FORCE` 本地验证。
```

- [ ] **Step 3: 全量测试**

```bash
cmake --build build --target check
```
Expected: 全部测试通过。

- [ ] **Step 4: Commit + push**

```bash
git add docs/architecture.md docs/roadmap.md
git commit -m "docs(cstd): document C standard capability matrix and Phase 8"
git push origin main
```

---

## Self-Review

- [x] Spec coverage：设计文档中的 10 项能力、强制 C99 验证、文档更新均有对应任务。
- [x] Placeholder scan：无 TBD/TODO；所有宏都给出实现示例。
- [x] Type consistency：`ddup_atomic_*` 签名、内存顺序常量、溢出函数返回值在各任务中保持一致。
- [x] 跨平台：`src/pal/` 内使用平台宏；测试线程临时用 PAL/平台原语，并标注技术债。
