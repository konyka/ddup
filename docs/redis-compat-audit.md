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

## C. 已注册但选项/语义不完整（手工确认，命令名差分不可发现）

- `ZADD`：仅裸 `score member` 对，缺 `NX/XX/GT/LT/CH/INCR`；`ZRANGE` 缺 `REV/BYSCORE/BYLEX/LIMIT` 统一语法（当前只支持索引 + WITHSCORES）。
- `EXPIRE/PEXPIRE/EXPIREAT/PEXPIREAT`：min/max argc 仅 3，缺 `NX/XX/GT/LT`；`GETEX` 已支持 `PERSIST/EX/EXAT/PX/PXAT`。
- `OBJECT`：`ENCODING/HELP/FREQ/IDLETIME/REFCOUNT`；`FREQ` 恒为 0、`REFCOUNT` 恒为 1（无 LFU/共享对象元数据）。
- `CONFIG`：`GET/SET/RESETSTAT/REWRITE/HELP`；SET 仅支持 `maxmemory`/`maxmemory-policy`，REWRITE 无配置文件时返回错误。
- `SCRIPT`：`LOAD/EXISTS/FLUSH/DEBUG/KILL/HELP`；DEBUG 仅接受模式，KILL 恒为 NOTBUSY。
- `CLIENT`：已覆盖 `ID/SETNAME/GETNAME/LIST/KILL/INFO/SETINFO/GETREDIR/NO-EVICT/NO-TOUCH/PAUSE/UNPAUSE/REPLY/CACHING/TRACKING/TRACKINGINFO/UNBLOCK/HELP`；部分为无状态/兼容性应答。
- `COMMAND`：已覆盖 `COUNT/LIST/INFO/GETKEYS/GETKEYSANDFLAGS/DOCS/HELP`；GETKEYSANDFLAGS 返回读写标志近似值。
- `MEMORY`：仅 `USAGE/STATS/DOCTOR/PURGE/MALLOC-STATS/HELP`。
- `SLOWLOG`：仅 `GET/LEN/RESET/HELP`。
- 脚本族新增只读别名 `EVAL_RO/EVALSHA_RO`；只读脚本内写命令会被拒绝。
- 脚本库族已实现 `FCALL/FCALL_RO/FUNCTION`：`FUNCTION` 支持
  `LOAD [REPLACE]/DELETE/LIST [LIBRARYNAME pattern] [WITHCODE]/FLUSH/
  STATS/HELP`，`DUMP/RESTORE` 返回明确不支持，`KILL` 返回 `NOTBUSY`。
  当前把库代码按名称保存并按 EVAL 风格执行，Redis 的
  `redis.register_function` 多函数库格式记录为本次范围外。
- Stream 核心族已实现 `XADD/XLEN/XRANGE/XREVRANGE/XDEL/XTRIM/XSETID`；
  消费组/读取族已实现 `XGROUP/XACK/XPENDING/XCLAIM/XAUTOCLAIM/XREAD/
  XREADGROUP/XINFO`；`BLOCK` 当前按非阻塞立即返回处理，记录在案。
- 阻塞 list/zset 族已实现 `BLPOP/BRPOP/BRPOPLPUSH/BLMOVE/BLMPOP/
  BZPOPMIN/BZPOPMAX/BZMPOP`：服务端事件循环持有挂起会话并在 key 就绪或
  超时到期时唤醒；mt 模式暂不路由这些命令（记录在案）。
- 复制/运维族：已注册 `REPLCONF/FAILOVER/MONITOR/WAIT/WAITAOF`；单机/共享无副本语义下 `WAIT/WAITAOF` 返回 0，`FAILOVER` 返回无可副本错误，`MONITOR` 返回明确不支持，`REPLCONF` 仅做握手应答。
- `LOLWUT` 已实现 `VERSION 5/6` 的最小 ASCII art；`RESTORE-ASKING` 已作为集群导入用 `RESTORE` 别名实现（当前仅支持 `REPLACE`，`ABSTTL/IDLETIME/FREQ` 仍缺）。
- 管理族已注册 `ACL/DEBUG/LATENCY/MODULE/SENTINEL`：ACL 提供最小匿名/default 视图，LATENCY 返回空事件集，MODULE 无扩展返回加载错误，SENTINEL 提供空拓扑视图，DEBUG 仅接受诊断参数。

## D. 项目范围外 / 明确不实施

- mt 模式不支持复制/集群运维命令（`src/server/mt_server.c` `mt_is_blocked` 清单：SHUTDOWN、SYNC、REPLICAOF、SLAVEOF、CLUSTER、MIGRATE、ASKING、KEYS、SCAN、RANDOMKEY、PSUBSCRIBE、PUNSUBSCRIBE）。
- Garnet/单机缓存存储不适配项（如分布式锁脚本、阻塞语义），不在本次兼容目标内。

## 审计基线（机器断言，勿手改格式）

<!-- AUDIT-BASELINE-START
AUDIT-BASELINE-END -->
