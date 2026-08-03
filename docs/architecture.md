# ddup 架构设计

参考 Garnet 的整体分层，用 C 重实现。本文档随代码演进同步更新。

## 分层

```
┌─────────────────────────────────────┐
│  RESP 命令层 (src/server, src/core) │  命令分发、参数解析、响应写出
├─────────────────────────────────────┤
│  存储层 (src/core, src/ds)          │  哈希表、过期、复杂数据结构
├─────────────────────────────────────┤
│  协议层 (src/resp)                  │  RESP2/RESP3 零拷贝解析/写出
├─────────────────────────────────────┤
│  网络层 (src/server + src/pal)      │  连接管理、事件循环
├─────────────────────────────────────┤
│  平台抽象层 PAL (src/pal)           │  socket/事件/线程/原子/时间
└─────────────────────────────────────┘
```

## 关键设计决策（对齐 Garnet，性能优先）

1. **Thread-per-core 无共享**：每个 IO 线程独立运行事件循环，连接上的解析、
   命令执行、存储访问全部在 IO 线程就地完成，避免线程切换与数据搬运。
2. **平台最优 IO 模型（readiness 模式已落地）**：
   - Linux：epoll（level-triggered）；io_uring（内核 ≥ 5.10）列为后续优化
   - macOS / FreeBSD：kqueue
   - Windows：select()（FD_SETSIZE 提升至 1024）；IOCP 列为后续优化
   - 统一抽象为 `pal_event`：`pal_loop_add/mod/del/wait`，事件携带
     fd + userdata + readable/writable。
3. **零拷贝 RESP 解析**：解析结果直接引用接收缓冲，不落盘复制；
   批量（pipelining）命令在一次 recv 缓冲内连续解析。
4. **内存管理**：热路径禁止逐次 malloc；arena + 对象池复用。
5. **主存哈希表**：Robin Hood 开放寻址 + 增量 rehash，缓存友好。
6. **C 标准自适应**：构建期探测 C23→C17→C11→C99，取最高可用标准；
   原子操作优先 C11 `<stdatomic.h>`，缺失时降级平台原生 API。

## 过期设计（Phase 4）

- **存储**：`db.expires` 为第二张 rh_table，key → 8 字节绝对过期时刻
  （wall-ms，`pal_wall_ms()`）。主表与 expires 表严格同生共死：
  覆盖写（SET/INCR/APPEND/MSET）与 DEL 同时清除过期项。
- **惰性过期**：所有命令的 key 查找经 `db_expire_if_needed()`，过期即删
  并计入 `expired_keys`。
- **主动过期**：server 每 100ms（`pal_now_ms`）跑一轮 `db_active_expire()`：
  从 expires 表随机采样至多 20 个 key（随机桶 + 前向扫描），删除已过期者；
  单轮过期率 >25% 则继续，至多 10 轮。
- **时间注入**：命令分发入口为 `command_execute_at(db, argv, argc, out,
  now_ms)`，`command_execute()` 传真实墙钟；单测全部用合成时间，无 sleep。

## 淘汰设计（Phase 4）

- **LRU 时钟**：`rh_entry.meta`（表本身不解释）存 24 位秒级时钟
  `(now_ms/1000) & 0xFFFFFF`，创建/访问时刷新（`rh_touch`）。
- **内存记账**：`db.used_memory` 增量维护，每个存活条目计
  `sizeof(rh_entry) + 16（malloc 开销）+ klen + vlen`，主表与 expires 表
  同口径；set/del/expire/persist/flush 时增减，无全表扫描。
- **淘汰策略**：`db.maxmemory`（0=不限，默认 0）+ `db.maxmemory_policy`
  （默认 allkeys-lru）。allkeys-lru：命令执行后若超限，循环采样 5 个
  key 淘汰时钟最旧者，直至达标，计 `evicted_keys`。noeviction：写命令
  （SET/MSET/INCR/DECR/APPEND）在超限时直接返回 OOM 错误。
- **采样随机源**：db 内置 xorshift32（`rng_state`），测试可固定种子，
  淘汰场景确定性复现。

## 命令清单

PING ECHO GET SET(NX/XX/EX/PX) DEL UNLINK EXISTS INCR DECR APPEND STRLEN
MGET MSET ｜ EXPIRE PEXPIRE EXPIREAT PEXPIREAT TTL PTTL PERSIST ｜
DBSIZE FLUSHDB CONFIG(GET/SET maxmemory, maxmemory-policy) INFO ｜
HSET HGET HDEL HEXISTS HLEN HGETALL HKEYS HVALS HMSET HMGET HINCRBY HSETNX ｜
LPUSH RPUSH LPUSHX RPUSHX LPOP RPOP LLEN LRANGE LINDEX LSET ｜
SADD SREM SISMEMBER SMISMEMBER SCARD SMEMBERS SPOP SRANDMEMBER SMOVE
SINTER SUNION SDIFF ｜
ZADD ZSCORE ZCARD ZINCRBY ZREM ZRANGE ZREVRANGE ZRANK ZREVRANK ZCOUNT
ZRANGEBYSCORE ZREMRANGEBYSCORE ｜
MULTI EXEC DISCARD WATCH UNWATCH ｜ SUBSCRIBE UNSUBSCRIBE PUBLISH QUIT ｜
SAVE LASTSAVE SHUTDOWN ｜ SYNC REPLICAOF

注：TTL 返回值四舍五入（(rem+500)/1000，同 Redis）；PTTL 精确到 ms。
INCR/APPEND 在本实现中清除 TTL（与 Redis 保留 TTL 不同，有意简化）。
DBSIZE 为 O(1)，可能计入尚未回收的过期 key。

## 对象存储模型（Phase 5.1）

主表保持字节通用（rh_table 不解释值）；所有值均为带类型标签的 blob：

```
{1 字节类型标签}{payload}
  DDUP_OBJ_STRING: payload = 原始字符串字节
  DDUP_OBJ_HASH:   payload = 8 字节指针 -> obj_hash（嵌套 rh_table：field -> 值）
  DDUP_OBJ_LIST:   payload = 8 字节指针 -> obj_list（双链表，每元素一个节点）
  DDUP_OBJ_SET:    payload = 8 字节指针 -> obj_set（rh_table：member -> 空值）
  DDUP_OBJ_ZSET:   payload = 8 字节指针 -> obj_zset（dict + skiplist）
```

- **所有权**：db 层拥有指针对象。任何覆盖/删除/过期/淘汰/FLUSHDB 路径
  经 `obj_free_value()` 释放对象；`obj_extra_mem()` 返回对象占用用于
  内存记账（hash：sizeof(obj_hash) + 每字段 entry 成本；list：
  sizeof(obj_list) + 每节点 sizeof(list_node)+16+元素字节；均为近似值，
  不含嵌套表的 slot 数组）。
- **类型错误**：字符串命令作用于 hash/list key（或反向）回复
  `-WRONGTYPE Operation against a key holding the wrong kind of value`。
  注：SET 覆盖其它类型在 Redis 中是允许的，本实现按 WRONGTYPE 处理
  （有意收紧）；MGET 对非字符串 key 先校验后统一报错（Redis 返回 null）。
- **空对象自动删除**：hash 字段清空 / list 弹空时 key 一并删除。
- **quicklist**：list 当前为逐节点双链表；块式 quicklist 列为后续优化。
- 过期、淘汰、LRU touch 对对象值透明生效（共用 db 层路径）。

## Set / ZSet 设计（Phase 5.2）

- **Set**：`obj_set` 即一张 member -> 空值的 rh_table（去重由表保证）。
  SPOP/SRANDMEMBER 用 db 内置 xorshift 随机源：先收集 member 视图，
  部分 Fisher-Yates 后取前 k 个（SPOP 删除选中的）；count<0 时有放回抽样。
  SINTER/SUNION/SDIFF 结果为临时 rh_table（天然去重），SINTER/SDIFF 遇
  缺失 key 结果为空，SUNION 忽略缺失 key。
- **ZSet**：Redis 风格 dict + skiplist。dict（rh_table）member -> 8 字节
  double，ZSCORE/ZINCRBY/ZREM 均 O(log N)；skiplist（src/ds/skiplist.c）
  维护 (score, member 字节序) 排序：分数升序，同分按 member 字典序
  （与 Redis 一致）。跳表为**无 span 简化变体**：ZRANK/索引访问走
  level-0 链 O(N)，span 优化列为后续工作。层数几何分布 p=1/4、上限 32，
  内部 xorshift32 随机源（确定性种子，测试可复现）。
- **分数格式**：`%.17g`（inf/-inf 原样），解析用 strtod 全量消费，
  NaN 一律拒绝（`ERR value is not a valid float`）；inf + -inf 的结果
  NaN 报 `ERR resulting score is not a number (NaN)`。
- **范围语法**：ZCOUNT/ZRANGEBYSCORE/ZREMRANGEBYSCORE 支持 `(x` 开区间
  与 `-inf`/`+inf`；范围比较只看 score（同分 member 同进同出）。

## Session、事务与发布订阅（Phase 5.3）

- **Session（src/core/session.h）**：每个连接一个执行上下文：db 指针 +
  MULTI 队列 + WATCH 条目 + pub/sub 钩子。分发入口
  `session_execute_at()`；`command_execute[_at]()` 以栈 session 包装，
  db 级旧测试零改动。server 在 accept 时创建 session、关闭时释放。
- **事务**：MULTI 后命令经静态 arity 表做入队时校验（未知命令/参数个数
  错误立即报错并置 multi_error，EXEC 回复 EXECABORT）；合法命令深拷贝
  入队（接收缓冲会复用），回复 +QUEUED。EXEC 顺序重放并打包为数组。
  MULTI/EXEC/DISCARD/WATCH 本身不入队。
- **WATCH**：db 内置 key 版本表 `keyvers`（key -> uint64 单调版本，
  删除/重建不复用）+ `flush_epoch`（FLUSHDB 递增）。所有写路径
  （db_set_kv/db_del_kv/惰性+主动过期/淘汰/对象原地修改经 mem_sync/
  FLUSHDB）bump 版本。EXEC 前逐条比对，失配回复 `*-1`。
  注意：keyvers 表只增不减（写过的 key 常驻一个版本条目），且不计入
  used_memory——有意的简化，记录于此。
- **发布订阅**：注册表在 server（`rh_table channels`：频道 -> 连接链表），
  不在 db。core 命令经 session 钩子（subscribe/unsubscribe/each_channel/
  publish/deliver）回调 server；推送直接写入订阅者 conn->out，
  server_run_once 末尾统一 flush 所有连接（单线程保证安全）。连接关闭
  自动退订。订阅态仅允许 (P)SUBSCRIBE/(P)UNSUBSCRIBE/PING/QUIT/SHUTDOWN。
  QUIT 当前只回 +OK 不断连（简化，后续补）。

## 持久化（Phase 6）

- **配置**：redis 风格扁平配置文件（`key value` 行、`#` 注释、键大小写
  不敏感）+ `--key value` 命令行覆盖（src/core/config.c）。样例见根目录
  ddup.conf。
- **AOF（src/server/aof.c）**：格式即 RESP 命令流。session 分发钩子对比
  命令前后的 `db.dirty` 计数（db_touch_key/FLUSHDB 递增），发生变更就把
  **原始 argv** 重序列化为 RESP 数组追加到缓冲；每个 server 循环刷盘一次
  （appendfsync everysec 式简化，stdio 缓冲，无 fsync——记录为后续优化）。
  EXEC 按逐条命令记录（不写 MULTI 包装）。启动时 `appendonly yes` 且文件
  存在则先重放（容忍截断尾部）。
- **快照（src/core/snapshot.c）**：自有二进制格式 `DDUP0001`，逐 key 存
  {类型标签、key、绝对过期毫秒、按类型的 payload}，显式小端编码。保存
  原子化（写 `<path>.tmp` 后 rename 覆盖）。加载**全有或全无**：先解析
  到临时 db，截断/损坏返回 -1 且目标 db 不变；加载时跳过已过期 key。
  SAVE 命令同步落盘（BGSAVE 不做真后台，单线程下无意义，记录在案）。
- **启动优先级**：`appendonly yes` 时只走 AOF；否则加载 dbfilename
  快照（AOF 优先于 RDB，同 Redis）。`save N` 秒自动快照（dirty 变化才
  写）。优雅退出（SIGINT/SIGTERM/SHUTDOWN）：AOF 必定 flush；配置了
  save 间隔且 AOF 关闭时额外写一次最终快照。

## 复制（Phase 7.1）

- **传播流**：分发层对每个成功应用的写命令（含 EXEC 内逐条）按原始
  argv 重序列化为 RESP 数组，经 server 复用缓冲 fan-out 到三类 sink：
  AOF、复制 backlog（环形缓冲，默认 1MB，`repl-backlog-size` 可调）、
  下游 replica 连接的 out 缓冲（run_once 末尾统一 flush）。
- **全量同步**：replica 发 SYNC；master 用 `snapshot_serialize` 把内存快照
  按 `$<len>\r\n<bytes>` 帧发回（二进制帧，非 RESP），随后把该连接标记
  为 replica 并持续推流。落后超过 4MB 未读的 replica 连接被丢弃
  （须重新 SYNC）。
- **复制侧**：REPLICAOF host port 建立 master link（同一事件循环内的
  特殊 conn），先按帧读快照（flush db 后 snapshot_load_mem 全量加载），
  随后切到 RESP 命令流模式逐条应用（回包丢弃）。link 断开每 500ms
  重连并**全量重同步**——无 PSYNC/部分重同步（记录为后续工作，backlog
  正是为此预留）。REPLICAOF NO ONE 断链并提升为 master。
- **只读副本**：replica 角色下客户端写命令一律 `-READONLY ...`（静态
  写命令表判定），master link 的复制会话豁免。replica 的 AOF 照常记录
  传播来的命令（同 Redis appendonly 行为）。
- INFO 增加 # Replication 段：role、connected_slaves、master_repl_offset、
  master_host/port/link_status。

## 目录结构

```
src/pal/     平台抽象：pal_platform(宏), pal_time, pal_socket(TCP), pal_event(事件循环)
src/resp/    RESP 协议（Phase 1）
src/core/    KV 存储、哈希表、过期、淘汰、命令分发、session、config、
             snapshot（Phase 2/4/5.3/6）
src/ds/      对象类型：obj（tagged blob、Hash 嵌套表、List 双链表、Set、
             ZSet dict+skiplist）、skiplist（无 span 跳表）（Phase 5.1/5.2）
src/server/  连接与服务器主循环（Phase 3）、aof（Phase 6）：单线程事件循环、
             recv 缓冲按需增长、解析→执行→推进零拷贝流水线；当前连接为阻塞
             socket（单次 recv + 阻塞发送循环），非阻塞写出缓冲随
             thread-per-core 阶段引入
tests/       单元测试（test.h 自研框架）+ 集成测试
bench/       压测客户端 ddup-bench（Phase 3，非 ctest 目标）
```
