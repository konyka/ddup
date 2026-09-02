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
  - [x] mt 复制/集群适配：worker 0 作为复制/集群控制面；REPLICAOF/
    SLAVEOF 操作 worker 0 的 master link，全量快照经临时 db 解码后按
    hash slot 分区恢复到各 worker，命令流经 mt 路由分发；CLUSTER 命令
    与总线/故障检测只跑在 worker 0，节点/槽位元数据以不可变快照扇出
    到其余 worker，使每个 worker 都能独立给出 MOVED/CLUSTERDOWN；
    master 侧 SYNC/PSYNC 全量同步按 worker 本地序列化后封成 DDUPMT01，
    聚合/MOVE/EXEC 命令复制推流恰好一次；FLUSHDB/FLUSHALL/SWAPDB 各
    worker AOF 按实际变更记录，INFO # Replication 从 worker 0 渲染

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
    所有权与 mt 路由分类同步；mt 的 `KEYS` 广播合并 RESP 数组，
    `RANDOMKEY` 聚合返回任一非空分片键，`SCAN` 使用带 worker 索引的
    复合游标顺序遍历分片（Phase 39）
  - [x] 字符串扩展命令：GETDEL/GETEX/SETEX/PSETEX/GETSET/SETRANGE/
    GETRANGE/INCRBY/DECRBY/INCRBYFLOAT；INCR/DECR 重构为共用 delta
    路径（溢出判定通用化）；SETRANGE 512MB 上限 + 补零写；
    GETRANGE Redis 归一化语义；INCRBYFLOAT strtold + %.17Lg
    （Phase 40）
  - [x] Hash/List/Set 补齐：HSTRLEN/HRANDFIELD、LPOS(RANK/COUNT/
    MAXLEN)/LREM/LTRIM/RPOPLPUSH、LPOP/RPOP count 扩展（缺失 key 回
    null array）、SINTERCARD/SINTERSTORE/SUNIONSTORE/SDIFFSTORE
    （setop_eval 共用求值）；命令统计槽位 [128]→CMD_STATS_SLOTS 192
    （Phase 41）
  - [x] ZSet 补齐：ZPOPMIN/ZPOPMAX/ZREMRANGEBYRANK/ZMSCORE/
    ZRANDMEMBER/ZRANGEBYLEX/ZREVRANGEBYLEX/ZREMRANGEBYLEX；skiplist
    新增 member 字典序范围定位 zsl_first/last_in_lex_range（span
    逐层定位，同分假定与 Redis 一致不校验）（Phase 42）
  - [x] 模式订阅：PSUBSCRIBE/PUNSUBSCRIBE + PUBSUB CHANNELS/NUMSUB/
    NUMPAT；第三张注册表 rh_table patterns，PUBLISH 线性扫 pattern
    表 glob 匹配投递 pmessage；mt 模式使用 worker-local pattern registry
    和异步 pmessage fan-out；PUBSUB CHANNELS/NUMSUB/NUMPAT 在 mt 模式
    广播归并为全局视图
    （Phase 43）
  - [x] 收尾与耐久性：QUIT 回 +OK 后经 send-then-close 断连（双后端
    + mt）；AOF appendfsync always|everysec|no（pal_file_sync：
    fsync/FlushFileBuffers；everysec 单线程节流；sync 失败并入
    fail-closed 闩锁）；SET KEEPTTL/GET 选项；COPY 命令（DUMP/
    RESTORE 序列化深拷贝——裸 blob 对指针对象有别名风险，记录在案；
    mt 已支持跨库 COPY 的 worker-local 多库 session）（Phase 44）
  - [x] 紧凑编码：Redis 7 兼容 listpack（src/ds/listpack）+ quicklist
    重写 list 存储（每节点 ≤128 条目 listpack，端点分裂/空节点摘除，
    迭代器访问）；hash/zset 小对象 listpack 双编码（128 条目 / 64 字节
    阈值，单向转换不降级）；记账模型 128 条目小对象内存 list/hash/
    zset 分别 -78%/-67%/-92%（Phase 45）
  - [x] set 小对象 listpack 双编码（128 成员 / 64 字节阈值）；
    SPOP/SRANDMEMBER listpack 模式按随机下标直取（SPOP 逐个随机弹、
    member 先拷栈再删规避 realloc 悬垂）；记账模型 128 成员
    -81%（Phase 46）
  - [x] 编码阈值接入配置：`list-max-listpack-size` +
    `{hash,zset,set}-max-listpack-{entries,value}` 七键（Redis 命名与
    默认值；entries/value 为 0 关闭紧凑编码，list fill >= 1）；
    进程级 obj_limits 全局 + quicklist fill 全局，main 启动时一次性
    应用（st/mt 共用点，之后只读无竞态）（Phase 47）
  - [x] quicklist 稀疏节点合并：删除后节点 < fill/4 且与邻居合计
    <= 2*fill 时合并（优先 next、否则 prev；fill<4 不触发）；
    ql_remove 迭代器合并后按序号 lp_seek 重定位指向同一逻辑元素；
    仅删除路径触发，无压缩（Phase 48）
  - [x] OBJECT ENCODING 命令：编码名与 Redis 7 对齐（raw/quicklist/
    listpack/hashtable/skiplist），缺失 key 回 null bulk（Phase 49）
  - [x] Redis 7.2.15 命令兼容性盘点：`docs/redis-compat-audit.md` 固化
    101 个缺失顶层命令、11 个整体缺失容器、26 个容器子命令缺口及
    选项/语义差异；`tools/audit_redis_compat.py` 接入 CTest/CI 持续审计
    （Phase 50）
  - [x] Bitmap 补齐：`BITOP`（AND/OR/XOR/NOT，8 字节字批量运算、缺失源
    按零填充、目标可兼作源）、`BITFIELD`/`BITFIELD_RO`（u1-u63/i1-i64、
    GET/SET/INCRBY、负偏移、OVERFLOW WRAP/SAT/FAIL；整条命令先在本地
    缓冲完成，错误时不落库），st/mt 路由与集群 CROSSSLOT 同步（Phase 51）
  - [x] sorted_set 聚合与范围补齐：`ZUNIONSTORE/ZINTERSTORE/ZDIFFSTORE`、
    `ZUNION/ZINTER/ZDIFF/ZINTERCARD`（numkeys 布局一次解析；交集从最小
    操作数遍历并提前剪枝，STORE 直接物化为目标 zset，不做临时哈希表
    二次搬运）、`ZLEXCOUNT/ZREVRANGEBYSCORE/ZRANGESTORE/ZMPOP`；
    st/mt 路由与集群 key 抽取同步，兼容审计缺 81 个顶层命令（Phase 52）
  - [x] 增量扫描补齐：`HSCAN/SSCAN/ZSCAN`（LP 路径按游标索引直取，
    HT 路径复用 `rh_scan` 桶游标；MATCH 与 COUNT 一次解析，批量收集上限
    与 SCAN 一致的 32 项避免热路径分配），st/mt 路由同步，兼容审计
    缺 78 个顶层命令（Phase 53）
  - [x] 服务端与连接族补齐：`FLUSHALL`（复用 `FLUSHDB` 清库逻辑并
    广播到全部 logical db）、`TIME`（注入时钟秒/微秒）、`HINCRBYFLOAT`
    （复用 `parse_ld`/`%.17Lg`）、`READONLY`/`READWRITE`（会话
    cluster 连接状态）；st/mt 路由与集群 keyless 分类同步，兼容审计
    缺 73 个顶层命令（Phase 54）
  - [x] 命令别名与跨库/角色补齐：`SUBSTR`（`GETRANGE` 别名）、
    `SLAVEOF`（`REPLICAOF` 别名）、`MOVE`（跨逻辑库移动并保留绝对
    TTL；mt 模式在属主 worker 全库栈会话执行）、`ROLE`（主/从角色
    响应，复用 `session.repl`）；st/mt 路由与集群分类同步，兼容审计
    缺 69 个顶层命令（Phase 55）
  - [x] 非阻塞 list 族补齐：`LINSERT`（BEFORE/AFTER，quicklist 内原地
    listpack 插入）、`LMOVE`（LEFT/RIGHT 四向移动，复用 pop/push）、
    `LMPOP`（首个非空列表按 COUNT 弹出）；st/mt 路由与集群 key 抽取
    同步，兼容审计缺 66 个顶层命令（Phase 56）
  - [x] 连接协商与跨结构查询补齐：`RESET`（MULTI/WATCH/READONLY/pub/sub
    连接态一键复位，server 级 reset hook 清理订阅）、`HELLO`（RESP2 数组
    /RESP3 map，版本协商）、`LCS`（LEN 使用滚动 DP，字符串/IDX 使用完整
    DP 回溯并支持 MINMATCHLEN/WITHMATCHLEN）、`SORT`/`SORT_RO`（list/set/
    zset，BY/GET/LIMIT/ASC/DESC/ALPHA/STORE，归并排序无 qsort 全局态）；
    st/mt 路由与集群分类同步，兼容审计缺 61 个顶层命令（Phase 57）
  - [x] HyperLogLog 族补齐：`PFADD`/`PFCOUNT`/`PFMERGE`/`PFDEBUG`/
    `PFSELFTEST`（dense-only Redis 7 编码，MurmurHash64A + 16384 个 6-bit
    寄存器；PFCOUNT 多键临时合并单次 12KB 缓冲，不产生逐键分配；
    PFMERGE 对缺失源按空 HLL 处理）；st/mt 路由与集群 key 抽取同步，
    兼容审计缺 56 个顶层命令（Phase 58）
  - [x] GEO 族补齐：`GEOADD`/`GEODIST`/`GEOHASH`/`GEOPOS`/
    `GEORADIUS`/`GEORADIUS_RO`/`GEORADIUSBYMEMBER`/
    `GEORADIUSBYMEMBER_RO`/`GEOSEARCH`/`GEOSEARCHSTORE`
    （zset-backed 52-bit geohash 编码、WGS84 解码、GEOHASH 标准
    [-90,90] 重编码、HAVERSINE 距离；GEOADD NX/XX/CH；GEORADIUS
    STORE/STOREDIST、COUNT/ANY/ASC/DESC/WITH*；GEOSEARCH
    FROMMEMBER/FROMLONLAT/BYRADIUS/BYBOX；st/mt 路由与集群 key 抽取同步）；
    兼容审计缺 46 个顶层命令（Phase 59）
  - [x] 服务端运维/自省族补齐：`COMMAND`（COUNT/LIST/INFO/GETKEYS/DOCS）、
    `CLIENT`（ID/SETNAME/GETNAME/LIST/KILL）、`MEMORY`（USAGE/STATS/
    DOCTOR/PURGE/MALLOC-STATS）、`SLOWLOG`（GET/LEN/RESET，128 条环 +
    log-slower-than 可调）、`BGSAVE`/`BGREWRITEAOF`；client/slowlog 经
    session 钩子访问 server 连接表与环形日志，st/mt 路由与集群 keyless
    分类同步；兼容审计缺 40 个顶层命令、7 个整体缺失容器、43 个容器
    子命令（Phase 60）
  - [x] Stream 核心族补齐：`XADD`（NOMKSTREAM/MAXLEN/MINID/LIMIT 与
    自动/显式/部分自动 ID）、`XLEN`、`XRANGE`/`XREVRANGE`（`-`/`+`、
    `(` 排他边界、COUNT）、`XDEL`、`XTRIM`（MAXLEN/MINID + LIMIT）、
    `XSETID`（ENTRIESADDED/MAXDELETEDID）；stream 采用连续有序 entry
    数组 + 二分查找（追加 O(1) 摊还、区间 O(log N + K)，字段值单块
    连续分配），并接入 TYPE/快照/DUMP/RESTORE、st/mt 单键路由与集群
    同槽校验；兼容审计缺 33 个顶层命令（Phase 61）
  - [x] Stream 消费组/读取族补齐：`XGROUP`（CREATE/SETID/DESTROY/
    CREATECONSUMER/DELCONSUMER/HELP）、`XACK`、`XPENDING`（summary +
    range）、`XCLAIM`（IDLE/TIME/FORCE/JUSTID/LASTID/RETRYCOUNT）、`XAUTOCLAIM`
    （COUNT/JUSTID）、`XREAD`、`XREADGROUP`
    （GROUP/COUNT/NOACK/BLOCK，支持 session 阻塞、唤醒、超时及 BLOCK 0）、`XINFO`
    （STREAM/GROUPS/CONSUMERS/HELP）；PEL/consumer 采用连续数组，
    group_mem 记账不含 capacity 数组（记录在案）；快照 STREAM 负载新增
    必选 group 块；st/mt 路由与集群 key 抽取同步；兼容审计缺 25 个
    顶层命令、5 个整体缺失容器（Phase 62）
  - [x] 容器帮助与只读脚本别名：`COMMAND`/`CLIENT`/`MEMORY`/`SLOWLOG`/
    `OBJECT`/`CONFIG`/`SCRIPT`/`PUBSUB`/`CLUSTER` 补齐 `HELP`；新增
    `EVAL_RO`/`EVALSHA_RO`，只读脚本内写命令直接拒绝；兼容审计缺 23
    个顶层命令、5 个整体缺失容器、34 个容器子命令（Phase 63）
  - [x] 迁移/趣味命令补齐：`RESTORE-ASKING`（隐式 ASKING 的 RESTORE 别名，
    集群导入态写入）与 `LOLWUT`（VERSION 5/6 最小 ASCII art）；兼容审计
    缺 21 个顶层命令、5 个整体缺失容器、34 个容器子命令（Phase 64）
  - [x] 阻塞 list/zset pop 族：`BLPOP/BRPOP/BRPOPLPUSH/BLMOVE/BLMPOP/
    BZPOPMIN/BZPOPMAX/BZMPOP`；session 增加阻塞态与 argv 深拷贝，
    server 就绪循环持有/唤醒，COMMAND GETKEYS 与集群/mt 键抽取同步；
    兼容审计缺 13 个顶层命令、5 个整体缺失容器、34 个容器子命令
    （Phase 65）
  - [x] 脚本库族：`FCALL/FCALL_RO/FUNCTION`；FUNCTION 以名称保存 Lua 源码，
    FCALL 复用 EVAL 键/参执行，FUNCTION LOAD/DELETE/LIST/FLUSH/STATS/HELP；
    支持 `redis.register_function` 多函数库格式并按函数名执行；兼容审计缺 10 个
    顶层命令、4 个整体缺失容器、34 个容器子命令（Phase 66）
  - [x] 兼容性收尾：补齐 `CLIENT/CLUSTER/COMMAND/CONFIG/OBJECT/SCRIPT`
    剩余子命令，并注册 `WAIT/WAITAOF/REPLCONF/FAILOVER/MONITOR` 与
    `ACL/DEBUG/LATENCY/MODULE/SENTINEL` 容器（单机最小/占位语义）；
    兼容审计缺 0 个顶层命令、0 个整体缺失容器、0 个容器子命令
    （Phase 67）
  - [x] 选项/语义差分收尾：`ZADD` 补 `NX/XX/GT/LT/CH/INCR`，`ZRANGE`
    补 `REV/BYSCORE/BYLEX/LIMIT/WITHSCORES` 统一语法，`EXPIRE/PEXPIRE/
    EXPIREAT/PEXPIREAT` 补 `NX/XX/GT/LT`，`RESTORE/RESTORE-ASKING` 补
    `ABSTTL/IDLETIME/FREQ`（IDLETIME/FREQ 仅解析接受）；新增对应单测，
    `docs/redis-compat-audit.md` C 节相应差分清零（Phase 68）
  - [x] FUNCTION 序列化收尾：`FUNCTION DUMP/RESTORE` 使用 ddup 专用
    二进制 payload 与 `FLUSH/APPEND/REPLACE` 策略；兼容审计 C 节统一改为
    “已实现或记录在案”，命令级与选项/语义级遗留差分全部清零（Phase 69）
  - [x] 范围化排除（已记录在案，明确不实施）：
    - Lua 分布式锁脚本等 Garnet/单机缓存存储不适配项
  - [x] Redis 8 hash 字段级 TTL：`HGETDEL/HGETEX/HSETEX` 与
    `HEXPIRE/HPEXPIRE/HEXPIREAT/HPEXPIREAT/HPERSIST/HTTL/HPTTL/
    HEXPIRETIME/HPEXPIRETIME`；`obj_hash` 增加独立 `expires` 表，
    惰性删除过期字段，普通覆盖写清 TTL，`FNX/FXX/KEEPTTL/PERSIST/
    NX/XX/GT/LT` 语义对齐 Redis 8；命令统计槽位扩容 512。Redis 8.10.1
    审计缺 36 个顶层命令、3 个整体缺失容器、3 个容器子命令（Phase 70）
  - [x] Redis 8 set 基数：`SUNIONCARD/SDIFFCARD`（`LIMIT` 提前停，
    `SUNIONCARD APPROX` 精确返回）；Redis 8.10.1 审计缺 34 个顶层命令、
    3 个整体缺失容器、3 个容器子命令（Phase 71）
  - [x] Redis 8 字符串安全/原子性增量：`INCREX`、`MSETEX`、`DELEX`、
    `DIGEST`；覆盖溢出/过期边界、批量前置校验、条件删除、XXH3 固定宽度
    摘要与 TDD 回归；审计缺口收敛为 23 个顶层命令（Phase 72）
  - [x] Redis 8 集群槽维护：`SFLUSH`（按本节点所有权求交并返回合并范围）
    与 `TRIMSLOTS RANGES`（禁止清理本节点服务槽，未服务槽安全删除）；
    TDD 覆盖边界、错误参数、数据删除与槽所有权（Phase 73）
  - [x] Redis 8 `CLUSTER SLOT-STATS`：实现 `key-count` 的 `SLOTSRANGE` 与
    `ORDERBY/LIMIT/ASC/DESC`，不伪造未维护的 CPU/内存/网络指标；TDD 与
    审计同步（Phase 74）
  - [x] Redis 8 内部集群迁移命令安全门：注册 `CLUSTER MIGRATION` 与
    `CLUSTER SYNCSLOTS`，外部会话明确拒绝且不修改迁移/槽元数据（Phase 75）
  - [x] Redis 8 管理容器：注册 `BACKUP/HIMPORT/HOTKEYS` 及已知子命令名；
    `HIMPORT PREPARE/SET/DISCARD/DISCARDALL` 以 session-local fieldset 和
    批量 hash 写入实现；`HOTKEYS` 提供带 DURATION/SAMPLE/SLOTS 校验的
    server-owned 生命周期与低开销命令计数；`BACKUP` 复用原子多库快照实现
    `START/STATUS/SEAL/LIST/ABORT/CLEANUP`，AOF 模式记录 durable offset 并在
    SEAL 生成原子 `.backup.aof` 增量文件；ddup 无 AOF rewrite/segment 回收
    路径，因此 durable offset + sealed immutable delta 即为等价安全 pinning
    边界（Phase 82）
  - [x] Redis 8 ARRAY 核心：`ARSET/ARGET/ARLEN/ARCOUNT`；`obj_array` 用
    稀疏索引表提供 O(1) 随机访问和 O(1) 长度/非空计数，支持快照持久化，
    并以 TDD 覆盖缺失键、连续写、边界索引与 WRONGTYPE（Phase 77）
  - [x] Redis 8 ARRAY 访问/删除：`ARGETRANGE/ARMGET/ARDEL/ARDELRANGE`；
    稀疏查找保持按请求数量复杂度，删除会同步内存、计数、长度和空对象
    自动回收（Phase 78）
  - [x] Redis 8 ARRAY 完整命令级收尾：`ARMSET/ARNEXT/ARSEEK/ARINSERT/ARRING/
    ARSCAN/ARINFO/ARLASTITEMS/AROP/ARGREP`；批量写、插入游标、环写、范围
    扫描、基础聚合和 EXACT/MATCH/GLOB 搜索均以 TDD 覆盖，8.10.1 命令审计
    缺口清零（Phase 79）
  - [x] 语义差异收敛：`ARGREP RE` 增加平台无关正则子集、`LIMIT/NOCASE`，
    `ARINFO FULL` 返回稀疏目录统计，`XCLAIM RETRYCOUNT` 更新投递次数并补齐
    回归测试（Phase 80）
  - [x] Stream 阻塞读取收尾：`XREAD/XREADGROUP BLOCK` 接入统一 session
    blocked 状态；新 entry readiness 唤醒、deadline 超时和 `BLOCK 0` 均以
    TDD 覆盖，保持多 stream、COUNT、NOACK、group 游标与 NOGROUP 安全语义
    （Phase 81）
  - [x] Redis 8 运行时管理收尾：`HIMPORT PREPARE/SET/DISCARD/DISCARDALL`、
    `HOTKEYS` 生命周期/计数、`BACKUP` 安全快照生命周期和 `MONITOR` 实时
    命令流以 TDD 覆盖；HOTKEYS 已收敛为预分配、有界 Top-K、SLOTS 过滤和
    实测 dispatch/请求字节指标，CPU/NET 双指标独立 Top-K 排序，BACKUP
    durable delta 生命周期已完成；Redis
    内部 hash-template/MP-AOF segment 编码不改变 ddup 外部语义
    （Phase 83）
  - [x] mt 集群路由安全收敛：`ASKING` 不再被误判为不支持命令，按连接
    会话保留一次性标志并交给现有 cluster ownership 检查；其余全库扫描和
    MIGRATE 已按源 key hash 路由到 source worker，并复用有界网络迁移逻辑；
    IOCP/io_uring-op 不安全迁移场景返回明确错误；mt SHUTDOWN 已实现
    home-worker shutdown hook 与全池协调停止
  - [x] Redis 字符串 TTL 语义收敛：`INCR/DECR/INCRBY/DECRBY/INCRBYFLOAT`
    与 `APPEND` 的读改写路径保留绝对过期时间，SET 风格覆盖仍清除 TTL；
    以注入时钟 TDD 覆盖过期保留与边界行为（Phase 84）
  - [x] glob 边界语义收敛：未闭合 `[` 字符类按 Redis
    `stringmatchlen` fail-closed，补充二进制安全回归测试（Phase 85）
  - [x] 运行时 AOF 策略配置：`CONFIG GET/SET appendfsync` 通过 server-owned
    session hook 实时读取/更新 AOF writer，非法策略 fail-closed 并以 TDD
    覆盖（Phase 86）
  - [x] mt Redis 8 路由收尾：字段 TTL/字符串安全/ARRAY 单 key 命令按 owner
    worker 分发；`MSETEX`、`SUNIONCARD`、`SDIFFCARD` 按声明 key 位置做
    同 worker 校验并在跨 worker 时返回 `CROSSSLOT`；新增连接级行为回归
    （Phase 87）
  - [x] mt sharded pub/sub 收尾：`SSUBSCRIBE/SUNSUBSCRIBE/SPUBLISH` 按频道
    owner worker 路由，独立维护 shard 订阅 kind 并发送 `smessage`；TDD
    覆盖跨 worker 注册、投递和退订（Phase 88）
  - [x] mt 列表移动路由收尾：`LMOVEM` 按 source/destination 双 key 做
    CROSSSLOT 校验，忽略方向、COUNT 和排序选项；新增跨连接 owner 回归
    （Phase 89）
  - [x] mt 对象元数据路由收尾：`OBJECT ENCODING/REFCOUNT/FREQ/IDLETIME`
    按第三参数 key 路由到 owner worker；新增跨连接 `OBJECT ENCODING` 回归
    （Phase 90）
  - [x] mt Stream 扩展路由收尾：`XDELEX/XACKDEL/XNACK` 纳入单 key owner
    分类，按 stream key 跨 worker 转发并保持流水线顺序；新增删除、消费组
    和重投递控制命令的跨连接 TDD 回归（Phase 91）
  - [x] 可移植性与输入安全收尾：C99 静态断言降级避免局部 typedef 警告；
    `nodes.conf` 节点地址解析在发布前校验可表示长度，超长地址 fail-closed，
    端口范围校验，并以 C99/默认标准 TDD 回归验证（Phase 92）
  - [x] mt 全量复制完成通知安全收敛：`MT_TASK_RESTORE` 使用无连接的 barrier
    完成任务，completion drain 先递减快照 pending 再读取连接状态，消除
    hardening 并发下的空指针解引用；全量 mt 回归与 hardening 定向回归通过
    （Phase 93）
  - [x] 集群少数派可用性门控：`CLUSTER INFO` 同时校验槽覆盖、FAIL 持有者和
    持槽 master 多数派可达性；完整覆盖但多数派失联时 fail-closed，并以
    两主/三主 TDD 回归锁定，保留两主无法自动 FAIL 的投票语义（Phase 94）
  - [x] C99 原子降级安全收敛：GCC/Clang 使用 `__atomic`，MSVC 使用
    Interlocked，补充 PAL 原子封装四线程并发 TDD，避免强制 C99 mt 复制状态
    的数据竞争（Phase 95）
  - [x] 迁移元数据防御性回归：`ASKING` 遇到非法 `IMPORTING` owner 索引时
    fail-closed 返回 `CLUSTERDOWN Hash slot not served`，并补充默认/C99
    构建的越界解引用回归测试（Phase 96）
  - [x] io_uring 固定发送缓冲与 SEND_ZC：PAL 注册有界 `iovec` 槽位，提供
    fixed SEND/SEND_ZC API 和通知后释放语义；服务器通过
    `DDUP_IOU_SEND_ZC=1` 显式启用，槽位耗尽或 UAPI/运行时不支持时安全回落
    普通发送（Phase 97）
  - [x] TLS 复制 master link：新增客户端 TLS PAL、非阻塞 outbound 握手、
    `tls-replication`/`tls-ca-file` 配置和 mt passthrough；TLS 复制强制
    readiness backend，证书校验由 CA 文件显式开启（Phase 98）
  - [x] 哈希表扩容阈值缓存：初始化/扩容时预计算 85% load threshold，
    移除插入热路径上的重复整数除法与乘法，并以 TDD 锁定扩容时机（Phase 99）
  - [x] 整数 RESP 写出优化：两位数字查表减少十进制转换除法，并以边界
    TDD 锁定 `0`/`UINT64_MAX` 格式化结果（Phase 100）
  - [x] RESP 长度解析热路径：bulk/aggregate 长度使用受 1 GiB 上限约束的
    `size_t` 专用解析器，`*N` bulk 快路径复用，保留有符号整数解析语义；
    补充 `-1`、非法字符与超限边界 TDD（Phase 101）
  - [x] RESP 长度位数分层：1–9 位长度走紧凑累加，10 位统一上限比较，
    超过 10 位在扫描前拒绝；补充超长输入回归并保持 C99/安全边界（Phase 102）
  - [x] RESP 长度前导零兼容性：位数早拒绝仅针对有效数字，保留填充长度
    的 Redis 客户端兼容性，并以 TDD 覆盖 `$000...` 边界（Phase 103）
  - [x] RESP 整数解析快速分支：少于 19 位整数走无逐位溢出检查的累加，
    19 位严格校验有符号边界，前导零/`-0` 保持兼容（Phase 104）
  - [x] RESP 填充长度累加优化：bulk 长度跳过前导零后再累加，全零长度
    直接返回 0；补充填充/全零回归（Phase 105）
  - [x] 整数 RESP 解析微基准：`bench_core` 增加真实 parser 循环，覆盖
    短正/负数及 `LLONG_MAX`，用于后续 A/B 测量（Phase 106）
  - [x] 整数 parser 微基准校准：样本长度预计算，移除循环内 `strlen()`
    干扰并记录校准后的测量范围（Phase 107）
  - [x] RESP 整数边界回归扩展：覆盖 `LLONG_MAX/MIN`、正负溢出、`-0`
    和填充极限输入，默认/C99/hardening parser 均通过（Phase 108）
  - [x] RESP 整数解析属性回归：通过完整 wire 覆盖 `-128..128` 步进值及
    `LLONG_MIN/MAX` 边界，共 919 checks（Phase 109）
  - [x] 解析基准多轮中位数：同一 Release 构建连续 5 次取中位数，记录
    parse-only 与 integer parser/writer 的稳定趋势数据（Phase 110）
  - [x] PAL 能力探测收口：新增布尔宏 `DDUP_HAS_WYHASH`，集中管理宽乘法
    编译器/目标能力检测，哈希表核心仅依赖 PAL 宏并补充 C99/默认构建测试
    （Phase 111）
  - [x] CRC16 路由表并发安全：将 hash-slot CRC 表从无同步懒初始化改为
    编译期只读表，消除多 worker 首次路由数据竞争并保留单次查表性能；新增
    并发与参考实现回归（Phase 112）
  - [x] CRC64 校验表并发安全：将 DUMP/RESTORE CRC64 表从无同步懒初始化改为
    编译期只读表，消除并发校验数据竞争并保留链式 CRC 语义；新增多线程回归
    （Phase 113）
  - [x] 命令哈希一次性发布：PAL 增加跨 C11/C99/Windows 的 compare-exchange
    封装，`cmd_hash` 使用 acquire/release 状态机安全发布，避免 mt 首次解析
    命令时并发构建表（Phase 114）
  - [x] Windows QPC 频率缓存并发安全：计时 PAL 使用原子 once-publish 状态机
    发布 `QueryPerformanceFrequency` 结果，失败时安全回落到非零频率；新增
    多线程首次计时回归（Phase 115）
  - [x] io_uring 固定发送槽位轮转提示：sbuf acquire 维护下一个探测槽位，
    连续周转避免从槽位 0 重复扫描忙槽位；互斥内原子式 busy 标记和
    SEND_ZC 通知回收语义不变，并以轮转顺序 TDD 锁定（Phase 116）
  - [x] Lua 脚本禁调命令词长固化：黑名单保留大小写不敏感语义，使用编译期
    词长消除每次 `redis.call/pcall` 的静态字符串 `strlen()` 扫描；新增全名单
    和混合大小写回归（Phase 117）
  - [x] Lua 脚本禁调命令长度分桶：按命令长度先行筛选候选，非黑名单长度
    零次比较，双候选长度最多两次比较；新增探测计数 TDD（Phase 118）
  - [x] Lua 命令桥接一次性发布：`g_cmd_fn` 通过 PAL acquire/release 状态机
    安全发布，避免并发 DB 初始化与脚本执行的数据竞争；新增 8 线程发布回归
    （Phase 119）
  - [x] RESP 流式解析 arena 回滚：不完整聚合或协议错误返回前回滚本次
    speculative allocations，保留块用于复用，避免恶意半包重解析持续增长；
    新增重复不完整聚合与 arena mark/rewind TDD（Phase 120）
  - [x] RESP 超大聚合预分配门控：按每个子项至少 3 字节的输入下界，在
    arena 分配前拒绝明显不完整的百万级聚合头；新增不分配回归，避免半包
    触发与输入无关的大内存申请（Phase 121）
  - [x] `CLUSTER SLOT-STATS memory-bytes`：复用单次 key 表扫描，按槽统计
    entry 与对象额外内存并以饱和加法防溢出；支持 `SLOTSRANGE` 输出和
    `ORDERBY memory-bytes`，CPU/network 因缺少逐槽计量仍显式拒绝（Phase 122）
  - [x] `CLUSTER SLOT-STATS` CPU/network 指标：在 cluster session dispatch
    边界累计 `cpu-usec`、`network-bytes-in/out`，仅归属可解析为单槽的
    数据命令；支持 `SLOTSRANGE`、`ORDERBY`、`LIMIT`、`ASC/DESC`，计数器
    饱和且无额外热路径分配（Phase 123）
  - [x] 脚本 key 位置统一：`EVAL_RO/EVALSHA_RO`、`FCALL/FCALL_RO` 纳入
    集群跨槽校验、路由和 `COMMAND GETKEYS`，按 `numkeys` 提取真实 key，
    防止将脚本/函数名误作为数据 key（Phase 124）
  - [x] 集群路由 key 提取补全：`MSETNX`、`HIMPORT SET`、`SUNIONCARD`、
    `SDIFFCARD` 纳入 ownership 检查，避免多节点模式下将参数或子命令误作
    key 导致错误 `MOVED` 或漏检（Phase 125）
  - [x] mt 脚本 owner 路由：`EVAL/EVALSHA/EVAL_RO/EVALSHA_RO` 与
    `FCALL/FCALL_RO` 按声明的 `numkeys` 转发到目标 worker，跨 worker
    请求拒绝为 `CROSSSLOT`，并以真实 socket 回归验证脚本写入可见性
    （Phase 126）
  - [x] mt 槽维护安全闸：`SFLUSH` 与 `TRIMSLOTS` 在缺少跨 worker 广播
    事务时显式返回 `ERR command not supported in mt mode`，禁止仅在 home
    worker 局部删除而造成静默数据不一致（Phase 127）
  - [x] mt 聚合内存不足 fail-closed：`DBSIZE/FLUSHDB/FLUSHALL/INFO/KEYS` 等
    聚合描述符分配失败时统一返回 `ERR out of memory`，禁止退化为 home worker
    局部执行；新增故障注入测试验证 `FLUSHDB` 不会部分清理（Phase 128）
  - [x] mt session-local 安全路由：`HIMPORT SET` 在无法携带连接 fieldset
    到远端 worker 时 fail-closed；`MEMORY USAGE <key>` 补齐 key-owner 路由，
    并在 Cluster ownership 检查中识别该 key（Phase 129）
  - [x] SORT STORE 双 key 安全路由：MT 与 Cluster 同时检查排序源 key 和
    `STORE` 目标 key；跨 worker/slot 在执行前返回 `CROSSSLOT`，并以真实
    loopback 回归锁定无部分写入（Phase 130）
