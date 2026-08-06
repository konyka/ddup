# AGENTS.md — ddup 开发约定

## 项目概述

ddup：参考微软 Garnet 设计，用 C（C99 起步，自动探测至 C23）重实现的 RESP 兼容
缓存存储。性能优先，跨平台（Windows/Linux/macOS/FreeBSD/Unix）。

## 构建与测试

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # 或 cmake --build build --target check
```

本地默认工具链示例（Windows）: `cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang`

## 硬性约定

1. **TDD**：任何新功能/模块先写失败测试，再实现，测试全绿才算完成。
2. **每小步验证通过后立即 `git commit` 并 `git push origin main`**（用户已授权）。
   提交信息用英文、conventional commits 风格（如 `feat(resp): ...`、`test(core): ...`）。
3. **C 标准**：代码必须兼容 C99 语法下限；使用 C11+ 特性（如 stdatomic）时经
   `pal_platform.h` 的 `DDUP_HAS_*` 宏检测并提供降级路径。禁止 C++ 特性。
4. **跨平台**：任何平台相关代码必须走 `src/pal/` 抽象，禁止在 `src/pal/` 之外
   出现 `#ifdef _WIN32` / `__linux__` 等平台宏。
5. **性能优先**：热路径（解析、命令执行、哈希表）禁止逐次 malloc/free；
   用 arena/对象池。改动热路径后在 `docs/performance.md` 补基准数字（如适用）。
6. **警告**：`-Wall -Wextra -Wpedantic`（MSVC `/W4`）下不允许新增警告。
7. **文档同步**：改架构更新 `docs/architecture.md`；完成阶段勾选 `docs/roadmap.md`；
   本文件约定变化时更新本文件。
8. 测试一律通过 `ddup_add_test()` 注册进 CTest。
9. **deps/ 第三方源码**：原样 vendor（任何补丁必须记录在对应 `deps/<name>/PATCHES.md`），
   保留原始许可证；第三方目标不挂项目警告旗标，C 标准按其上游要求放宽。

## 目录

见 `docs/architecture.md`。
