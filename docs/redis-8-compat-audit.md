# Redis 8.10.1 命令兼容性盘点

> 基线：Redis 8.10.1 官方 `src/commands/*.json`（459 个 JSON、459 条命令条目）。
> 对照：ddup `src/core/command.c` `CMD_TABLE`。
> 生成：`python3 tools/audit_redis_compat.py --fetch /tmp/redis810 --tag 8.10.1 --repo . --json`。
> 本文件维护 Redis 8 增量缺口；Redis 7.2.15 基线见 `docs/redis-compat-audit.md`。

## 总览

| 类别 | 数量 |
| --- | --- |
| 缺失顶层命令 | 0 |
| 整体缺失容器 | 0 |
| 已实现容器内缺失子命令 | 0 |

最近一次本地复核（2026-09-01）：

```sh
python3 tools/audit_redis_compat.py \
  --redis-json /home/timeshift/opensource/redis-8.10.1/src/commands \
  --tag 8.10.1 --repo .
```

输出确认 Redis 命令条目 459、JSON 文件 459，ddup 顶层命令 290，
缺失顶层命令/容器/子命令均为 0。该命令为只读检查，可在源目录位置变化时
替换 `--redis-json` 路径后重复执行。

协议边界补充复核：静态 RESP 错误消息统一使用字面量的编译期长度，避免
错误文本长度漂移造成截断或越界读取；`HIMPORT SET` 字段数错误已由命令测试
锁定完整 wire 响应。

复制协议补充复核：PSYNC 服务端在计算 backlog 可恢复范围和生成 `+CONTINUE`
前验证环形缓冲元数据（容量、起点、长度及绝对偏移关系）。检测到内部损坏时
直接拒绝握手，不写入响应缓冲，也不增加 replica 计数；合法 FULLRESYNC 与
partial-resync 语义保持不变。

Redis cluster bus 发布补充复核：原生 `PUBLISH` 与 `PUBLISHSHARD` 在被服务
端投递前必须携带零 gossip、完整的 8 字节长度字段，并且 channel/message
长度之和精确覆盖帧尾。截断或尾随 payload 被丢弃，未知消息类型仍保持 Redis
兼容的容忍忽略行为。

TLS 复制与 cluster bus 补充复核：在配置 CA 时，outbound TLS 客户端除验证
证书链外还将握手绑定到 `REPLICAOF` master host 或 cluster peer 地址；不匹配
的受信任证书不能建立链接。未配置 CA 的显式兼容模式仍维持原有不校验证书语义。

Cluster bus 服务端补充复核：在 topology codec 之前的发布快速投递路径同样
验证协议 magic、内嵌总长度、Redis 原生零 gossip 和 payload 精确边界，确保
带尾随数据或伪造长度的发布帧不会被本地订阅者接收。

测试可靠性补充：cluster migrate、failover 和 pfail 回归夹具在写入节点固定
字段前检查 `cluster_node_add()` 返回值，避免在内存失败注入或 LTO 静态分析下
掩盖有效的协议语义回归。

Cluster bus 配置补充复核：服务端 setter 现在只接受 ddup 与 Redis 两个公开
协议枚举；非法数值不会改变当前协议，避免后续连接在未定义 wire 模式下运行。

io_uring 发送可靠性补充复核：SEND_ZC 的 fixed buffer 和 notification 状态
由统一 helper 在 live/zombie completion 中释放和复位；连接关闭后仍等待最终
CQE，再回收连接对象，避免内核仍持有 buffer 时发生复用或泄漏。

迁移控制面补充复核：`MIGRATE` 内部 API 对目标 host、key 数组和数量组合做
fail-closed 校验；`nkeys == 0` 仍返回 no-op，合法的 `KEYS`/多 key 迁移协议
和目标确认后删除语义不变。

迁移超时补充复核：deadline 计算采用 64 位饱和加法，避免极大 timeout 在
墙钟毫秒值相加时回绕；正常 MIGRATE 超时、目标确认和部分删除语义保持不变。

Cluster bus 地址补充复核：自身监听、节点公告和 `CLUSTER MEET` 的 bus 端口
均验证 `base_port + 10000` 可表示为 16 位端口；超范围输入被拒绝而不会回绕
到无关端口，合法 Redis 端口映射保持不变。

Cluster announce 一致性补充复核：已绑定 cluster bus listener 后公告端口不能
再被修改，避免 peer 获得未监听地址；合法 IP 公告变更同步写入 myself node，
后续 gossip/slots 响应使用同一地址状态。

ACL 诊断补充复核：`ACL LOG` 的 `age-seconds` 在 wall-clock 回拨时饱和为零，
避免无符号减法将近期安全事件显示为异常大的年龄；正向时间语义不变。

Cluster 故障检测补充复核：failure report 有效期与 peer liveness 在时钟前进
时才计算 elapsed；wall-clock 回拨不会把新鲜报告删除或把近期节点误标为
PFAIL，正向 timeout/quorum 语义不变。

HOTKEYS 诊断补充复核：采样窗口 duration 使用有界 elapsed 检查；wall-clock
回拨不会因无符号下溢提前停止采样，正常 START/GET/STOP 生命周期不变。

MIGRATE 空输入补充复核：合法的零 key 请求在分配和网络连接前确定地完成为
no-op，不依赖 C 运行时对 `malloc(0)` 的返回约定；非空请求语义不变。

集群持久化/总线槽位文本的输入复核：槽位解析按有界 token 扫描，拒绝非数字、
缺失范围端点和溢出十进制值；错误 token 不会阻断后续合法槽位恢复，且不会导致
加载或 gossip 处理线程进入死循环。该防御不改变合法 Redis cluster node slot
范围的渲染和解析语义。

nodes.conf 复核：ping、pong 和 config epoch 以输入行的明确长度为界进行严格
64 位十进制解析；非数字或溢出字段会在节点表更新前被拒绝。这保留有效 Redis
节点行的兼容性，同时消除无 NUL 终止持久化数据上的越界 libc 解析风险。

集群总线帧复核：RCM2 sender 扩展在节点发布前完成完整长度校验，截断或伪造
长度的帧不会留下半初始化节点状态；有效 PING/PONG/MEET 的 gossip 与槽位合并
路径保持不变。

集群 gossip 完整性复核：接收 RCM2/RCMB 节点帧时，先预检 gossip count 及每个
条目的地址、槽位文本和 epoch 扩展，再发布 sender 或 gossip 节点。截断 gossip
帧会原子拒绝，不会把部分拓扑状态暴露给后续路由与故障检测。

io_uring 发送生命周期复核：固定 buffer 与 `SEND_ZC` 提交现在要求槽位处于
`sbuf_acquire` 持有状态，防止调用方提前 release 后仍提交内核请求；正常注册
缓冲区和零拷贝通知语义保持不变。

集群 gossip 地址复核：gossip 条目的 `ipl` 在预检和解码阶段均限制在本地固定
地址字段可表示范围内，超长地址帧被拒绝且不修改拓扑；合法节点地址行为不变。

io_uring SEND_ZC 错误路径复核：终止 CQE 现在在清除通知标志前完成固定槽位释放
和 pending-op 计数，确保异常关闭与僵尸连接回收不会遗留异步引用。

AOF 持久化复核：`appendfsync everysec` 在墙钟回拨后不会因无符号时间差下溢而
跳过同步，而是在下一次 flush 建立新的 durability barrier；单调时钟下仍保持
Redis 兼容的一秒节流语义。

集群总线身份复核：sender/gossip node ID 现在必须满足 40 位小写十六进制节点
标识契约，v2 master ID 仅允许合法节点标识或 `-`。不符合格式的帧在拓扑变更前
被拒绝，避免伪造身份参与槽位声明和故障仲裁。

集群回归夹具同步完成分配结果检查，确保身份/epoch 兼容性测试本身不会因模拟
OOM 产生未定义写入，从而使协议边界结论可重复。

复制握手复核：FULLRESYNC/CONTINUE 返回的 replid 现在必须是 40 位小写十六进制，
非法 replid 在缓存或切换 streaming 状态前被拒绝，避免伪造复制历史污染 PSYNC。

Redis cluster bus 回归夹具同步检查节点分配结果，避免模拟 OOM 时出现未定义字段
写入，使 redbus 编解码兼容性结论在严格警告构建下可重复。

Redis 原生 cluster bus 复核：sender/gossip 及 UPDATE/FAIL/AUTH 相关节点身份均
按 40 位小写十六进制契约校验，非法身份帧在节点表或槽位状态更新前被拒绝，避免
伪造节点参与原生 Redis 集群故障和选主流程。

Redis 原生 cluster bus 帧完整性复核：不同消息类型现在严格匹配 gossip count
及 payload 长度，检查 count 乘法溢出并拒绝尾随数据；UPDATE/FAIL/AUTH 截断或
伪造负载不会进入槽位、故障或选主状态机。

原生 cluster bus 节点发布复核：sender/slaveof 身份在 `apply_node` 插入节点表前
完成校验，非法 master ID 会原子拒绝，不会留下半初始化拓扑节点。

原生 cluster bus API 边界复核：构帧和接收入口现在拒绝 NULL 节点表、输出、帧及
非零长度 NULL channel/message，避免异常管理或网络错误路径解引用无效对象。

MT 管理命令复核：SWAPDB 的 worker 快路径现拒绝前导空格、尾随字符和溢出
数据库索引，避免 libc 宽松解析造成错误数据库交换；合法索引行为保持不变。

PAL 可靠性补充：io_uring pbuf/SQPOLL 状态和测试注入入口对空 ring 统一
fail-closed，避免管理/诊断调用破坏服务进程。

TLS 可靠性补充：PAL TLS 上下文、握手和 I/O 入口拒绝空对象、空证书路径及
无效套接字；真实 TLS replication/cluster 测试仍覆盖成功握手与失败回收。

安全随机源补充：`pal_secure_random` 对非零长度空缓冲区 fail-closed，避免
ACL 密码生成等安全路径把无效指针传给平台随机数接口。

持久化 PAL 补充：文件打开、读写、同步和路径操作对空参数统一 fail-closed，
避免损坏输入穿透到 stdio/平台文件 API。

持久化边界回归覆盖空路径、空句柄及 NULL 数据组合，保证 AOF、快照和 Tier
调用共享同一跨平台失败语义。

复制控制面补充：REPLICAOF、backlog resize 和 TLS replication 配置入口对空
server fail-closed，避免非法管理调用破坏复制状态。

服务器配置面补充：TLS/AOF、快照、节点加载、资源限制和生命周期入口统一拒绝
空 server/空路径，避免管理调用导致进程崩溃或半初始化状态。

连接控制面补充：AOF 诊断、监听/唤醒、路由和迁移辅助 API 对 NULL/无效 fd
fail-closed；输出失败路径不会再访问空连接。

基础查询面补充：server 元数据、缓冲池、数据库选择和 I/O 计数 API 对空对象
及越界索引统一返回确定结果，避免管理调用触发非法内存访问。

事件循环补充：`server_run_once` 对空 server 统一返回失败，防止生命周期错误
穿透到过期处理、路由和后端 I/O 调度。

集群控制补充：节点超时、总线协议、集群 TLS 和 nodes.conf 保存辅助入口对空
server fail-closed，避免无效管理调用破坏集群状态。

集群节点表补充：检测损坏的 `nnodes` 元数据并在查找、渲染、构帧和状态判定中
拒绝越界遍历，防止 malformed topology 触发内存破坏。

集群故障状态补充：`nreports` 异常值 fail-closed，跨 worker 恢复状态在复制前
验证节点数量，防止损坏 topology 或报告表扩散成越界访问。

状态恢复补充：逐节点验证故障报告计数，任何异常都拒绝整份 topology 快照，
避免部分恢复造成 worker 间集群状态分叉。

总线输入补充：gossip/FAIL/PONG 帧处理在解析前验证本地节点计数，损坏 topology
直接拒绝，避免恶意帧触发越界合并。

历史缺口集中在三块，当前 Redis 8.10.1 命令级审计已清零：

1. Redis 8.8 新增 `ARRAY` 类型（18 个 `AR*` 命令）。
2. Redis 7.4/8.0 hash 字段级 TTL 与 `HGETDEL/HGETEX/HSETEX`（13 个命令）。
3. Redis 8.2+ 的运维/诊断命令，以及少量 Cluster 子命令。

## 已实现增量

- Hash 字段级 TTL 全族：`HGETDEL/HGETEX/HSETEX` 与
  `HEXPIRE/HPEXPIRE/HEXPIREAT/HPEXPIREAT/HPERSIST/HTTL/HPTTL/
  HEXPIRETIME/HPEXPIRETIME`。`obj_hash` 增加独立 `expires` 表，字段
  TTL 为绝对毫秒时间戳；读路径惰性删除过期字段，全量视图先 purge，
  空 hash 自动删除 key。
- Set 基数：`SUNIONCARD/SDIFFCARD` 支持 `LIMIT` 提前停与 union 的
  `APPROX` 参数解析；复用 `obj_set` 双编码遍历，临时 `rh_table` 去重，
  不物化完整结果集。
- String safety/atomicity tranche: `INCREX` uses checked integer/long-double
  arithmetic and bounded expiry parsing; `MSETEX` validates the complete batch
  before mutation and supports `NX/XX/KEEPTTL/EX/PX/EXAT/PXAT`; `DELEX`
  supports conditional string deletion; `DIGEST` uses vendored XXH3 with
  fixed-width output. All four commands are covered by TDD tests.
- Redis-compatible string read-modify-write semantics preserve absolute key TTL
  for `INCR/DECR/INCRBY/DECRBY/INCRBYFLOAT/APPEND`; SET-style overwrites retain
  their documented TTL-clearing behavior.
- Runtime `CONFIG GET/SET appendfsync` now updates the active server AOF writer
  (`always`, `everysec`, or `no`) and rejects invalid policies atomically.
- `CLUSTER INFO` also enforces Redis minority-partition availability: besides
  slot coverage and FAIL holders, serving masters must be reachable by majority;
  a fully covered minority partition therefore fails closed.
- Forced C99 builds retain thread-safe mt replication state: GCC/Clang use
  compiler `__atomic` primitives and MSVC uses Interlocked operations.
- io_uring op-mode optionally uses registered/fixed send buffers and
  `SEND_ZC` when `DDUP_IOU_SEND_ZC=1`; notification CQEs pin buffers until
  kernel completion, while unsupported kernels and pool exhaustion fall back
  to ordinary SEND without changing the wire protocol.
- Outbound replication supports TLS with non-blocking client handshakes;
  `tls-replication yes` enables it and `tls-ca-file` enables peer verification.
  TLS replication forces the readiness backend because the existing proactor
  contract does not carry TLS WANT_READ/WANT_WRITE state.
- Cluster bus connections support optional TLS with independent server/client
  contexts, non-blocking handshakes, and strict certificate validation when
  `tls-ca-file` is configured. `tls-cluster yes` requires `cluster-enabled yes`
  and readable certificate/key files; failed handshakes close the connection
  before gossip state is applied.
- ACL uses a bounded server-owned user table with atomic rule replacement,
  constant-time password comparison, command bitsets, and allocation-free key
  pattern matching. `AUTH`, `WHOAMI`, `LIST`, `USERS`, `SETUSER`, `GETUSER`,
  and `DELUSER` operate on the live registry; ACL mutation is restricted to
  the default user.
  The default user is initialized with `~*`; custom users without key rules
  fail closed for key-bearing commands.
  User-management and metadata-bearing ACL subcommands are restricted to the
  default user to avoid credential and policy disclosure.
  Category grants `@all`, `@read`, `@write`, and `@connection` are expanded into
  command bitsets at update time; unknown commands fail closed.
  `ACL LIST` returns complete bounded rule lines suitable for policy auditing.
  `ACL GETUSER` exposes the effective allow/deny command set and key rules.
  Each fixed user slot carries a monotonic generation. Sessions validate the
  cached generation before local execution and before MT routing; deleting and
  recreating a username therefore invalidates stale connections fail-closed.
  Generation counters are registry-local, avoiding cross-worker shared mutable
  state and keeping invalidation deterministic for independent server instances.
  Category rule names are parsed case-insensitively (`@READ`, `@WRITE`, `@ALL`,
  and `@CONNECTION`) using bounded ASCII comparison, matching Redis command
  casing semantics without adding allocations.
  `ACL CAT` lists available categories and filters commands for a requested
  category; unknown categories fail closed before command metadata traversal.
  `ACL DRYRUN` resolves the target user and command/key arguments through the
  existing authorization path, returning `OK` or `NOPERM` without side effects;
  unknown users and commands fail closed.
  `ACL GENPASS` follows Redis' default 256-bit and bounded bits semantics, emits
  hexadecimal output from the PAL secure RNG, and fails closed on unavailable
  entropy or invalid ranges.
  `ACL LOG` now keeps a bounded 32-entry ring of authentication and command
  authorization failures, supports count-limited reads and `RESET`, and avoids
  unbounded memory growth from repeated failures.
  MT home-worker authorization denials are recorded in the same bounded ring
  before routing is rejected, preserving audit visibility across workers.
  ACL channel patterns (`&pattern` and `&*`) are enforced for subscribe,
  pattern-subscribe, and publish commands with bounded allocation-free matching;
  unauthorized channels fail closed before registration or delivery.
  The default user is initialized with unrestricted channel access, preserving
  normal Pub/Sub behavior when no custom ACL channel policy is configured.
  Channel patterns are exposed in `ACL LIST` and `ACL GETUSER` metadata so
  policy inspection remains lossless.
  ACL failure logging safely handles absent user/object fields before event
  coalescing, preventing malformed callers from causing a crash.
  Negative `ACL LOG` counts are normalized to zero (empty array), while only
  non-integer counts are rejected, matching Redis query semantics.
  `nopass` uses an explicit flag to accept any password, while `resetpass`
  restores password verification, matching Redis ACL semantics without conflating
  an empty password with unrestricted authentication.
  `nocommands` clears both allow and deny command bitsets, preventing stale deny
  rules from surviving an alias reset and matching Redis policy semantics.
  `reset` also clears `nopass`, and `allcommands`/`+@all` remove stale deny
  bits so unrestricted users cannot inherit contradictory historical rules.
  `resetkeys` clears only key patterns and preserves enabled state, credentials,
  command rules, and channel permissions, matching Redis alias scope.
  `allchannels` and `resetchannels` aliases are accepted and map to the same
bounded channel state as `&*` and an empty rule set.
  `PUBLISH`/`SPUBLISH` channel checks stop before the message argument, while
  subscribe and pattern-subscribe validate every supplied channel argument.
  `ACL SETUSER` accepts common Redis aliases (`allkeys`, `resetkeys`,
  `allcommands`, `nocommands`, `nopass`, and `resetpass`) with bounded atomic
  rule replacement.
  The `reset` rule clears password and all bounded permission patterns and turns
  the user off; subsequent rules in the same atomic update can explicitly turn
  it back on, matching Redis behavior.
  `ACL LIST` reserves bounded stack space for both key and channel patterns, so
  complete rule lines remain well-formed even at maximum configured cardinality.
  Consecutive identical ACL failure events coalesce into a saturating `count` in
  the fixed log ring, preserving distinct audit events during repeated attacks.
  `ACL GETUSER` reports `nocommands` for non-unrestricted command policies,
  rather than mislabeling command state as a channel reset.
  `ACL GETUSER` preserves the Redis `~` prefix for key patterns, alongside `&`
  for channel patterns, so policy metadata is unambiguous.
  Multi-key source/destination commands (`RENAME`, `SMOVE`, `LMOVE`, `COPY`, and
  related variants) now authorize every key position, preventing a permitted
  source from being used to access an unauthorized destination.
  `EXISTS`, `TOUCH`, `SINTER`, `SUNION`, and `SDIFF` likewise authorize every
  supplied key instead of only the first operand.
  ACL key extraction distinguishes keyless administrative subcommands from
  `MEMORY USAGE` and `DEBUG OBJECT`, which continue to enforce key patterns.
  `OBJECT` and `XINFO` extract the key from the correct subcommand position;
  `FLUSHDB`, `FLUSHALL`, and `SHUTDOWN` options are not treated as keys.
  Store-style commands (`SORT STORE`, `SINTERSTORE`, `ZUNIONSTORE`, and
  `GEOSEARCHSTORE`) authorize destination and source keys, with checked
  `numkeys` parsing and fail-closed handling for malformed requests.
  `PFCOUNT`/`PFMERGE`, `SINTERCARD`, and sorted-set union/intersection/difference
  commands authorize every declared source key rather than only the first.
  Replication and control commands (`ASKING`, `PSYNC`, `REPLCONF`, `REPLICAOF`,
  `FAILOVER`, and `MONITOR`) treat protocol parameters as keyless arguments.
  Script and function commands (`EVAL*`/`FCALL*`) authorize only the declared
  `numkeys` slice, with checked zero-key and malformed-count handling.
  Blocking pop/bit operations and legacy geo store forms authorize every actual
  key while excluding timeout, count, coordinate, and option tokens.
  Stream reads/group management and multi-pop commands extract keys from their
  `STREAMS`/`numkeys` positions and fail closed on malformed or truncated input.
  `MIGRATE` authorizes its primary key and every key after an optional `KEYS`
  marker while excluding transport and authentication parameters.
  `PUBSUB CHANNELS` and `PUBSUB NUMSUB` enforce channel ACLs for all query
  operands; `PUBSUB NUMPAT` remains keyless.
  `CLUSTER` and `SENTINEL` control containers keep their protocol arguments
  keyless, avoiding false key-pattern denials.
  Persistence/range-store commands and `MSETEX` authorize destination and source
  keys at their Redis-defined positions, excluding payload and option arguments.
  `XREAD`/`XREADGROUP` reject odd or truncated key/ID tails after `STREAMS` rather
  than silently dropping an unmatched stream ID.
  ddup extension commands keep `BACKUP`/`HOTKEYS` keyless and authorize only the
  hash key of `HIMPORT SET`; fieldset lifecycle subcommands have no keys.
  ACL key checks fail closed for non-string key values and malformed odd-length
  `MSET/MSETNX` argument lists.
  `OBJECT HELP` and `XINFO HELP` are keyless help forms; keyed subcommands still
  authorize their actual key argument.
  `ACL LIST` renders restricted users as `nocommands` and emits `nopass` when
  arbitrary-password authentication is enabled, preserving round-trip policy.
  `ACL GETUSER` likewise exposes `nopass` and unrestricted `~*`/`&*` metadata so
  clients can distinguish all-access domains from empty rule sets.
  Adding `>password` clears `nopass` and restores password verification, matching
  Redis ACL rule ordering semantics.
  `MEMORY USAGE key SAMPLES count` now requires exactly one non-negative integer
  count; ddup validates the Redis option boundary even though its deterministic
  object model does not need sampling work.
  `SLOWLOG GET` accepts negative counts as an explicit request for all retained
  entries, matching Redis pagination semantics while retaining the bounded ring.
  `LATENCY GRAPH <event>` returns Redis' no-samples error when the event has no
  recorded data; ddup no longer emits a fabricated graph payload.
  `LATENCY HISTORY <event>` applies the same no-samples error semantics instead
  of returning an empty array for an unknown event.
  `DEBUG STRINGMATCH <value> <pattern>` now returns the actual glob match result
  through ddup's binary-safe matcher rather than a constant zero placeholder.
  `INFO` accepts standard section arguments (including multiple sections) and
  returns the bounded ddup snapshot instead of rejecting `INFO SERVER` as an
  unsupported section; the internal `__STATS__` transport remains unchanged.
- Cluster slot maintenance: `SFLUSH` intersects requested ranges with local
  ownership and returns coalesced flushed ranges; `TRIMSLOTS RANGES` validates
  ownership before deleting keys from unserved slots. Both paths collect keys
  before mutation, preserve expiry/accounting invariants, and fail closed on
  allocation errors.
- `CLUSTER SLOT-STATS` supports low-overhead `key-count`, `memory-bytes`,
  `cpu-usec`, `network-bytes-in`, and `network-bytes-out` metrics for
  `SLOTSRANGE` and `ORDERBY [LIMIT] [ASC|DESC]`. CPU/network values are
  cumulative per-slot counters attributed only to commands with an unambiguous
  single hash slot; keyless, topology, and cross-slot requests are excluded.
- Script key extraction is aligned for `EVAL_RO/EVALSHA_RO` and
  `FCALL/FCALL_RO`; cluster routing and `COMMAND GETKEYS` use the declared
  `numkeys` positions rather than treating the script/function name as a key.
- `CLUSTER MIGRATION` and `CLUSTER SYNCSLOTS` are recognized with an
  internal-client-only security gate; external sessions cannot alter migration
  state or slot metadata.
- `MONITOR` streams subsequent commands to subscribed server connections,
  including GET/SET lean fast paths. `BACKUP` implements a safe synchronous
  snapshot-backed lifecycle (`START/STATUS/SEAL/LIST/ABORT/CLEANUP`) using an
  atomic `.backup` artifact; AOF-enabled backups record a durable baseline offset
  and SEAL atomically writes a `.backup.aof` delta artifact. Redis MP-AOF
  immutable-file pinning is equivalent here because ddup has no AOF rewrite or
  segment-reclamation path. `HOTKEYS` implements a server-owned lifecycle, bounded preallocated sampled
  key table, and Redis 8 START option validation; `GET` reports tracking state,
  sample ratio, start time, total commands, and bounded CPU/network-like key
  lists with independent CPU and NET rankings. ddup reports measured dispatch microseconds and raw request bytes when
  available, with stack-session argument-byte fallback.
  `HIMPORT PREPARE/SET/DISCARD/DISCARDALL` is implemented with session-local
  fieldsets, Redis-compatible integer removal counts for DISCARD/DISCARDALL,
  and validated batched writes to ordinary hash objects.
- ARRAY core: `ARSET/ARGET/ARLEN/ARCOUNT` use a sparse `rh_table` keyed by
  fixed-width indexes. `ARLEN` and `ARCOUNT` read object metadata in O(1),
  while `ARSET` validates every input before modifying the object. The type is
  included in the snapshot encoding, so persisted arrays retain sparse indexes.
- ARRAY access/deletion tranche: `ARGETRANGE`, `ARMGET`, `ARDEL`, and
  `ARDELRANGE` are implemented with sparse lookups and bounded deletion scans.
- ARRAY completion: `ARMSET`, `ARNEXT`, `ARSEEK`, `ARINSERT`, `ARRING`,
  `ARSCAN`, `ARINFO`, `ARLASTITEMS`, `AROP`, and `ARGREP` are registered and
  covered by TDD. Exact/glob predicates and core numeric aggregates are
  supported; unsupported predicate forms fail explicitly.
- `ARGREP RE` now uses a platform-neutral bounded regex subset (`^`, `$`, `.`,
  `*`, `+`, `?`, escapes) with `LIMIT`, `NOCASE`, and `WITHVALUES` handling.
- `XCLAIM RETRYCOUNT` is accepted and applied to the resulting pending entry
  delivery count instead of being rejected.
- `XREAD` and `XREADGROUP` implement `BLOCK` using the session blocked state:
  new stream entries wake the request, finite deadlines return a null array,
  and `BLOCK 0` waits indefinitely. Readiness checks avoid materializing a
  response until the request is actually ready.
- mt routing covers the Redis 8 key extensions: single-key hash TTL, string
  safety, and ARRAY commands are dispatched to the key owner; `MSETEX`,
  `SUNIONCARD`, and `SDIFFCARD` extract only declared key positions and reject
  cross-worker requests with Redis-compatible `CROSSSLOT` before execution.
- mt Stream extension commands `XDELEX`, `XACKDEL`, and `XNACK` are classified
  as single-key operations and dispatched by stream-key ownership; group and
  delivery-state mutations therefore execute atomically on the owning worker.

## 实现策略（性能优先）

- 数据面命令优先实现，保证核心语义与复杂度级别一致。
- hash 字段 TTL 采用字段级绝对过期时间元数据，listpack 与 rh_table 两
  编码下均 O(fields) 查询/清理；过期字段惰性删除，读路径零额外 malloc。
- Set 基数复用 `setop_eval`/`obj_set` 遍历，`LIMIT` 提前停，不物化结果。
- List 多元素移动复用现有 `obj_list` 批量 pop/push 助手，避免逐元素复制。
- Stream 精确删除/PEL 控制复用现有 stream PEL 索引；`XIDMPRECORD` 作为
  内部命令做参数校验与幂等应答。
- Array 类型采用 `obj_array`：以 `rh_table` 存稀疏 index→value，O(1)
  随机读写，遍历按索引跨度扫描；`ARCARD/ARCOUNT` 与 `ARNEXT` 元数据常驻
  对象头，O(1)。
- 管理/危险命令优先做安全前置校验；不支持或无法等价实现的模块命令
  记录在下方范围外清单。

## 架构差异（无用户可见剩余项）

ACL SETUSER 别名在固定容量临时副本上原子应用：`allkeys`/`~*` 与
`allchannels`/`&*` 会先清空对应旧 pattern，再设置全开标志；`resetpass` 清除
`nopass` 标志并恢复密码校验。这样别名组合不会残留互相矛盾的状态，授权路径仍为
有界 bitset/glob 检查，无额外堆分配。

- `BACKUP`：已实现基于原子多库快照的安全同步生命周期；Redis 的
  AOF durable-offset + sealed immutable delta 已实现；由于 ddup 无 AOF
  rewrite/segment 回收路径，该边界等价于安全 pinning。`HOTKEYS` 已实现安全生命周期、参数校验、预分配采样表、有界 Top-K、CRC16 `SLOTS` 过滤、实测 dispatch 微秒和原始请求字节计量；Redis 内部 hash-template 编码不改变外部语义。`HIMPORT` 已实现会话级字段集准备、批量写入和丢弃。
- ARRAY 高级语义：当前稀疏对象模型提供命令级安全行为；`ARINFO FULL`
  返回与当前稀疏模型对应的基础目录/切片统计，不伪造 Redis 内部编码细节。
- mt 全库命令：`KEYS` 已广播到所有 worker 并合并 RESP 数组，`RANDOMKEY`
  已选择首个非空 bulk 回复，`SCAN` 使用带 worker 索引的复合游标顺序
  遍历分片；mt `PSUBSCRIBE/PUNSUBSCRIBE` 使用 worker-local 模式注册表，
  PUBLISH 按 glob 匹配并 fan-out `pmessage`；`SSUBSCRIBE/SUNSUBSCRIBE/
  SPUBLISH` 按 channel owner 路由并 fan-out `smessage`，与普通频道注册表
  和模式注册表隔离。
- mt 的双 key 列表移动 `LMOVEM` 现在与 `LMOVE` 使用相同的 source/destination
  槽校验和 owner 路由，避免把 `LEFT/RIGHT/COUNT` 选项误当作 key。
- mt 的 `OBJECT` 子命令按 `OBJECT <subcommand> <key>` 的第三个参数提取 key，
  因而跨连接查询可见正确 owner 上的对象元数据。

## 审计基线（机器断言，勿手改格式）

<!-- AUDIT-BASELINE-START
missing_top:
missing_containers:
AUDIT-BASELINE-END -->

### CLIENT REPLY

`CLIENT REPLY ON|OFF|SKIP` is implemented as connection-local state. `OFF`
suppresses subsequent command replies until `ON`; `SKIP` suppresses exactly one
subsequent reply and automatically returns to normal mode. The control command
itself is acknowledged before the new mode takes effect.

`CLIENT NO-TOUCH ON|OFF` is implemented as connection-local state. While enabled,
key reads skip LRU metadata refresh but retain expiry checks, value materialization,
command side effects, AOF logging, and monitoring. `RESET` restores normal touch
behavior.

`CLIENT TRACKING ON|OFF` now keeps connection-local BCAST/OPTIN/OPTOUT, NOLOOP,
REDIRECT, and bounded PREFIX metadata. Incompatible option combinations are
rejected atomically. `CLIENT CACHING YES|NO` requires the corresponding OPTIN or
OPTOUT mode, and `CLIENT TRACKINGINFO` reports Redis-compatible flags, redirect,
and prefixes fields.

`CLIENT CACHING YES|NO` is now a one-shot hint: after the next non-CACHING
command, `TRACKINGINFO` no longer reports `caching-yes`/`caching-no`. Disabling
tracking and `RESET` clear the hint immediately.

`CLIENT GETREDIR` now returns the configured redirect client ID while tracking is
enabled and `-1` otherwise, matching the `TRACKINGINFO.redirect` field.

Network sessions now reject `CLIENT TRACKING ... REDIRECT <id>` when the target
client does not exist, matching Redis' fail-closed control-plane validation.

Mode-less `CLIENT TRACKING ON` preserves Redis' default tracking mode instead of
implicitly selecting OPTOUT. `CLIENT TRACKINGINFO` therefore reports only `on`
for a default session, while `CLIENT CACHING YES|NO` is rejected unless OPTIN or
OPTOUT was explicitly enabled.

`CLIENT TRACKING` option parsing is atomic for bounded PREFIX metadata: malformed
or incompatible requests no longer leave a partially applied prefix in the
session. Validation uses a fixed stack scratch copy and commits only on success.

Mode-less tracking re-enable matches Redis transitions: OPTIN/OPTOUT returns to
the default mode and clears mode-specific state, while an active BCAST client
cannot implicitly switch modes. Tracking errors are emitted with compile-time
literal lengths so responses are never truncated.

The mode transition also clears PREFIX, NOLOOP, REDIRECT, and CACHING metadata,
preventing mode-specific state from leaking into the default tracking mode.

Redis-compatible PREFIX validation now rejects duplicate or overlapping prefix
strings atomically. The bounded session state remains unchanged on failure, and
the check uses only fixed inline storage.

`CLIENT SETINFO LIB-NAME/LIB-VER` now validates printable ASCII values, stores
bounded metadata in the connection session, supports empty-value clearing, and
exposes the fields through `CLIENT INFO` and `CLIENT LIST` with safe bounded
formatting.

Unknown `CLIENT SETINFO` attributes now return `ERR Unrecognized option '<attr>'`
with bounded stack formatting, matching Redis diagnostics without exposing
unbounded input or changing ordinary command execution.

Unknown subcommands for `ACL`, `LATENCY`, `MODULE`, `SENTINEL`, and `DEBUG` now
use bounded diagnostic rendering. Inputs are truncated to a fixed prefix and
the RESP writer receives only bytes actually present in the stack buffer,
preventing malformed oversized arguments from causing out-of-bounds reads.

Top-level unknown-command responses and queued MULTI command validation now use
bounded command-name rendering as well; SCRIPT unknown-subcommand errors follow
the same fixed-buffer path. This keeps Redis diagnostics useful while ensuring
RESP output lengths never exceed initialized stack storage.

Multi-thread AOF and snapshot configuration now rejects null inputs and truncated
worker paths before applying settings. This keeps persistence control-plane setup
fail-closed and prevents distinct configurations from silently sharing a truncated
filename.

Tiering control-plane setup now rejects truncated log paths and overlong tier
directories before opening storage. Node path configuration safely ignores null
inputs, preventing accidental dereference during cluster setup.

Node persistence path updates now clear the configured path when formatting would
truncate, preventing cluster metadata from being written to an unintended file.

Cluster nodes persistence also validates the derived temporary filename before
opening it, preserving dirty state when formatting would truncate.

Duplicate `FUNCTION LOAD` library diagnostics now use bounded library-name
rendering and checked output lengths, preserving useful Redis-style errors for
large library metadata without exposing stack-buffer overreads.

Unknown `CONFIG SET` parameter errors now use the same bounded rendering policy,
preserving Redis-style diagnostics while preventing oversized parameter tokens
from causing out-of-bounds RESP writes.

Unknown options for `HEXPIRE`/`HPEXPIRE` and their absolute-time variants now
use bounded diagnostics, preserving fail-closed parsing without exposing
uninitialized stack bytes for oversized option tokens.

`ACL DRYRUN` now bounds the missing-user diagnostic as well, retaining the
Redis-style error while preventing oversized usernames from extending the RESP
write beyond initialized stack storage.

`LATENCY GRAPH` and `LATENCY HISTORY` no-sample diagnostics now safely truncate
oversized event names, preserving Redis-compatible errors without allowing
unbounded input lengths to escape fixed stack storage.

`HSETEX` unknown-option diagnostics now follow the same bounded policy as the
hash expiration family, preventing oversized field-write options from extending
the RESP write beyond initialized stack storage.

Pub/Sub subscribed-context rejection errors now clamp `snprintf` output lengths,
so long command tokens cannot make RESP read past the initialized diagnostic
buffer.

INFO multi-section snapshots use bounded append semantics, and ACL metadata is
covered with maximum-size rule sets to ensure fixed-buffer RESP rendering stays
within initialized storage under adversarial configuration sizes.

ACL rule-line rendering now bounds every appended password, key pattern, and
channel pattern and preserves a complete `CRLF` terminator when the fixed
buffer fills. This keeps policy exports well-formed under maximum configuration
cardinality.

MONITOR output now treats each emitted command as a transactional append:
oversized quoted arguments fail safely and discard the partial frame, preserving
well-formed monitoring output under memory pressure.

Replication snapshot and partial-resync headers now reject negative or truncated
format results before converting lengths to `size_t`, preserving atomic frame
construction under malformed or extreme state.

Server control-plane entry points now safely ignore null snapshot paths and reject
null or empty cluster node IDs, preventing configuration-time crashes and partial
cluster initialization.

Cluster identity configuration now rejects oversized node IDs and announced IPs
instead of silently truncating them, preserving stable node identity and routing
metadata.

Cluster node-table APIs now reject null, malformed, short, or oversized node IDs
before lookup or insertion, preventing partial identities from entering gossip
and persistence state.

Cluster bus PING/PUBLISH/FAIL builders now reject null inputs before buffer writes,
keeping malformed control-plane calls fail-closed without changing valid frames.

Replica configuration now rejects empty or oversized master hosts before changing
the link state, preventing silent truncation and inconsistent PSYNC resume data.

Cluster slot bitmap helpers now fail closed on null buffers and out-of-range slots,
preventing malformed control-plane inputs from causing memory access faults.

Cluster failure-report APIs now reject malformed reporter IDs and null state, and
saturate timeout-window arithmetic so malformed gossip cannot crash or bypass
failure expiry.

Cluster bus receive handling now rejects null or empty frame inputs before parsing,
keeping malformed gossip fail-closed without mutating cluster state.

Cluster node-ID persistence now rejects formatting truncation and close failures,
so identity creation cannot report success after an incomplete durable write.

PUBLISH and FAIL frame builders now perform null-output checks before reading
buffer lengths, eliminating a control-plane null dereference edge case.

Nodes.conf rendering and parsing now reject null database, line, and output
buffers before touching persistence state.

Cluster state helpers now reject null databases, claimants, and bitmaps before
mutation, keeping malformed control-plane requests fail-closed.

AOF open, logging, flushing, callback injection, replay, and delta-copy entry
points now reject null or empty inputs before file operations.

Snapshot save/load, memory replay, and per-key DUMP/RESTORE entry points now
reject null or empty inputs before parsing or file operations.

Sharded and multi-database snapshot entry points now reject null contexts,
callbacks, paths, and buffers before serialization or replay mutation.

Single-database snapshot loading now rejects capacity-doubling overflow before
reallocation, preventing hostile file sizes from wrapping allocation lengths.

Snapshot buffer helpers now reject null destinations and non-empty null sources,
while preserving valid zero-length payload framing.

Tier storage open, append, read, delete, flush, and compaction APIs now reject
null or empty inputs before touching cold-layer files.

Tier log replay now verifies every declared record body is fully readable before
publishing its index entry, preventing truncated persistence data from appearing
as valid cold records.

Tier replay and append now reject maximum record IDs and lengths that cannot fit
the fixed index representation, preventing ID wraparound and locator truncation.

Tier append and compaction now fail closed on offset arithmetic overflow or index
insertion failure, preventing durable records from becoming unaddressable.

Tier replay now rejects unknown operation codes before consuming record bodies,
preventing undefined persistence operations from mutating recovery state.

Tier replay now rejects DEL/FLUSH records carrying unexpected key/value bodies,
preventing hidden bytes from being silently accepted during recovery.

Shared hash-table APIs now reject null tables, keys, values, and output pointers
before probing or mutation, preventing malformed internal calls from crashing
command, ACL, snapshot, or tier paths.

Tier reads now validate persisted offsets and value bounds before file access and
clear output parameters on failure, preventing corrupt indexes from causing
out-of-range reads.

Tier compaction now latches failure when atomic replacement or reopening fails,
preventing subsequent writes against an uncertain cold-layer file state.

Sharded snapshot serialization now rejects null shard contexts before invoking the
database callback, preventing invalid context dereferences during full sync.

Hash-table iteration and SCAN helpers now reject null tables, callbacks, and output
views before traversal, preventing malformed management scans from crashing.

Hash-table SCAN now checks combined-capacity arithmetic and rejects out-of-range
cursors, preventing unsigned wraparound during management iteration.

Hash-table operations now fail closed on uninitialized storage handles, preventing
zero-capacity masking and invalid slot dereferences in shared data paths.

Hash-table capacity helpers now reject null output pointers before arithmetic,
keeping diagnostics and test seams fail-closed.

Hash-table iteration, lookup, random sampling, and teardown now fail closed on
inconsistent slot-array metadata and avoid double-free on aliased storage,
improving resilience against corrupted internal handles without changing valid
Redis command behavior.

Hash-table ownership and sampling outputs are now deterministic: insert calls
clear the old-value result on a new key, while failed random sampling clears all
views, preventing stale pointers from escaping into command and eviction paths.

Hash-table teardown now handles aliased primary/old slot arrays without a second
free, keeping corrupted internal handles fail-closed during cleanup.

Replication backlog helpers now fail closed on null inputs and saturate the
absolute stream offset at `UINT64_MAX`, preventing malformed replication data
from causing pointer faults or offset wraparound.

Cluster CRC16/hash-slot helpers now reject non-empty null keys and safely handle
null hashtag output buffers, preventing malformed routing inputs from crashing
the command path while preserving Redis hashtag semantics for valid keys.

Replication backlog operations now validate ring metadata before arithmetic and
fail closed on corrupt state, preventing offset underflow or out-of-bounds data
movement during partial-resync handling.

Session queue, blocking, WATCH, and teardown APIs now reject null or malformed
RESP views and roll back partial copies, preventing malformed client requests
from leaking memory or corrupting connection state.

The cross-platform socket layer now rejects invalid handles and null network
arguments before platform calls, preventing malformed client/replication setup
from causing process faults while preserving valid TCP behavior.

PAL send/receive and asynchronous-connect APIs now reject malformed buffers or
handles and clear failed output descriptors, preventing stale sockets from
escaping into client and replication connection state.

RESP parser and writer entry points now fail closed on null objects, buffers, and
non-empty null payloads, preventing malformed protocol input from causing memory
faults without changing valid RESP2/RESP3 wire behavior.

The io_uring proactor PAL now rejects malformed ring, fd, buffer, and wait
arguments before SQE publication, preventing invalid asynchronous operations or
length truncation from destabilizing client and replication paths.

The cross-platform readiness loop now fails closed on invalid loop, descriptor,
event-buffer, and timeout arguments across all backends, preventing malformed
connection-control calls from crashing the server.

Empty RESP bulk arguments now remain valid zero-length session views, and server
error rendering uses exact compile-time message lengths, preserving Redis wire
semantics while eliminating allocator-dependent OOM behavior and over-reads.

Session MULTI and blocking state now preserve valid zero-argument lists without
`malloc(0)`, preventing allocator-dependent failures on empty protocol frames.

Cluster regression fixtures now validate node allocation before field setup, so
the compatibility test suite remains free of undefined behavior under allocation
failure instrumentation.

The cross-platform wakeup primitive now fails closed on null or invalid handles,
keeping multi-thread connection-control notifications safe during startup and
teardown.

Server periodic maintenance timers now handle wall-clock rollback safely:
expiration, snapshot checks, replica reconnect, cluster gossip, and nodes.conf
persistence only run after a forward elapsed interval, preserving Redis timing
semantics without unsigned-underflow-triggered bursts.

Cluster failover election and retry deadlines now use saturating arithmetic, so
an extreme timestamp cannot wrap a future election into an immediate promotion
attempt.

Lua script execution deadlines now saturate at `UINT64_MAX`, preserving the
configured execution budget when the platform clock is near its numeric limit
instead of wrapping into an already-expired deadline.

Relative expiration commands now saturate both timestamp addition and
seconds-to-milliseconds conversion. Extremely large valid TTLs therefore remain
future expirations rather than wrapping into immediate deletion.

ARRAY management lookups now fail closed on a null object handle, returning a
deterministic miss instead of dereferencing invalid state; valid ARGET semantics
are unchanged.
