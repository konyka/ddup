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
2. **平台最优 IO 模型**：
   - Linux：epoll（level-triggered）；io_uring 双模式——readiness
     （Phase 14）与 op 模式 proactor（Phase 32b）
   - macOS / FreeBSD：kqueue
   - Windows：select()（FD_SETSIZE 提升至 1024）；IOCP proactor
     （pal_iocp，completion 模型，Phase 7.5）
   - 统一抽象为 `pal_event`：`pal_loop_add/mod/del/wait`，事件携带
     fd + userdata + readable/writable；proactor 后端（IOCP /
     io_uring op）共享 server.c 的 completion 路径。
3. **零拷贝 RESP 解析**：解析结果直接引用接收缓冲，不落盘复制；
   批量（pipelining）命令在一次 recv 缓冲内连续解析。
4. **内存管理**：热路径禁止逐次 malloc；arena + 对象池复用。
   外部 RESP 请求接收缓冲默认上限为 1 GiB（`proto-max-request-bytes`），
   复制快照接收默认上限同为 1 GiB（`repl-max-snapshot-bytes`）；两者在
   缓冲扩容或快照分配前检查，避免不受控的资源消耗。
5. **主存哈希表**：Robin Hood 开放寻址 + 增量 rehash，缓存友好。
6. **快照完整性**：哈希表插入在分配前拒绝不能存入 `uint32_t` 或会导致
   `size_t` 溢出的长度；快照序列化对所有编码长度和追加操作做检查，失败时
   回滚输出缓冲区，原子保存不会创建临时文件或替换已有快照。
7. **C 标准自适应**：构建期探测 C23→C17→C11→C99，取最高可用标准；
   原子操作优先 C11 `<stdatomic.h>`，缺失时降级平台原生 API。

### C 标准能力矩阵（Phase 8）

`cmake/DetectCStandard.cmake` 在构建期探测编译器支持的最高 C 标准，并导出细粒度能力宏：

| 能力 | 宏 | 启用标准 | C99 降级 |
|---|---|---|---|
| 原子操作 | `DDUP_HAS_C_ATOMICS` | C11 | 单线程语义占位（多线程场景后续加锁或 intrinsics） |
| 线程头 | `DDUP_HAS_C_THREADS` | C11 | 平台原生线程（PAL 封装） |
| 对齐 | `DDUP_HAS_C_ALIGNAS` | C11 | `__attribute__((aligned))` / `__declspec(align)` |
| 静态断言 | `DDUP_HAS_C_STATIC_ASSERT` | C11 | 数组大小技巧 |
| noreturn | `DDUP_HAS_C_NORETURN` | C11 | 编译器属性 |
| thread_local | `DDUP_HAS_C_THREAD_LOCAL` | C11 | `__thread` / `__declspec(thread)` |
| typeof | `DDUP_HAS_C_TYPEOF` | C23 | 不可用（调用方需门控） |
| constexpr | `DDUP_HAS_C_CONSTEXPR` | C23 | `const` |
| 溢出算术 | `DDUP_HAS_C_STDCKDINT` | C23 | `__builtin_*_overflow` / `long long` 分支检测 |
| _BitInt | `DDUP_HAS_C_BITINT` | C23 | 不可用 |

上层代码统一包含 `src/pal/pal_cstd.h`，使用 `ddup_*` 前缀宏（如 `ddup_static_assert`、`ddup_alignas`、`ddup_atomic_*`、`ddup_add_overflow`），不再直接依赖具体 C 标准。`pal_platform.h` 中遗留的 `DDUP_HAS_C_ATOMICS` 探测仅在 CMake 未定义时生效，避免与构建期探测冲突。

## 命令 ID 表与缓冲池（Phase 9）

- **命令 ID 表**：`src/core/command.c` 维护一张统一的 `cmd_entry` 表，所有
  命令名经 `cmd_resolve(name, len)` 解析为稳定 16-bit ID（case-insensitive，
  开放寻址哈希）。命令分发由原先的字串比较链改为 `cmd_id` 整数比较；
  写命令判定、最小/最大参数个数、参数奇偶校验等元数据均通过 ID 直接索引，
  O(1)。未知命令快速返回 `CMD_ID_UNKNOWN`。
- **lean GET/SET（Phase 36）**：plain 会话（已认证、非 MULTI、未订阅、
  集群关；SET 另限无选项且非只读副本）在 session 层 resolve 一次后
  直接执行，跳过 dispatch 层的二次 resolve、READONLY/ownership 包装
  与 if 链；语义与通用块逐条一致（含 dirty→aof_log 传播与 LRU 驱逐
  尾），cmd_calls 仍计数、usec 计时不计。
- **缓冲池**：`src/core/buf_pool.{h,c}` 提供单线程、无锁分层固定大小缓冲池：
  4 KiB / 16 KiB / 64 KiB / 256 KiB 四档。`buf_pool_get(size, &actual)` 返回
  不小于请求大小的缓冲（命中空闲链则复用，否则 malloc），`buf_pool_put`
  按实际尺寸归还给对应空闲链；超过最大档位的请求直接 malloc/free。
- **接入点**：
  - `resp_buf` 新增 `pool` 指针；设置后 `resp_buf_reserve` 从池中取缓冲、
    扩容量时拷贝并归还旧缓冲，`resp_buf_free` 归还而非释放。
  - `server` 拥有 `buf_pool pool`；`conn_create` 从池中借 64 KiB 接收缓冲，
    并把 `c->out.pool` 指向服务器池；`conn_rbuf_grow()` 统一处理接收缓冲
    扩容（`server_run_once`、`server_run_once_iocp`、`repl_link_service`）。
  - 连接关闭时 `conn_free` 把接收缓冲与 out 缓冲归还池中，避免热路径
    malloc/free。
- **收益与限制**：单线程借/还为零系统调用，64 KiB 档位微基准约 3.9G ops/s；
  cmd_resolve 约 83M ops/s。缓冲池目前与 server 生命周期绑定，未跨 server
  共享；后续 thread-per-core 阶段每个 IO 线程可保留独立池。

## SIMD 解析与 socket 调优（Phase 10）

- **SIMD CRLF 扫描**：`src/pal/pal_simd.h` 提供 `ddup_find_crlf()`，在
  x86_64 SSE2 与 ARM64 NEON 下用 16 字节向量比较加速 `\r` 扫描，无 SIMD
  时回退到原 `memchr` 循环。`resp_parser.c` 统一经该助手查找 CRLF，语义
  与标量参考实现完全一致（测试覆盖 basic/CR-not-LF/edge/random）。
- **解析微基准**：`bench_core` 增加 parse-only 阶段，隔离解析器成本：
  SET ~31M ops/s、GET ~42M ops/s，说明解析器不是当前瓶颈。
- **socket 调优**：`pal_set_tcp_nodelay()` 封装 TCP_NODELAY；服务器在
  readiness/IOCP accept、复制 master link、集群 bus connect 上默认禁用
  Nagle，降低小回复 loopback 延迟。监听 backlog 从 128 提升到 511（与
  Redis 默认一致）。
- **批量发送调查**：`conn_flush` 的输出为单块连续缓冲，pub/sub 与复制
  fan-out 按连接独立冲刷；在当前单线程模型下 `writev` 或跨连接批量聚合
  不能减少系统调用次数，未落地。该优化点保留给 thread-per-core 阶段的
  per-worker 输出合并。

## Thread-per-core（Phase 11，mt_server）

对齐 Garnet 的 shared-nothing 设计：`ddup-server --io-threads N`（N>1）
启用 `src/server/mt_server.{h,c}`；默认 N=1 保持原单线程路径。

- **线程模型**：1 个 acceptor 线程持有唯一公网 listener（非阻塞 +
  pal_loop），accept 后按 round-robin 把 fd 投入目标 worker 的 accept
  队列；N 个 worker 线程各运行一个独立的 readiness 事件循环（复用
  `server`，经 `server_close_listener`/`server_adopt_fd` 接入），每个
  worker 拥有自己的 db 与 buf_pool，单 worker 内命令路径零锁。
- **跨线程基础设施**（PAL）：`pal_thread`（Win32 `_beginthreadex` +
  CRITICAL_SECTION + CONDITION_VARIABLE / POSIX pthread）、`pal_wakeup`
  （POSIX socketpair / Windows loopback TCP 对的 self-pipe）。worker 的
   wakeup fd 注册进自己的事件循环（`server_set_wakeup`），回调里统一
   drain accept/inbox/completion 三个队列。队列为互斥保护的单链表；
   生产者在成功入队后都尝试 kick，由 pending 原子标志合并重复唤醒，避免
   丢失并发入队的通知。
- **key 路由**：命令经 `server_set_route` 安装的钩子拦截
  （conn_process_input 内）。worker = `hash_slot(key) % nworkers`（复用
  集群 crc16/hashtag）。单 key 命令（字符串/过期/hash/list/set/zset 全部
  单 key 操作）路由到属主 worker；多 key 命令（MGET/MSET/DEL/UNLINK/
  EXISTS/SMOVE）所有 key 必须同属一个 worker，否则 `-CROSSSLOT`（与集群
  语义一致）。无 key 命令就地执行；DBSIZE/FLUSHDB/INFO 为广播聚合
  （home 就地一份 + 扇出其余 worker）。
- **任务与顺序**：跨 worker 命令以**原始 RESP 字节拷贝**打包为 mt_task
  （目标 worker 就地重新解析，避免逐元素深拷贝）；同一 parse pass 内
  目标相同的连续命令**合并为一个任务**（span>1）。回复经 home worker
  的 completion 队列返回；每个 conn 维护 seq 序号 + reorder 缓冲，本地
  命令在有未决路由回复时也算好答案暂存，完成回复按 seq 追加到
  conn->out，保证流水线回复顺序与请求一致。队列全部为**按生产者拆分
   的无锁 SPSC 环**（C11 acquire/release 原子操作；强制 C99 构建降级为
   互斥环），每次成功入队都尝试 kick，由 pending 标志去重。
 - **生命周期**：conn 的 pending/closing 由 home worker 的 `pending_mu`
  保护（跨线程投递可安全 test-and-increment）；连接关闭时有未决工作
  则成为 zombie（摘出事件循环、关 fd，保留 conn/session/mt_state），
   最后一项工作回收时由 mt 层释放（per-worker zombie 清单兜底）。
 - **io_uring provided buffers**：pbuf storage is page-aligned and sized for
   the ring header plus every descriptor. Registration is published only after
   initialization. Before teardown, the PAL submits a raw cancel-all request
   and waits for its CQE, then unregisters with a zeroed `io_uring_buf_reg`
   containing only `bgid`; pbuf storage is released only after successful
   unregister. The owner stops and joins all PAL users before freeing a ring.
- **任务对象池（Phase 31）**：单命令路由任务的对象经 home worker 的
  自由列表回收复用（互斥锁保护；跨线程回收仅限 UNWATCH/UNSUB 等
  少数路径），命令字节两级内联（conn 态 batch_inline 暂存 + 任务态
  inline_buf 携带，≤256B 全程零分配）；多命令批、大载荷、特殊任务
  保持堆路径。池空回退 malloc、池满回退 free。销毁顺序（记录在案
  的修复）：先对全部 worker 置 pool_off 再排空环与池——否则后排
  空把任务回收到互斥锁已销毁的 worker 池里。
- **背压与唤醒（Phase 27/29/30）**：kick 去重（pending 原子标志合并
  重复 kick）；SPSC head/tail 分缓存行；环满退避为自排干 +
  sched_yield（且必须检查 running——否则关停 join 永远等不到退出
  的 worker，进程活而不哑的 CI wedge 根因）。inbox/completion 环
  8192 槽。
- **事务（mt 层实现）**：MULTI/EXEC/DISCARD/WATCH/UNWATCH 由路由层接管
  （session 不进入 MULTI）。EXEC 要求所有排队命令与 WATCH key 同属一个
  worker（否则 -EXECABORT），打包为单个 bundle 在属主 worker 上重新
  校验 watch 版本并顺序重放。db.watch_refs 在 key 属主 worker 上增减，
  UNWATCH/DISCARD/连接关闭经 fire-and-forget 任务跨 worker 释放。
- **pub/sub**：频道按 hash_slot(channel) 归属 worker；SUBSCRIBE/
  UNSUBSCRIBE 由 home worker 即时确认并向属主注册/注销；PUBLISH 路由到
  频道属主，属主把预构造的 message 推帧扇出到各订阅者 home worker。
  投递经 pending 计数保护，且在投递时按 home 订阅表复核（与
  UNSUBSCRIBE 的竞态）。
- **连接-键亲和**：干净的连接（无未决任务/批次/事务/watch/订阅）在
  首个带 key 命令时**一次性迁移**到 key 属主 worker（fd/session/db
  指针/钩子上下文整体 rehome），之后该连接的命令全部就地执行。具有
  客户端 key 局部性的负载（hashtag、按用户前缀）零跨线程流量。
- **持久化**：每 worker 独立 `<dir>/worker-<id>-<file>`；路由任务的
  mutation 经 dirty 计数记录到执行 worker 的 AOF；SAVE/LASTSAVE 广播
  聚合（LASTSAVE 取 max）；`FLUSHDB/FLUSHALL/SWAPDB` 在广播归并的
  home 端与各 follower 都按实际变更记录到本 worker AOF；启动时按
  worker 重放/加载。
- **聚合命令**：DBSIZE（求和）、FLUSHDB、SAVE、LASTSAVE（max）、
  SWAPDB、INFO 广播到全部 worker 归并。INFO 走**结构化归并**：每个
  worker 执行内部变体 `INFO __STATS__`（机器格式：标量 `k:v` 行 +
  `db:<i>:<keys>:<expires>` + `c:<id>:<calls>:<usec>`，覆盖该 worker
  全部逻辑库），home 端把各部分累加进 `info_stats`（used_memory/
  expired/evicted/dbsize 求和、按库与按命令 id 合并），再由共享的
  `command_info_render()` 渲染为单份人类可读 INFO（maxmemory/策略/
  cluster 标志取 home worker 值）。
- **限制（记录在案）**：mt 模式下 SHUTDOWN/MIGRATE/
  PSUBSCRIBE/PUNSUBSCRIBE 返回 `-ERR command not supported in mt mode`；
  `SCAN` 使用带 worker 索引的复合游标顺序遍历各分片；
  `KEYS` 广播并合并各 worker 的 RESP 数组，`RANDOMKEY` 广播后由 home
  worker 返回首个非空 key；
  `SYNC/PSYNC/REPLICAOF/SLAVEOF/CLUSTER` 已由
  worker 0 控制面支持（SYNC/PSYNC 分类到 worker 0，master 侧全量快照
  见 mt 复制/集群适配）。INFO # Replication 由 home 端从 worker 0 的
  `repl_info` 渲染（mt master 暴露 connected_slaves/master_repl_offset，
  mt 副本暴露 master_host/master_link_status）。
- **并发可靠性**：跨 worker 队列满时，生产者背压重试会**自排空本
  worker 的 inbox/completion 队列**以打破环形等待；drain 按环限批
  （512 条/次）并在有剩余时自 kick，避免持续生产下的 drain 活锁。
- **性能现状**：见 docs/performance.md Phase 11。

## AUTH 与多数据库（Phase 13）

- **AUTH**：`requirepass` 配置（server_set_requirepass / mt 每 worker）。
  session 默认 authed=1；配置了密码的 server 在 accept 时置 0，分发入口
  对未认证连接只放行 AUTH/QUIT（`-NOAUTH Authentication required.`），
  mt 路由层前置同样的门（未认证命令不路由）。
- **多数据库（16 库）**：session 增加 `db_index + sel_fn/sel_ctx/sel_ndbs`
  选择钩子（server 提供 `srv_select_db`，栈 session 无钩子即单库）。
  SELECT 校验范围并切换 `session.d`；SWAPDB 原子交换两个逻辑库（含
  keyvers/flush_epoch，使 WATCH 正确失效）。server 持有
  `extra_dbs[15]`（db0 内嵌）。AOF 对非当前库的命令前补 `SELECT <n>`
  前缀（重放经 aof_replay_session 恢复原路由）；快照升级为 `DDUP0002`
  多库格式（兼容加载 DDUP0001）。INFO 输出 Redis 风格 `dbN:keys=...`
  段；主动过期与 maxmemory 覆盖全部逻辑库；maxmemory 为全局限额
  （逐库相同值，Redis 语义）。mt 下 db_index 随路由任务传递，SWAPDB
  广播到全部 worker 执行。
- **commandstats**：db 增加 `cmd_calls[128]/cmd_usecs[128]`（按命令 id），
  分发入口用 `pal_now_us()` 计时累加；INFO # Commandstats 输出 Redis
  风格 `cmdstat_<name>:calls=,usec=,usec_per_call=`。A/B 实测开销 <1%
  （pal_now_us 单次 ~19.5ns）；`DDUP_NO_CMDSTATS` 编译开关可完全移除。

## 服务端自省与 SLOWLOG（Phase 60）

- `COMMAND COUNT/LIST/INFO/GETKEYS/DOCS` 直接遍历稳定命令 ID 表（`CMD_TABLE`）
  生成元数据，不引入运行时额外字符串查找；`GETKEYS` 与集群/mt 路由共用同一套
  命令键位规则。
- `CLIENT ID/SETNAME/GETNAME/LIST/KILL` 通过 session 钩子访问 server 连接表；
  连接 id 单调递增，name 内联在 `conn` 中（无额外堆分配）。`KILL` 只标记
  `close_after_send`，待当前回复刷出后关闭目标连接。
- `MEMORY USAGE/STATS` 复用既有增量内存记账与对象额外内存估算，无热路径逐次
  遍历；`PURGE/MALLOC-STATS` 为无分配占位兼容响应。
- `SLOWLOG` 采用 128 条环形缓冲（新条目尾插、读取时逆序），每条深拷贝命令
  argv；默认阈值 10000us，`server_set_slowlog_threshold()` 可调（0 记录全部）。
  计时复用 commandstats 的 `pal_now_us`，无慢日志时不增加时钟读取。
- 自省/容器族统一补齐 `HELP`（`COMMAND/CLIENT/MEMORY/SLOWLOG/OBJECT/CONFIG/
  SCRIPT/PUBSUB/CLUSTER`），返回各容器当前实现子命令的 Redis 风格数组，走
  冷路径、无热路径分配。
- `BGSAVE` 复用 `SAVE` 的 `snapshot_save[_multi]` 路径，单线程模型下同步落盘并
  返回 Redis 风格启动确认；`BGREWRITEAOF` 当前为 AOF 强制 flush 兼容实现，
  无 fork/双文件 rewrite（内存缓存存储定位，记录在案）。

## 兼容性收尾（Phase 65–67）

- **阻塞 pop 族**：`BLPOP/BRPOP/BRPOPLPUSH/BLMOVE/BLMPOP/BZPOPMIN/BZPOPMAX/
  BZMPOP` 在命令分发层记录 `session.blocked`（key 列表、截止时间、重放
  argv 深拷贝），server 就绪循环经 `command_blocked_try` 在数据可读或超时
  到期时重试；mt 模式不路由这些命令（记录在案）。
- **容器子命令补全**：`CLIENT/CLUSTER/COMMAND/CONFIG/OBJECT/SCRIPT` 的
  Redis 7.2.15 子命令名全部注册；`COMMAND GETKEYSANDFLAGS` 复用既有
  GETKEYS 键位表并按命令写标志返回 `RW/RO` 近似标志。
- **管理/复制容器**：`WAIT/WAITAOF` 在共享无副本模型下返回 0 或
  `[0,0]`；`REPLCONF` 做握手应答；`FAILOVER` 在无副本时返回明确错误；
  `MONITOR` 通过 server-owned subscription stream 输出后续命令事件。
  `ACL/LATENCY/MODULE/SENTINEL/DEBUG` 注册为
  容器并提供最小空视图或错误应答（无扩展模块、无 ACL 文件、无 Sentinel
  拓扑），避免未知命令中断客户端；所有应答均为冷路径、无热路径分配。

## Stream 核心族（Phase 61）

- **表示**：新增 `DDUP_OBJ_STREAM`，对象 `obj_stream`（src/ds/stream.h）
  为**插入序连续 entry 数组**。合法 `XADD` 只能追加单调递增 ID，因此普通
  追加为 O(1) 摊还（容量倍增）；区间查询用 `obj_stream_lower_bound` 二分
  定位，区间发射 O(log N + K)。每条 entry 的 field/value 字节存放在一个
  连续块中，`lens[]` 记录各段长度，避免逐字段散落分配。
- **ID 语义**：`*`/`ms-*` 自动 seq 与 Redis 一致（同 ms 自增、跨 ms 归零）；
  显式 ID 必须严格大于 `last_id`；`XDEL/XTRIM` 不降低 `last_id`，`XSETID`
  可推进并回填 `entries_added/max_deleted_id` 元数据。
- **修剪**：`MAXLEN`/`MINID` + 可选 `LIMIT` 从前端批量释放并 `memmove`
  收缩；`=` 与 `~` 当前采用同一精确语义（记录在案：无 radix-tree 节点粒度，
  因此不做 Redis `~` 的过量修剪）。
- **命令覆盖**：`XADD/XLEN/XRANGE/XREVRANGE/XDEL/XTRIM/XSETID`；
  `XRANGE/XREVRANGE` 支持 `-`/`+`、`(` 排他边界与 `COUNT`。TYPE 报告
  `stream`；快照/DUMP/RESTORE 已支持该对象类型。
- **消费组/读取族（Phase 62）**：`XGROUP/XACK/XPENDING/XCLAIM/
  XAUTOCLAIM/XREAD/XREADGROUP/XINFO`。group/consumer/PEL 均为连续数组，
  `group_mem` 只记结构体、名称与 PEL 元素，不记 capacity 数组的暂存空间；
  `XREADGROUP` 游标严格 `>`，NOACK 不写 PEL；`XREAD` 普通读取按
  `obj_stream_lower_bound` 扫描，COUNT 限制发射数量。
- **阻塞语义**：`XREAD`/`XREADGROUP` 的 `BLOCK` 参数进入统一
  `session.blocked` 状态；server 就绪循环通过轻量 readiness 检查在新 entry
  到达时唤醒，截止时间到期返回 Null array，`BLOCK 0` 表示无限等待。检查只
  扫描 stream 元数据和二分定位，不提前物化 RESP 回复；group 模式保留
  `>` 游标、NOACK、COUNT 及 NOGROUP 校验语义。
- **快照扩展**：STREAM payload 在 entry 数组后追加必选 group 块
  （group→consumer→PEL），加载器同样按块读取，保证 stream 后仍可跟
  其他 key；旧版无 group 块的 STREAM 快照不再兼容（记录在案）。

## io_uring 后端（Phase 14，Linux）

- **探测与接入**：`pal_loop_create_iouring()` 在 Linux 上以直接
  syscall（无 liburing 依赖）建环并做 NOP 探测；不可用时返回 NULL，
  server 回落 epoll。`--io iouring` / `SERVER_BACKEND_IOURING`，
  非 Linux 为 stub。test_event/test_server 检测到 io_uring 时整套
  跑第二遍（Linux CI 覆盖，内核 6.x）。
- **模型**：io_uring 在此用作**异步就绪源**（不是全异步 IO）：注册表
  ep_reg 记录每个 fd 的 want_read/want_write；oneshot poll 完成后按
  最近兴趣集 re-arm；`sq_tail/cq_head` 用 `__atomic_*` acquire/release
  同步。
- **关键语义（记录在案）**：reap 只认 `res > 0`（POLL* 掩码）为事件；
   `res == 0` 是 POLL_REMOVE/POLL_UPDATE 的**控制回执**，`res < 0` 为
   错误。del = deactivate + POLL_REMOVE + dead 链（loop_free 统一释放）；
   mod = remove + add。TIMEOUT sqe 的 `__kernel_timespec` 在 enter 同步
   提交期内有效（栈上安全）。环与 SQE 映射均使用内核返回的实际大小创建、
   保存并以同一大小释放。

## io_uring op 模式后端（Phase 32b，Linux）

Phase 14 的 io_uring 是 epoll 替代品（readiness）；op 模式是真正的
proactor——提交 IORING_OP_RECV/SEND/ACCEPT 操作本身，完成携带结果。

- **API 与共享路径**：`pal_iouring_op.{h,c}` 镜像 `pal_iocp.h`（op
  kind 数值一致，server.c 静态断言保证）；server.c 的 proactor 路径
  （accept → 建 conn + post recv；RECV 完成 → parse→execute →
  kick_flush → 补投 recv；SEND 完成 → 推进 out_sent 续发）经 pro_*
  分发被 IOCP 与 io_uring op 两种后端共享，事件归一化为同一形状。
  `--io iouring-op` / `SERVER_BACKEND_IOURING_OP`，仅 st；mt workers
  保持 readiness（记录在案，不接）。TLS/集群总线维持 readiness 限定。
  内核不支持 io_uring 时回落 epoll/select；非 Linux 为不可用后端 stub。
- **完成关联**：cqe 只有 user_data——op kind 编进低 3 位（指针至少
  8 对齐），解tag 得 op + conn/listen cookie。超时经 IORING_OP_TIMEOUT
  （count=1 提前退役）；超时参数存放在环拥有的固定节点池，节点只在对应
  CQE 被消费后复用，提前被其他完成唤醒或 enter 失败都不会留下栈悬垂指针。
- **multishot accept**：优先 IORING_ACCEPT_MULTISHOT（5.19+）——单个
  accept 请求持续产出 ACCEPT 完成（F_MORE），补投调用在其存活期间为
  no-op；首个 -EINVAL 完成自愈降级为单发补投（同 IOCP 的完成即补挂
  模型）。接受的 fd 带 SOCK_NONBLOCK。
- **SQE 批处理**：recv/send/accept 只入队不提交，wait 时一次
  io_uring_enter（submit all + 按需 GETEVENTS）刷出；跨线程唤醒
  （IORING_OP_NOP）立即提交。互斥锁不跨阻塞 enter 持有，否则唤醒
  投递永远无法把 NOP 送进环（死锁）。SQ 槽位在互斥锁内先保留并完整
  填写 SQE 与间接数组，最后才用 release store 推进 `sq_tail`；因此
  SQPOLL 内核线程与并发 enter 永远看不到半初始化请求。槽位可用量由
  acquire-load 的内核 `sq_head` 计算，不能用“已交给 enter”的游标推断；
  普通模式按 enter 实际返回的提交数推进游标并处理部分提交/失败，SQPOLL
  模式读取 `sq_flags`，需要时发 IORING_ENTER_SQ_WAKEUP，满环时以
  IORING_ENTER_SQ_WAIT 等待内核消费后再保留槽位。SQ_WAKEUP 的 enter
  失败会沿 publish/flush 返回公开投递 API 的 tri-state 结果（-1 未发布、
  0 已发布、1 已发布但需重试）；此时 SQE 已完整 release-publish，绝不会
  暴露半初始化请求，并仍可能由恢复后的 SQPOLL 线程消费。调用方对 1 必须
  保留 buffer/userdata/listener 所有权，不得重投，后续 wait/flush 会重试已
  发布的 tail。
- **关停语义**：`pal_iouring_close` = shutdown + close——在飞 recv
  立即以 0（对端 orderly）完成，send 以 -EPIPE 完成，io_uring 请求
  持有文件引用故 close 本身安全；完成按 user_data 回到 zombie 排水
  路径（与 IOCP 同一契约）。server_destroy 先释放 proactor（关环 =
  内核同步取消全部在飞请求）再释放 conn，保证没有内核写落入已释放
   的 rbuf/sbuf。SEND 带 MSG_NOSIGNAL。
- **协议错误关闭**：proactor 在排入 `-ERR Protocol error` 后标记连接为
  send-then-close，直到 detached send buffer 与新 out 均排空才执行
  shutdown + close；这保证 io_uring 的用户态 SQE 会先提交完成，避免
  close 抢先使错误回复变为 EOF。
- **测试**：test_iouring_op 以公开 API 覆盖 SQPOLL 请求下的完整 SQE
  发布（仅在查询确认 SQPOLL 实际启用后运行）、提前唤醒时的超时参数生命
  周期，以及单槽 provided-buffer 环的重复 buffer id/数据/recycle；环或
  pbuf UAPI 不可用时分别跳过。test_server 探测到 io_uring 时追加 op
  模式整轮（另有 DDUP_TEST_IOURING_OP_ONLY 单跑开关）；Ubuntu CI
  （内核 6.x）覆盖。
- **multishot recv + provided-buffer 环（Phase 33）**：每连接一次
  IORING_OP_RECV|IORING_RECV_MULTISHOT（SQE 设置 IOSQE_BUFFER_SELECT、
  `len=0`，buf_group 指向 IORING_REGISTER_PBUF_RING 注册的
  256×64KB 环），武装期间零补投；注册环内存按运行时页大小显式对齐，
  满足内核 UAPI 对 `ring_addr` 的页对齐要求。构建期编译探测同时引用
  pbuf ring/buf/reg 三个结构、所用字段及注册/选择/CQE 常量；旧或不完整
  Linux 头文件会把该优化编译掉，运行时注册失败仍回落单发 recv 补投。
  每个收到块一条 CQE（F_BUFFER 携带槽位 id，F_MORE 表示请求仍武装）。
  server 把块拷入 conn rbuf 后立刻 recycle 槽位；同一 socket 的 CQE
  按接收序到达（multishot 请求是 socket 接收队列的单一顺序消费者），
  按 CQE 序追加即精确。recycle 只重写 addr/len/bid 后 release-store tail，
  绝不写 `io_uring_buf.resv`（第 0 项该字段与共享 tail 别名）。环饥饿时
  请求以 -ENOBUFS 终态结束（无 F_MORE、
  无槽位）——server 识别后立即重武装而不是关连接。完成事件契约扩展
  err/buf_id/op_done：pending_ops 只在终态 CQE 归账；zombie 完成也
  必须回收槽位。master link 保持单发补投（复制流量大块、按需增长）。
  64KB 槽位对齐补投模型的 SERVER_RECV_CHUNK——16KB 会把每次管道突发
  切成多条 CQE（Phase 32a 批次碎片化教训）。DDUP_IOU_RECV_MS=0 回落
  补投模型（bench A/B 用）。
- **SQPOLL / DEFER_TASKRUN|SINGLE_ISSUER（Phase 33，探测+静默回落）**：
  `pal_iouring_create_ex(flags)`——F_SQPOLL 设 IORING_SETUP_SQPOLL
  （sq_thread_idle=1s，提交不再需要 enter）；F_DEFER 设
  DEFER_TASKRUN（UAPI bit 13）|SINGLE_ISSUER（taskwork 只在 enter 时跑，wait 因此
  每次必泵；该模式下 pal_iouring_post 仅允许属主线程调用——st 模式
  本就无人跨线程投递）。setup 失败即重试裸 flags。env 门控
  （DDUP_IOU_SQPOLL=1 / DDUP_IOU_DEFER=1），默认关。
- **未做（记录在案）**：registered/fixed send buffers 与 SEND_ZC——
  sbuf 按需 realloc 与注册缓冲的固定地址模型冲突（每次扩容要注销
  重注册），loopback 上 pin 开销相对拷贝本身微小；SEND_ZC 的通知
  CQE 对与缓冲生命周期规则同 sbuf 复用模式不兼容。评估结论：收益
  不确定、复杂度实在，跳过（详见 performance.md Phase 33）。

## mt 生产化（Phase 15）

- **mt TLS（15.2）**：acceptor 持有第二个（TLS）listener，accept 后按
  round-robin 把 fd 投入 worker 的 `accepts_tls` 队列；每个 worker 经
  `server_tls_ctx_init()` 装载**自己的** TLS ctx（shared-nothing，无跨
  线程 SSL_CTX 使用），`server_adopt_fd_tls()` 包装 fd 并在 worker 自己
  的 readiness 循环内驱动非阻塞握手（与单线程同一状态机）。
  config_validate 解除 mt+TLS 互斥；集群/复制由 worker 0 控制面适配。
  IOCP 后端不支持 TLS（回落 readiness workers）。
- **IOCP worker 后端（15.3，Windows）**：`mt_server_create_ex(...,
  SERVER_BACKEND_IOCP)`；main 在 Windows 默认用 IOCP workers（TLS 开启
  时回落 readiness，`--io iouring` 在 Linux 选 io_uring workers）。
  - 唤醒：`pal_iocp_post()` 投递 PAL_IOCP_WAKEUP 完成事件；
    `server_set_wakeup` 在 IOCP 下只存回调，worker 的任务队列 kick 经
    `server_wakeup_kick()` 转为完成事件投递（mt_kick 抽象两种后端）。
  - adopt/flush：`server_adopt_fd` 在 IOCP 下建 conn 并 post 首个
    WSARecv；`server_conn_flush` 走 kick_flush（重叠发送）。
  - 生命周期合并：conn_close 组合 mt zombie（路由层 pending 未清）与
    IOCP zombie（重叠操作未清）——两者都归零才真正释放
    （`zombie_mt_free` 标记 + `server_conn_free_now` 延迟）。
  - **连接迁移在 IOCP 后端禁用**（记录在案）：任何时刻都有在飞的重叠
    recv，连接无法安全跨完成端口移动；任务路由不受影响（亲和优化
    仅 readiness workers 享有）。
- **mt 复制/集群适配（15.4）**：worker 0 是唯一的复制/集群控制面。
  - 复制：`mt_server_replicaof()` 只把 `server_replicaof()` 作用在
    worker 0；全量快照先载入 worker 0 的临时 db，再把每个 key 用
    `snapshot_dump_key()` 转成 DUMP/RESTORE payload，经 `MT_TASK_RESTORE`
    投递给 `hash_slot(key) % nworkers` 的属主 worker，等全部完成才进入
    streaming；之后的 master 命令流走 worker 0 的 mt 路由，与客户端
    流量同一条分区路径。master link 连接不参与连接迁移。
  - **master 侧全量同步（15.4 续）**：`SYNC/PSYNC` 握手在 worker 0 的
    复制控制面完成，快照不再从其他 worker 的 db 跨线程读取。每个
    follower 经 `MT_TASK_REPL_SNAPSHOT` 在自己的事件循环内用
    `snapshot_serialize_multi()` 序列化本 worker 的 DDUP0002 快照；
    worker 0 就地序列化自身分片并排空 completion 环收集各分片，再经
    `snapshot_serialize_multi_buffers()` 封成单帧 `DDUPMT01` 发给副本。
    副本加载器按逻辑库合并所有分片（与 master 的 shared-nothing 分片
    解耦）。
  - **复制推流恰好一次**：普通路由命令由执行 worker 把原始 RESP 字节
    投递到 worker 0 的中央 backlog；`FLUSHDB/FLUSHALL/SWAPDB` 等广播
    聚合命令只由 home 端在全部 worker 成功归并后转发一次（follower
    子任务抑制重复转发）；`MOVE` 与 `MULTI/EXEC` 重放也在成功应用后
    逐条转发。
  - 集群：只有 worker 0 `server_enable_cluster()`（绑定总线、跑
    gossip/failover/nodes.conf 持久化）；`cluster_state_snapshot()`
    把节点/槽位/epoch 等元数据拷成不可变快照，`MT_TASK_CLUSTER_SYNC`
    按 SPSC 顺序扇出到 follower。follower 的 db 有完整 `slot_owner`/
    migrating/importing 表，因此任意 worker 都能在本地给出
    `MOVED/CLUSTERDOWN/ASK`；`server.cluster_control=0` 阻止 follower
    重复绑总线或保存 nodes.conf。`CLUSTER` 命令强制路由到 worker 0，
    `REPLICAOF/SLAVEOF` 直接操作 worker 0 master link。
- **批量写出（Phase 23）**：完成队列 drain 一轮内，同一连接的回复
  （已由 seq/reorder 排好序进入 conn->out）只在轮末统一 flush 一次，
  消除每完成项一次 send 调用的放大；pub/sub 推送同策。本机 c200 规模
  A/B 为噪声内持平（保留，零回归；收益场景是更高 fan-in）。

## 集群运维工具（Phase 16）

- **ddup-reshard**（tools/）：redis-cli `--cluster reshard` 风格：
  `--from host:port --to host:port --slot N [--count K] [--timeout ms]`。
  端口范围为 1..65535，槽号为 0..16383，count 至少为 1，timeout
  为非负且不超过平台 `int` 范围；所有数值参数必须是完整十进制串。
  流程：双端 CLUSTER MYID → 源 SETSLOT MIGRATING TO / 目标 SETSLOT
  IMPORTING FROM → 循环 GETKEYSINSLOT（每批 K 键）+ MIGRATE ... REPLACE
  KEYS（批量）→ 双端 SETSLOT NODE 收尾。失败时槽可能停留在
  MIGRATING/IMPORTING 态（与 redis-cli 一致，记录在案）。
- **结构**：阻塞式 RESP 客户端与编排逻辑在 `tools/reshard_client.[ch]`
  （复用 ddup_core 的 RESP parser/writer；零拷贝注意点：回复串指向连接
  缓冲，构造下一条请求时先行拷贝）；`tools/ddup-reshard.c` 仅参数
  解析。`tests/test_reshard.c` 用两个后台线程跑的集群节点做端到端
  集成（批量迁移、-MOVED、源槽清空）。

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

PING ECHO GET SET(NX/XX/EX/PX/KEEPTTL/GET) DEL UNLINK EXISTS INCR DECR INCRBY DECRBY
INCRBYFLOAT APPEND STRLEN MGET MSET GETDEL GETEX SETEX PSETEX GETSET
SETRANGE GETRANGE ｜ EXPIRE(NX/XX/GT/LT) PEXPIRE(NX/XX/GT/LT)
EXPIREAT(NX/XX/GT/LT) PEXPIREAT(NX/XX/GT/LT) TTL PTTL PERSIST
EXPIRETIME PEXPIRETIME ｜ TYPE KEYS SCAN RENAME RENAMENX TOUCH RANDOMKEY
COPY ｜
DBSIZE FLUSHDB CONFIG(GET/SET maxmemory, maxmemory-policy) INFO ｜
HSET HGET HDEL HEXISTS HLEN HGETALL HKEYS HVALS HMSET HMGET HINCRBY HSETNX
HSTRLEN HRANDFIELD ｜
LPUSH RPUSH LPUSHX RPUSHX LPOP(count) RPOP(count) LLEN LRANGE LINDEX LSET
LPOS LREM LTRIM RPOPLPUSH ｜
SADD SREM SISMEMBER SMISMEMBER SCARD SMEMBERS SPOP SRANDMEMBER SMOVE
SINTER SUNION SDIFF SINTERCARD SINTERSTORE SUNIONSTORE SDIFFSTORE ｜
ZADD(NX/XX/GT/LT/CH/INCR) ZSCORE ZCARD ZINCRBY ZREM
ZRANGE(REV/BYSCORE/BYLEX/LIMIT/WITHSCORES) ZREVRANGE ZRANK ZREVRANK ZCOUNT
ZRANGEBYSCORE ZREMRANGEBYSCORE ZPOPMIN ZPOPMAX ZREMRANGEBYRANK ZMSCORE
ZRANDMEMBER ZRANGEBYLEX ZREVRANGEBYLEX ZREMRANGEBYLEX ｜
BLPOP BRPOP BRPOPLPUSH BLMOVE BLMPOP BZPOPMIN BZPOPMAX BZMPOP ｜
MULTI EXEC DISCARD WATCH UNWATCH ｜ SUBSCRIBE UNSUBSCRIBE PUBLISH
PSUBSCRIBE PUNSUBSCRIBE
SSUBSCRIBE SUNSUBSCRIBE SPUBLISH
PUBSUB(CHANNELS/NUMSUB/NUMPAT/SHARDCHANNELS/SHARDNUMSUB/HELP) QUIT ｜
AUTH SELECT SWAPDB ｜ SAVE LASTSAVE SHUTDOWN ｜ SYNC REPLICAOF ｜
DUMP RESTORE RESTORE-ASKING MIGRATE ASKING ｜ EVAL EVALSHA EVAL_RO EVALSHA_RO
FCALL FCALL_RO FUNCTION(LOAD/DELETE/LIST/FLUSH/DUMP/RESTORE/STATS/KILL/HELP)
SCRIPT(LOAD/EXISTS/FLUSH/DEBUG/KILL/HELP) ｜
WAIT WAITAOF REPLCONF FAILOVER MONITOR ｜
ACL CAT DELUSER DRYRUN GENPASS GETUSER LIST LOAD LOG SAVE SETUSER USERS WHOAMI ｜
DEBUG ｜ LATENCY ｜ MODULE ｜ SENTINEL ｜
LOLWUT(VERSION 5/6) ｜ INFO（内部变体 INFO __STATS__ 供 mt 聚合）

注：TTL 返回值四舍五入（(rem+500)/1000，同 Redis）；PTTL 精确到 ms。
INCR/APPEND 在本实现中清除 TTL（与 Redis 保留 TTL 不同，有意简化）。
DBSIZE 为 O(1)，可能计入尚未回收的过期 key。

## 对象存储模型（Phase 5.1）

主表保持字节通用（rh_table 不解释值）；所有值均为带类型标签的 blob：

```
{1 字节类型标签}{payload}
  DDUP_OBJ_STRING: payload = 原始字符串字节
  DDUP_OBJ_HASH:   payload = 8 字节指针 -> obj_hash（小对象 listpack，
                   超阈值转嵌套 rh_table，见"紧凑编码"一节）
  DDUP_OBJ_LIST:   payload = 8 字节指针 -> obj_list（quicklist：
                   listpack 节点的双链表）
  DDUP_OBJ_SET:    payload = 8 字节指针 -> obj_set（小对象 listpack，
                   超阈值转 rh_table：member -> 空值）
  DDUP_OBJ_ZSET:   payload = 8 字节指针 -> obj_zset（小对象 listpack，
                   超阈值转 dict + skiplist）
  DDUP_OBJ_ARRAY:  payload = 8 字节指针 -> obj_array（稀疏 rh_table：
                   固定宽度 index -> 原始元素字节，length/count 常驻元数据）
```

- **所有权**：db 层拥有指针对象。任何覆盖/删除/过期/淘汰/FLUSHDB 路径
  经 `obj_free_value()` 释放对象；`obj_extra_mem()` 返回对象占用用于
  内存记账（均为近似值，每次 malloc 计 16 字节开销，不含嵌套表的
  slot 数组；各编码的具体布局见"紧凑编码"一节）。
- **类型错误**：字符串命令作用于 hash/list key（或反向）回复
  `-WRONGTYPE Operation against a key holding the wrong kind of value`。
  注：SET 覆盖其它类型在 Redis 中是允许的，本实现按 WRONGTYPE 处理
  （有意收紧）；MGET 对非字符串 key 先校验后统一报错（Redis 返回 null）。
- **空对象自动删除**：hash 字段清空 / list 弹空时 key 一并删除。
- 过期、淘汰、LRU touch 对对象值透明生效（共用 db 层路径）。

## 紧凑编码：listpack 与 quicklist（Phase 45）

- **listpack（src/ds/listpack）**：Redis 7 兼容的紧凑字节布局
  （4B 总长 + 2B 条目数 + 条目序列 + 0xFF）。条目编码：7-bit uint、
  13/16/24/32/64-bit int、6/12/32-bit 长度字符串，尾随变长 backlen
  支持反向遍历。规范形式的整数字符串（无前导零、无 "-0"、在
  int64 范围内）自动按整数存储；条目数达 0xFFFF 后计数转为
  "unknown"，`lp_length()` 退化为扫描。所有 mutator 可能 realloc，
  调用方须重新定位（与 Redis 同约定）。
- **quicklist（src/ds/quicklist）**：listpack 节点的双链表，每节点至多
  fill 个元素（默认 `DDUP_QL_FILL` 128，配置键
  `list-max-listpack-size`）；端点节点满则push 分裂新节点，空节点
  即摘除。删除路径上的稀疏合并（Phase 48）：删除后节点条目数低于
  fill/4 时，若与邻居合计不超过 2*fill 则合并（优先并入 next，
  否则并入 prev；fill<4 时阈值为 0 天然不触发）；ql_remove 的
  迭代器在合并后按序号 lp_seek 重定位，语义不变。与 Redis 差异：
  无压缩、仅删除路径触发合并（push 不产生稀疏节点）。
  list 元素访问走迭代器（seek 从近端跳块、双向遍历、
  remove 后迭代器落在后继）。
- **小对象双编码**：hash、zset、set 默认使用单个 listpack——
  hash 存 field/value 交替；zset 存 member/score（`%.17g` 十进制）
  交替并按 (score, member) 保序；set 直接存 member（插入序，lp_find
  查重）。阈值默认与 Redis 一致（128 条目、单值 64 字节），运行时可
  经 `{hash,zset,set}-max-listpack-{entries,value}` 配置键调整：
  进程级 `obj_limits` 全局（src/ds/obj.c），main 启动时一次性应用、
  之后只读（mt 无竞态）；entries 或 value 为 0 即关闭该类型的紧凑
  编码；宏 `OBJ_*_MAX_LISTPACK_*` 仅作默认值来源（Phase 47）。
  写入超阈值时
  一次性转换为全功能结构（rh_table / dict+skiplist），只单向转换
  不降回（同 Redis）。listpack 模式下范围/RANK/LEX 等操作为 ≤128
  条目的线性扫描；SPOP/SRANDMEMBER/HRANDFIELD/ZRANDMEMBER 直接按
  随机下标取元素（SPOP 先把 member 拷到栈再删除，规避 listpack
  realloc 悬垂）。
- **内存收益**（记账模型实测，128 条目小对象）：list -78%、
  hash -67%、zset -92%、set -81%（数字见 docs/performance.md
  Phase 45/46）。

## Set / ZSet 设计（Phase 5.2）

- **Set**：小 set 为 listpack 编码（见"紧凑编码"一节）；转换后
  `obj_set` 为一张 member -> 空值的 rh_table（去重由表保证）。
  SPOP/SRANDMEMBER 用 db 内置 xorshift 随机源：listpack 模式直接按
  随机下标取/弹（SPOP 逐个随机弹出，天然去重）；rh_table 模式先收集
  member 视图，部分 Fisher-Yates 后取前 k 个（SPOP 删除选中的）；
  count<0 时有放回抽样。
  SINTER/SUNION/SDIFF 结果为临时 rh_table（天然去重），SINTER/SDIFF 遇
  缺失 key 结果为空，SUNION 忽略缺失 key。
- **ZSet**：小 zset 为 listpack 编码（见"紧凑编码"一节）；转换后为
  Redis 风格 dict + skiplist。dict（rh_table）member -> 8 字节
  double，ZSCORE/ZINCRBY/ZREM 均 O(log N)；skiplist（src/ds/skiplist.c）
  维护 (score, member 字节序) 排序：分数升序，同分按 member 字典序
  （与 Redis 一致）。跳表带**逐层 span**（Redis zsl 同构，Phase 21）：
  ZRANK/索引访问 O(log N)；插入/删除维护各层 span（微基准 100k 成员：
  rank/at 查询 ~0.5ms → ~0.1µs 量级）。层数几何分布 p=1/4、上限 32，
  内部 xorshift32 随机源（确定性种子，测试可复现）。
- **分数格式**：`%.17g`（inf/-inf 原样），解析用 strtod 全量消费，
  NaN 一律拒绝（`ERR value is not a valid float`）；inf + -inf 的结果
  NaN 报 `ERR resulting score is not a number (NaN)`。
- **范围语法**：ZCOUNT/ZRANGEBYSCORE/ZREMRANGEBYSCORE 支持 `(x` 开区间
  与 `-inf`/`+inf`；范围比较只看 score（同分 member 同进同出）。

## Session、事务与发布订阅（Phase 5.3）

- **Session（src/core/session.h）**：每个连接一个执行上下文：db 指针 +
  MULTI 队列 + WATCH 条目 + pub/sub 钩子。分发入口
  `session_execute_at()`；`command_execute[_at]()` 以栈 session 包装，
  db 级旧测试零改动。server 在 accept 时创建 session、关闭时释放。
- **事务**：MULTI 后命令经静态 arity 表做入队时校验（未知命令/参数个数
  错误立即报错并置 multi_error，EXEC 回复 EXECABORT）；合法命令深拷贝
  入队（接收缓冲会复用），回复 +QUEUED。EXEC 顺序重放并打包为数组。
  MULTI/EXEC/DISCARD/WATCH 本身不入队。
- **WATCH**：db 内置 key 版本表 `keyvers`（key -> uint64 单调版本，
  删除/重建不复用）+ `flush_epoch`（FLUSHDB 递增）。所有写路径
  （db_set_kv/db_del_kv/惰性+主动过期/淘汰/对象原地修改经 mem_sync/
  FLUSHDB）bump 版本。EXEC 前逐条比对，失配回复 `*-1`。
  注意：keyvers 表只增不减（写过的 key 常驻一个版本条目），且不计入
  used_memory——有意的简化，记录于此。
- **发布订阅**：注册表在 server（`rh_table channels`：频道 -> 连接链表），
  不在 db。core 命令经 session 钩子（subscribe/unsubscribe/each_channel/
  publish/deliver）回调 server；推送直接写入订阅者 conn->out，
  server_run_once 末尾统一 flush 所有连接（单线程保证安全）。连接关闭
  自动退订。订阅态仅允许 (P)SUBSCRIBE/(P)UNSUBSCRIBE/PING/QUIT/SHUTDOWN。
  QUIT 回 `+OK` 后置 session.quit，conn_process_input 停止消费后续
  字节并标记 close_after_send——out 缓冲排空后经既有 send-then-close
  路径关闭连接（readiness/proactor 双后端、mt 本地路径同款语义，
  Phase 44；QUIT 之后的 pipelined 字节丢弃）。
- **模式订阅（Phase 43）**：PSUBSCRIBE/PUNSUBSCRIBE 与第三张注册表
  `rh_table patterns`（pattern -> 连接链表）+ conn `psubs` + session
  `npsub`，订阅/退订主体与普通频道共用 chan_subscribe/chan_unsubscribe。
  PUBLISH 在普通投递后线性扫 patterns 表、按 `ddup_glob_match` 匹配频道，
  每个匹配的 (conn, pattern) 对收一条 pmessage，返回值 = message 接收者数
  + pmessage (conn,pattern) 对数（Redis 语义；pattern 总量小，线性扫
  可接受，记录于此）。PUBSUB CHANNELS/NUMSUB/NUMPAT 走 session 钩子
  （pubsub_channels/channel_nsub/numpat；无钩子的栈 session 回空/0），
  NUMPAT 由 server 侧 numpats 计数器维护。订阅确认帧计数为
  nsub+nssub+npsub 总和。mt 模式不支持模式订阅（PSUBSCRIBE/
  PUNSUBSCRIBE 进 mt_is_blocked）；PUBSUB 自查子命令在 mt 下与
  SHARDCHANNELS 一致走 MT_PASS 本地执行（数据为本 worker 视角，记录在案）。

## 持久化（Phase 6）

- **配置**：redis 风格扁平配置文件（`key value` 行、`#` 注释、键大小写
  不敏感）+ `--key value` 命令行覆盖（src/core/config.c）。样例见根目录
  ddup.conf。
- **AOF（src/server/aof.c）**：格式即 RESP 命令流。session 分发钩子对比
  命令前后的 `db.dirty` 计数（db_touch_key/FLUSHDB 递增），发生变更就把
  **原始 argv** 重序列化为 RESP 数组追加到缓冲；每个 server 循环刷盘一次
  （stdio 缓冲）。`appendfsync always|everysec|no`（默认 everysec，
  Phase 44）：always 每次落盘后 `pal_file_sync`（POSIX fsync /
  Windows FlushFileBuffers）；everysec 单线程 wall-ms 节流、每秒至多
  一次且仅在有实际落盘字节时（不用 Redis 的 bio 线程，记录在案）；
  优雅退出在非 no 模式下保底最后 sync 一次。sync 失败与写/flush
  失败共用 fail-closed 闩锁。CONFIG GET/SET 未接 appendfsync
  （在 server/aof 层而非 db 层，需新跨层 hook，记录在案）。
   EXEC 按逐条命令记录（不写 MULTI 包装）。启动时 `appendonly yes` 且文件
   存在则先重放（容忍截断尾部）；损坏或无效命令帧会 fail closed，且重放
   先在临时 db 上完成，失败时不修改现有数据。运行中 AOF 写入/flush 失败
   会冻结失败缓冲、拒绝后续写命令并停止服务循环，避免确认不可持久化变更。
- **快照（src/core/snapshot.c）**：自有二进制格式 `DDUP0001`，逐 key 存
  {类型标签、key、绝对过期毫秒、按类型的 payload}，显式小端编码。保存
  原子化（写 `<path>.tmp` 后 rename 覆盖）。加载**全有或全无**：先解析
  到临时 db，截断/损坏返回 -1 且目标 db 不变；加载时跳过已过期 key。
  SAVE 命令同步落盘（BGSAVE 不做真后台，单线程下无意义，记录在案）。
- **启动优先级**：`appendonly yes` 时只走 AOF；否则加载 dbfilename
  快照（AOF 优先于 RDB，同 Redis）。`save N` 秒自动快照（dirty 变化才
  写）。优雅退出（SIGINT/SIGTERM/SHUTDOWN）：AOF 必定 flush；配置了
  save 间隔且 AOF 关闭时额外写一次最终快照。

## 复制（Phase 7.1 + Phase 12 PSYNC）

- **传播流**：分发层对每个成功应用的写命令（含 EXEC 内逐条）按原始
  argv 重序列化为 RESP 数组，经 server 复用缓冲 fan-out 到三类 sink：
  AOF、复制 backlog（环形缓冲，默认 1MB，`repl-backlog-size` 可调）、
  下游 replica 连接的 out 缓冲（run_once 末尾统一 flush）。
- **backlog 安全性**：`repl-backlog-size` 必须为非零且能表示为本机
  `size_t`；初始化或重配置分配失败会明确返回错误，重配置会保留原环形
  缓冲，不会留下不可用的零容量状态。溢出时按一次计算直接丢弃最旧字节，
  而不是逐字节淘汰；保留的字节流与绝对 offset 语义不变。
- **复制标识（Phase 12）**：每个实例启动时生成 40-hex `master_replid`
  （`cluster_gen_id`）；INFO replication 暴露 `master_replid` 与
  `master_repl_offset`（backlog 绝对 offset）。
- **PSYNC 握手**：replica 的 master link 一律发 `PSYNC <replid> <offset>`
  （首次为 `PSYNC ? -1`）。master 判定：replid 匹配且 offset 落在
  `[backlog.offset - backlog.len, backlog.offset]` 内 → 回复
  `+CONTINUE <replid>` 并仅把 backlog 尾部字节流追加给该连接（**部分
  重同步，无快照、无 db 清空**）；否则回复
  `+FULLRESYNC <replid> <offset>` + `$<len>\r\n<snapshot>` 帧（全量）。
  SYNC 旧路径保留兼容。
- **副本侧状态机**：记录上游 replid + 已应用 offset（`repl.master_offset`，
  传播流逐条推进）；link 断开每 500ms 以缓存的 replid/offset 重连尝试
  部分重同步；`REPLICAOF NO ONE` 与“同 master 重连”保留缓存，指向不同
  master 才清空。+CONTINUE 跳过 `db_flush`（本地多余数据得以保留，
  与 Redis 行为一致——记录在案：Redis 部分重同步同样不清库）。
- **链式复制（A→B→C）**：B 的 master link 会话应用命令时走同一
  `srv_propagate`，天然 fan-out 到 B 自己的 backlog 与下游 replicas，
  故 B 可直接作为 C 的 master（C 对 B 做 PSYNC，使用 B 的 replid/offset）。
- **快照帧接收修复（Phase 12 附带）**：>64 KiB 快照帧的接收改为“头部
  即时消费 + 帧体从 rbuf 前缘持续取”，修复了旧实现在跨 chunk 时的
  size_t 下溢崩溃（大快照全量同步此前不可用）。
- **简化（记录在案）**：无 REPLCONF ACK/心跳（断线靠读超时与写失败）、
  无 WAIT；backlog 环形覆盖后的旧 offset 一律回退全量（同 Redis）；
  落后超过 16MB 未读的 replica 连接被丢弃（须重新 SYNC/PSYNC）。
- **只读副本**：replica 角色下客户端写命令一律 `-READONLY ...`（静态
  写命令表判定），master link 的复制会话豁免。replica 的 AOF 照常记录
  传播来的命令（同 Redis appendonly 行为）。
- INFO 增加 # Replication 段：role、connected_slaves、master_replid、
  master_repl_offset、master_host/port/link_status。

## TLS（Phase 7.2）

- **可选依赖**：CMake `find_package(OpenSSL)` + `DDUP_TLS` 选项；找到则
  `DDUP_HAS_TLS=1` 并链接 OpenSSL::SSL/Crypto，否则 `pal_tls` 全部为
  stub（创建 ctx 返回 NULL，`tls-port` 启动报明确错误）。OpenSSL 头文件
  只出现在 `pal_tls.c` 与 `tests/test_tls.c`。
- **独立端口**：`tls-port`（默认 0=off）与明文端口并行监听；配置校验
  `config_validate()` 要求 cert/key 文件可读。conn 增加 `pal_tls*`，
  server 的所有读写经 conn_read/conn_write 包装分发到 TLS 或明文。
- **简化（记录在案）**：accept 上的握手为非阻塞式（事件循环内分步完成）；
  复制 master link 暂不支持 TLS。
- **Windows 状态（Phase 18 已解决）**：此前 test_tls 在 Windows CI 被禁用，
  根因是测试自身缺陷——plain 端口对照连接用阻塞 socket，首个 pal_recv
  在服务器处理前永久阻塞（Windows 上稳定复现，POSIX 靠时序侥幸通过），
  另有 PING 回复字节数断言笔误（6 应为 7）。修复后 test_tls 全平台
  启用；所有轮询循环带 15s 墙钟上限，回归时快速报 FAIL 而非 CI 超时。
  找到 OpenSSL 的构建才会注册 test_tls；当前 Linux/macOS CI job 会安装并
  运行该测试，FreeBSD CI 仅安装 CMake，因此构建 TLS stub 且不注册 test_tls。本地无 OpenSSL
   开发包时的验证方法（记录在案）：取 Git for Windows
  自带的 mingw OpenSSL 3.5.6 DLL，用源码包 + 仓库内 perl 桩模块跑
  OpenSSL Configure + dofile.pl 生成头文件，llvm-objdump/dlltool 生成
  COFF 导入库，再 `-DOPENSSL_*` 显式指向即可（build/openssl-dev，
  不提交）。

## 非阻塞写出（Phase 7.4）

- 所有 accept 的连接 `pal_set_nonblocking(fd, 1)`；读路径把 would-block
  视为"本轮无数据"（仅 0/硬错误才关闭）。
- 每个 conn 的回复先写入 out 缓冲（resp_buf），随后立即尽力 flush：
  pal_send/pal_tls_write 直到 would-block（普通 socket 经 pal_would_block
  判定，TLS 经 -2 约定）。剩余字节保留，conn 通过 pal_loop_mod 注册
  read+write；可写事件到达时继续 flush，全部发完改回 read-only。
  want_write 状态记忆避免重复 mod 系统调用。
- 慢副本（replica conn）pending 输出超过 16MB 仍按原策略丢弃（须重新
  SYNC）。SYNC 快照帧走同一 out 缓冲路径，无阻塞发送循环残留。
- 单线程服务器不再会被停滞客户端拖死：socket 级测试覆盖 2000 条流水
  命令、慢客户端并行服务、分片读取恢复。

## IOCP 后端（Phase 7.5，Windows）

- **后端选择**：`server_create_ex(host, port, backend)`；`--io select|iocp`
  配置项，默认运行时探测（Windows 用 IOCP，其他平台 select）。IOCP 不可
  用时自动回落 readiness。
- **流程**：`pal_iocp_listen` 监听（Phase 32a 起常态保持 2 个 AcceptEx
  在飞，完成即补挂）；ACCEPT 完成 → 建 conn（session/arena/64KB recv
  缓冲）→ post 首个 WSARecv；RECV 完成 → 同一
  parse→execute 流水线（与 readiness 共享 conn_process_input）→
  kick_flush → 补投下一个 WSARecv（Phase 32a 评估过"先补投再处理"，
  loopback 上批次被切碎、一致性负收益，已回退，见 performance.md）。
  SEND 完成 → 推进发送进度，未发完则续发（单块 ≤256KB；Phase 34c 起
  回复缓冲整体 detach 移交发送角色、out 取池温热备件顶替，零拷贝；
  发完的缓冲归还 buf_pool）。发布订阅、复制
  推流、SYNC 帧共用 kick_flush（有 send 在飞时自动跳过）。16MB 慢副本
  丢弃策略一致（计入 detach 尾部）；AOF flush/主动过期与 autosave 照旧。
- **生命周期**：conn 有在飞操作时关闭走 zombie 路径（CancelIoEx 后等
  pending_ops 归零再真正释放，避免完成事件悬垂引用）。
- **复制副本侧（Phase 22）**：master link 支持 IOCP——阻塞 connect（既有
  简化）后 post 首个重叠 recv，PSYNC 经 kick_flush 发出，recv 完成喂入
  与 readiness 共用的两阶段状态机（repl_link_feed）；大于接收块的快照
  帧按 link_got/link_need 增量装载，与 readiness 路径一致。双后端
  full-cycle + 断线重同步测试覆盖（非 Windows 上 IOCP 变体自动跳过）。
  io_uring 后端本就工作（它是 pal_loop 就绪 API 的实现，走 readiness
  路径，无额外改动）。
- **限制（记录在案）**：IOCP 后端不支持 TLS（tls-port 自动回落
  readiness）。pal_iocp 在非 Windows 为空 stub（创建返回 NULL）。
- ping-pong 基准见 docs/performance.md Phase 7.5 表。

## 单节点集群模式（Phase 7.7）

- **范围**：兼容 Redis cluster-enabled 的单节点形态，独占 16384 个槽。
  多节点 gossip/MEET/迁移为后续工作。
- **hash slot**：`slot = crc16(hashtag) % 16384`（CRC16-XMODEM，表驱位运算；
  hashtag 取首个非空 `{}` 内容，规则同 Redis）。
- **节点身份**：`cluster-enabled yes` 时首次启动生成 40 位 hex node id 并
  写入 `cluster-config-file`（Redis nodes.conf 风格单行），重启复用。
- **命令族**：CLUSTER INFO/MYID/NODES/SLOTS/KEYSLOT/COUNTKEYSINSLOT/
  GETKEYSINSLOT；未启用时统一 `-ERR This instance has cluster support
  disabled`。INFO 增 # Cluster 段（cluster_enabled:0/1）。
- **CROSSSLOT 执行**：启用后 MGET/MSET/DEL/UNLINK/EXISTS/SINTER/SUNION/
  SDIFF 的所有 key、SMOVE 两端、WATCH 的 key 必须同槽；EXEC 重放前用共享
  槽累加器检查队列内全部命令的 key，违例以 CROSSSLOT 整体中止且无副作用。
- **快照/AOF**：不受影响（对象存储路径一致）。

## 集群总线与 gossip（Phase 7.8a，多节点第一部分）

- **集群总线**：cluster-enabled 时在 `port+10000` 开第二个监听（仅
  readiness 后端；IOCP 后端强制回落 select，同 TLS）。总线连接非阻塞，
  接入同一事件循环；慢连接按常规 out 缓冲冲刷策略处理。
- **协议（ddup cluster protocol，简化自有格式，不与 Redis 总线逐字节
  兼容，记录在案）**：`"RCM2"`（v2；v1 魔数 `"RCMB"` 仍可解析——v1
  发送者没有角色/master_id/epoch 字段，按 master、"-"、epoch 0 处理）
  + u32le totlen + u16le type（PING=1/PONG=2/MEET=3），body 为发送者
  id/ip/port@bus/flags/完整槽位图/master_id/config_epoch + 至多 10 条
  gossip 条目（同构字段，槽以区间串表示）。总包 ≤16KB，防御式解析
  （坏包/超长直接关闭）。
- **gossip**：每 1s 向全部出站连接发 PING（携带 gossip 条目）；收 PING/
  MEET 回 PONG；PONG 刷新 last_seen。发送者段直接合并；gossip 条目对新
  节点全量加入（handshake 标记），对已知节点只在 epoch 不更旧时合并槽
  声明（flags/ip 不被第三方改写）。MEET 由 `CLUSTER MEET ip port` 触发
  （主动建连并发 MEET 帧）。收敛方式与 Redis 相同：A 认识 B、B 认识 C
  ⇒ A 经由 B 的 gossip 负载学会 C。
- **节点表**：db 内置 32 节点表（id/ip/port@bus/flags/槽位图/last_seen），
  nodes.conf 多行格式 render/parse 双向序列化；每 10s 脏检测持久化
  （原子 rename），启动时先装载再覆盖 myself 条目。
- **故障检测**：NODE_TIMEOUT 默认 15s（测试可调；必须大于 1s 的 gossip
  周期，否则在线节点会在 PING 间隔内被误标）；超时标记 PFAIL +
  disconnected 链路位。PFAIL→FAIL 的法定人数语义、cluster_state 规则
  与自动故障转移门控见 Phase 26/7.10。
- **TLS**：总线不支持 TLS（记录在案）。

## 槽分配与重定向（Phase 7.8b，多节点第二部分）

- **新节点语义**：fresh boot 的 myself 不持有任何槽（Redis 行为；7.7 的
  "独占全部" 只是单节点便利语义，由 nodes.conf 持久化保留旧部署）。
  cluster_state 在覆盖完备前为 fail。
- **槽命令**：CLUSTER ADDSLOTS（空闲才加，"Slot <n> is already busy"）、
  DELSLOTS（仅 myself 持有才可删，"already unassigned"）、SETSLOT slot
  NODE id（任意节点间移动，未知 id 报错）。位图变更即时生效并经 gossip
  传播（负载本就携带槽区间串）；nodes.conf 经 cluster_changes 计数触发
  周期持久化。
- **owner 缓存**：`db.slot_owner[16384]`（节点索引，0xFFFF=未分配），任何
  位图变更置脏，下一个命令前一次性重建（O(16384×节点数)）。常规命令
  每条一次 O(1) 所有权查询。
- **-MOVED 执行**：命令分派入口统一做所有权检查（keyless 命令表豁免；
  MGET/DEL/EXISTS/集合运算/WATCH 全参数、MSET 奇数位、SMOVE 两端、其余
  取 argv[1]）：槽未分配 → `-CLUSTERDOWN Hash slot not served`；槽属他人
  → `-MOVED <slot> <ip>:<port>`。EXEC 逐条元素独立判定（Redis 行为：
  MOVED 是该元素的回复，其余照常执行；CROSSSLOT 检查仍在 MOVED 之前）。
- **非集群零开销**：cluster_enabled=0 时所有权检查为单分支短路。

## 槽在线迁移（Phase 7.9，多节点第三部分）

- **DUMP/RESTORE**：DUMP 返回单键二进制 payload（`u16 版本(1) + u8 类型 +
  值负载 + u64 CRC64`，复用 snapshot 的分类型编码；CRC 覆盖此前全部字节，
  采用反射 ECMA 多项式 0xC96C5795D7870F42、全 1 初值/异或出（CRC-64/XZ
  参数，"123456789" 校验值 0x995DC9BBDF1939FA）。RESTORE key ttl-ms
  payload [REPLACE] [ABSTTL] [IDLETIME n] [FREQ n]：ttl 默认是相对毫秒
  （0=不过期），ABSTTL 时改为绝对 Unix ms；IDLETIME/FREQ 仅解析接受
  （无 LRU/LFU 对象元数据）。payload 截断/坏 CRC/版本不符 →
  `Bad data format`；无 REPLACE 且键已存在 → `BUSYKEY`。
- **RESTORE-ASKING**：与 RESTORE 共用实现；集群所有权检查将其视为隐式
  ASKING，供导入态目标写入，选项与 RESTORE 一致。
- **MIGRATE host port key dbid timeout [COPY] [REPLACE] [KEYS k...]**：
  源端把每个存活键 dump 成 RESTORE 命令（前缀一条 ASKING，兼容导入态
  目标）流水线发往目标，deadline 内逐条等 +OK；目标确认后本地删除
  （COPY 保留）。任一失败即止（`-IOERR error or timeout writing to
  target instance`），已确认键仍删除、其余保留。dbid!=0 →
  `DB index is out of range`；单键形式键不存在 → `-NOKEY No such key`。
  与 Redis 一样会阻塞事件循环；测试经 pump hook 驱动同进程目标服务器。
  MIGRATE 属管理命令，豁免所有权检查。
- **迁移状态**：`db.slot_migrating[] / slot_importing[]`（节点索引，
  0xFFFF=无），仅本地（不 gossip、不进 nodes.conf）。`SETSLOT slot
  MIGRATING TO id` 要求 myself 持有该槽（否则 "Can't migrate slot:
  hash slot is not served by this node"，目标是 myself 报 "Can't
  migrate slot to myself"）；`SETSLOT slot IMPORTING FROM id` 要求
  myself 不持有（否则 "Can't import slot: hash slot is already served
  by this node"）。`SETSLOT slot NODE id` 与 DELSLOTS 清除两态。
- **-ASK 与 ASKING**：迁移源端槽属 myself 但键已不在 → `-ASK <slot>
  <ip>:<port>`（键仍在则照常服务）。导入端槽属他人但 slot_importing
  匹配时，客户端先发 ASKING（+OK，置一次性 session 标志），下一条
  命令豁免所有权检查一次，随后标志被消费、恢复 -MOVED。

## 副本与故障转移（Phase 7.10，多节点第四部分）

- **副本角色**：cluster_node 增加 slave 标志位与 master_id（nodes.conf
  的 master 列、v2 总线帧均携带）。`CLUSTER REPLICATE <id>` 使 myself
  成为某已知 master 的副本（`Unknown node` / `Can't replicate myself` /
  `I can only replicate a master`），并经由 session 钩子同时启动数据
  复制（等价于对目标自动执行 REPLICAOF；提升时自动 NO ONE）。snapshot
  装载改为仅替换数据（保留集群/配置状态），副本全量重同步不再清空
  节点表。
- **config epoch**：`db.cluster_current_epoch`（初始 1）；认领槽位的
  节点把 claim 打上 `++current_epoch`（ADDSLOTS→myself、SETSLOT NODE→
  目标、failover→提升者）。冲突裁决（cluster_merge_claims，对发送者段
  与足够新的 gossip 条目生效）：epoch 高者胜，平局取节点 id 字典序大者
  （Redis 规则），败者位图即时清除——包括 myself 的让步；未再声明的位
  被收回。CLUSTER INFO 如实上报 current/my epoch。
- **自动故障转移**：master 被 quorum 确认为 FAIL（见 Phase 26）且
  （按本地视图）持有槽时，其 slave 设定选举定时（node_timeout +
  500ms，记录在案的简化：无 Redis 的随机化延迟）；到期且 master
  仍为 FAIL 即提升：转 master、清 master_id、以新 epoch 认领其全部
  槽、停止数据复制并立即 gossip。死亡 master 重新上线后看到更高
  epoch 的声明即让步。
- **手动**：`CLUSTER FAILOVER [TAKEOVER]`，仅副本可执行（否则
  `You should send CLUSTER FAILOVER to a replica`）。简化：无 TAKEOVER
  时不等待 master 同意，与 TAKEOVER 行为一致（记录在案）。
- **其余简化（记录在案）**：ddup 协议下无 FAILOVER AUTH 投票轮
  （redis 总线模式的投票见 Phase 20/24）、无副本迁移（replica
  migration）；last_seen 只被直连帧刷新（第三方 gossip 不影响失联
  判定）。

## 客观故障判定与法定人数（Phase 26）

- **状态机**（对齐 Redis 7.0，源码核实）：无标记 → **PFAIL**（主观
  怀疑：本地静默 > node_timeout 时由 fail_check 设置，渲染为
  `fail?`，节点的直连帧立即清除）→ **FAIL**（客观故障；两条路径：
  法定人数提升、或收到 FAIL 帧强令）。FAIL 同样被直连帧清除——
  记录在案的简化：Redis 对仍持槽的 master 延迟撤销 FAIL
  （fail_time + node_timeout*2，CLUSTER_FAIL_UNDO_TIME_MULT），
  ddup 一律即收即清。
- **failure reports**：每节点一张报告表（reporter id + 时间戳，上限
  32 条），有效期 node_timeout*2（CLUSTER_FAIL_REPORT_VALIDITY_
  MULT），过期在计数时懒清除。gossip 条目里已知第三方带 PFAIL/FAIL
  标志 = 帧发送者的一票报告（干净标志 = 撤回该发送者的报告）；
  只有 **master 发送者**的报告计数，subject 为 myself 忽略。
- **法定人数提升**（markNodeAsFailingIfNeeded 语义）：本地已怀疑
  （PFAIL）且 有效报告数 +（myself 是 master 则 +1）≥ 持槽 master
  数/2+1 时标记 FAIL，并向全部总线连接广播 FAIL 帧（redbus type 3
  clusterMsgDataFail；RCM2 新类型 `CLUSTER_MSG_FAIL=5`：
  [RCM2][totlen=50][type][40 字节 id]）。收 FAIL 帧立即标记（清
  PFAIL、记 fail_time），未知节点容忍忽略，myself 永不标记。提升
  评估点在 gossip 接收与本地怀疑设置两处（Redis 只在前者；后者是
  为确定性加的，记录在案）。
- **cluster_state 新规**：只有 FAIL 持有者使状态 fail；PFAIL 节点
  的槽仍计入覆盖、不影响状态（Redis 的 minority-partition 规则
  ——可达 master 不足半数即 fail——未实现，记录在案）。
- **failover 门控**：ddup 自动提升与 redis 模式投票轮（AUTH_REQUEST
  授权方 likewise）都要求 master 为 FAIL；仅 PFAIL/失联不再触发任何
  选举。
- **局限（记录在案）**：2 个 master 的集群永远无法自动 FAIL
  （majority=2 而死者无法投票，Redis 相同）；本地怀疑要求曾有的
  直连接触——ddup 不会自动向 gossip 学到的节点建总线连接（Redis
  由 clusterCron 全互联），部分网状拓扑里无直连的节点对不能互相
  怀疑，自动 FAIL 需要全互联 MEET（测试拓扑即如此）。
- **wire 映射**：redbus 的 PFAIL/FAIL 位与 ddup 标志 1:1 直译；
  DISCONNECTED 是纯本地链路状态，从不上线。

## Redis 总线协议兼容（Phase 20）

- **redbus 编解码**（src/core/redbus.{h,c}）：真实 Redis clusterMsg
  ——2256 字节头（偏移经 redis 7.0 static_assert 核实）、全部整数字段
  **大端**、魔数 `"RCmb"`（小写 mb，勿写成 RCMB）、104 字节 gossip 条目
  （无槽位图；slaveof 仅随发送者段传播）。PING/PONG/MEET 完整实现；
  UPDATE 应用槽位 claim（指向 myself 时经 cluster_adopt_claims 收养）；
  FAIL 标记 disconnected；其余类型容忍忽略。ddup↔redis 标志映射：
  MASTER/SLAVE/HANDSHAKE/NOADDR 直译，DISCONNECTED↔FAIL（PFAIL 一并
  映射，记录在案）。
- **myip 自动发现**：redis 仅在配置 cluster-announce-ip 时填 myip，否则
  全零，接收方须用连接对端地址（nodeIp2String 语义；pal_get_peer_ip
  经 bus_conn 传入），空 myip 永不覆盖已知地址。
- **双协议**：`cluster-bus-protocol ddup|redis`（默认 ddup，既有部署与
  测试零影响）。总线监听与出站连接按协议切换编解码与 totlen 端序；
  gossip 节奏、故障检测、failover、nodes.conf 持久化与编解码无关。
- **首启 nodes.conf**：`cluster_node_id_load_or_create` 生成的首启行
  不再预填 0-16383（与 7.8b fresh-boot 语义一致；旧持久化文件不受影响）
  ——这是混入既有集群时"认领槽全部 busy + epoch 平局被抹"的根因。
- **CI 互操作验证**（.github/workflows/cluster-interop.yml，ubuntu +
  redis-server 7.0）：3 个真实 redis 节点 + ddup 第 4 节点 redis 模式，
  双向收敛（redis CLUSTER NODES 见 ddup id、ddup known_nodes=4）、
  ddup 认领 0-99 槽经 gossip 被 redis 接受、SET 落 ddup、redis 侧 GET
  收到指向 ddup 的 MOVED。已验证可用：PING/PONG/MEET/UPDATE/FAIL、
  epoch 冲突裁决、槽位移交。Phase 24 补齐：FAILOVER_AUTH_REQUEST/ACK
  投票帧（redis 模式下副本提升需多数派 ACK，含 lastVoteEpoch 防重复
  投票）、UPDATE 帧通用收养、混部集群副本 failover 全流程 CI（杀主→
  ddup 副本投票提升→接槽→旧主重连让步）。Phase 26 补齐：PFAIL/FAIL
  法定人数语义（报告窗、多数派提升、FAIL 帧强令），混部 failover
  CI 在新门控下保持绿色。

## 分片发布订阅（Phase 25）

- **命令面**：SSUBSCRIBE/SUNSUBSCRIBE（语义同普通订阅，推送种类为
  `smessage`）、SPUBLISH（返回**本地**接收者数，异步传播不计）、
  PUBSUB SHARDCHANNELS [pattern] / SHARDNUMSUB（glob 仅 `*`/`?`，
  记录在案）。SPUBLISH 走默认单键槽位校验（hash_slot(channel)：
  非本节点槽 → MOVED，未指派 → CLUSTERDOWN）；SSUBSCRIBE 无属主
  校验，任何节点可订——比 redis 宽（redis 经命令 key spec 对
  非本地槽回 MOVED，7.0.15 核实），记录在案的放宽，正确路由的
  客户端行为一致。订阅态白名单并入 SSUBSCRIBE/SUNSUBSCRIBE。
- **注册表**：与 channels 平行的第二张表（`rh_table schannels` +
  conn `ssubs` + session `nssub`），订阅/退订主体与普通频道共用
  （chan_subscribe/chan_unsubscribe 以表指针为参数）；连接关闭时
  两张表都自动清退。
- **总线传播**：SPUBLISH 本地投递后向**所有**总线连接广播一帧——
  redbus PUBLISHSHARD（type 10，Redis 7.0 编号核实；2256 头 +
  count=0 无 gossip + u32BE chlen/msglen 负载，即 redis
  clusterMsgDataPublish 布局）或 RCM2 CLUSTER_MSG_PUBLISH（4：
  [RCM2][totlen][type][u32le chlen][ch][u32le msglen][msg]，无节点
  记录）。接收端在节点表 handler 之前按类型拦截：redbus type 4
  （普通 PUBLISH，redis 全集群广播）投递本地普通订阅者
  （"message"），type 10 / RCM2 publish 投递本地分片订阅者
  （"smessage"）；长度畸形的帧静默吞掉（容错，不杀连接）。
- **记录在案的分歧**：redis 7.0 的 PUBLISHSHARD 只发往发布槽所在
  shard（主 + 其副本，7.0.15 clusterPropagatePublish 核实）；ddup
  广播给全部总线对等——任意节点的订阅者都能收到，无需加入发布
  槽的 shard。接收侧双方都不做槽位校验（7.0.15 clusterProcessPacket
  核实：仅要求发送者已知 + 本地有对应订阅）。超过总线 16KiB 上限
  （CLUSTER_MSG_MAX）的帧不传播（本地投递不受影响）。
- **CI 互操作**（cluster-interop.yml）：ddup SPUBLISH 的 type 10 出站
  帧被真实 redis 接受（CLUSTER INFO 的 publishshard_received 计数
  递增——redis 对非本地槽的 SSUBSCRIBE 回 MOVED，故 redis 客户端
  无法订阅 ddup 属主的频道，端到端方向只能验到线上受理）；redis
  PUBLISH → ddup SUBSCRIBE 端到端（type 4 入站）；redis SPUBLISH →
  ddup 副本（CLUSTER REPLICATE 加入 7101 的 shard）上的 SSUBSCRIBE
  端到端（type 10 入站）。

## Lua 脚本（Phase 19）

- **嵌入**：vendored Lua 5.1.5（deps/lua，MIT，源码未改动，PATCHES.md
  记录；独立 ddup_lua 静态库，第三方代码不挂项目警告旗标）。每个 db
  惰性创建**一个共享 lua_State**（Redis 模型；线程间无共享，mt 下每个
  worker 各持一份）。沙箱（记录在案的简化）：只开 base/string/table/
  math 库——无 io/os/debug/package，脚本无法触达文件系统与进程。
- **脚本缓存**：db 内 sha1（自研 FIPS 180-1 实现）小写 hex → Lua registry
  引用；命中不重编译，SCRIPT FLUSH 全部 unref。
- **桥**：`redis.call`/`redis.pcall` 把命令（字符串/数字参数）送回客户端
  命令所用的同一 command_dispatch，单条 RESP 回复再转回 Lua 值；返回值
  按 Redis 规则转换（number→:int、string→bulk、table 数组遇 nil 止、
  true→:1、nil/false→null bulk、{ok=}→简单串、{err=}→错误）。
  禁调名单（记录在案）：EVAL 族、SUBSCRIBE 族、SHUTDOWN。
- **效果复制**（Redis 5+ 语义，记录在案）：redis.call 的写命令经既有
  dirty 钩子**逐条**写入 AOF/backlog/副本流（记录 SET 而非 EVAL）；EVAL
  argv 本身经 `session.aof_skip` 抑制（含 MULTI 队列逐项）。
- **只读脚本别名**：`EVAL_RO/EVALSHA_RO` 复用 EVAL/EVALSHA 的加载与执行，
  执行期间标记 `session.in_ro_script`；`redis.call/pcall` 内写命令由
  command_dispatch 在入口处拒绝（`Write commands are not allowed from
  read-only scripts.`），不会产生副作用。
- **脚本库族**：`FCALL/FCALL_RO` 编译并执行命名库的源码，复用 EVAL 的
  KEYS/ARGV 全局表；`FUNCTION LOAD` 以 `#!lua name=<lib>` 头把源码按名称
  存入 `db.function_libs` 哈希表，`DELETE/LIST/FLUSH/STATS/KILL/HELP` 维护
  该表。`FUNCTION DUMP/RESTORE` 以 ddup 专用二进制 payload 一次序列化/
  重建全部库：固定魔数 + 每库 8 位十六进制长度前缀（name/code），冷路径
  两遍 `rh_each` 定长后一次分配；RESTORE 支持 `FLUSH/APPEND/REPLACE`
  策略（默认 APPEND，冲突报错，REPLACE 覆盖）。
  Redis 的 `redis.register_function` 多函数库格式为本次范围外（记录在案）。
- **错误文本**：对齐 Redis 5/6（编译 `Error compiling script (new
  function):`、`-NOSCRIPT`、运行时 `Error running script (call to
  f_<sha>):`，含 `script:N:` 位置前缀）。
- **mt 说明（记录在案）**：脚本在命令分发内执行，mt 下自然落在各
  worker 的库上；路由按 argv[1] 决策（脚本串），不保证 KEYS[1] 属主
  恰为本 worker——mt 下脚本应只操作单槽亲和或自包含的数据（测试覆盖
  单 worker 内 SET+GET）。

## 架构对比：分片存储 + 消息路由（ddup mt）vs 共享存储（Garnet/Tsavorite）

（Phase 28–37 实测基础上的定性对比，2026-08）

**ddup mt（shared-nothing 分片）**：每 worker 独占槽位分片，跨 worker 命令经
任务环路由。每命令路由税 = 路由判定 + 任务对象（已池化）+ 原始字节拷贝 +
双环跳 + 目标端重解析 + 完成回跳。数据路径零锁零原子——正确性靠构造保证。

**Garnet（共享 latch-free 存储）**：网络线程就地解析/执行/回写，零移交；
数据由 CPU 缓存一致性带到执行线程。代价是存储层必须无锁：epoch 保护、
hazard pointer/延迟回收、索引桶 CAS、检查点与恢复的并发协议——Tsavorite
源自 FASTER 多年研究积累，内存序 bug 风险面大。

| 维度 | 分片 + 路由（ddup mt） | 共享存储（Garnet） |
|------|------------------------|---------------------|
| 单命令固定开销 | 2 环跳 + 任务拷贝（~0.3-0.5µs） | ≈0（就地执行） |
| 热点键 | 拥有者线程天然串行，无重试 | CAS 重试风暴 + 缓存行乒乓 |
| 多键/事务 | 需同槽约束（CROSSSLOT） | 原生支持（两阶段锁） |
| 读扩展性 | 读也必须路由到拥有者 | 任意线程读（epoch 保护） |
| 实现复杂度 | 低（单线程语义 per worker） | 极高（无锁回收/内存序） |
| 运维特性（过期/淘汰/AOF） | per-worker 自治，简单 | 需全局协调 |
| 低并发 | 路由税占优，mt ≈ 或 < st | 无税，恒优 |
| 高并发/多核 | 税被吞吐摊薄（实测 mt4 c500P64 2083k/2597k 反超 st 与 Garnet） | 线性扩展但热点受制 |

**实测结论（本机 16C、CI 4C）**：st 单核 ddup ≈ Garnet 单核效率；mt4 在深
流水高并发下与 Garnet 同档或更优。路由税约占每命令 20-30%（4 worker），
被批量与多核吞吐摊薄后非瓶颈；Garnet 的优势集中在"低并发 + 多键事务"
组合场景。

**潜在混合路线（未实施，评估记录）**：epoch 保护的跨 worker 只读共享读
（GET 免路由，写仍路由）——读多写少负载可吃掉大部分共享存储优势，
复杂度集中在桶级版本计数与读重试；Dragonfly（分片）与 Garnet（共享）
并立证明两条路线都是 SOTA，ddup 当前路线无需更换。

## 健壮性加固（Phase 38）

全库大小/溢出与失败路径审计，统一约定：**容量与字节计算先验算、失败
不留半状态、错误沿返回值上抛**。

- **分配尺寸验算**：rhtable 槽位字节与容量翻倍、resp 聚合 items 字节、
  session MULTI 队列增长、skiplist 节点字节（uint64）都在乘法前检查
  上界；不可表示的请求返回错误（分发层回 `-ERR`，如超长 MULTI 入队
  被拒）而不是回绕成小分配。rhtable 负载阈值计算改为先除后乘，
  `cap * RH_MAX_LOAD_NUM` 不再可能溢出。
- **arena**：对齐上调与块尺寸（`sizeof(block) + cap + 对齐余量`）全程
  验算，溢出返回 NULL 且不触碰 arena 状态（事务性）；对齐 padding
  计入 `used`，大块请求同样保证 16 字节对齐。
- **失败原子性**：resp 解析先填本地聚合再一次性提交 `*out`；
  `session_queue_push`/`zsl_insert` 改为返回错误码，调用方失败即报错
  且不产生副作用；nodes.conf 落盘在 close/rename 失败时保留
  nodes_dirty（后续周期自动重试）并删除 `.tmp` 残留。
- **脚本缓存**：sha1 按"恰好 40 字节 hex"校验（不再需要 NUL 结尾），
  缓存命中额外复核 registry ref 仍是函数——畸形条目视为未命中并
  重编译，flush 忽略。
- **io_uring readiness 后端**：SQ 操作前统一 `uring_reserve`（满则先
  提交腾空），enter 支持部分提交（按返回数推进游标）与 EINTR 重试；
  TIMEOUT 的 `__kernel_timespec` 从栈上移到环持有存储，以完成事件
  复位 active 标志，消除悬垂指针窗口。
- **TLS**：OpenSSL 库初始化改为线程安全 once（pthread_once /
  InitOnceExecuteOnce，mt 多 worker 并发建 ctx 安全）；fd 超 int
  范围或 `SSL_set_fd` 失败即拒绝；读写超过 INT_MAX 直接报错。

## 通用键与字符串扩展命令（Phase 39/40）

- **glob 匹配器**（src/ds/glob.{h,c}）：Redis stringmatchlen 语义
  （`*`、`?`、`[...]` 含范围与 `[^...]` 取反、`\x` 转义），供 KEYS 与
  SCAN MATCH 使用。记录在案的偏离：未终结的 `[` 按字面字符处理
  （Redis 几乎必然不匹配），取反只支持 `^`（Redis 7 行为）。
- **rh_scan 游标迭代**（src/core/rhtable）：游标 = 虚拟桶下标，主表
  `[0, cap)` + rehash 在飞时旧表 `[cap, cap+old_cap)`；返回 0 表示
  迭代完。回调可删除条目（惰性过期）——每轮重读表字段，旧表被迁移
  释放也不会 UAF；删除/迁移中途可能跳过或重复 key（Redis SCAN 同
  为弱保证，记录在案）。空桶不占 COUNT 额度。SCAN 游标只接受纯数字
  非负整数（不容忍前导 `+`/空白，记录在案）。
- **RENAME/RENAMENX**：值 blob 复制到目标 key 后经
  `db_del_kv_keep_obj()` 删源——对象所有权随 blob 副本转移，普通
  `db_del_kv` 会 obj_free 造成悬垂（TDD 用 HSET+RENAME+HGET 抓到）。
  TTL 随 blob 搬迁；两 key 的 WATCH 版本都 bump。集群模式两 key 须
  同槽（-CROSSSLOT），mt 按两 key 同 worker 路由（同 SMOVE）。
- **KEYS/SCAN/RANDOMKEY 的作用域**：集群模式只覆盖本节点本地数据
  （Redis 行为）；mt 模式下 `KEYS`/`RANDOMKEY` 聚合各分片，`SCAN`
  通过复合游标顺序遍历各 worker；每次请求只访问一个分片，避免全库
  结果物化和跨线程共享 hash 表。
  RANDOMKEY 为占用桶随机探测（≤100 次）+ 顺序兜底，不保证均匀分布。
- **字符串扩展**：GETDEL/GETSET 先把旧值拷入回复再删除/覆盖（零
  额外分配）；INCR/DECR 重构为共用 delta 路径（INCRBY/DECRBY 加入，
  溢出判定通用化，DECRBY 对 LLONG_MIN 归 overflow 错误）；SETRANGE
  512MB 上限（`STRING_MAX_BYTES`）+ 空洞补 `\0`，空 value 不建缺
  失 key；GETRANGE 严格 Redis 归一化（负下标、钳位、反向区间→空）；
  INCRBYFLOAT 用 strtold + `%.17Lg`（已知边缘差异：十六进制浮点串
  会被接受，Redis 拒绝，记录在案）。覆盖写（GETSET/SETEX/INCRBY 族）
  清 TTL，与既有 INCR/APPEND 的有意行为一致；SETEX/GETSET 对非
  string 旧值回 WRONGTYPE（跟随本实现 SET 的收紧语义）。

## 容器命令补齐（Phase 41/42）

- **统计槽位扩容**：命令 id 总数越过 128，`db.cmd_calls/cmd_usecs`
  与 `info_stats` 的 `[128]` 统一改为 `CMD_STATS_SLOTS`（256）；
  mt INFO __STATS__ 按 id 文本传输不受影响。**硬约束**：CMD_TABLE
  条目与枚举必须同步追加在末尾——`cmd_table_entry` 按
  `CMD_TABLE[id-1]` 位置索引，插中间会使后续命令 id 全部错位。
- **List**：LREM/LTRIM/RPOPLPUSH 共用新 ds 助手 `obj_list_remove()`
  （unlink + len/mem 记账）；LPOS 两遍扫描零分配流出；LPOP/RPOP
  count 形式对缺失 key 回 null array（RESP2 `*-1`，Redis 行为）。
- **Set**：SINTER/SUNION/SDIFF 求值抽取为 `setop_eval()`，STORE 族
  物化结果覆盖 dst（任意旧类型允许、空结果删 dst）；SINTERCARD 经
  `rh_scan` 提前停（LIMIT 达到即返回，不物化交集）。
- **ZSet**：skiplist 新增 member 字典序范围定位
  `zsl_first_in_lex_range()/zsl_last_in_lex_range()`（与 score range
  同构的逐层 span 定位；"全元素同分"假定与 Redis 一致不校验）；
  ZPOPMIN/MAX 先写回复再删节点（无悬垂）；ZREMRANGEBYRANK/BYLEX
  共用 `zset_rem_node_span` 沿 level-0 删除，dict/skiplist 两侧一致。
- **语义取舍（记录在案）**：ZPOPMIN/MAX 无 count 也回数组（Redis
  6.2+）；SINTERCARD numkeys≤0 按 "Number of keys can't be greater
  than number of args" 报错。

## 收尾命令与耐久性（Phase 44）

- **SET KEEPTTL/GET**：KEEPTTL 覆盖前读出绝对过期时刻、覆盖后恢复
  （与 EX/PX 互斥，与 NX/XX 自由组合）；GET 先把旧值拷入回复再覆盖，
  旧值非 string 回 WRONGTYPE 且不执行（Redis 行为）。带选项 SET 走
  通用 dispatch，lean SET 快路径（仅无选项）不受影响。
- **COPY src dst [DB n] [REPLACE]**：统一走 DUMP/RESTORE 序列化深
  拷贝——裸 blob 拷贝对 hash/list/set/zset 的指针对象会造成跨 key
  别名（double-free/UAF），记录在案。TTL 以绝对时刻语义搬迁；AOF/
  复制自包含（argv 自带 DB 选项，重放经 SELECT 前缀恢复）。集群
  两 key 同槽；mt 下拒绝跨库（任务栈 session 无多库钩子，AOF
  dirty 检查只覆盖源库，`-ERR COPY across databases is not
  supported in mt mode`，记录在案）。

## Hash 字段级 TTL（Redis 8 增量）

## Redis 8 string safety tranche

`INCREX` performs checked integer/long-double arithmetic before committing a
new value, and parses all expiry forms into absolute milliseconds with overflow
guards. `MSETEX` parses and validates every key/value and condition before any
write, preserving atomic NX/XX and KEEPTTL behavior. `DELEX` only evaluates
conditional predicates on string objects and returns a dedicated wrong-type
error; `DIGEST` uses the vendored XXH3 implementation and emits a fixed 16-byte
hex digest. These paths avoid per-element temporary allocations and keep the
mutation boundary after all validation, which is important for crash/AOF
replay consistency.

## Redis 8 cluster slot maintenance

`SFLUSH` parses slot pairs, intersects them with local ownership, collects
matching keys before deletion, and replies with coalesced flushed ranges.
`TRIMSLOTS RANGES` rejects any range served by the local node, then removes
only unserved-slot keys. Collection is separate from mutation so Robin Hood
table iteration is never invalidated; allocation failure aborts without
partial deletion.

`CLUSTER SLOT-STATS` currently exposes only `key-count`, which is derived from
one bounded table scan and filtered by local slot ownership. `SLOTSRANGE` keeps
slot order; `ORDERBY` uses a fixed 16K-item stack array with deterministic slot
tie-breaking and optional `LIMIT/ASC/DESC`. CPU, memory, and network metrics
are rejected until their counters can be maintained consistently.

The internal migration commands `CLUSTER MIGRATION` and `CLUSTER SYNCSLOTS`
are deliberately fail-closed for client sessions. They are recognized for
compatibility auditing, but require an internal transport hook before any
future migration state or slot metadata can be changed.

`MONITOR` uses a server-owned subscription flag and appends timestamped,
quoted command lines to each monitor connection's existing output buffer. The
Redis 8 management containers `BACKUP`, `HIMPORT`, and `HOTKEYS` are recognized
for protocol compatibility. `HIMPORT` implements session-local fieldsets
(`PREPARE`, `SET`, `DISCARD`, `DISCARDALL`) and writes ordinary hash objects
after validating the whole batch; DISCARD and DISCARDALL return removal counts.
`BACKUP` uses the existing multi-database
snapshot serializer to create an atomic `.backup` artifact beside the configured
snapshot path. Its `START/STATUS/SEAL/LIST/ABORT/CLEANUP` state machine is
server-owned, fail-closed on missing paths or IO errors, and never overwrites
the live snapshot. It is a synchronous baseline until an incremental MP-AOF
engine is available. `HOTKEYS` provides a server-wide, low-overhead lifecycle model:
`START METRICS <count> [CPU|NET] [COUNT k] [DURATION seconds] [SAMPLE ratio]
[SLOTS ...]`, `STOP`, `RESET`, and `GET` expose active state, sampling ratio,
collection start time, command count, and bounded `by-cpu-time-us`/
`by-net-bytes` key lists. The table is preallocated at START and uses bounded
sampled command/argument-byte units, so the hot path adds no per-command
allocation. These units are ddup estimates rather than Redis process CPU/
network accounting.

- `obj_hash` 在 listpack/rh_table 双编码之外增加独立的 `expires` 表：
  键为 field，值为 8 字节小端绝对过期毫秒时间戳。紧凑编码不会退化为
  逐字段分配；字段 TTL 只在设置时进入额外表，未设置 TTL 的普通 hash
  热路径仅多一次 `rh_get`。
- 读路径 `obj_hash_get_at` 对过期字段惰性删除；`HLEN/HGETALL/HKEYS/
  HVALS/HSCAN` 等全量视图调用 `obj_hash_purge_expired` 后再遍历。
  单字段操作 O(field)；全量 purge O(带 TTL 字段数)，不扫描整个 hash。
- `HSET/HSETNX/HINCRBY/HINCRBYFLOAT` 普通覆盖写清字段 TTL；
  `HSETEX` 的 `KEEPTTL/EX/PX/EXAT/PXAT` 与 `HGETEX` 的
  `EX/PX/EXAT/PXAT/PERSIST` 按 Redis 8 解析；`FNX/FXX` 全字段前置
  判定后原子设置。
- `HEXPIRE` 族支持 `NX/XX/GT/LT`；到期时间落在过去时删除字段并回
  `2`，条件不满足回 `0`，字段不存在回 `-2`。TTL 查询族同样使用
  `-2`/`-1` 区分“无字段”与“无 TTL”。
- 命令 id 总数已突破原 `CMD_STATS_SLOTS` 128 的上限，扩容为 512；
  CMD_TABLE 与枚举继续只追加尾部。
- **Set 基数**：`SUNIONCARD` 用临时 `rh_table` 做 union 去重，
  `SDIFFCARD` 先收集后续集合成员再扫首集合；`LIMIT` 达到即停，
  `APPROX` 当前按精确基数返回（ddup set 无 HLL 编码）。

## 目录结构

```
src/pal/     平台抽象：pal_platform(宏), pal_time, pal_socket(TCP), pal_event(事件循环),
             pal_file, pal_tls(可选 OpenSSL), pal_iocp(Windows IOCP proactor，仅 Windows)
src/resp/    RESP 协议（Phase 1）
src/core/    KV 存储、哈希表、过期、淘汰、命令分发、session、config、
             snapshot（Phase 2/4/5.3/6）
src/ds/      对象类型：obj（tagged blob、Hash 嵌套表、List 双链表、Set、
             ZSet dict+skiplist）、skiplist（span 跳表）（Phase 5.1/5.2/21）
src/server/  连接与服务器主循环（Phase 3）、aof（Phase 6）：单线程事件循环、
             recv 缓冲按需增长、解析→执行→推进零拷贝流水线；连接全部
             非阻塞（Phase 7.4）：写出经 out 缓冲 + writable 事件驱动，
             慢客户端不再阻塞主循环（详见 architecture 网络层说明）
tests/       单元测试（test.h 自研框架）+ 集成测试
deps/lua/    vendored Lua 5.1.5（MIT，未改动；PATCHES.md 记录）
bench/       压测客户端 ddup-bench（Phase 3，非 ctest 目标）
tools/       ddup-reshard 集群迁槽工具 + reshard_client（阻塞 RESP 客户端
             与迁槽编排，Phase 16）
```
