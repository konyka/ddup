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
   - Windows：select()（FD_SETSIZE 提升至 1024）；IOCP proactor 层
     （pal_iocp，completion 模型）已就位，服务器接入列为下一阶段
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
SAVE LASTSAVE SHUTDOWN ｜ SYNC REPLICAOF ｜ DUMP RESTORE MIGRATE ASKING

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

## TLS（Phase 7.2）

- **可选依赖**：CMake `find_package(OpenSSL)` + `DDUP_TLS` 选项；找到则
  `DDUP_HAS_TLS=1` 并链接 OpenSSL::SSL/Crypto，否则 `pal_tls` 全部为
  stub（创建 ctx 返回 NULL，`tls-port` 启动报明确错误）。OpenSSL 头文件
  只出现在 `pal_tls.c` 与 `tests/test_tls.c`。
- **独立端口**：`tls-port`（默认 0=off）与明文端口并行监听；配置校验
  `config_validate()` 要求 cert/key 文件可读。conn 增加 `pal_tls*`，
  server 的所有读写经 conn_read/conn_write 包装分发到 TLS 或明文。
- **简化（记录在案）**：accept 上的握手为非阻塞式（事件循环内分步完成）；
  复制 master link 暂不支持 TLS。Windows CI 上 test_tls 集成测试暂时禁用
  （握手在 Windows  runner 上超时，调查中）；TLS 库与服务器集成代码在
  Windows 正常编译，Linux/macOS/FreeBSD CI 覆盖完整 test_tls。

## 非阻塞写出（Phase 7.4）

- 所有 accept 的连接 `pal_set_nonblocking(fd, 1)`；读路径把 would-block
  视为"本轮无数据"（仅 0/硬错误才关闭）。
- 每个 conn 的回复先写入 out 缓冲（resp_buf），随后立即尽力 flush：
  pal_send/pal_tls_write 直到 would-block（普通 socket 经 pal_would_block
  判定，TLS 经 -2 约定）。剩余字节保留，conn 通过 pal_loop_mod 注册
  read+write；可写事件到达时继续 flush，全部发完改回 read-only。
  want_write 状态记忆避免重复 mod 系统调用。
- 慢副本（replica conn）pending 输出超过 16MB 仍按原策略丢弃（须重新
  SYNC）。SYNC 快照帧走同一 out 缓冲路径，无阻塞发送循环残留。
- 单线程服务器不再会被停滞客户端拖死：socket 级测试覆盖 2000 条流水
  命令、慢客户端并行服务、分片读取恢复。

## IOCP 后端（Phase 7.5，Windows）

- **后端选择**：`server_create_ex(host, port, backend)`；`--io select|iocp`
  配置项，默认运行时探测（Windows 用 IOCP，其他平台 select）。IOCP 不可
  用时自动回落 readiness。
- **流程**：`pal_iocp_listen` 监听；ACCEPT 完成 → 建 conn（session/arena/
  64KB recv 缓冲）→ post 首个 WSARecv 并重挂 AcceptEx；RECV 完成 → 同一
  parse→execute 流水线（与 readiness 共享 conn_process_input）→
  kick_flush；SEND 完成 → 推进 out_sent，未发完则续发（单块 ≤256KB，经
  conn 私有稳定 sbuf 发送，避免 resp_buf 扩容导致悬垂）。发布订阅、复制
  推流、SYNC 帧共用 kick_flush（有 send 在飞时自动跳过）。16MB 慢副本
  丢弃策略一致；AOF flush/主动过期与 autosave 照旧。
- **生命周期**：conn 有在飞操作时关闭走 zombie 路径（CancelIoEx 后等
  pending_ops 归零再真正释放，避免完成事件悬垂引用）。
- **限制（记录在案）**：IOCP 后端不支持 TLS（tls-port 自动回落
  readiness）；不支持复制副本侧 master link（REPLICAOF 报错），master
  侧供流正常。pal_iocp 在非 Windows 为空 stub（创建返回 NULL）。
- ping-pong 基准见 docs/performance.md Phase 7.5 表。

## 单节点集群模式（Phase 7.7）

- **范围**：兼容 Redis cluster-enabled 的单节点形态，独占 16384 个槽。
  多节点 gossip/MEET/迁移为后续工作。
- **hash slot**：`slot = crc16(hashtag) % 16384`（CRC16-XMODEM，表驱位运算；
  hashtag 取首个非空 `{}` 内容，规则同 Redis）。
- **节点身份**：`cluster-enabled yes` 时首次启动生成 40 位 hex node id 并
  写入 `cluster-config-file`（Redis nodes.conf 风格单行），重启复用。
- **命令族**：CLUSTER INFO/MYID/NODES/SLOTS/KEYSLOT/COUNTKEYSINSLOT/
  GETKEYSINSLOT；未启用时统一 `-ERR This instance has cluster support
  disabled`。INFO 增 # Cluster 段（cluster_enabled:0/1）。
- **CROSSSLOT 执行**：启用后 MGET/MSET/DEL/UNLINK/EXISTS/SINTER/SUNION/
  SDIFF 的所有 key、SMOVE 两端、WATCH 的 key 必须同槽；EXEC 重放前用共享
  槽累加器检查队列内全部命令的 key，违例以 CROSSSLOT 整体中止且无副作用。
- **快照/AOF**：不受影响（对象存储路径一致）。

## 集群总线与 gossip（Phase 7.8a，多节点第一部分）

- **集群总线**：cluster-enabled 时在 `port+10000` 开第二个监听（仅
  readiness 后端；IOCP 后端强制回落 select，同 TLS）。总线连接非阻塞，
  接入同一事件循环；慢连接按常规 out 缓冲冲刷策略处理。
- **协议（ddup cluster protocol，简化自有格式，不与 Redis 总线逐字节
  兼容，记录在案）**：`"RCM2"`（v2；v1 魔数 `"RCMB"` 仍可解析——v1
  发送者没有角色/master_id/epoch 字段，按 master、"-"、epoch 0 处理）
  + u32le totlen + u16le type（PING=1/PONG=2/MEET=3），body 为发送者
  id/ip/port@bus/flags/完整槽位图/master_id/config_epoch + 至多 10 条
  gossip 条目（同构字段，槽以区间串表示）。总包 ≤16KB，防御式解析
  （坏包/超长直接关闭）。
- **gossip**：每 1s 向全部出站连接发 PING（携带 gossip 条目）；收 PING/
  MEET 回 PONG；PONG 刷新 last_seen。发送者段直接合并；gossip 条目对新
  节点全量加入（handshake 标记），对已知节点只在 epoch 不更旧时合并槽
  声明（flags/ip 不被第三方改写）。MEET 由 `CLUSTER MEET ip port` 触发
  （主动建连并发 MEET 帧）。收敛方式与 Redis 相同：A 认识 B、B 认识 C
  ⇒ A 经由 B 的 gossip 负载学会 C。
- **节点表**：db 内置 32 节点表（id/ip/port@bus/flags/槽位图/last_seen），
  nodes.conf 多行格式 render/parse 双向序列化；每 10s 脏检测持久化
  （原子 rename），启动时先装载再覆盖 myself 条目。
- **故障检测**：NODE_TIMEOUT 默认 15s（测试可调；必须大于 1s 的 gossip
  周期，否则在线节点会在 PING 间隔内被误标）；超时标记 disconnected。
  cluster_state = ok 当 16384 槽被在线节点全覆盖且无 disconnected 节点
  持有槽，否则 fail。自动故障转移见 Phase 7.10。
- **TLS**：总线不支持 TLS（记录在案）。

## 槽分配与重定向（Phase 7.8b，多节点第二部分）

- **新节点语义**：fresh boot 的 myself 不持有任何槽（Redis 行为；7.7 的
  "独占全部" 只是单节点便利语义，由 nodes.conf 持久化保留旧部署）。
  cluster_state 在覆盖完备前为 fail。
- **槽命令**：CLUSTER ADDSLOTS（空闲才加，"Slot <n> is already busy"）、
  DELSLOTS（仅 myself 持有才可删，"already unassigned"）、SETSLOT slot
  NODE id（任意节点间移动，未知 id 报错）。位图变更即时生效并经 gossip
  传播（负载本就携带槽区间串）；nodes.conf 经 cluster_changes 计数触发
  周期持久化。
- **owner 缓存**：`db.slot_owner[16384]`（节点索引，0xFFFF=未分配），任何
  位图变更置脏，下一个命令前一次性重建（O(16384×节点数)）。常规命令
  每条一次 O(1) 所有权查询。
- **-MOVED 执行**：命令分派入口统一做所有权检查（keyless 命令表豁免；
  MGET/DEL/EXISTS/集合运算/WATCH 全参数、MSET 奇数位、SMOVE 两端、其余
  取 argv[1]）：槽未分配 → `-CLUSTERDOWN Hash slot not served`；槽属他人
  → `-MOVED <slot> <ip>:<port>`。EXEC 逐条元素独立判定（Redis 行为：
  MOVED 是该元素的回复，其余照常执行；CROSSSLOT 检查仍在 MOVED 之前）。
- **非集群零开销**：cluster_enabled=0 时所有权检查为单分支短路。

## 槽在线迁移（Phase 7.9，多节点第三部分）

- **DUMP/RESTORE**：DUMP 返回单键二进制 payload（`u16 版本(1) + u8 类型 +
  值负载 + u64 CRC64`，复用 snapshot 的分类型编码；CRC 覆盖此前全部字节，
  采用反射 ECMA 多项式 0xC96C5795D7870F42、全 1 初值/异或出（CRC-64/XZ
  参数，"123456789" 校验值 0x995DC9BBDF1939FA）。RESTORE key ttl-ms
  payload [REPLACE]：ttl 为相对毫秒（0=不过期），payload 截断/坏 CRC/
  版本不符 → `Bad data format`；无 REPLACE 且键已存在 → `BUSYKEY`。
- **MIGRATE host port key dbid timeout [COPY] [REPLACE] [KEYS k...]**：
  源端把每个存活键 dump 成 RESTORE 命令（前缀一条 ASKING，兼容导入态
  目标）流水线发往目标，deadline 内逐条等 +OK；目标确认后本地删除
  （COPY 保留）。任一失败即止（`-IOERR error or timeout writing to
  target instance`），已确认键仍删除、其余保留。dbid!=0 →
  `DB index is out of range`；单键形式键不存在 → `-NOKEY No such key`。
  与 Redis 一样会阻塞事件循环；测试经 pump hook 驱动同进程目标服务器。
  MIGRATE 属管理命令，豁免所有权检查。
- **迁移状态**：`db.slot_migrating[] / slot_importing[]`（节点索引，
  0xFFFF=无），仅本地（不 gossip、不进 nodes.conf）。`SETSLOT slot
  MIGRATING TO id` 要求 myself 持有该槽（否则 "Can't migrate slot:
  hash slot is not served by this node"，目标是 myself 报 "Can't
  migrate slot to myself"）；`SETSLOT slot IMPORTING FROM id` 要求
  myself 不持有（否则 "Can't import slot: hash slot is already served
  by this node"）。`SETSLOT slot NODE id` 与 DELSLOTS 清除两态。
- **-ASK 与 ASKING**：迁移源端槽属 myself 但键已不在 → `-ASK <slot>
  <ip>:<port>`（键仍在则照常服务）。导入端槽属他人但 slot_importing
  匹配时，客户端先发 ASKING（+OK，置一次性 session 标志），下一条
  命令豁免所有权检查一次，随后标志被消费、恢复 -MOVED。

## 副本与故障转移（Phase 7.10，多节点第四部分）

- **副本角色**：cluster_node 增加 slave 标志位与 master_id（nodes.conf
  的 master 列、v2 总线帧均携带）。`CLUSTER REPLICATE <id>` 使 myself
  成为某已知 master 的副本（`Unknown node` / `Can't replicate myself` /
  `I can only replicate a master`），并经由 session 钩子同时启动数据
  复制（等价于对目标自动执行 REPLICAOF；提升时自动 NO ONE）。snapshot
  装载改为仅替换数据（保留集群/配置状态），副本全量重同步不再清空
  节点表。
- **config epoch**：`db.cluster_current_epoch`（初始 1）；认领槽位的
  节点把 claim 打上 `++current_epoch`（ADDSLOTS→myself、SETSLOT NODE→
  目标、failover→提升者）。冲突裁决（cluster_merge_claims，对发送者段
  与足够新的 gossip 条目生效）：epoch 高者胜，平局取节点 id 字典序大者
  （Redis 规则），败者位图即时清除——包括 myself 的让步；未再声明的位
  被收回。CLUSTER INFO 如实上报 current/my epoch。
- **自动故障转移**：master 被标记 disconnected 且（按本地视图）持有槽
  时，其 slave 设定选举定时（node_timeout + 500ms，记录在案的简化：无
  投票/法定人数，多 slave 同时提升时靠 epoch 平局规则收敛）；到期且
  master 仍失联即提升：转 master、清 master_id、以新 epoch 认领其全部
  槽、停止数据复制并立即 gossip。死亡 master 重新上线后看到更高 epoch
  的声明即让步。
- **手动**：`CLUSTER FAILOVER [TAKEOVER]`，仅副本可执行（否则
  `You should send CLUSTER FAILOVER to a replica`）。简化：无 TAKEOVER
  时不等待 master 同意，与 TAKEOVER 行为一致（记录在案）。
- **其余简化（记录在案）**：无 FAILOVER AUTH 授权、无副本迁移
  （replica migration）、无 PFAIL 主观失联状态；last_seen 只被直连
  帧刷新（第三方 gossip 不影响失联判定）。

## 目录结构

```
src/pal/     平台抽象：pal_platform(宏), pal_time, pal_socket(TCP), pal_event(事件循环),
             pal_file, pal_tls(可选 OpenSSL), pal_iocp(Windows IOCP proactor，仅 Windows)
src/resp/    RESP 协议（Phase 1）
src/core/    KV 存储、哈希表、过期、淘汰、命令分发、session、config、
             snapshot（Phase 2/4/5.3/6）
src/ds/      对象类型：obj（tagged blob、Hash 嵌套表、List 双链表、Set、
             ZSet dict+skiplist）、skiplist（无 span 跳表）（Phase 5.1/5.2）
src/server/  连接与服务器主循环（Phase 3）、aof（Phase 6）：单线程事件循环、
             recv 缓冲按需增长、解析→执行→推进零拷贝流水线；连接全部
             非阻塞（Phase 7.4）：写出经 out 缓冲 + writable 事件驱动，
             慢客户端不再阻塞主循环（详见 architecture 网络层说明）
tests/       单元测试（test.h 自研框架）+ 集成测试
bench/       压测客户端 ddup-bench（Phase 3，非 ctest 目标）
```
