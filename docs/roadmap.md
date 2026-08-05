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
  - [x] ZSet：skiplist（无 span）+ ZADD/ZSCORE/ZCARD/ZINCRBY/ZREM/ZRANGE/ZREVRANGE/ZRANK/ZREVRANK/ZCOUNT/ZRANGEBYSCORE/ZREMRANGEBYSCORE（Phase 5.2）
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

- [ ] **Phase 7+ — 长期**
  集群模式、TLS、io_uring 优化落地、SIMD 解析优化、与 Garnet/Redis 基准对比
  - [x] 主从复制：全量同步（SYNC 快照帧 + 命令推流）、REPLICAOF/NO ONE、
    只读副本、断线全量重同步（无 PSYNC，backlog 预留）（Phase 7.1）
  - [x] TLS：可选 OpenSSL（独立 tls-port、阻塞握手、DDUP_HAS_TLS 门控）（Phase 7.2）
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
  - [ ] PSYNC 部分重同步、复制积压区利用、链式复制拓扑测试
  - [ ] 集群运维工具：reshard 工具（redis-cli 风格）、真正的 Redis 总线
    协议兼容
