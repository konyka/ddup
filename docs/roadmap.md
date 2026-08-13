# ddup 开发路线图

开发方式：TDD。每个小步骤 = 先写失败测试 → 实现 → 全部测试通过 → commit + push。

- [x] **Phase 0 — 脚手架**
  CMake 工程、C 标准探测（C23→C99）、自研测试框架、PAL 骨架（时间/平台宏）、
  Windows/Linux/macOS/FreeBSD 的 CMake 构建与 CTest CI（FreeBSD job 不安装
  OpenSSL）、文档骨架
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
  redis 风格配置文件 + 命令行覆盖、save 间隔自动快照、信号/命令优雅退出；
  配置化请求接收缓冲与复制快照大小上限
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
   - [x] TLS：可选 OpenSSL（独立 tls-port、非阻塞握手、DDUP_HAS_TLS 门控）（Phase 7.2）；
     Windows test_tls 修复；测试仅在构建时找到 OpenSSL 时注册（Phase 18：测试
     阻塞 socket 根因）
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
  - [x] IOCP 副本侧复制：REPLICAOF master link 接入 proactor
    （重叠 recv 完成驱动 repl_link_feed，大快照帧增量装载），双后端
    full-cycle + 断线重同步覆盖；io_uring 本就走 readiness 路径
    （Phase 22）
  - [x] mt 批量写出：完成队列 drain 轮末每连接一次 flush（seq 顺序
    不变），消除逐完成项 send 放大；A/B 中性如实记录（Phase 23）
  - [x] 混部集群 failover：FAILOVER_AUTH_REQUEST/ACK 投票帧、UPDATE
    通用收养、redis 模式副本投票提升（多数派 ACK + lastVoteEpoch）、
    混部杀主→提升→接槽→旧主让步全流程 interop CI（Phase 24）
  - [x] 分片发布订阅：SSUBSCRIBE/SUNSUBSCRIBE/SPUBLISH（槽属主校验）、
    PUBSUB SHARDCHANNELS/SHARDNUMSUB、总线 PUBLISH 帧传播（redbus
    type 10 / RCM2 type 4，入站 type 4 投递普通订阅者）、双向
    interop CI（Phase 25）
  - [x] PFAIL/FAIL 法定人数：主观怀疑状态 + 失败报告窗
    （node_timeout*2）、master 多数派提升 FAIL + FAIL 帧广播（RCM2
    type 5 / redbus type 3）、failover 门控于 FAIL、2 主死锁记录在案
    （Phase 26）
  - [x] 性能剖析与优化设施：场景矩阵（st/mt × c50-c1000 × P1-P64 ×
    16B/1KB + ping）、常开 IO 计数器（INFO # IO）、WPR 采样剖析；
    mt 唤醒去重（c500 P64 mt4 +55%）、传播 raw 转发、SET 单探测
    （进程内 +10.5%）（Phase 27）
  - [x] 高并发三方 bench CI（5 变体 × 4 场景）+ 其暴露的修复：
    io_uring SQ 数组根因、1KB 写路径（backlog 空汇跳过 + 融合存储，
    CI +4-7x）、mt 背压自旋 wedge 缓解（Phase 28）
  - [x] mt wedge 根因修复（关停 join 楔死：退避循环不查 running）
    + 吞吐崩塌修复（yield 退避 + 8192 深环）；mt4 c500 P64 首超 st
    （Phase 29）
  - [x] mt 结构化优化评估：SPSC 缓存行对齐（保留）；按目标分组合批
    经本机+CI 双重数据否定后回退（负结果如实记录）；mt4 c500 P64
    可达 ~1.8M/2.0M（Phase 30）
  - [x] mt 任务对象池：inline 命令存储 + 跨线程回收自由列表；顺带
    修复池化暴露的销毁顺序 bug；本机 c500 P64 +8.6%（Phase 31）
  - [x] IOCP 后端深度优化：AcceptEx 池（2 个在飞，保留）；RECV 先补投
    后处理（roff 偏移窗口模型实现正确但 loopback A/B 一致性 -4~-14%，
    负结果回退并记录）；send 零拷贝评估为不可行（resp_buf realloc
    悬垂），flush 纪律验证已满足（Phase 32a）
  - [x] io_uring op 模式 proactor 后端：IORING_OP_RECV/SEND/ACCEPT 真
    提交模型，镜像 IOCP 设计（共享 server.c proactor 路径）；
    multishot accept 自愈降级、SQE 批提交、zombie 排水；`--io
    iouring-op`（仅 st，默认关闭）；CI c500 P64 与 epoll 持平或略胜
    （Phase 32b）
  - [x] io_uring op 进阶：multishot recv + 256×64KB provided-buffer
    环（零补投、ENOBUFS 重武装、zombie 槽位回收）、SQPOLL 与
    DEFER_TASKRUN|SINGLE_ISSUER 探测（env 门控默认关）；registered
    send buffers/SEND_ZC 评估后不做（记录在案）；bench 常驻 repost
    vs 全栈双变体，全栈持平或略胜（Phase 33）
  - [x] 本地 Garnet 对垒定点优化：proactor 发送路径零拷贝 detach
    （st 中性、mt4 +2~3%，保留）；c500 P64/P16 计数器取证 IO 已最优
    （P64 63 命令/recv、1 send/批），差距归为单核天花板与 mt 扩展
    效率，记录在案（Phase 34c）
  - [x] 函数级剖析（WPR CPU 采样）：两个差距单元格无单点热点——成本
    均匀分布在 解析→resolve→分发→哈希→存取 分层链；解析器 $bulk
    快路径实现后 A/B 不胜噪声、按规则回退（未入历史）；最终矩阵与
    top-10 函数表入档（Phase 35）
  - [x] 命令热路径压平（bench_core 为判定仪器）：解析器 $bulk 内联
    （parse +20%）、rh_migrate_some 内联早退（GET +8%）、plain 会话
    lean GET/SET（GET +40%、SET +24%，6/6 全正）；expire 内联证不出
    胜利已回退；socket 终检 c500 P64 与 garnet 打平（Phase 36）
  - [x] mt 路由成本削减：WPR 取证路由层构成；crc16 逐位循环改表驱动
    （c500 P16 mt4 +5~8%，保留）；回复直写/批路由 v2/完成合批经分析
    或实测否决（竞态、分配搅动、排序契约、噪声下不可测），
    parsed-forward 实测负收益回退——否定结论全部入档（Phase 37）
  - [x] 全库健壮性加固（大小/溢出与失败路径审计）：arena 对齐与块
    分配溢出安全且失败不留半状态、rhtable 容量翻倍/槽位字节溢出
    检查与溢出安全负载计算、resp 聚合分配字节检查（先写本地聚合
    再一次性提交 out）、session MULTI 队列增长溢出检查
    （session_queue_push 返回错误，分发回 -ERR 不入队）、skiplist
    zsl_insert 错误返回 + 节点字节 uint64 溢出检查、obj_str 空
    payload 下溢防护、脚本缓存 sha1 长度/字符集校验 + registry
    ref 有效性复核（畸形条目重编译/flush 忽略）、io_uring
    readiness 后端 SQ 预留/部分提交推进/EINTR 重试、timeout 参数
    生命周期由栈改为环持有、TLS 库初始化改线程安全 once + fd 与
    读写字节 INT 上限、nodes.conf 落盘 close/rename 失败保留脏态
    并清理临时文件（Phase 38）
  - [x] 通用键命令补齐：TYPE/KEYS/SCAN/RENAME/RENAMENX/TOUCH/
    RANDOMKEY/EXPIRETIME/PEXPIRETIME；Redis 风格 glob 匹配器
    （src/ds/glob）、rhtable 游标迭代 rh_scan（主表+rehash 旧表
    虚拟桶下标，回调可删除——字段逐轮重读）；RENAME 对象所有权
    转移经 db_del_kv_keep_obj（免 obj_free 悬垂）；集群同槽/
    所有权与 mt 路由分类同步（KEYS/SCAN/RANDOMKEY mt 模式不支持，
    记录在案）（Phase 39）
  - [x] 字符串扩展命令：GETDEL/GETEX/SETEX/PSETEX/GETSET/SETRANGE/
    GETRANGE/INCRBY/DECRBY/INCRBYFLOAT；INCR/DECR 重构为共用 delta
    路径（溢出判定通用化）；SETRANGE 512MB 上限 + 补零写；
    GETRANGE Redis 归一化语义；INCRBYFLOAT strtold + %.17Lg
    （Phase 40）
  - [ ] 范围化排除（记录在案，不实施）：
    - 分层存储（热/冷数据落盘）：超出"内存缓存存储"定位
    - mt 模式的复制/集群适配：见 Phase 15 说明
