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
