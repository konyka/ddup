# C 标准自适应能力层设计文档

## 背景与目标

ddup 的构建系统已经能在 C23→C17→C11→C99 之间自动探测最高可用标准，并把 `DDUP_C_STD` 注入源码。但当前代码里对高级特性的使用是散点式的（只有 `DDUP_HAS_C_ATOMICS`），缺少一套统一、可测试、可降级的“能力层”。

本设计参考 Garnet 的“按平台/运行时选择最优实现”思想，为 ddup 增加 **C 标准自适应能力层**：在构建期/编译期探测编译器实际支持的 C 标准特性，并为每个特性提供“最优原生实现 + C99 降级路径”的封装宏。后续热路径（RESP 解析、命令分发表、存储引擎）可以基于这些宏写出标准无关、性能最优的代码。

## 范围（本次子项目）

- 扩展 `cmake/DetectCStandard.cmake`，探测并导出以下布尔能力宏：
  - `DDUP_HAS_C_ATOMICS`（`<stdatomic.h>`）
  - `DDUP_HAS_C_THREADS`（`<threads.h>`）
  - `DDUP_HAS_ALIGNAS`（`alignas` / `<stdalign.h>`）
  - `DDUP_HAS_STATIC_ASSERT`（`static_assert` / `_Static_assert`）
  - `DDUP_HAS_TYPEOF`（C23 `typeof` / GNU `__typeof__`）
  - `DDUP_HAS_CONSTEXPR`（C23 `constexpr`）
  - `DDUP_HAS_STDCKDINT`（C23 `<stdckdint.h>` 溢出检测算术）
  - `DDUP_HAS_BITINT`（C23 `_BitInt(n)`）
  - `DDUP_HAS_NORETURN`（C11 `noreturn` / `_Noreturn`）
  - `DDUP_HAS_THREAD_LOCAL`（C11 `thread_local` / `_Thread_local`）
- 新增 `src/pal/pal_cstd.h`：统一封装上述能力，所有特性都有 C99 降级路径。
- 新增 `tests/test_cstd.c`：TDD 测试，覆盖每个宏的正向行为、降级语义、跨标准一致性。
- 支持 `DDUP_C_STD_FORCE` CMake 选项，强制使用指定 C 标准，便于在本地验证 C99 降级路径。
- 更新文档：`docs/architecture.md`、`docs/roadmap.md`。
- 所有变更严格遵循：先写失败测试 → 实现 → `ctest` 全绿 → `git commit` → `git push origin main`。

## 非目标

- 不替换现有存储引擎、命令表或网络层；本层是后续改造的**基础接口**。
- 不引入 C++、Rust 或其他语言。
- 不在 `src/pal/` 之外使用平台宏（`#ifdef _WIN32` 等）。

## 方案对比

| 方案 | 描述 | 优点 | 缺点 |
|---|---|---|---|
| A. 渐进式改造 ddup（推荐） | 在现有仓库里新增 `pal_cstd.h` 与 CMake 探测，作为后续 Garnet 化的底座 | 复用现有 PAL/RESP/测试/CI；风险低；每步可独立验证 | 需要与现有代码保持兼容 |
| B. 新建仓库从零重写 | 在 `E:/work/ddup-next` 重新搭 Garnet C 版 | 不受既有代码约束 | 重复已有大量工作（RESP、集群等），交付周期长，浪费 |
| C. 只做探测不做封装 | 仅扩展 CMake 输出宏，不统一封装 | 改动小 | 散落的能力宏无法被热路径安全使用，后续重复 `#if` 逻辑 |

**推荐方案 A**。ddup 已经实现了 RESP 兼容、跨平台 PAL、集群/复制/持久化等大量工作，推倒重来的收益无法覆盖成本。本次聚焦“C 标准自适应能力层”，是最小、最有价值的第一步，也直接回应“根据 C 标准不同，检测最适合方案”的要求。

## 架构

```
┌─────────────────────────────────────────┐
│  上层模块（resp / core / ds / server）   │  使用 ddup_* 宏，不再关心 C 标准
├─────────────────────────────────────────┤
│  src/pal/pal_cstd.h                     │  统一封装：能力检测 + 最优实现 + 降级
├─────────────────────────────────────────┤
│  cmake/DetectCStandard.cmake            │  标准探测 + 特性探测 + 编译定义
└─────────────────────────────────────────┘
```

所有能力封装宏命名以 `ddup_` 为前缀，能力检测宏以 `DDUP_HAS_` 为前缀。调用方只依赖 `pal_cstd.h`，不直接包含 `<stdatomic.h>`、`<stdalign.h>` 等标准头（封装内部按需包含）。

## 关键接口（部分示例）

```c
#include "pal/pal_cstd.h"

/* 静态断言：C11+ 用 static_assert，C99 用数组大小技巧 */
ddup_static_assert(sizeof(int) == 4, "int must be 4 bytes");

/* 对齐：C11+ 用 alignas，否则用编译器扩展或忽略 */
ddup_alignas(64) char cache_line[64];

/* noreturn：C11+ 用 _Noreturn/noreturn，否则用 __attribute__((noreturn)) 等 */
DDUP_NORETURN void ddup_die(const char *msg);

/* thread_local：C11+ 用 _Thread_local/thread_local，否则用 __thread/__declspec(thread) */
ddup_thread_local int thread_id;

/* typeof：C23 用 typeof，GNU 用 __typeof__，否则不可用（宏返回 0） */
ddup_typeof(int) x = 42;

/* constexpr：C23 用 constexpr，否则退化为 const */
ddup_constexpr int max_keys = 1000000;

/* 溢出检测加法：C23 用 ckd_add，否则用分支/内建 fallback */
int r;
bool overflow = ddup_add_overflow(INT_MAX, 1, &r);

/* 原子操作：C11+ 用 stdatomic.h，否则用编译器内建或 pthread_mutex 兜底 */
ddup_atomic_int counter;
ddup_atomic_init(&counter, 0);
int v = ddup_atomic_fetch_add(&counter, 1, ddup_memory_order_relaxed);
```

完整接口在实现阶段通过 TDD 逐步敲定，确保“每个宏都有测试”。

## 测试策略

- `tests/test_cstd.c` 注册为 `test_cstd`，通过 `ddup_add_test()` 加入 CTest。
- 测试覆盖：
  1. `DDUP_C_STD` 取值在合法集合内。
  2. 每个 `DDUP_HAS_*` 宏为 0 或 1。
  3. `ddup_static_assert` 正向编译通过；通过 `#ifdef` 控制临时验证负向用例在 CI 中编译失败（可选，不默认开启）。
  4. `ddup_alignas` 产生预期对齐（`alignof` / `_Alignof` 或运行时地址校验）。
  5. `ddup_thread_local`：多线程场景下每个线程看到独立值（测试线程用 pthread/Windows thread，封装在 pal 中）。
  6. `ddup_typeof`：声明变量并赋值，验证类型正确。
  7. `ddup_constexpr`：用于数组长度或 static 初始化。
  8. `ddup_add_overflow`：正溢出、负溢出、正常加法都返回正确结果和标志。
  9. `ddup_atomic_*`：多线程自增到目标值，验证原子性。
- 本地可用 `-DDDUP_C_STD_FORCE=99` 强制走 C99 降级路径，确保封装仍有可用语义。

## 跨平台约束

- 所有 `#ifdef _WIN32` / `__linux__` / `__APPLE__` / `__FreeBSD__` 限制在 `src/pal/` 内。
- 编译器适配覆盖：MSVC（CL）、Clang、GCC；MinGW 视同 Windows。
- 探测逻辑优先使用 `__has_include`（C17 起，但 Clang/GCC/MSVC 均支持），再回退到标准版本号判断。

## 性能考量

- 本层全部为编译期宏/内联封装，运行时零开销。
- `ddup_add_overflow` 在 C23 下直接使用 `ckd_add`，在旧标准下优先使用 `__builtin_add_overflow`（GCC/Clang）或 `_addoverflow_*`（MSVC intrinsics），C99 兜底为分支检测。
- `ddup_atomic_*` 在 C11 下使用 `<stdatomic.h>` 内存顺序参数；降级路径按 `memory_order_seq_cst` 语义实现，保证正确性。

## 风险与回退

- 某些特性（如 `_BitInt`）可能探测结果与编译器 bug 不一致：通过 `__has_include` + 小片段编译测试（`check_c_source_compiles`）双重确认。
- 若某个封装宏导致警告，立即修复或增加 `__extension__` / `__pragma` 包装；保持 `-Wall -Wextra -Wpedantic` / `/W4` 干净。
- 若 CI 平台不支持 C23，不影响 C99/C11 路径；C23 代码被 `#if DDUP_C_STD >= 23` 门控。

## 文档更新

- `docs/architecture.md`：在“C 标准自适应”小节补充 `pal_cstd.h` 能力矩阵。
- `docs/roadmap.md`：新增 Phase 8 “C 标准自适应与 Garnet 化底座”，完成后打勾。
- 本文档提交到 `docs/superpowers/specs/2026-08-05-cstd-adaptive-design.md`。

## 后续衔接

完成本层后，下一步子项目候选：
1. **稳定命令 ID 表**：参考 Garnet `RespCommandHashLookup`，把命令名映射到稳定 ID，便于 AOF/复制/集群序列化。
2. **缓冲池/arena 升级**：参考 Garnet `LimitedFixedBufferPool`，减少热路径 malloc。
3. **io_uring 后端**：在 `pal_event` 下新增 Linux io_uring 路径。
