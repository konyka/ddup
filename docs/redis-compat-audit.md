# Redis 7.2.15 命令兼容性盘点

> 基线：Redis 7.2.15 官方 `src/commands/*.json`（392 个 JSON、392 条命令条目；242 个顶层命令 + 150 个容器子命令）。
> 对照：ddup `src/core/command.c` `CMD_TABLE`（169 个顶层命令）。
> 生成：`python3 tools/audit_redis_compat.py --redis-json <Redis src/commands> --json`，机器可复算；`--check` 校验本文档 AUDIT-BASELINE 块与代码一致。

## 总览

| 类别 | 数量 |
| --- | --- |
| 完全未实现的独立顶层命令 | 73 |
| 整体缺失的顶层容器 | 11（对应 Redis 子命令条目 124） |
| 已实现容器内缺失的子命令 | 26（CLUSTER 15、CONFIG 3、OBJECT 4、PUBSUB 1、SCRIPT 3） |
| 已注册但选项/语义不完整 | 见下方手工清单 |

## A. 完全缺失的独立顶层命令（按 Redis group 分组）

### connection（3）

`CLIENT`, `HELLO`, `RESET`

### generic（5）

`MOVE`, `SORT`, `SORT_RO`, `WAIT`, `WAITAOF`

### geo（10）

`GEOADD`, `GEODIST`, `GEOHASH`, `GEOPOS`, `GEORADIUS`, `GEORADIUSBYMEMBER`, `GEORADIUSBYMEMBER_RO`, `GEORADIUS_RO`, `GEOSEARCH`, `GEOSEARCHSTORE`

### hyperloglog（5）

`PFADD`, `PFCOUNT`, `PFDEBUG`, `PFMERGE`, `PFSELFTEST`

### list（8）

`BLMOVE`, `BLMPOP`, `BLPOP`, `BRPOP`, `BRPOPLPUSH`, `LINSERT`, `LMOVE`, `LMPOP`

### scripting（5）

`EVALSHA_RO`, `EVAL_RO`, `FCALL`, `FCALL_RO`, `FUNCTION`

### server（16）

`ACL`, `BGREWRITEAOF`, `BGSAVE`, `COMMAND`, `DEBUG`, `FAILOVER`, `LATENCY`, `LOLWUT`, `MEMORY`, `MODULE`, `MONITOR`, `REPLCONF`, `RESTORE-ASKING`, `ROLE`, `SLAVEOF`, `SLOWLOG`

### sorted_set（3）

`BZMPOP`, `BZPOPMAX`, `BZPOPMIN`

### stream（15）

`XACK`, `XADD`, `XAUTOCLAIM`, `XCLAIM`, `XDEL`, `XGROUP`, `XINFO`, `XLEN`, `XPENDING`, `XRANGE`, `XREAD`, `XREADGROUP`, `XREVRANGE`, `XSETID`, `XTRIM`

### string（2）

`LCS`, `SUBSTR`

### sentinel（1）

`SENTINEL`

## B. 容器子命令对照

### 已实现容器内缺失的子命令

- `CLUSTER`：缺 `addslotsrange`, `bumpepoch`, `count-failure-reports`, `delslotsrange`, `flushslots`, `forget`, `help`, `links`, `myshardid`, `replicas`, `reset`, `saveconfig`, `set-config-epoch`, `shards`, `slaves`
- `CONFIG`：缺 `help`, `resetstat`, `rewrite`
- `OBJECT`：缺 `freq`, `help`, `idletime`, `refcount`
- `PUBSUB`：缺 `help`
- `SCRIPT`：缺 `debug`, `help`, `kill`

### 整体缺失的顶层容器

- `ACL`
- `CLIENT`
- `COMMAND`
- `FUNCTION`
- `LATENCY`
- `MEMORY`
- `MODULE`
- `SENTINEL`
- `SLOWLOG`
- `XGROUP`
- `XINFO`

## C. 已注册但选项/语义不完整（手工确认，命令名差分不可发现）

- `HELLO`：无命令入口；RESP3 类型收发能力存在（`resp/resp.h`、`resp_writer.h`、`tests/test_resp3.c`），但没有协议协商命令。
- `ZADD`：仅裸 `score member` 对，缺 `NX/XX/GT/LT/CH/INCR`；`ZRANGE` 缺 `REV/BYSCORE/BYLEX/LIMIT` 统一语法（当前只支持索引 + WITHSCORES）。
- `EXPIRE/PEXPIRE/EXPIREAT/PEXPIREAT`：min/max argc 仅 3，缺 `NX/XX/GT/LT`；`GETEX` 已支持 `PERSIST/EX/EXAT/PX/PXAT`。
- `OBJECT`：只有 `ENCODING`。
- `CONFIG`：只有 `GET/SET`，且 SET 仅支持 `maxmemory`/`maxmemory-policy`。
- `SCRIPT`：只有 `LOAD/EXISTS/FLUSH`。
- 复制/运维族：无 `REPLCONF`、`ROLE`、`SLAVEOF`（`REPLICAOF` 存在）、`BGSAVE`、`BGREWRITEAOF`、`MONITOR`、`WAIT/WAITAOF`、`RESTORE-ASKING`。
- 管理族整体缺失：`COMMAND`、`ACL`、`CLIENT`、`DEBUG`、`LATENCY`、`MEMORY`、`MODULE`、`SLOWLOG`、`FUNCTION`、`SENTINEL`、`XGROUP`、`XINFO`。

## D. 项目范围外 / 明确不实施

- mt 模式不支持复制/集群运维命令（`src/server/mt_server.c` `mt_is_blocked` 清单：SHUTDOWN、SYNC、REPLICAOF、CLUSTER、MIGRATE、ASKING、KEYS、SCAN、RANDOMKEY、PSUBSCRIBE、PUNSUBSCRIBE）。
- Garnet/单机缓存存储不适配项（如分布式锁脚本、阻塞语义），不在本次兼容目标内。

## 审计基线（机器断言，勿手改格式）

<!-- AUDIT-BASELINE-START
missing_top: acl bgrewriteaof bgsave blmove blmpop blpop brpop brpoplpush bzmpop bzpopmax bzpopmin client command debug eval_ro evalsha_ro failover fcall fcall_ro function geoadd geodist geohash geopos georadius georadius_ro georadiusbymember georadiusbymember_ro geosearch geosearchstore hello latency lcs linsert lmove lmpop lolwut memory module monitor move pfadd pfcount pfdebug pfmerge pfselftest replconf reset restore-asking role sentinel slaveof slowlog sort sort_ro substr wait waitaof xack xadd xautoclaim xclaim xdel xgroup xinfo xlen xpending xrange xread xreadgroup xrevrange xsetid xtrim
missing_containers: acl client command function latency memory module sentinel slowlog xgroup xinfo
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
missing_sub: config help
missing_sub: config resetstat
missing_sub: config rewrite
missing_sub: object freq
missing_sub: object help
missing_sub: object idletime
missing_sub: object refcount
missing_sub: pubsub help
missing_sub: script debug
missing_sub: script help
missing_sub: script kill
AUDIT-BASELINE-END -->
