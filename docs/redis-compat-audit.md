# Redis 7.2.15 命令兼容性盘点

> 基线：Redis 7.2.15 官方 `src/commands/*.json`（392 个 JSON、392 条命令条目；242 个顶层命令 + 150 个容器子命令）。
> 对照：ddup `src/core/command.c` `CMD_TABLE`（221 个顶层命令）。
> 生成：`python3 tools/audit_redis_compat.py --redis-json <Redis src/commands> --json`，机器可复算；`--check` 校验本文档 AUDIT-BASELINE 块与代码一致。

## 总览

| 类别 | 数量 |
| --- | --- |
| 完全未实现的独立顶层命令 | 0 |
| 整体缺失的顶层容器 | 0 |
| 已实现容器内缺失的子命令 | 0 |

## A. 完全缺失的独立顶层命令（按 Redis group 分组）

（无：Redis 7.2.15 独立顶层命令名均已注册。）

## B. 容器子命令对照

### 整体缺失的顶层容器

（无：`ACL/DEBUG/LATENCY/MODULE/SENTINEL` 均已注册为容器。）

### 已实现容器内缺失的子命令

（无：`CLIENT/CLUSTER/COMMAND/CONFIG/OBJECT/SCRIPT` 及管理容器子命令名均已覆盖。）

## C. 已注册命令的语义近似与占位（均已实现或记录在案）

命令名差分已清零；以下仅保留按项目定位采用的最小/近似语义，均已有对应
单测或文档说明，不构成待办缺口。

- `OBJECT ENCODING/FREQ/IDLETIME/REFCOUNT/HELP` 全部可用；ddup 不维护
  共享对象/LFU 元数据，因此 `REFCOUNT` 恒为 1、`FREQ` 恒为 0，
  `IDLETIME` 用 LRU 时钟近似。
- `CONFIG GET/SET/RESETSTAT/REWRITE/HELP` 全部可用；`SET` 当前接受
  `maxmemory`/`maxmemory-policy`，`REWRITE` 在无配置文件时返回明确错误。
- `SCRIPT LOAD/EXISTS/FLUSH/DEBUG/KILL/HELP` 全部可用；`DEBUG` 校验
  诊断模式，`KILL` 因 ddup 无异步脚本执行而恒回 `NOTBUSY`。
- `CLIENT` 子命令全部注册并返回 Redis 风格应答；部分仅需无状态兼容应答
  （如 `SETINFO/GETREDIR/CACHING/TRACKING`）。
- `COMMAND COUNT/LIST/INFO/GETKEYS/GETKEYSANDFLAGS/DOCS/HELP` 全部可用；
  `GETKEYSANDFLAGS` 按命令读写元数据返回近似 `RW/RO` 标志。
- `MEMORY USAGE/STATS/DOCTOR/PURGE/MALLOC-STATS/HELP` 全部可用；
  `DOCTOR/PURGE/MALLOC-STATS` 为单机内存模型下的兼容响应。
- `SLOWLOG GET/LEN/RESET/HELP` 全部可用。
- 脚本族新增只读别名 `EVAL_RO/EVALSHA_RO`；只读脚本内写命令会被拒绝。
- 脚本库族已实现 `FCALL/FCALL_RO/FUNCTION`：`FUNCTION` 支持
  `LOAD [REPLACE]/DELETE/LIST [LIBRARYNAME pattern] [WITHCODE]/FLUSH/
  DUMP/RESTORE [FLUSH|APPEND|REPLACE]/STATS/KILL/HELP`；`DUMP/RESTORE`
  使用 ddup 专用二进制 payload（非 Redis RDB），`KILL` 恒回 `NOTBUSY`。
  当前把库代码按名称保存并按 EVAL 风格执行，Redis 的
  `redis.register_function` 多函数库格式已支持：LOAD 时保留原库源码，FCALL
  按注册函数名构造受限执行包装器；plain chunk 库继续兼容。
- Stream 核心族已实现 `XADD/XLEN/XRANGE/XREVRANGE/XDEL/XTRIM/XSETID`；
  消费组/读取族已实现 `XGROUP/XACK/XPENDING/XCLAIM/XAUTOCLAIM/XREAD/
  XREADGROUP/XINFO`；`XREAD/XREADGROUP BLOCK` 使用 session 阻塞态和 server
  就绪循环，支持新 entry 唤醒、超时 Null array 及 `BLOCK 0` 无限等待。
- 阻塞 list/zset 族已实现 `BLPOP/BRPOP/BRPOPLPUSH/BLMOVE/BLMPOP/
  BZPOPMIN/BZPOPMAX/BZMPOP`：服务端事件循环持有挂起会话并在 key 就绪或
  超时到期时唤醒；mt 模式将空闲连接迁移到 key owner 后复用同一 waiter，
  pipeline/事务或不可迁移后端返回明确安全错误。
- 复制/运维族：已注册 `REPLCONF/FAILOVER/MONITOR/WAIT/WAITAOF`；单机/
  共享无副本语义下 `WAIT/WAITAOF` 返回 0，`FAILOVER` 返回无可副本错误，
  `MONITOR` 已接入 server 级实时文本流，订阅连接收到后续命令事件；
  `REPLCONF` 仍仅做握手应答。
- `LOLWUT` 已实现 `VERSION 5/6` 的最小 ASCII art；`RESTORE/RESTORE-ASKING`
  已支持 `REPLACE/ABSTTL/IDLETIME/FREQ`（IDLETIME/FREQ 仅做参数解析与
  兼容接受，ddup 无 LRU/LFU 对象元数据）。
- 字符串读改写命令 `INCR/DECR/INCRBY/DECRBY/INCRBYFLOAT/APPEND` 保留
  key 的绝对 TTL；SET 风格覆盖（包括 `GETSET`）清除旧 TTL，已由注入时钟
  回归测试锁定。
- KEYS/SCAN 的 glob 匹配器对未闭合字符类按 Redis 的 fail-closed 规则处理，
  避免把畸形模式误当作字面前缀。
- 管理族已注册 `ACL/DEBUG/LATENCY/MODULE/SENTINEL`：ACL 提供最小
  anonymous/default 视图，LATENCY 返回空事件集，MODULE 无扩展返回加载
  错误，SENTINEL 提供空拓扑视图，DEBUG 仅接受诊断参数。

## D. 项目范围外 / 明确不实施

- mt 模式 `KEYS/RANDOMKEY/SCAN` 已通过 worker 路由或聚合；`PUBSUB CHANNELS/NUMSUB/NUMPAT` 广播归并为全局视图；阻塞 pop 族按 key owner 连接亲和迁移并复用原生 session waiter（IOCP/io_uring-op 返回明确的 migratable-connection 错误）；`COPY DB n` 通过 worker-local 多库 session 执行；`MIGRATE` 按源 key hash 路由到 source worker，连接亲和迁移后复用有界网络迁移逻辑（IOCP/io_uring-op 不可安全迁移时返回明确错误）；`SHUTDOWN` 已由 home worker 执行并协调停止全部 worker；`PSUBSCRIBE/PUNSUBSCRIBE` 已通过 worker-local pattern registry 和 `pmessage` fan-out 支持；`ASKING` 已在每个 worker 的本地会话安全门中支持一次性标志；`SYNC/PSYNC/REPLICAOF/SLAVEOF/CLUSTER` 已由 worker 0 控制面适配。
- Garnet/单机缓存存储不适配项（如分布式锁脚本、阻塞语义），不在本次兼容目标内。

## 审计基线（机器断言，勿手改格式）

<!-- AUDIT-BASELINE-START
AUDIT-BASELINE-END -->
