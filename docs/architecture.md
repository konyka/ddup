# ddup 架构设计

参考 Garnet 的整体分层，用 C 重实现。本文档随代码演进同步更新。

## 分层

```
┌─────────────────────────────────────┐
│  RESP 命令层 (src/server, src/core) │  命令分发、参数解析、响应写出
├─────────────────────────────────────┤
│  存储层 (src/core, src/ds)          │  哈希表、过期、复杂数据结构
├─────────────────────────────────────┤
│  协议层 (src/resp)                  │  RESP2/RESP3 零拷贝解析/写出
├─────────────────────────────────────┤
│  网络层 (src/server + src/pal)      │  连接管理、事件循环
├─────────────────────────────────────┤
│  平台抽象层 PAL (src/pal)           │  socket/事件/线程/原子/时间
└─────────────────────────────────────┘
```

## 关键设计决策（对齐 Garnet，性能优先）

1. **Thread-per-core 无共享**：每个 IO 线程独立运行事件循环，连接上的解析、
   命令执行、存储访问全部在 IO 线程就地完成，避免线程切换与数据搬运。
2. **平台最优 IO 模型（readiness 模式已落地）**：
   - Linux：epoll（level-triggered）；io_uring（内核 ≥ 5.10）列为后续优化
   - macOS / FreeBSD：kqueue
   - Windows：select()（FD_SETSIZE 提升至 1024）；IOCP 列为后续优化
   - 统一抽象为 `pal_event`：`pal_loop_add/mod/del/wait`，事件携带
     fd + userdata + readable/writable。
3. **零拷贝 RESP 解析**：解析结果直接引用接收缓冲，不落盘复制；
   批量（pipelining）命令在一次 recv 缓冲内连续解析。
4. **内存管理**：热路径禁止逐次 malloc；arena + 对象池复用。
5. **主存哈希表**：Robin Hood 开放寻址 + 增量 rehash，缓存友好。
6. **C 标准自适应**：构建期探测 C23→C17→C11→C99，取最高可用标准；
   原子操作优先 C11 `<stdatomic.h>`，缺失时降级平台原生 API。

## 目录结构

```
src/pal/     平台抽象：pal_platform(宏), pal_time, pal_socket(TCP), pal_event(事件循环)
src/resp/    RESP 协议（Phase 1）
src/core/    KV 存储、哈希表、过期、命令分发（Phase 2/4）
src/ds/      Hash/List/Set/ZSet（Phase 5）
src/server/  连接与服务器主循环（Phase 3）：单线程事件循环、recv 缓冲按需增长、
             解析→执行→推进零拷贝流水线；当前连接为阻塞 socket（单次 recv +
             阻塞发送循环），非阻塞写出缓冲随 thread-per-core 阶段引入
tests/       单元测试（test.h 自研框架）+ 集成测试
bench/       压测客户端 ddup-bench（Phase 3，非 ctest 目标）
```
