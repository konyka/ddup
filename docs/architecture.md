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

## 过期设计（Phase 4）

- **存储**：`db.expires` 为第二张 rh_table，key → 8 字节绝对过期时刻
  （wall-ms，`pal_wall_ms()`）。主表与 expires 表严格同生共死：
  覆盖写（SET/INCR/APPEND/MSET）与 DEL 同时清除过期项。
- **惰性过期**：所有命令的 key 查找经 `db_expire_if_needed()`，过期即删
  并计入 `expired_keys`。
- **主动过期**：server 每 100ms（`pal_now_ms`）跑一轮 `db_active_expire()`：
  从 expires 表随机采样至多 20 个 key（随机桶 + 前向扫描），删除已过期者；
  单轮过期率 >25% 则继续，至多 10 轮。
- **时间注入**：命令分发入口为 `command_execute_at(db, argv, argc, out,
  now_ms)`，`command_execute()` 传真实墙钟；单测全部用合成时间，无 sleep。

## 淘汰设计（Phase 4）

- **LRU 时钟**：`rh_entry.meta`（表本身不解释）存 24 位秒级时钟
  `(now_ms/1000) & 0xFFFFFF`，创建/访问时刷新（`rh_touch`）。
- **内存记账**：`db.used_memory` 增量维护，每个存活条目计
  `sizeof(rh_entry) + 16（malloc 开销）+ klen + vlen`，主表与 expires 表
  同口径；set/del/expire/persist/flush 时增减，无全表扫描。
- **淘汰策略**：`db.maxmemory`（0=不限，默认 0）+ `db.maxmemory_policy`
  （默认 allkeys-lru）。allkeys-lru：命令执行后若超限，循环采样 5 个
  key 淘汰时钟最旧者，直至达标，计 `evicted_keys`。noeviction：写命令
  （SET/MSET/INCR/DECR/APPEND）在超限时直接返回 OOM 错误。
- **采样随机源**：db 内置 xorshift32（`rng_state`），测试可固定种子，
  淘汰场景确定性复现。

## 命令清单

PING ECHO GET SET(NX/XX/EX/PX) DEL UNLINK EXISTS INCR DECR APPEND STRLEN
MGET MSET ｜ EXPIRE PEXPIRE EXPIREAT PEXPIREAT TTL PTTL PERSIST ｜
DBSIZE FLUSHDB CONFIG(GET/SET maxmemory, maxmemory-policy) INFO

注：TTL 返回值四舍五入（(rem+500)/1000，同 Redis）；PTTL 精确到 ms。
INCR/APPEND 在本实现中清除 TTL（与 Redis 保留 TTL 不同，有意简化）。
DBSIZE 为 O(1)，可能计入尚未回收的过期 key。

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
