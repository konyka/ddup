# ddup 开发路线图

开发方式：TDD。每个小步骤 = 先写失败测试 → 实现 → 全部测试通过 → commit + push。

- [x] **Phase 0 — 脚手架**
  CMake 工程、C 标准探测（C23→C99）、自研测试框架、PAL 骨架（时间/平台宏）、
  四平台 CI（Windows/Linux/macOS/FreeBSD）、文档骨架
- [x] **Phase 1 — RESP 协议**
  RESP2 解析器（含流式/分包边界）、写出器、解析-回写一致性随机测试、RESP3 类型
- [x] **Phase 2 — 内存 KV 核心**
  arena 分配器、对象池、Robin Hood 哈希表 + 增量 rehash、命令分发表、
  PING/ECHO/GET/SET/DEL/EXISTS/INCR/DECR/APPEND/STRLEN/MGET/MSET
- [x] **Phase 3 — 网络服务器**
  readiness 事件循环（Linux epoll、macOS/FreeBSD kqueue、Windows select；
  IOCP/io_uring 列为后续优化）、TCP 监听、连接生命周期、pipelining、
  socket 级集成测试、压测客户端与基准记录
- [x] **Phase 4 — 过期与淘汰**
  EXPIRE/PEXPIRE/EXPIREAT/PEXPIREAT/TTL/PTTL/PERSIST、SET NX/XX/EX/PX、
  惰性过期 + 主动采样过期（每 100ms，样本 20，>25% 过期则循环）、
  maxmemory 与 allkeys-lru/noeviction 采样淘汰、CONFIG GET/SET、INFO、
  DBSIZE、FLUSHDB、增量内存记账
- [x] **Phase 5 — 复杂数据结构**
  Hash/List/Set/ZSet（跳表）、MULTI/EXEC/DISCARD/WATCH、SUBSCRIBE/PUBLISH
  - [x] 对象存储模型（tagged value blob）+ WRONGTYPE 矩阵（Phase 5.1）
  - [x] Hash：HSET/HGET/HDEL/HEXISTS/HLEN/HGETALL/HKEYS/HVALS/HMSET/HMGET/HINCRBY/HSETNX（Phase 5.1）
  - [x] List：LPUSH/RPUSH/LPUSHX/RPUSHX/LPOP/RPOP/LLEN/LRANGE/LINDEX/LSET（Phase 5.1）
  - [x] Set：SADD/SREM/SISMEMBER/SMISMEMBER/SCARD/SMEMBERS/SPOP/SRANDMEMBER/SMOVE/SINTER/SUNION/SDIFF（Phase 5.2）
  - [x] ZSet：skiplist + ZADD/ZSCORE/ZCARD/ZINCRBY/ZREM/ZRANGE/ZREVRANGE/ZRANK/ZREVRANK/ZCOUNT/ZRANGEBYSCORE/ZREMRANGEBYSCORE（Phase 5.2；span 见 Phase 21）
  - [x] 事务：session 上下文、MULTI/EXEC/DISCARD/WATCH/UNWATCH（key 版本表）（Phase 5.3）
  - [x] 发布订阅：SUBSCRIBE/UNSUBSCRIBE/PUBLISH（server 级频道注册表）（Phase 5.3）
- [x] **Phase 6 — 持久化与配置**
  AOF（RESP 命令流追加，启动重放，容忍截断尾部）、RDB 风格二进制快照
  （原子写、全有或全无加载、过期 key 跳过）、SAVE/LASTSAVE/SHUTDOWN、
  redis 风格配置文件 + 命令行覆盖、save 间隔自动快照、信号/命令优雅退出
- [x] **Phase 8 — C 标准自适应与 Garnet 化底座**
  细粒度 C 标准能力探测（atomics/threads/typeof/constexpr/stdckdint 等）、
  `src/pal/pal_cstd.h` 统一封装与 C99 降级路径、`test_cstd` 覆盖、
  支持 `DDUP_C_STD_FORCE` 本地验证。
- [x] **Phase 9 — 命令 ID 表 + 缓冲池**
  统一 `cmd_entry` 命令表 + 开放寻址哈希解析 `cmd_resolve`、写命令/arity
  等元数据 O(1) 查表；分层固定大小缓冲池 `buf_pool`（4K/16K/64K/256K）
  接入 `resp_buf` 与连接 recv/out 缓冲，替代热路径 malloc/realloc。
  微基准：cmd_resolve ~83M ops/s、buf_pool get/put ~3.9G ops/s。
- [x] **Phase 10 — SIMD 解析与 socket 调优**
  `pal_simd.h` 提供 SSE2/NEON 加速的 `ddup_find_crlf`（标量回退），
  `resp_parser` 统一走该助手；bench_core 增加 parse-only 微基准
  （SET ~31M / GET ~42M ops/s）。socket 默认开启 TCP_NODELAY，监听
  backlog 提升至 511。批量发送/writev 调查结论：当前单线程模型下无
  系统调用收益，保留给 thread-per-core 阶段。
- [x] **Phase 11 — Thread-per-core（mt_server，shared-nothing key 分区）**
  PAL 线程/条件变量/self-pipe 唤醒；acceptor 线程 + N 个独立 worker
  （各自事件循环/db/buf_pool）；单 key 命令按 crc16 槽路由（原始字节
  任务 + 完成队列 + per-conn seq/reorder 保序）；多 key 与集合运算同
  worker 校验（-CROSSSLOT）；DBSIZE/FLUSHDB/SAVE/LASTSAVE 广播聚合；
  `--io-threads N` 配置。
  - [x] mt 优化：同目标连续命令合并为一个任务、无锁 SPSC 队列（C11
    atomics，C99 降级互斥）、路由免逐元素深拷贝（raw bytes + 目标端
    重新解析）、连接-键亲和（一次性连接迁移）
  - [x] mt 功能补齐：mt 层 MULTI/EXEC/DISCARD/WATCH/UNWATCH（同 worker
    bundle + watch_refs 跨 worker 记账）、pub/sub 按频道 hash 路由、
    per-worker AOF/快照（路由 mutation 记录到执行 worker 的 AOF）、
    SINTER/SUNION/SDIFF 同槽路由
  - [x] mt 生产化（Phase 15）：INFO 跨 worker 聚合（内部 INFO __STATS__
    机器格式快照 + home 端归并渲染）、TLS 支持（acceptor 持 TLS listener，
    每 worker 独立 TLS ctx + worker 内嵌握手）、IOCP worker 后端
    （pal_iocp_post 唤醒；IOCP 后端禁用连接迁移）
  - [ ] mt 后续（范围化排除，记录在案）：复制/集群在 per-worker 模型下
    的适配——worker 各自为战与全库复制/集群总线的语义冲突大、收益低，
    集群/复制请用单线程模式

- [x] **Phase 13 — 安全与多数据库**
  AUTH（requirepass + NOAUTH 门）、16 逻辑库（SELECT/SWAPDB、session
  选择钩子、快照 DDUP0002 多库格式、AOF SELECT 前缀、INFO dbN 段、
  全库主动过期与全局 maxmemory）、commandstats（每命令 calls/usec，
  INFO # Commandstats，实测开销 <1%，DDUP_NO_CMDSTATS 可关）
- [x] **Phase 14 — io_uring 后端（Linux）**
  `pal_loop_create_iouring()` 直接 syscall 探测（内核 ≥5.10），oneshot
  poll + 按最近兴趣集 re-arm，控制完成（POLL_REMOVE/UPDATE 的 res==0
  回执）与事件完成区分；`--io iouring` / `SERVER_BACKEND_IOURING`；
  非 Linux 为 stub；test_event/test_server 双后端跑（Linux CI 覆盖）
- [x] **Phase 16 — 集群运维工具**
  `ddup-reshard`（redis-cli 风格：--from/--to/--slot/--count/--timeout；
  SETSLOT MIGRATING/IMPORTING → 批量 GETKEYSINSLOT+MIGRATE REPLACE KEYS
  → 双端 SETSLOT NODE）；编排逻辑在 tools/reshard_client.[ch]，
  tests/test_reshard.c 双节点线程集成覆盖

- [x] **Phase 7+ — 长期**（子项全部完成）
  集群模式、TLS、io_uring 优化落地、SIMD 解析优化、与 Garnet/Redis 基准对比
  - [x] 主从复制：全量同步（SYNC 快照帧 + 命令推流）、REPLICAOF/NO ONE、
    只读副本、断线全量重同步（无 PSYNC，backlog 预留）（Phase 7.1）
  - [x] TLS：可选 OpenSSL（独立 tls-port、阻塞握手、DDUP_HAS_TLS 门控）（Phase 7.2）；
    Windows test_tls 修复并全平台启用（Phase 18：测试阻塞 socket 根因）
  - [x] 剖析驱动优化：bench_core 进程内基准 + expires 空表早退、无 WATCH 免
    keyvers 写入、get+touch 单探测、传播 fan-out O(1)（CPU 侧 SET +71%/
    GET +24%，端到端 +8–11%）（Phase 7.3）
  - [x] 非阻塞写出缓冲：连接全非阻塞，out 缓冲 + writable 事件驱动 flush，
    慢客户端不阻塞主循环（Phase 7.4）
  - [x] Windows IOCP 后端：pal_iocp proactor + server 双后端可切换
    （--io select|iocp，Windows 默认 iocp；全套 socket 测试双后端跑）（Phase 7.5）
  - [x] CI 对比压测：bench.yml 每周 ddup vs Garnet vs Redis
    （ubuntu-latest，结果发布 bench-results 分支）（Phase 7.6）
  - [x] 单节点集群模式：CRC16 hash slot、节点身份、CLUSTER 命令族、
    CROSSSLOT 执行（Phase 7.7）
  - [x] 多节点集群（一）：集群总线（port+10000）、gossip PING/PONG/MEET、
    CLUSTER MEET、节点表持久化、故障检测（Phase 7.8a）
  - [x] 多节点集群（二）：ADDSLOTS/DELSLOTS/SETSLOT 槽分配、-MOVED/
    -CLUSTERDOWN 所有权执行、owner 缓存（Phase 7.8b）
  - [x] 多节点集群（三）：DUMP/RESTORE/MIGRATE、SETSLOT MIGRATING/
    IMPORTING、ASKING 与 -ASK 重定向、双端迁移流程（Phase 7.9）
  - [x] 多节点集群（四）：CLUSTER REPLICATE 副本角色、config epoch 冲突
    裁决、自动故障转移 + CLUSTER FAILOVER TAKEOVER（Phase 7.10）
  - [x] PSYNC 部分重同步（master_replid + +CONTINUE/+FULLRESYNC 握手、
    复制积压区断点续传、陈旧 offset/replid 不匹配回退全量）、链式复制
    A→B→C、>64KiB 快照帧接收修复（Phase 12）
  - [x] 集群运维工具：reshard 工具（redis-cli 风格，Phase 16）
  - [x] ddup-bench 真并发引擎：-c 连接同时在线（单事件循环 + 非阻塞
    socket + 每连接 -P 在飞）、min/p50/p99/max 延迟直方图、-r 随机键；
    c50 P16 SET 310k→733k、GET 382k→858k+ req/s（Phase 17）
  - [x] Lua 脚本：vendored Lua 5.1.5、SHA1 脚本缓存、EVAL/EVALSHA/
    SCRIPT、redis.call/pcall、效果复制（AOF/复制流记录效果命令）、
    mt per-worker 执行（Phase 19）
  - [x] Redis 总线协议兼容：clusterMsg 编解码（大端 2256B 头）、
    cluster-bus-protocol ddup|redis 双协议、与真实 redis-server 混部
    收敛 + 槽位移交 CI（Phase 20）
  - [x] 深度优化与测试稳定化：集群 wire 测试全面改墙钟轮询（抖动
    根治）、wyhash 替换 FNV（rhtable：SET +19.6%/GET +13.5%）、
    skiplist 逐层 span（rank/index ~3 个数量级）、GET 路径分析与
    有界读排干（负结果如实记录）（Phase 21）
  - [ ] 范围化排除（记录在案，不实施）：
    - 分层存储（热/冷数据落盘）：超出"内存缓存存储"定位
    - mt 模式的复制/集群适配：见 Phase 15 说明
