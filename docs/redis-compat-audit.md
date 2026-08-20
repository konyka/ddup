# Redis 7.2.15 命令兼容性盘点

> 基线：Redis 7.2.15 官方 `src/commands/*.json`（392 个 JSON、392 条命令条目；242 个顶层命令 + 150 个容器子命令）。
> 对照：ddup `src/core/command.c` `CMD_TABLE`（209 个顶层命令）。
> 生成：`python3 tools/audit_redis_compat.py --redis-json <Redis src/commands> --json`，机器可复算；`--check` 校验本文档 AUDIT-BASELINE 块与代码一致。

## 总览

| 类别 | 数量 |
| --- | --- |
| 完全未实现的独立顶层命令 | 33 |
| 整体缺失的顶层容器 | 7 |
| 已实现容器内缺失的子命令 | 43 |

## A. 完全缺失的独立顶层命令（按 Redis group 分组）

### generic（2）

`WAIT`
`WAITAOF`

### list（5）

`BLMOVE`
`BLMPOP`
`BLPOP`
`BRPOP`
`BRPOPLPUSH`

### scripting（5）

`EVALSHA_RO`
`EVAL_RO`
`FCALL`
`FCALL_RO`
`FUNCTION`

### sentinel（1）

`SENTINEL`

### server（9）

`ACL`
`DEBUG`
`FAILOVER`
`LATENCY`
`LOLWUT`
`MODULE`
`MONITOR`
`REPLCONF`
`RESTORE-ASKING`

### sorted_set（3）

`BZMPOP`
`BZPOPMAX`
`BZPOPMIN`

### stream（8）

`XACK`
`XAUTOCLAIM`
`XCLAIM`
`XGROUP`
`XINFO`
`XPENDING`
`XREAD`
`XREADGROUP`


## B. 容器子命令对照

### 整体缺失的顶层容器

- `acl` `function` `latency` `module` `sentinel` `xgroup` `xinfo`

### 已实现容器内缺失的子命令

- `CLIENT`：缺 `caching`, `getredir`, `help`, `info`, `no-evict`, `no-touch`, `pause`, `reply`, `setinfo`, `tracking`, `trackinginfo`, `unblock`, `unpause`
- `CLUSTER`：缺 `addslotsrange`, `bumpepoch`, `count-failure-reports`, `delslotsrange`, `flushslots`, `forget`, `help`, `links`, `myshardid`, `replicas`, `reset`, `saveconfig`, `set-config-epoch`, `shards`, `slaves`
- `COMMAND`：缺 `getkeysandflags`, `help`
- `CONFIG`：缺 `help`, `resetstat`, `rewrite`
- `MEMORY`：缺 `help`
- `OBJECT`：缺 `freq`, `help`, `idletime`, `refcount`
- `PUBSUB`：缺 `help`
- `SCRIPT`：缺 `debug`, `help`, `kill`
- `SLOWLOG`：缺 `help`

## C. 已注册但选项/语义不完整（手工确认，命令名差分不可发现）

- `ZADD`：仅裸 `score member` 对，缺 `NX/XX/GT/LT/CH/INCR`；`ZRANGE` 缺 `REV/BYSCORE/BYLEX/LIMIT` 统一语法（当前只支持索引 + WITHSCORES）。
- `EXPIRE/PEXPIRE/EXPIREAT/PEXPIREAT`：min/max argc 仅 3，缺 `NX/XX/GT/LT`；`GETEX` 已支持 `PERSIST/EX/EXAT/PX/PXAT`。
- `OBJECT`：只有 `ENCODING`。
- `CONFIG`：只有 `GET/SET`，且 SET 仅支持 `maxmemory`/`maxmemory-policy`。
- `SCRIPT`：只有 `LOAD/EXISTS/FLUSH`。
- `CLIENT`：仅 `ID/SETNAME/GETNAME/LIST/KILL`；`INFO`、`SETINFO`、`NO-EVICT/NO-TOUCH`、`PAUSE/UNPAUSE`、`REPLY`、`TRACKING*` 等缺。
- `COMMAND`：仅 `COUNT/LIST/INFO/GETKEYS/DOCS`；缺 `HELP`、`GETKEYSANDFLAGS`。
- `MEMORY`：仅 `USAGE/STATS/DOCTOR/PURGE/MALLOC-STATS`；缺 `HELP`。
- `SLOWLOG`：仅 `GET/LEN/RESET`；缺 `HELP`。
- Stream 核心族已实现 `XADD/XLEN/XRANGE/XREVRANGE/XDEL/XTRIM/XSETID`；
  消费组、阻塞读与 `XINFO/XGROUP` 管理族仍缺。
- 复制/运维族：无 `REPLCONF`、`MONITOR`、`WAIT/WAITAOF`、`RESTORE-ASKING`（`REPLICAOF`/`ROLE`/`SLAVEOF` 已实现）。
- 管理族整体缺失：`ACL`、`DEBUG`、`LATENCY`、`MODULE`、`FUNCTION`、`SENTINEL`、`XGROUP`、`XINFO`。

## D. 项目范围外 / 明确不实施

- mt 模式不支持复制/集群运维命令（`src/server/mt_server.c` `mt_is_blocked` 清单：SHUTDOWN、SYNC、REPLICAOF、SLAVEOF、CLUSTER、MIGRATE、ASKING、KEYS、SCAN、RANDOMKEY、PSUBSCRIBE、PUNSUBSCRIBE）。
- Garnet/单机缓存存储不适配项（如分布式锁脚本、阻塞语义），不在本次兼容目标内。

## 审计基线（机器断言，勿手改格式）

<!-- AUDIT-BASELINE-START
missing_top: acl blmove blmpop blpop brpop brpoplpush bzmpop bzpopmax bzpopmin debug eval_ro evalsha_ro failover fcall fcall_ro function latency lolwut module monitor replconf restore-asking sentinel wait waitaof xack xautoclaim xclaim xgroup xinfo xpending xread xreadgroup
missing_containers: acl function latency module sentinel xgroup xinfo
missing_sub: client caching
missing_sub: client getredir
missing_sub: client help
missing_sub: client info
missing_sub: client no-evict
missing_sub: client no-touch
missing_sub: client pause
missing_sub: client reply
missing_sub: client setinfo
missing_sub: client tracking
missing_sub: client trackinginfo
missing_sub: client unblock
missing_sub: client unpause
missing_sub: cluster addslotsrange
missing_sub: cluster bumpepoch
missing_sub: cluster count-failure-reports
missing_sub: cluster delslotsrange
missing_sub: cluster flushslots
missing_sub: cluster forget
missing_sub: cluster help
missing_sub: cluster links
missing_sub: cluster myshardid
missing_sub: cluster replicas
missing_sub: cluster reset
missing_sub: cluster saveconfig
missing_sub: cluster set-config-epoch
missing_sub: cluster shards
missing_sub: cluster slaves
missing_sub: command getkeysandflags
missing_sub: command help
missing_sub: config help
missing_sub: config resetstat
missing_sub: config rewrite
missing_sub: memory help
missing_sub: object freq
missing_sub: object help
missing_sub: object idletime
missing_sub: object refcount
missing_sub: pubsub help
missing_sub: script debug
missing_sub: script help
missing_sub: script kill
missing_sub: slowlog help
AUDIT-BASELINE-END -->
