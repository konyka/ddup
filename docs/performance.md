# ddup 性能基准

每个涉及热路径的阶段完成后更新。测试方法、硬件环境随数字一并记录。

- 与 Garnet/Redis 的对比：本机同窗口矩阵见下方 Phase 34–37 各节；
  CI 每周自动对比见 `bench-results` 分支（bench.yml）。
- 架构层面对比（分片路由 vs 共享存储的定性分析）：
  见 [architecture.md](architecture.md)「架构对比：分片存储 + 消息路由
  （ddup mt）vs 共享存储（Garnet/Tsavorite）」一节。
- 资源边界检查只发生在接收缓冲扩容和复制快照分配之前，不增加每字节或
  每条命令的热路径开销；默认 `proto-max-request-bytes` 与
  `repl-max-snapshot-bytes` 均为 1 GiB。

## 记录格式

| 日期 | 版本 | 测试 | 环境 | QPS | p99 延迟 | 备注 |
|------|------|------|------|-----|----------|------|
| 2026-08-03 | Phase 2.1 | 哈希表 1M SET | Windows 11, clang 22, -O3+LTO | 2.32M ops/s | - | 含 snprintf key 生成开销 |
| 2026-08-03 | Phase 2.1 | 哈希表 1M GET | 同上 | 4.69M ops/s | - | 同上 |
| 2026-08-03 | Phase 3 | 服务器 SET (-n 100000 -c 50 -P 16) | Windows 11, clang 22, -O3+LTO, TCP loopback | 364k–532k req/s | - | ddup-bench 顺序客户端，两次运行区间 |
| 2026-08-03 | Phase 3 | 服务器 GET (-n 100000 -c 50 -P 16) | 同上 | 395k–565k req/s | - | 同上 |
| 2026-08-03 | Phase 4 | 服务器 SET (-n 100000 -c 50 -P 16) | 同上 | 339k req/s | - | 含过期检查/内存记账/LRU touch 开销 |
| 2026-08-03 | Phase 4 | 服务器 GET (-n 100000 -c 50 -P 16) | 同上 | 345k req/s | - | 同上 |
| 2026-08-03 | Phase 5.3 | 服务器 SET (-n 100000 -c 50 -P 16) | 同上 | 412k req/s | - | 含 WATCH 版本表 touch，开销在波动范围内 |
| 2026-08-03 | Phase 5.3 | 服务器 GET (-n 100000 -c 50 -P 16) | 同上 | 377k req/s | - | 同上 |
| 2026-08-03 | Phase 7.3 baseline | 服务器 SET (-n 100000 -c 50 -P 16) | 同上 | 549k/578k req/s | - | bench/run_bench.sh，两次运行 |
| 2026-08-03 | Phase 7.3 baseline | 服务器 GET (-n 100000 -c 50 -P 16) | 同上 | 680k/709k req/s | - | 同上 |
| 2026-08-03 | Phase 7.3 baseline | bench_core SET（进程内，20 万命令） | 同上 | 3.52M ops/s | - | 无 socket 的 CPU 侧基准 |
| 2026-08-03 | Phase 7.3 baseline | bench_core GET（进程内，20 万命令） | 同上 | 8.13M ops/s | - | 同上 |
| 2026-08-03 | 7.3 opt-A：expires 空表早退 | bench_core SET（30 万×4 median） | 同上 | 3.06M→3.15M ops/s (+3%) | - | db_expire_if_needed 在 expires 为空时 O(1) 早退 |
| 2026-08-03 | 7.3 opt-A：expires 空表早退 | bench_core GET（30 万×5 median） | 同上 | 7.39M→7.87M ops/s (+6.5%) | - | 同上 |
| 2026-08-03 | 7.3 opt-B：无 WATCH 时跳过 keyvers 写入 | bench_core SET（30 万×4 median） | 同上 | 2.90M→5.11M ops/s (+76%) | - | db_touch_key 在无活跃 WATCH 时只增 dirty，不做两次哈希写 |
| 2026-08-03 | 7.3 opt-B（A+B 累计） | bench_core GET（30 万×3 median） | 同上 | 7.50M→8.19M ops/s (+9%) | - | 累计 opt-A+B |
| 2026-08-03 | 7.3 opt-C：get+touch 单次探测 | bench_core GET（30 万×4 median） | 同上 | 7.49M→9.29M ops/s (+24%) | - | 累计 A+B+C；rh_get_touch 合并 LRU 刷新与读取 |
| 2026-08-03 | 7.3 opt-C（A+B+C 累计） | bench_core SET（30 万×3 median） | 同上 | 3.03M→5.18M ops/s (+71%) | - | 累计 opt-A+B+C |
| 2026-08-03 | 7.3 opt-D：传播 fan-out 无副本时 O(1) | 服务器 SET（run_bench ×2） | 同上 | 625k/625k req/s | - | connected_slaves==0 时跳过全连接扫描 |
| 2026-08-03 | 7.3 opt-D（最终累计） | 服务器 GET（run_bench ×2） | 同上 | 746k/752k req/s | - | 最终：SET 564k→625k (+11%)，GET 695k→749k (+8%) |
| 2026-08-03 | Phase 7.4 非阻塞写出 | 服务器 SET（run_bench ×2） | 同上 | 606k/621k req/s | - | 与 7.3 持平（噪声内）；收益在慢客户端韧性 |
| 2026-08-03 | Phase 7.4 非阻塞写出 | 服务器 GET（run_bench ×2） | 同上 | 735k/746k req/s | - | 同上 |
| 2026-08-03 | Phase 7.4 非阻塞写出 | bench_core SET / GET | 同上 | 5.01M / 8.89M ops/s | - | 同上 |
| 2026-08-03 | Phase 7.5 IOCP | 服务器 SET/GET c50 P16（iocp） | 同上 | 324k / 382k req/s | - | select: 389k / 442k（iocp 慢 ~15%） |
| 2026-08-03 | Phase 7.5 IOCP | 服务器 SET/GET c200 P16（iocp） | 同上 | 315k / 348k req/s | - | select: 303k / 375k（基本持平） |
| 2026-08-05 | Phase 9 命令 ID 表 + 缓冲池 | bench_core SET（进程内，20 万命令） | Windows 11, clang 22.1.6, -O3+LTO | 3.62M ops/s | - | 含命令 ID 表与缓冲池底座，默认 C23 |
| 2026-08-05 | Phase 9 命令 ID 表 + 缓冲池 | bench_core GET（进程内，20 万命令） | 同上 | 5.73M / 6.28M ops/s | - | 同上；run 2 为缓存热后 |
| 2026-08-05 | Phase 9 命令 ID 表 + 缓冲池 | cmd_resolve（mixed 9 命令名） | 同上 | 83.4M ops/s | - | bench_core 微基准，case-insensitive 哈希解析 |
| 2026-08-05 | Phase 9 命令 ID 表 + 缓冲池 | buf_pool get/put（64 KiB） | 同上 | ~3.92G ops/s | - | 热缓存下单线程借/还，零系统调用路径 |
| 2026-08-05 | Phase 9 命令 ID 表 + 缓冲池（强制 C99） | bench_core SET / GET | 同上，-DDDUP_C_STD_FORCE=99 | 3.62M / 5.82M / 5.50M ops/s | - | 与 C23 同量级，在测量噪声范围内 |
| 2026-08-05 | Phase 9 命令 ID 表 + 缓冲池（强制 C99） | cmd_resolve / buf_pool 64 KiB | 同上 | 81.3M / ~3.92G ops/s | - | 同上 |
| 2026-08-05 | Phase 10 SIMD RESP 解析 + socket 调优 | bench_core SET / GET（20 万命令） | Windows 11, clang 22.1.6, -O3+LTO | 3.35M / 5.98M / 6.05M ops/s | - | 含 SIMD find_crlf；与 Phase 9 同量级 |
| 2026-08-05 | Phase 10 SIMD RESP 解析 + socket 调优 | bench_core parse-only SET / GET | 同上 | 30.7M / 42.4M ops/s | - | 仅解析，不含 dispatch/storage；解析器余量充足 |
| 2026-08-05 | Phase 10 SIMD RESP 解析 + socket 调优 | cmd_resolve / buf_pool 64 KiB | 同上 | 82.5M / ~3.45G ops/s | - | 与 Phase 9 持平 |
| 2026-08-05 | Phase 10 SIMD RESP 解析 + socket 调优 | 服务器 SET/GET（run_bench，默认 IOCP） | 同上，loopback，-n 100000 -c 50 -P 16 | 313k / 392k req/s | - | TCP_NODELAY + backlog 511；IOCP 基线范围内 |
| 2026-08-05 | Phase 10 SIMD RESP 解析 + socket 调优 | 服务器 SET/GET（--io select） | 同上 | 331k / 376k req/s | - | 同上 |
| 2026-08-05 | Phase 11 mt 首版（io-threads=2） | 服务器 SET/GET（ddup-bench -n 100000 -c 50 -P 16） | Windows 11, clang 22.1.6, -O3+LTO, loopback | 101k / 89k req/s | - | 互斥队列 + 逐元素深拷贝；空队列才 kick 后（优化前 66k/54k） |
| 2026-08-05 | Phase 11 mt 首版（io-threads=4） | 服务器 SET/GET | 同上 | 47k / 47k req/s | - | worker 增多路由比例上升，开销随之增大 |
| 2026-08-05 | Phase 11 mt 优化后（io-threads=2） | 服务器 SET/GET | 同上 | 110k / 111k req/s | - | 任务合并 + 无锁 SPSC + 免深拷贝 + 连接-键亲和累计 |
| 2026-08-05 | Phase 11 mt 优化后（io-threads=4） | 服务器 SET/GET | 同上 | 58–60k / 57–62k req/s | - | 3 次运行稳定（修复跨 worker 死锁/活锁后） |
| 2026-08-05 | Phase 12 PSYNC | 副本断线追平（50k 键 ×32B，新 replid 全量） | Windows 11, clang 22.1.6, -O3+LTO, loopback | 0.07s | - | 含 ~2.3MB 快照传输 + 清库重载 |
| 2026-08-05 | Phase 12 PSYNC | 副本断线追平（同上，backlog 部分重同步） | 同上 | 0.03s（~2.1x） | - | 仅 backlog 尾部 100 条命令，无快照/清库；收益随数据集与写入量放大 |
| 2026-08-05 | Phase 13 commandstats | 计时开销 A/B（bench_core，启用 vs DDUP_NO_CMDSTATS） | Windows 11, clang 22.1.6, -O3+LTO | 差异 <1%（噪声内） | - | 每命令 2× pal_now_us（实测单次 ~19.5ns）+ 两次数组累加 |
| 2026-08-05 | Phase 13 commandstats | pal_now_us 微基准 | 同上 | ~19.5 ns/call | - | QueryPerformanceCounter 封装 |
| 2026-09-01 | Phase 128 MT aggregate OOM | fail-closed guard | Linux, Release | 无额外热路径分配；仅聚合命令执行一次受 mutex 保护的故障注入计数检查 | 防止 OOM 时错误的 home-only fallback |
| 2026-09-01 | Phase 129 MT key routing | MEMORY USAGE / HIMPORT SET classification | Linux, Release | 仅命令分类路径增加常量比较；数据面仍无额外分配 | 避免 key 错 shard 与 session-local 状态丢失 |
| 2026-09-02 | Phase 130 SORT STORE routing | dual-key classification | Linux, Release | 常量扫描可选 STORE 参数；无额外热路径分配 | 在排序结果写入前阻止跨 worker/slot 部分写入 |
| 2026-09-02 | Phase 131 SORT key metadata | COMMAND GETKEYS | Linux, Release | 仅管理面扫描可选 STORE 参数 | 保持客户端 key 元数据与实际路由一致 |
| 2026-09-02 | Phase 132 management key metadata | COMMAND GETKEYS | Linux, Release | 仅管理面常量分支，无数据面开销 | 修正 MEMORY/OBJECT 子命令位置 |
| 2026-09-02 | Phase 133 HIMPORT owner routing | session-local fast path | Linux, Release | 同 worker 无任务分配；仅远端请求增加一次 owner 判断 | 保留本地功能并阻止 fieldset 丢失 |
| 2026-09-02 | Phase 134 script cache broadcast | control-plane fan-out | Linux, Release | 仅 SCRIPT/FUNCTION 管理命令创建聚合任务；EVALSHA/FCALL 数据热路径不变 | 消除跨 worker NOSCRIPT/未知函数 |
| 2026-09-02 | Phase 135 script broadcast errors | completion aggregation | Linux, Release | 复用既有聚合 completion，无新增数据面分配 | 任一 worker 错误时整体 fail-closed |
| 2026-09-02 | Phase 136 CONFIG consistency | control-plane fan-out | Linux, Release | SET/RESETSTAT 才广播；GET 保持本地无任务路径 | 消除多 worker 配置漂移 |
| 2026-09-02 | Phase 137 SAVE consistency | worker multi-db session | Linux, Release | 复用既有 snapshot serializer；无新增数据面分配 | 保证 SELECT 非零 DB 时快照完整 |
| 2026-09-02 | Phase 138 BGSAVE consistency | worker multi-db session + aggregate fan-out | Linux, Release | 复用 SAVE 序列化与既有聚合任务；仅控制面增加一次命令分类/广播 | 保证非零 DB 触发 BGSAVE 时所有逻辑库均落盘 |
| 2026-09-02 | Phase 139 BGREWRITEAOF consistency | aggregate fan-out | Linux, Release | 复用 server-owned AOF flush hook；仅管理命令增加每个远端 worker 一个任务 | 消除多 worker AOF 控制面状态漂移，错误统一收敛 |
| 2026-09-02 | Phase 140 CLIENT LIST global view | aggregate fan-out + bulk payload merge | Linux, Release | 仅 CLIENT LIST 创建远端任务并线性拼接 bulk 负载；连接热路径无额外分配 | 返回所有 worker 的本地连接，避免 home-worker 视图遗漏 |
| 2026-09-02 | Phase 141 HOTKEYS control consistency | aggregate fan-out | Linux, Release | 仅 START/STOP/RESET 创建远端控制任务；GET 仍为本地读取 | 统一采样生命周期，避免 worker 间监控状态漂移 |
| 2026-09-02 | Phase 142 SLOWLOG RESET consistency | aggregate fan-out | Linux, Release | 仅 RESET 创建远端控制任务；LEN/GET 无跨 worker 复制 | 确保所有 worker 的慢日志环同时清空 |

Phase 12 说明：PSYNC 的收益不在 loopback 小数据集（绝对值几十毫秒），
而在大数据集 + 高写入场景——全量重同步成本 = 快照序列化 + 全量传输 +
清库重载（O(db 大小)），部分重同步 = backlog 尾部字节（O(离线期间写入
量)）。测试同时发现并修复了 >64KiB 快照帧接收的 size_t 下溢崩溃
（大快照全量同步此前不可用）。

复制 backlog 的追加路径在 2026-08-08 修复：溢出淘汰由逐字节循环改为一次
计算的起点推进，因而每次追加的淘汰成本为 O(1)，数据复制仍最多分成环尾和
环首两段。超大于容量的追加直接保留输入尾部，不改变累计 offset 或可恢复
offset 区间；零容量和分配失败状态也不会进入取模运算。

## Phase 13–16 说明（如实记录）

- **测量环境波动**：本阶段（2026-08-05 晚些时候）整机基准数字较上午
  整体下降约 3x（同一二进制、同一脚本），判定为环境热节流/干扰而非
  代码回退；本阶段只做 A/B 对比（同条件同时段），不与上午的绝对值
  直接比较。
- **Phase 14 io_uring**：设计要点是区分控制完成与事件完成（oneshot
  poll 的 re-arm 只在 res>0 时视为就绪事件；POLL_REMOVE/UPDATE 的
  res==0 回执直接丢弃）。Linux CI（kernel 6.x）双后端跑全量
  test_event/test_server；本地 Windows 仅验证 stub 回落路径。
- **Phase 15 mt INFO 聚合**：聚合成本 = 每 worker 一次 INFO __STATS__
  快照（O(库数×命令数) 累加）+ 一次跨 worker 广播往返；INFO 为低频
  管理命令，对热路径零影响（commandstats 计数本身 <1%）。
- **Phase 15 IOCP workers**：mt worker 在 Windows 默认改用 IOCP；
  每 worker 一次 `pal_iocp_post` 替代 self-pipe 写字节作为任务队列
  唤醒。连接迁移（key 亲和优化）在 IOCP 后端禁用——随机 key 负载
  不受影响（本就按任务路由），hashtag 亲和负载在 Windows mt 下仍走
  跨线程路由，记录在案。

## Phase 11 mt 性能分析（如实记录）

mt 首版在 loopback ping-pong 型基准上**显著慢于单线程**（2 worker 约为
select 单线程的 1/3）。原因：每个跨 worker 命令都要深拷贝 argv、两次
互斥队列投递、最多两次 wakeup 写字节，加上跨核缓存迁移；而单线程路径
每命令 CPU 成本仅 ~0.3µs。Garnet 的 thread-per-core 优势场景（海量并发
连接 + 小批量）当时的基准无法体现（Phase 17 前 ddup-bench 为顺序流水，
无法形成真实并发；现为真并发引擎）。

已落地的优化（本阶段全部完成）：

1. **任务合并**：同一 parse pass 内目标相同的连续流水命令合并为一个
   任务（实测 4 命令流水线 = 1 个跨线程任务）。
2. **无锁 SPSC 队列**：按生产者拆分的环形队列，C11 acquire/release；
   强制 C99 构建降级互斥环。kick 仅在队列空→非空。
3. **路由免深拷贝**：任务携带原始 RESP 字节，目标 worker 就地重新解析，
   消除逐元素 malloc。
4. **连接-键亲和**：干净连接一次性迁移到 key 属主 worker；hashtag /
   按用户前缀负载可零跨线程（随机 key 负载仍按 ~1/N 就地）。

累计效果（io-threads=2，loopback）：101k/89k → 110k/111k req/s。仍低于
单线程（~330k/~380k），差距来自每次跨线程往返的 wakeup + 缓存迁移；
随机 key 负载下约 3/4（4 worker）或 1/2（2 worker）命令需要跨线程，
因此 4 worker 反而更慢——共享无架构的收益场景是 CPU 成为瓶颈且连接
具备 key 局部性（hashtag/前缀亲和时跨线程趋零）。

压测中暴露并修复的并发缺陷（如实记录）：

1. `batch_target` 未初始化（calloc 0 被误认为指向 worker 0 的开放批次），
   导致每连接首个路由命令错投 worker 0。
2. 连接迁移时 `session->d` 未更新到新 worker 的 db，迁移后读到旧
   keyspace。
3. 队列满时的跨 worker 环形等待死锁（A 等 B 的环、B 等 A 的环）——
   mt_push_task 改为背压时自排空本 worker 队列。
4. 无界 drain 在持续生产下活锁（worker 永不回到 socket IO）——drain
   按环限批（512/次）并在有剩余时自 kick。

Phase 10 调查记录：`conn_flush` 的输出缓冲为单块连续内存，pub/sub 与复制
fan-out 按连接独立冲刷；当前模型下 `writev`/跨连接批量聚合无法减少系统
调用次数，因此未落地。批量发送的真正收益点在 thread-per-core 阶段的
per-worker 输出合并，记录在案。

## 对比压测 (CI, Phase 7.6)

每周 CI（.github/workflows/bench.yml，ubuntu-latest，loopback；CPU 数量以运行时
的 `nproc` 记录为准）对
ddup / Garnet / Redis 用同一 redis-benchmark（-t set|get -n 200000
-c 50 -P 16，三次取中位数）压测，结果发布在 `bench-results` 分支
（bench-latest.md + 按日期归档）。首次实测（2026-08-04，Redis 7.0.15）：

| server | SET req/s | GET req/s |
|--------|-----------|-----------|
| ddup   | 1,092,896 | 1,010,101 |
| redis  | 1,058,201 | 1,197,605 |
| garnet | n/a（dotnet 构建失败，见分支内 garnet-build.log） | n/a |

ddup 与 Redis 同量级：SET 略快 ~3%，GET 慢 ~16%（GET 路径的过期检查与
LRU touch 成本）。内部一致性：ddup-bench 顺序客户端仅 ~311k/380k，
说明 ddup-bench 客户端本身无法打满 50 并发下的服务器——已由
Phase 17 的真并发引擎解决（见下文）。

第二次实测（2026-08-04，Garnet 修复为 `-f net10.0` 后补齐三方对比；
CI runner 波动较大，两次 ddup 数字差 ~40%，以趋势为准）：

| server | SET req/s | GET req/s |
|--------|-----------|-----------|
| ddup   | 675,676 | 800,000 |
| redis  | 675,676 | 677,966 |
| garnet | 325,203 | 549,451 |

该轮 ddup SET 与 Redis 持平、GET 领先 ~18%，且 SET/GET 均明显领先
Garnet（~2x / ~1.5x）。

## Phase 7.5 IOCP 备注

ping-pong 型流水负载下 IOCP 每个往返比 readiness 多一次完成等待
（recv 完成 → 执行 → 发 send → send 完成），c50 慢约 15%；并发连接数
升高后差距消失（c200 持平）。Phase 17 之前 bench 客户端为顺序连接，
未压出 select 的 1024 fd 上限与 FD_SET 重建成本——IOCP 的真正优势
场景（海量并发长连接）仍未被基准覆盖，如实记录。

## Phase 23 mt 批量写出（2026-08-07，含中性结果）

| 场景 | mt4 基线（2 轮） | mt4 批量冲刷（2 轮） |
|------|------------------|----------------------|
| SET c50 P16 | 438k / 502k req/s | 389k–506k req/s |
| GET c50 P16 | 607k / 641k req/s | 516k–647k req/s |
| SET c200 P16 | 491k / 511k req/s | 448k–504k req/s |
| GET c200 P16 | 578k / 602k req/s | 465k–519k req/s |

单线程参考（同机同旗标）：SET ~410–516k、GET ~491–555k req/s。
c200 GET 严格交替 A/B（12 对）：旧中位 ~565k、新 ~575k req/s——**噪声内
持平**。改动保留的理由：消除"每完成一次任务一次 send 系统调用"的放大
（合并流水线任务时同一连接一轮多个完成项），顺序由 seq/reorder 先行保证，
零回归；在本机连接规模下无可见收益（syscall 非瓶颈），如实记录。

## Phase 21 优化汇总（2026-08-07）

| 项目 | 基准 | 前 | 后 | 变化 |
|------|------|-----|-----|------|
| wyhash（rhtable） | bench_core SET（20 万命令×3） | 4.19M ops/s | 5.01M ops/s | +19.6% |
| wyhash（rhtable） | bench_core GET warm（×3） | 6.68M ops/s | 7.59M ops/s | +13.5% |
| skiplist span | zsl_rank ×10 万（10 万成员） | 53.9 s | 7 ms | ~7700x |
| skiplist span | zsl_at ×10 万（10 万成员） | 51.4 s | 15 ms | ~3400x |
| 有界读排干（server） | 交替 A/B c50 P16 GET（12 对中位） | 893k req/s | 895k req/s | 噪声内（负结果，保留） |

- **flaky 稳定化**：集群 wire 测试全部改为墙钟 deadline 轮询（迭代计数
  在事件密集时不足一轮 gossip）；本地 test_cluster_migrate/epoch ×20、
  test_cluster_bus ×10 全绿。
- wyhash 为 deps/  vendored final v4.3（public domain，128 位乘法平台
  启用，FNV 回退保留）；span 算法与 Redis zsl 同构（1-based 内部语义），
  差分覆盖含删后 rank/index 对照。

## Phase 21 GET 路径差距分析（2026-08-07，含负结果）

- **CPU 侧**：bench_core 显示 GET 解析余量 50.9M ops/s、dispatch+存储
  7.3M ops/s，相对 ~900k req/s 的线速 CPU 占比约 14%——差距不在
  dispatch/存储/过期检查（GET/SET 共享同一条 db_get 单探测路径）。
- **线侧**：回复聚合本就按事件整批 flush；新增**有界读排干**（每次就绪
  最多 4 次 conn_read，把流水线输入并成单个 dispatch+flush 批次）。
- **A/B 实测（本机交替 12 对 c50 P16 GET）**：旧版中位 893k req/s，
  新版 895k req/s——**噪声内无差异**；c200 P16（4 对）同样持平
  （~867k 两侧）。保留该改动（原则性合流、零回归），如实记录负结果：
  CI 上 GET 对 Redis 的 ~16% 差距归因于 runner 波动与段聚合差异，
  而非本机可复现的热点。

## Phase 17 bench 真并发引擎（2026-08-04）

ddup-bench 重写为真并发客户端：-c 个连接全部同时在线（非阻塞 socket +
单事件循环），每连接维持 -P 在飞请求；新增 min/p50/p99/max 延迟统计
（log2 微秒直方图）与 -r keyspace 随机化；严格应答核对（数量一致、
SET 必须 +OK、停滞看门狗）。CLI 兼容（rps 行保持在最后一行供 CI 解析）。

新旧引擎同机对比（Windows 11, clang 22.1.6, -O3+LTO, loopback,
-n 200000，单线程 ddup-server）：

| 场景 | 旧（顺序客户端） | 新（真并发） | 提升 |
|------|------------------|--------------|------|
| SET c50 P16 | 310k req/s | 733k req/s | 2.4x |
| GET c50 P16 | 382k req/s | 858k–962k req/s | ~2.4x |

新引擎完整协议（本机）：SET/GET × c50/c200 × P16：733k/962k（c50）、
694k/826k（c200）；P1（无流水）：~80k–87k req/s（RTT 受限，符合预期）。
-r 10000 随机键 GET c50 P16：971k req/s。结论修正：此前"服务器仅
~310k"是客户端瓶颈；服务器在 50 真实并发下可达 ~730k–960k req/s，
与 CI 上 redis-benchmark 对 ddup 的实测（~1.09M/1.01M）同量级。
bench_core 同步复测无回退（SET 3.09M / GET 4.44–4.99M ops/s，本阶段
无核心代码改动）。

最终对比（Phase 7.3 baseline → 全部优化后，同机同旗标）：

| 场景 | baseline | 最终 | 提升 |
|------|----------|------|------|
| bench_core SET | 3.03M ops/s | 5.18M ops/s | +71% |
| bench_core GET | 7.49M ops/s | 9.29M ops/s | +24% |
| 服务器 SET (P16) | 564k req/s | 625k req/s | +11% |
| 服务器 GET (P16) | 695k req/s | 749k req/s | +8% |

CPU 侧提升主要来自写路径去重哈希查询（opt-B 去 keyvers 写入、
opt-A 去过期表查询、opt-C 合并 LRU 探测）；端到端受 loopback syscall
主导，提升相应被摊薄。服务器数字为两次运行，bench_core 为 3–5 次
交替运行中位数。

## Phase 7.3 性能剖析记录

方法论：bench_core（进程内 parse+dispatch 热路径，排除 socket）与
run_bench.sh（真实 loopback 服务器）双层测量。结论：**服务器 QPS 由
loopback syscall/RTT 主导**——bench_core 单命令成本 SET 0.28µs、
GET 0.12µs，而 loopback 每命令墙钟 ~1.8µs，CPU 占比约 15%。
因此优化目标 = 写路径上的重复哈希查询与传播开销，以 bench_core 验证
CPU 收益，run_bench 记录端到端影响。

## Phase 27 剖析报告（2026-08-07）

测量设施：ddup-bench 新增 `-d`（值大小）与 `-t ping`（纯 RTT 上限）；
`bench/run_matrix.sh` 场景矩阵（CSV 输出；从 /tmp 副本运行以规避
Windows Defender 对新建未签名二进制的瞬态查杀）；服务器常开 IO 计数器
（INFO `# IO`：loops/events/reads/writes/bytes，mt 经 __STATS__ 聚合）。
CPU 剖析：WPR CPU 采样 + xperf 导出 + llvm-symbolizer（构建关 ASLR +
CodeView PDB）。环境：Windows 11，16C/32T，clang 22.1.6，loopback。
注意：本机有一个杀不掉的僵尸 lld-link 进程独占 1 核（~3% 总容量），
所有数字含此底噪（建议重启机器后再做精确复测）。

### 基线矩阵（n=200000，qps 为单次运行；延迟单位 µs）

| 场景 | SET qps | GET qps | 备注 |
|------|---------|---------|------|
| base-st (c50 P16 16B) | 985k | 1099k | |
| base-mt4 | 844k | 844k | mt 低并发不占优 |
| base-mt8 | 775k | 763k | worker 越多开销越大 |
| c500-st | 743k | 840k | |
| c500-mt4 | 694k | 687k | |
| c1000-st | 457k | 601k | 含客户端饱和（见下） |
| c1000-mt4 | 542k | 455k | |
| p1-st (c50) | 92k | 84k | 客户端循环受限（见下） |
| p1-mt4 | 87k | 75k | |
| p64-st | 1905k | 2439k | 流水加深有效 |
| p64-mt4 | 2151k | 2273k | mt 首次反超 st |
| d1k-st (1KB 值) | 319k | 873k | st 大值写入明显吃亏 |
| d1k-mt4 | 738k | 769k | mt 大值 2.3x st |
| ping-p1-st / mt4 | 97k / 72k | - | 同 p1：客户端受限 |
| ping-p64-st / mt4 | 4348k / 5000k | - | 纯协议+回写上限 |
| ping-c1000-st / mt4 | 592k / 587k | - | |

### IO 计数器（c500 P64 SET，3M 命令，同一次负载前后采样）

| 指标 | st | mt4 |
|------|-----|-----|
| loops | 2314 | 89663（39x） |
| events | 95006 | 894905（9.4x） |
| reads（recv 调用） | 47503（63 cmd/recv） | 65241 |
| writes（send 调用/投递） | 47001（64 回复/send） | 98929 |
| bytes_read / written | 183M / 15M | 同 |

结论：st 的读写合并已经很好（每命令 ~0.032 次收发调用），sys-call
数不是 st 的瓶颈；mt 的唤醒/事件搅动是 st 的 9 倍量级（每次任务
投递一次 kick），这是 mt 低并发吃亏的主因之一。

### CPU 热点（WPR 采样，ddup-server 进程内占比）

st（c500 P64 SET）：用户态 44% / 内核 43% / 用户 DLL 13%。用户态热点：

- **哈希表路径 ≈16%**（wyhash/_wymix/_wymum + rh_hash/rh_find_in/
  rh_get_touch 合计）：每命令一次 18B 键哈希 + 探测 + LRU touch。
  剖析构建无 LTO（wyhash 未内联），Release（thin-LTO）占比应更低，
  但仍是最大用户态块。
- **命令传播 ≈5%**（srv_propagate + repl_backlog_append）：零副本零
  AOF 时仍在做 prop_buf 序列化 + backlog 追加。
- resp 解析 parse_at、session_execute、pal_wall_ms 等各 ~0.5–1.5%。

mt4（c500 P64 SET）：内核+DLL 占 72%（唤醒投递 + 双倍 send 的代价）；
代码侧 parse_at 1.3%、mt_exec_task ~1.3%、crc16+hash_slot（路由哈希）
~0.6%——注意 mt 对每命令做 crc16（路由）+ wyhash（查找）两次哈希。

### 客户端饱和与 P1 说明（诚实记录）

- ddup-bench 单线程事件循环每事件约 10µs；c50 P1 时每客户端周期
  ~513µs ≈ 50 × 客户端每事件成本，P1 行测的是客户端而非服务器。
  c1 P1 ping 服务器端 p50 = 8µs（loopback 全链路）。
- c1000 行：客户端与服务器的 CPU 采样几乎相等（2884 vs 2914），
  客户端接近饱和，c1000 数字是两侧共同瓶颈。

### 优化方向（按数据排序，27.3 执行）

1. mt 唤醒合并：按批 kick 而非逐任务（events 9.4x 的主因）。
2. 传播路径：无副本/AOF 时转发原始请求字节，跳过 prop_buf 序列化。
3. 哈希路径：确认 Release LTO 下 wyhash 内联后评估；mt 路由+查找
   双哈希可合并（crc16 与 wyhash 都全读键）。
4. c1000-st 的 select fd_set 重建与客户端饱和分摊（如需）。

### 27.3 优化记录

**opt-1 mt 唤醒去重**（commit 见日志）：kick 从逐任务一次 self-pipe
send/IOCP post 改为原子 pending 标志去重——worker 每次唤醒 drain 开头
重装标志，生产者仅在 0→1 时真踢。无锁序：push 先于 kick、标志重置
先于队列 drain，任何生产者推送要么被本次 drain 看到、要么触发新
唤醒。A/B（c500 P64 SET mt4，同机）：1.10M → 1.70M req/s（+55%）；
事件搅动 0.30 → 0.096 events/cmd（3.1x 收敛）。低并发（c50 P16）
基本持平（826k vs 844k，噪声内）：去重只在多生产者并发时有冗余可消。

**opt-2 传播路径 raw 转发**：顶层客户端命令的 AOF/backlog/副本流改为
直接转发原始请求字节（conn_process_input 把当前命令的原始区间挂在
session 上，aof_log 钩子加 raw 参数；MULTI 重放、Lua 效果命令与内部
执行仍走原来的规范化序列化，AOF 始终规范化）。交错 A/B（c50 P16，
before/after ×3）：SET 881k→902k（+2.4%，接近噪声但方向一致）；
GET 1049k→1008k（不经过传播路径，差异为噪声）。端到端收益被
loopback 内核时间摊薄——如实记录为边际收益，保留（结构更简单、
零行为变化）。

**opt-3 SET 路径单探测**：db_set_kv 原来对同一键做 4 次哈希+探测
（expires 查、旧值查、rh_set、rh_touch）。新增 rh_set_ex（一次探测
完成 查旧+覆盖+LRU meta，旧 kv 块交还调用方析构），expires 探测加
空表跳过。bench_core 进程内 A/B（4 轮交替）：SET 4.12M→4.56M ops/s
（+10.5%，每轮都胜）；socket 端到端被 loopback 内核时间摊薄为噪声级
中性（941k vs 922k，重叠区间）。GET 未触碰。

### 27.4 最终矩阵与定位（2026-08-07）

最终矩阵（全部优化后，n=200000，同机）与基线对照。**机器存在跨时段
漂移**（±15%：未触碰的 ping-p64 行在基线窗口 4.35M/5.00M、最终窗口
3.64M/4.08M——同代码不同窗口差 15%），因此跨窗口的基线↔最终对比仅
看趋势；各项优化的真实收益以 27.3 的交错 A/B 为准（同窗口交替测量）。

| 场景 | 基线 SET | 最终 SET | 基线 GET | 最终 GET |
|------|---------|---------|---------|---------|
| base-st (c50 P16) | 985k | 837k | 1099k | 939k |
| base-mt4 | 844k | 833k | 844k | 760k |
| base-mt8 | 775k | 752k | 763k | 794k |
| c500-st | 743k | 678k | 840k | 619k |
| c500-mt4 | 694k | 588k | 687k | 738k |
| c1000-st | 457k | 345k | 601k | 363k |
| c1000-mt4 | 542k | 351k | 455k | 372k |
| p1-st | 92k | 73k | 84k | 70k |
| p64-st | 1905k | 1739k | 2439k | 2222k |
| p64-mt4 | 2151k | 1961k | 2273k | 1923k |
| d1k-st (1KB) | 319k | 281k | 873k | 709k |
| d1k-mt4 (1KB) | 738k | 662k | 769k | 658k |
| ping-p64-st | 4348k | 3636k | - | - |
| ping-p64-mt4 | 5000k | 4082k | - | - |

结构结论（两窗口一致）：mt 在 c50 不再稳定落后（opt-1 消除了大部分
唤醒开销，base-mt4 ≈ base-st）；mt 在大值（d1k SET 2.2-2.3x st）与
高并发下保持优势；ping-p64 显示纯协议上限 ~3.6-5M req/s（Windows
loopback）。

c1000 分解（诚实记录）：单客户端 c1000 457k → 双客户端各 c500 合计
525k（+15% 来自客户端分流），剩余 c500→c1000 的服务端降幅
（743k→525k）主要是每连接完成事件开销与 1000×64KB 缓冲工作集的
缓存效应；未修（收益不明确）。P1 行测的是 bench 客户端循环
（~10µs/事件），c1 P1 下服务器端 p50 为 8µs。

**ddup vs Garnet 定位**：CI（ubuntu 4C，redis-benchmark c50 P16，
bench-results 分支最新）：ddup SET 481k/GET 590k，Garnet 394k/548k，
redis 697k/769k——c50 P16 上 ddup 已领先 Garnet，落后 redis。本阶段
补上了 CI 未测的维度：高连接（c500/c1000）与深流水（P64）+ 大值
（1KB），这些正是 Garnet 的强项场景；本机无 Garnet 对照，mt4 在
P64 达 ~2.0M req/s、d1k-mt4 达 2.2-2.3x st 的扩展性是 ddup 在那些
维度的证据。下一步与 Garnet 的高并发对比应在 CI（Linux epoll）上
做——Windows select/IOCP 与 Linux epoll 的高连接成本结构不同，
本机数字外推到 Garnet 主场不公平。

## Phase 28 高并发三方对比与修复（2026-08-07）

CI 矩阵扩展（bench.yml）：5 个变体 × 4 场景，redis-benchmark 同一
客户端、median-of-3（ubuntu 4C runner）。官方一轮（4916203，
同窗口数字可比）：

| 场景 | ddup-st | ddup-st-uring | ddup-mt4 | garnet | redis |
|------|---------|---------------|----------|--------|-------|
| c50 P16 16B | 1333k/1408k | 1183k/1105k | 935k/1064k | 629k/939k | 939k/1370k |
| c500 P16 16B | 1163k/1093k | 1075k/1449k | 1130k/1136k | 1333k/1274k | 1504k/1770k |
| c500 P64 16B | 2105k/2020k | 2041k/2247k | 1481k/1786k | 3226k/3509k | 2198k/2817k |
| c50 P16 1KB | 1242k/1198k | 1176k/1307k | 1042k/1099k | 1036k/1064k | 1205k/1316k |

（单元格 = SET/GET req/s。runner 窗口间漂移 ±30% 常见；同表内可比。）

**格局**：c50 P16 ddup-st SET/GET 双双第一（SET 超 redis 42%）；1KB
ddup-st ≈ redis、超 garnet；c500 P16 redis 领先 ddup ~30%；c500 P64
garnet 领先 ddup-st ~1.6x——garnet 的主场优势真实存在，ddup-mt4 在
该场景反不及 st（跨线程成本 vs 4 核竞争者瓜分 CPU），列为后续目标。

### 本阶段修复（全部由新矩阵暴露）

1. **io_uring SQ 间接数组从未写入**（dbbaf76）：uring_get_sqe 只写
   sqe 槽位、不写内核解引用用的 SQ 数组——所有提交都别名到 sqe[0]，
   首个 poll（监听 fd）碰巧可用，之后重复完成导致第二次阻塞 accept
   挂死：进程活着但哑巴。strace 实证（listen→accept→accept 阻塞）。
   补数组写入 + SQ 满时冲刷。这是 bench 里 uring 变体哑掉的根因；
   ctest 此前未暴露因为 ubuntu runner 上该变体在测试规模……修复后
   test_server 的 io_uring 轮在 ubuntu 实测通过。
2. **1KB SET 写路径**（323c2ee + c6a78a1）：(a) 无 AOF 且无副本从未
   接入时跳过整个 backlog 追加（每命令一次 1KB 环形拷贝白付）；
   (b) rh_set_ex2 把类型标签+载荷一次分配写入（省一次 1KB malloc+
   拷贝）。本机 loopback A/B：d1024 SET 382k→932k（+143%）；
   bench_core 进程内 +11%；CI 同场景从 ~160-175k 到 1042-1242k
   （> garnet，≈ redis）。16B 路径中性偏好。
3. **mt 背压自旋楔死**（e1f58cf）：环形队列满时生产者空转自排干，
   4 核 runner 上自旋者饿死被调度的消费者，全池互等成 wedge（进程
   活着但哑，线程全在 ep_poll 空等）。改为 64 轮自排干后让出 1ms。
   注：CI bench 里 mt4 仍偶发 wedge（探针无法复现， bench 高复发），
   bench.yml 暂以重启该进程兜底并在报告披露重启次数（本轮为 0）。
   **根因已在 Phase 29 定位并修复**（关停 join 楔死 + 退避量化
   崩塌，见下节）。

### 方法论备注

- bench.yml 防挂：所有存活检查带 timeout；cell 以"15s 内完成 100 请求"
  为准入（半残后端不再烧 24×120s）；n/a 时把进程/线程状态写入
  bench-diag.txt 随 bench-results 发布。
- 教训：CI 矩阵每加一个变体都要先过"哑server 不拖死全 job"这关。

## Phase 29 mt 深度流水扩展与 wedge 根因（2026-08-07）

### Wedge RCA（终结 Phase 28 的悬案）

gdb 线程取证（bench.yml 在变体 n/a 时自动 dump 全线程栈）钉死了链条：
主线程卡在 mt_server_stop 的 pthread_join，而某 worker 停在
mt_push_task 的环满退避循环里——该循环从不检查 running。一次运行中
的优雅停止（SIGTERM 触发 g_stop）因此永远无法完成：acceptor 与其他
worker 退出、卡住的 worker 不退、join 永远等待——进程活着、监听
开着、无人服务，即"活着但哑"的 wedge。停止的触发源有二：bench 的
restart 兜底 pkill（计数器写入文件，因此 cell() 在 $() 子壳中执行时
仍会保留）；更早无 pkill 的运行里的触发源未最终定位
（疑似 runner 侧信号），修复后即使触发也只是干净退出+被重启。
修复（cf2b4ef）：退避循环检查 running，停止时丢弃在途任务（将死
进程的未发回复无意义）让 join 完成。

### 随后暴露的吞吐崩塌（同根因家族）

关停楔死修好后，mt4 在 c500 P64 / d1k 单元格仍有 8k-39k/s 的爬行：
生产者/消费者瞬时滞后填满 1024 槽任务环，1ms 睡眠退避把全池进度
量化成毫秒跳。修复（26c994f）：退避改 sched_yield（新增
pal_thread_yield），inbox/completion 环加深到 8192。

### 修复后官方表（26c994f，同窗口）

| 场景 | ddup-st | ddup-st-uring | ddup-mt4 | garnet | redis |
|------|---------|---------------|----------|--------|-------|
| c50 P16 d16 | 556k/667k | 521k/576k | 448k/496k | 427k/518k | 733k/826k |
| c500 P16 d16 | 567k/641k | 554k/592k | 518k/535k | 552k/495k | 433k/545k |
| c500 P64 d16 | 581k/612k | 521k/549k | **722k/778k** | 2062k/2128k | 1626k/2020k |
| c50 P16 d1024 | 588k/580k | 568k/562k | 512k/528k | 543k/565k | 826k/749k |

mt4 全场景有数、无 n/a、无 0 值（1 次重启自愈）。mt4 在 c500 P64
首次稳定超过 st（722k vs 581k）。剩余差距：garnet 在 c500 P64 仍
领先 ~2.8x（4 核 runner 上 garnet 的批量/channel 设计 + ddup mt 的
跨线程路由成本）；这是下一个结构性目标，不是回归。

### 防再发

bench.yml：n/a 时自动采集 ps -T / VmSize / fd 数 / INFO / gdb 全线程
栈并发布到 bench-results（bench-diag.txt）——本次 RCA 全靠它。
教训入库：mt 任何等待/退避循环都必须检查 running；CI 的进程兜底
（pkill/重启）会把"优雅停止"路径变成负载路径的一部分，必须同样
可测。

## Phase 30 mt 结构化优化（2026-08-07）

目标：收窄 c500 P64 与 garnet 的差距。逐项结果：

1. **SPSC 缓存行对齐（保留，c2a3482）**：ring 的 head/tail 原子量原来
   共享一条缓存行，生产/消费者每次推拉都跨核弹跳。ddup_alignas(64)
   分开。CI 同窗口（c2a3482 运行）：mt4 c500 P64 1835k/2062k——mt4
   在该场景的历史最高值（Phase 29 修复后基线窗口为 722k/778k）。
2. **按目标分组的突发合批（已回退，015fd3b→151c756）**：把同一
   recv 突发内的命令按目标 worker 分组、每组一个任务、完成时按命令
   切片回插重排缓冲。本机（16C/32T）A/B：-4~6%（环操作本非瓶颈，
   切片 malloc 反而新增每命令一次分配）；CI（4 核）：mt4 c500 P64
   崩到 55k（停顿）。数据否定，revert 保留原状（连续同目标合批）。
3. **双哈希消除（不做）**：路由 crc16（home 侧）与查找 wyhash
   （worker 侧）分属不同哈希函数与不同线程，无法合并；记录在案。
4. **线程绑定（暂缓）**：4 核 runner 上 worker/acceptor/客户端抢核，
   pthread_setaffinity 只在 CI 可评估；留作后续（需 CI 可重复的
   A/B 设施）。
5. **kick 纪律**：Phase 27 的去重在现有路径上复核无误（空→非空才
   踢 + pending 原子）；本阶段无改动。

剩余差距的诚实分析：4 核 runner 上 garnet c500 P64 ≈ 3.8M 仍领先
ddup-st ~1.6x、mt4 ~2.1x。mt4 的每命令固定成本 = 2 次环操作 +
1 次任务分配 + 1 次原始字节拷贝 + 跨线程释放；要再降需要任务对象
池/回收集散（跨线程 free 回生产者侧池），复杂度与风险都不符合本
阶段范围。st 在同场景 2.4M 已是 loopback 单线程的合理上限附近。

### CI 可复现性注记（Phase 30 收尾）

同一份 mt 代码（c2a3482 padding 与 151c756 revert 后逐字节相同）在
CI 上一轮 mt4 全绿（c500 P64 1835k/2062k）、一轮 stall（9.7k/n/a，
3 次重启自愈）——4 核共享 runner 的负载抽签，非代码回退。mt4 的
CI 数字应按"可达上限"解读：c500 P64 ≈ 1.8M/2.0M（与 st 同量级、
约为 garnet 同场景的 45-55%）。wedge 类停摆（进程哑死）自
cf2b4ef 起未再出现；残留的偶发 stall 归因为 runner 争用下的
背压雪崩，重启兜底有效。

## Phase 31 mt 任务对象池（2026-08-08）

**设计**：路由单命令任务不再触碰分配器。突发内第一条命令的字节先
落 conn 态的 batch_inline（≤256B 免分配）；flush 时从 home worker
的互斥自由列表取回收任务，命令随 inline_cmd/inline_buf 走；
mt_task_free 统一把对象还回 home 的池（跨线程回收由互斥锁覆盖，
仅限 UNWATCH/UNSUB 等少数路径）。多命令批、>256B 载荷与特殊任务
类型保持堆路径。池上限 256/worker，回复缓冲 >256KB 直接释放。

**池化暴露并修复的真实 bug**：销毁顺序——ring 排空时任务被回收进
home worker 的池，而排在前面的 worker 的池互斥锁已销毁，回收即锁
已毁互斥体崩溃（Windows  vectored 异常栈 + llvm-symbolizer 定位到
mt_task_free←mt_server_destroy）。修复：销毁前先把全 worker 置
pool_off，回收退化为普通释放。

**数字**：本机 A/B（16C/32T，mt4）：c500 P64 SET 1856k→2016k
（+8.6%）、GET +2.3%；c50 P16 中性（噪声内）。CI（4 核）：见
bench-results 最新表——mt4 c500 P64 685k/717k（该窗口 st 797k/
820k、garnet 1980k/2041k）。

**遗留（诚实记录）**：CI 全负载 bench 里 mt4 仍偶发爬行单元格
（~8k/s 或 0，重启可自愈）——关停楔死/acceptor 丢连接/背压量化
三个真 bug 已修，残余 stall 只在 4 核 runner 五服务全负载下偶发，
本地（Windows/Linux 皆不可得）与隔离探针均无法复现。已排除项与
取证（线程栈、fd 计数、INFO 采样）在 bench-diag.txt 设施里；建议
后续用一台本地 Linux 机或 WSL 复现攻坚。bench.yml 的重启兜底保留，
重启次数随报告披露。

## Phase 32a IOCP 后端深度优化（2026-08-08）

针对 Windows IOCP（st）后端的逐项评估：accept 池、recv 补投时机、
send 路径、flush 纪律。本机 16C/32T，`--io iocp` 单线程；基线
（32a 前）：c50 P16 SET 996k/GET 1044k；c500 P64 1385k/1558k；
d1024 673k/825k（本机有一个杀不掉的僵尸 lld-link 独占 1 核，
所有数字含此底噪，A/B 同窗口交错运行可比对）。

**1. AcceptEx 池（保留，8ce9131）**：listen 时多挂一个 AcceptEx
（常态 2 个在飞，每个完成补挂一个）。消除连接风暴窗口内"上一个
accept 完成到补挂之间"的 accept 空转；稳态吞吐中性（收益在连接
建立速率，不在 ping-pong 吞吐）。

**2. RECV 先补投后处理（负结果，已回退）**：设想是 RECV 完成时先
补投下一个 WSARecv 再 parse→execute，让内核在处理期间填充下一块。
配套把接收缓冲改为 roff 消费偏移窗口 rbuf[roff, roff+rlen)——
conn_process_input 只推进偏移不再 memmove（在飞 recv 的落点
roff+rlen 从而保持有效），物理 compact/grow 只在完成入口（唯一
无在飞 recv 的时刻）进行；readiness 路径无在飞 recv，compact 随时
安全。实现正确性无误（ctest 55/55，含 IOCP 轮），但 A/B（base=仅
accept 池，3 轮交错 × {c50,c500}×{P16,P64}×{16B,1KB}×{set,get}）
一致性变差：

| 场景（中位数） | base | post-first | 差值 |
| --- | --- | --- | --- |
| c50 P16 16B SET | 938k | 887k | -5.5% |
| c50 P16 16B GET | 863k | 745k | -13.7% |
| c50 P16 1KB SET/GET | 502k/494k | 480k/472k | ~-4.4% |
| c500 P64 16B SET | 1418k | 1324k | -6.6% |
| c500 P64 16B GET | 1740k | 1608k | -7.6% |
| c500 P64 1KB SET | 641k | 629k | 噪声内 |

反向运行顺序（after 先跑）复验仍为负（c50 P16 GET 1143-1149k 对
1234-1276k），排除顺序偏差。原因：loopback 上处理期间到达的数据
本就在内核 TCP 缓冲里累积，旧模型"处理后补投"下一跳收到的是累积
的大块；先补投则下一完成事件只含处理窗口内到达的小块——批次被
切碎，每字节的完成周期（GQCS + 解析调度）开销变高。代码已回退，
设计记录于此备查（roff 偏移模型本身正确，可供将来真正需要双在飞
recv 的场景复用）。

**3. send 路径（评估后不改）**：零拷贝（直接投 out.data）不可行——
resp_buf out 在回复追加时可能 realloc，在飞 WSASend 引用它会悬垂；
经 conn 私有稳定 sbuf 拷贝是必要代价。多并发 send 缓冲队列复杂度
高、收益存疑（单在飞 send 已足够喂满 loopback）。

**4. flush 纪律（验证已满足）**：SEND 完成即推进 out_sent 并
kick_flush 续发；无 send 在飞时任何产出路径都直接 kick_flush。
无改动。

**附带观察（非本次改动引入）**：c500 P64 1KB GET 单元格在矩阵中曾
三轮连续 stall 到 ~10-20k/s，但同一二进制单独复跑正常
（1681-1948k/s）——运行窗口性环境抖动（Defender 扫描/僵尸进程
争用），非代码回退；该单元格标记为不稳定，若再现需按 Phase 30/31
的 wedge 取证流程排查。

**测试设施**：test_server 新增 `DDUP_TEST_IOCP_ONLY=1` 环境开关，
只跑 IOCP 轮（调试 IOCP 路径时免去 select 轮的等待）。

## Phase 32b io_uring op 模式后端（2026-08-08）

把 io_uring 从"epoll 替代品"（Phase 14 的 oneshot POLL_ADD readiness）
升级为真正的 proactor：提交 IORING_OP_RECV/SEND/ACCEPT 操作本身。
API 与 server 路径镜像 IOCP（pal_iouring_op.h 对 pal_iocp.h，op kind
数值一致；server.c proactor 路径两后端共享）：单在飞 recv/send、
kick_flush、sbuf 稳定发送缓冲、zombie 排水，全部复用 Phase 7.5-32a
验证过的模型。multishot accept（5.19+，-EINVAL 自愈降级单发补投）、
SQE 批量提交（每 loop 迭代一次 enter）、跨线程唤醒 NOP（锁不跨阻塞
enter）。仅 st；mt/TLS/集群总线维持原有限定。

**数字（CI ubuntu runner，4 核共享，redis-benchmark 同客户端，median
of 3，bench.yml 六变体矩阵，commit 863392e）**：

| 场景 | st select | st uring(readiness) | st uring-op |
| --- | --- | --- | --- |
| c50 P16 d16 | 1092k/1298k | 1142k/1098k | 1136k/0 * |
| c500 P16 d16 | 980k/1212k | 1273k/1342k | 1257k/1333k |
| c500 P64 d16 | 1960k/2247k | 1960k/2061k | **2061k/2272k** |
| c50 P16 d1024 | 1111k/1298k | 1156k/1226k | 1123k/1092k |

*首跑该单元格 GET median 0；复跑（2235a4d，整体更慢窗口）该格
781k 正常，确认是 runner 抖动而非后端缺陷（复跑中 mt4 自己的
c50 GET 也出了 0，同类 flake 各变体都会中）。复跑各格对比：
uring-op 763k/781k、c500 P16 738k/769k、c500 P64 1156k/1197k、
d1024 677k/673k——与 select/uring 同窗口互有胜负，全在噪声内。
两轮合并结论：op 模式对 epoll 持平（c500 P64 曾领先 5-10%），
满足保留标准；默认仍关闭（`--io iouring-op` 开启；默认后端选择
逻辑不变）。

**过程中修的 bug**：`config_apply` 的 io 白名单不认识 iouring-op，
`--io iouring-op` 直接 invalid option 退出——bench 变体首跑全 n/a
暴露（服务器日志即根因）。修复 + test_config 白名单测试（863392e）。

**设计备注**：multishot recv + provided buffers / fixed buffers /
SQPOLL 未做——Phase 32a（IOCP）证明 loopback 上批次完整性比提前武装
更重要，先以简单补投模型取数；若后续复跑显示 op 模式持续无胜，
负结果如实记录、后端保持默认关闭。

## Phase 33 io_uring op 模式进阶（2026-08-08）

在 32b 的补投模型上加三个特性，全部运行时探测 + 静默回落，CI 驱动
验证（本机无 Linux）。

**1. multishot recv + provided-buffer 环**（b27119b + 2d2c1a9 头文件
哨兵修复 + f21cd35 64KB 槽位）：每连接一次 IORING_OP_RECV|
RECV_MULTISHOT 挂 256×64KB pbuf 环（bgid 0），武装期间零补投；CQE
经 F_BUFFER 带槽位 id、F_MORE 表示请求存活；server 拷入 rbuf 即回收
槽位；环饥饿以 -ENOBUFS 终态结束→立即重武装（不关连接）；zombie
完成同样回收槽位；pending_ops 只在终态 CQE 归账。默认开（测试默认
覆盖），DDUP_IOU_RECV_MS=0 回落补投。

**2. SQPOLL / DEFER_TASKRUN|SINGLE_ISSUER**（32ed076）：
pal_iouring_create_ex(flags)，setup 失败重试裸 flags；env 门控
DDUP_IOU_SQPOLL=1 / DDUP_IOU_DEFER=1，默认关。DEFER 模式下 wait
每次必泵 enter（taskwork 只在 enter 跑），pal_iouring_post 限属主
线程。

**3. registered send buffers / SEND_ZC（Phase 97）**：sbuf 使用有界固定
大小池，按需启用，不做运行时扩容；`SEND_ZC` 的通知 CQE 到达前保持槽位
占用，确保内核 DMA 引用不会观察到复用内存。内核或 UAPI 不支持时回落普通
SEND。Phase 116 增加轮转提示游标，连续周转时从上次成功槽位的下一个位置
开始扫描，避免重复检查低编号忙槽位；分配仍受互斥保护，最坏扫描复杂度为
O(count)，不改变协议和回收语义。

Phase 116 的 TDD 回归验证 4 槽池连续 acquire/release 返回 0、1、2 的轮转
顺序；Linux io_uring 不可用时测试安全跳过。该优化减少高占用池的无效探测，
收益与同时在用槽位数成正比，建议在目标内核/NIC 上用 `ddup-bench` 做 A/B。

**A/B 数字**（bench.yml：7776 = op repost 基线 DDUP_IOU_RECV_MS=0；
7777 = 全栈 multishot(+64KB 槽)+SQPOLL+DEFER；同窗口对比才有效，
4 核共享 runner 跨窗口波动可达 2x）：

| 窗口 | cell | repost 7776 | 全栈 7777 | 结论 |
| --- | --- | --- | --- | --- |
| 32ed076（ms 16KB 无 sqpoll） | c500P64 d16 | 1176k/1250k | 1136k/1176k | ms -3/-6% |
| 同上 | c50P16 d16 | 671k/635k | 615k/651k | 互有胜负 |
| ca51d6a（64KB+SQPOLL+DEFER） | c50P16 d16 | 554k/621k | 619k/639k | +12%/+3% |
| 同上 | c500P16 d16 | 574k/597k | 591k/617k | +3%/+3% |
| 同上 | c500P64 d16 | 586k/711k | 586k/692k | 0/-3% |
| 同上 | c50P16 d1024 | 495k/501k | 522k/540k | +5.5%/+7.8% |
| b0a3ae8（同配置复窗口） | c50P16 d16 | 593k/628k | 649k/626k | +9%/0% |
| 同上 | c500P16 d16 | 684k/706k | 763k/787k | +11%/+11% |
| 同上 | c500P64 d16 | 1015k/1075k | **1219k/1282k** | **+20%/+19%** |
| 同上 | c50P16 d1024 | 626k/625k | 680k/682k | +8.5%/+9% |

第一窗口（16KB 槽、无 SQPOLL）multishot 一致性小负——16KB 槽把管道
突发切成多条 CQE，正是 Phase 32a 在 IOCP 上量到的批次碎片化；换
64KB 槽（对齐 SERVER_RECV_CHUNK）+ SQPOLL/DEFER 后连续两个窗口
反转为胜，c500 P64 最高 +20% 且超过全部 ddup 变体。64KB 槽与
SQPOLL/DEFER 同窗口切换、归因不细分；包级结论：**全栈胜，幅度
≥ 噪声，保留**。默认配置保持保守：op 后端默认关（`--io
iouring-op`），multishot 在其内默认开（测试默认覆盖），
SQPOLL/DEFER 维持 env 门控（DDUP_IOU_SQPOLL=1 DDUP_IOU_DEFER=1
即实测配置）；bench 常驻 7776(repost)/7777（全栈）双变体持续
对照。

## Phase 34c 本地 Garnet 对垒定点优化（2026-08-08）

本机 16C/32T 全矩阵（ddup-bench 同客户端，compare.sh full）：

| 场景 | ddup-st | ddup-mt4 | garnet |
| --- | --- | --- | --- |
| c50 P16 16B | 917k/935k | 806k/980k | 833k/905k |
| c500 P16 16B | 778k/683k | 687k/830k | 794k/823k |
| c500 P64 16B | 1429k/1770k | 1493k/1786k | 1905k/2000k |
| c50 P16 1KB | 606k/702k | 769k/769k | 680k/971k |

**方差警示（诚实记录）**：本机绝对值窗口间波动极大（st c50 P16
1KB GET 在对垒窗口 702k、A/B 窗口 1246-1317k），只有同窗口交错
A/B 可比对；跨窗口的绝对对比（包括对 garnet 的百分比差）应按
趋势解读。

**Target 1 — 1KB GET 消除二次拷贝（555bb80，保留）**：proactor 发送
路径从"out 拷入稳定 sbuf 再投重叠发送"改为**缓冲区 detach**——out
的分配整体移交发送角色（out 立即从 buf_pool 取同档温热备件，否则
每批重爬 256B→128KB 翻倍链、代价比省下的拷贝还大），发送完成确认
后归还池。readiness 路径本就是原地发送（一次拷贝），不动；慢副本
outbuf 检查计入 detach 尾部。A/B（c50 P16 d1024 GET，3 轮交错 ×
两版实现）：st 中性（噪声内），mt4 两轮各 3 回合一致 +2~3%。
预期的 +20-35% 没有出现——旧 sbuf 拷贝是缓存热的、近乎免费；
garnet 在 1KB GET 的领先不在这里。

**Target 2 — c500 P64 深管道（st IOCP）：计数器取证后不改代码**。
INFO # IO 差值（300 万 GET）：47502 次 recv 完成 = **63 命令/recv**
（P64 近似完美合批）、47001 次 send = 1 次/批、syscall/命令 ≈ 0.031、
events/loop ≈ 18。IO 路径已接近最优；对 garnet 的差是**单核天花板
对多核**（st 单核打满；该窗口 st 实测 2.31M rps，已超对垒窗口的
garnet 读数）。out 在 64 回复突发的翻倍重分配问题已被 Target 1 的
温热备件顺带消除（备件即预分配）。

**Target 3 — c500 P16：同样取证无异常**（15.97 命令/recv = P16 满批，
1 send/批，events/loop ≈ 25），按"先测量、小修为主"规则跳过。

**剩余差距**：st 单核天花板 vs garnet 多核；mt4 的每命令路由开销
吃掉多核扩展性（c500 P64 mt4 ≈ st，4 worker 未换算成倍数）——后者
是 Phase 28-31 未竟之地，候选后续靶点。

## Phase 35 函数级剖析与定点优化（2026-08-08）

方法：同窗口交错 A/B 确认差距 → WPR CPU 采样（build-prof 含 PDB，
xperf `-symbols -a profile -detail` 导出函数级自重）→ 只动 ddup 侧
头部热点 → 每修复独立 A/B（≥3 轮交错取中位）。

**差距确认（本窗口，garnet vs ddup-st 中位，3 轮）**：c500 P64 16B
SET 2005k vs 1472k（+36%）、GET 2183k vs 1896k（+15%）；c50 P16
1KB GET 1126k vs 963k（+17%）。

**剖析 top（ddup 侧自重，采样权重）**：

- c500 P64 SET：resp_parse 26% > session_execute 13% > rh_set_ex2 9%
  > conn_process_input 9% > db_expire_if_needed 7% > rh_hash 6% >
  rh_get_touch 6% > rh_migrate_some 5% > cmd_resolve 4.5%
- c50 P16 1KB GET：resp_write_bulk（value→out 的 1KB 拷贝本身）>
  resp_parse > rh_get_touch > conn_process_input > session_execute >
  db_expire_if_needed > command_dispatch > cmd_resolve > rh_hash

**结论：无可单点修复的热点**。成本均匀分布在 解析→命令解析→分发→
哈希→存取 的分层链上（每层 4-9%）；expire 快路径（expires 空即返）
与 cmd 哈希表已存在，LTO 已开。IO 层此前已证最优（Phase 34c）。

**尝试并回退**：RESP 解析器"数组内联 \$bulk 快路径"（省每子元素一次
递归分发）——实现语义等价、ctest 55/55，但 A/B 中位无任何一格超过
噪声（c500P64 GET 中位 -13% 但三轮散布 1533k-1976k，噪声量级
±10%），按"只留下可测胜利"规则回退（git checkout，未进入历史）。

**最终矩阵（compare.sh full，本窗口单轮，绝对值随窗口漂移）**：

| 场景 | ddup-st | ddup-mt4 | garnet |
| --- | --- | --- | --- |
| c50 P16 16B | 1025k/975k | 854k/947k | 909k/1058k |
| c500 P16 16B | 704k/645k | 680k/793k | 668k/706k |
| c500 P64 16B | 1379k/1307k | 1098k/1449k | 1526k/1515k |
| c50 P16 1KB | 552k/751k | 619k/749k | 729k/873k |

**剩余差距的定性**：st 单核分层每命令 CPU 是天花板所在；要再近
garnet 需要结构性减重（更浅的命令路径、存储层内联）而非微优化，
或接受单核定位。mt4 在 c500 P64 GET 已超 st（1449k vs 1307k）。

## Phase 36 命令热路径压平（2026-08-08）

Phase 35 剖析显示每命令成本均匀碎在分层链上、无单点热点；本阶段做
**深度削减**。判定仪器换成 bench_core（进程内，方差 <1-2%）——
socket A/B 的 ±10% 噪声地板对 3-8% 的胜利不可见（Phase 35 教训）。

逐项结果（bench_core 400k 命令，6 组交错，中位数）：

| 项 | 内容 | SET cold | GET warm | parse-only | 结论 |
| --- | --- | --- | --- | --- | --- |
| 解析器 \$bulk 内联（6975a4c） | \*N 数组的 \$ 子元素内联解析，免每元素递归分发 | 中性 | +3.6%（5/6） | **+20%（6/6）** / GET +13% | 保留 |
| rh_migrate_some 内联早退（d1aae9a） | 每次表操作的无迁移检查从跨函数调用变成内联 load+branch | 中性 | **+8%（6/6）** | - | 保留 |
| lean GET/SET（0670412） | plain 会话（已认证/非 MULTI/未订阅/无集群；SET 限无选项+非副本）跳过二次 cmd_resolve、READONLY/ownership 包装与分发 if 链 | **+24%（6/6）** | **+40%（6/6）** | - | 保留 |
| db_get expire 检查内联 | 把空表快路径移进 db_get | 符号不一致 | 符号不一致 | - | **回退**（仪器证不出，守纪律） |

关键发现：通用路径每条命令跑 **2-3 次 cmd_resolve**（session 层、
dispatch 层、写命令的 is_write_command 又一次）+ 长 if 链分发 +
每次 2 次时钟读（commandstats 计时）——lean 路径省掉这些；语义与
command_dispatch 块逐条一致（参数/类型检查、回复字节、dirty→
aof_log 传播、LRU 驱逐尾）。cmd_calls 仍计数（usec 计时不计——
两次时钟读比该统计值钱；INFO commandstats 的 calls= 保持精确）。

socket 终检（compare.sh full，单轮噪声窗口）：c500 P64 st 与 garnet
打平（1265k/1449k vs 1265k/1470k），c50 P16 16B st 领先；无归因
性回退。1KB 单元格 garnet 仍领先（绝对值随窗口漂移，差距结构未变）。

## Phase 37 mt 路由成本结构性削减（2026-08-08）

先取证：mt4 c500 P64 WPR 函数级剖析（prof.exe 含 PDB）。mt 特有成本
排序：mt_exec_task（含目标侧**重解析**）> mt_classify > cmd_resolve >
mt_push_task ≈ mt_drain_completions > mt_route > worker_on_wakeup >
mt_batch_flush；命令被路由时经历 home 解析 + target 重解析**两次**。

**逐项结论**：

1. **execute-in-place（home==owner）——已存在**。mt_route 的 local
   fast path（target==home->id 且无未决回复时直接 session_execute，
   有未决时算好进 reorder 缓冲）本就是就地执行，Phase 36 的 lean
   路径对它同样生效。本轮只是确认，无代码改动。
2. **mt_classify 热点 = crc16 逐位循环**（保留，a38bb15）：每字节 8
   次带分支迭代，每条被路由命令至少一次。改表驱动（每字节一次查表，
   惰性初始化、同值写竞态模式同 cmd_hash_init）。mt4 A/B 3 轮中位：
   **c500 P16 +5~8%（6/6 同号）**，c50 P16/c500 P64 噪声内。
   test_hashslot 全绿（槽位值不变）。
3. **回复直写（reply direct-write）——分析否决，未实施**。目标侧直接
   写 home conn 的 out 有数据竞争（out 可能正在 append/realloc），安全
   子集要求 home 借出期间推迟本地回复进 reorder——用 reorder 开销换
   拷贝，得不偿失。缓冲区 take 变体（空 out 时整体接管 reply 缓冲）：
   task 池回复容量随 take 流失，每 take 引入一对 malloc/free（~150ns）
   对 16B~1KB 拷贝（30-50ns）——稳态更贵；把 task reply 挂到 home 池
   则是跨线程池访问（池单线程无锁）。两条路都不通，记录在案。
4. **批路由 v2（按目标分组）——推迟（记录在案）**。现合批只合并
   连续同目标命令；按目标分组要求组内 seq 非连续，而 reorder 契约是
   seq+span 连续区间——这是 Phase 30 失败的同一个结构性原因，要动
   排序契约本身，风险/收益不划算。
5. **完成合批（completion coalescing）——推迟（记录在案）**。估算省
   每任务约 2 次原子操作（~1-3%），低于本机 socket 噪声地板且需引入
   带部分成功语义的链式 push（与背压循环交织），不可测量的复杂性
   不合并。
6. **parsed-forward（免目标侧重解析）——实现后回退（未入历史）**。
   home 把 argv 以 (off,len,type) 内联元数据随 raw 前递，target 重建
   argv 跳过 resp_parse。A/B：c500 P64 GET 3/3 一致 -5~-8%——meta
   的写入落在 home（瓶颈侧）而省下的解析在 target（非瓶颈侧），且
   blob 结构 +88B 伤池缓存局部性。方向性负收益，git checkout 回退。

**st 路径**：本轮未触碰（crc16 只在 mt 路由与集群槽位计算中使用；
st 非集群不经过）。

**CI 官方数字（bench.yml @ a38bb15，含 Phase 36 lean + Phase 37
crc16）**：st c500 P64 2247k/1904k、c50 P16 1492k/1515k；mt4 c500
P64 2083k/2597k（本轮 mt4 最强格，超 st）；uring 全栈 7777 c500
P64 1550k/3124k。本轮 mt4 两个单元格 0/爬行（c50 GET、c500 P16）
为已知 CI 抖病（2 次重启兜底），与本阶段改动无关（test_hashslot
及全套 mt/集群测试绿；Windows CI 失败日志分支最新条目为 08-07
旧 flake）。

## Phase 45：紧凑编码（listpack / quicklist / 小对象双编码）

对象内存（记账模型 `obj_*_mem`，两侧同一套每 malloc 16B 开销约定；
直接调 obj 层 API 的测量桩，改造前 = a700f27，改造后 = eb19d9e，
128 条目小对象）：

| 对象 | 布局 | 改造前 | 改造后 | 变化 |
| --- | --- | --- | --- | --- |
| list | 128 × 8B 元素 | 6176 B（48.2 B/元素） | 1375 B（10.7 B/元素） | -78%（4.5×） |
| hash | 128 × (8B 字段 + 8B 值) | 8256 B（64.5 B/字段） | 2711 B（21.2 B/字段） | -67%（3.0×） |
| zset | 128 × 8B 成员 | 20888 B（163.2 B/成员） | 1647 B（12.9 B/成员） | -92%（12.7×） |

吞吐：bench_core 只覆盖 SET/GET 存储路径（HSET/LPUSH/ZADD 仅为
cmd_resolve 名字样本），故无热路径 A/B 数字。复杂度注记：listpack
模式下 hash/zset 的范围、RANK、LEX 操作为 ≤128 条目的线性扫描
（Redis 同策略），超阈值自动转 rh_table / dict+skiplist。

## Phase 46：set 小对象 listpack 编码

同一记账模型测量桩（改造前 = a700f27，改造后 = 4c554f5），
128 × 8B 成员的 set：

| 对象 | 布局 | 改造前 | 改造后 | 变化 |
| --- | --- | --- | --- | --- |
| set | 128 × 8B 成员 | 7232 B（56.5 B/成员） | 1407 B（11.0 B/成员） | -81%（5.1×） |

SPOP/SRANDMEMBER 在 listpack 模式按随机下标直取，不再先收集全体
member 视图；SINTER/SUNION/SDIFF 的求值遍历改走 obj_set_each，
结果集仍是临时 rh_table（去重语义不变）。

## Phase 91：Stream 扩展 owner 路由

`XDELEX`、`XACKDEL`、`XNACK` 在 mt 热路径中仅增加常量时间的命令分类
分支；key hash、任务合并和 completion 顺序机制与既有 Stream 命令共用，
不引入额外 per-command 分配或跨 worker 锁。目标 worker 继续一次解析并在
本地 stream/PEL 索引上完成修改，home worker 只负责有序回复。此次改动未改变
数据结构或复制格式，因此没有可归因的新基准数字；完整 mt 回归为 5288 checks。

## Phase 92：C99 与节点输入安全

本阶段不改变数据面热路径：C99 静态断言回退从 typedef 改为编译期枚举
表达式，避免函数作用域的 unused-typedef 诊断；nodes.conf 地址长度校验只
发生在启动/配置解析路径。未产生吞吐变化，C99 `test_cstd` 32 checks 与
`test_cluster_nodes` 44 checks 均通过。

## Phase 93：mt 全量复制完成通知

`MT_TASK_RESTORE` 的 completion 仍为 O(1) barrier 计数路径；由于该任务不携带
客户端连接，drain 逻辑在读取连接状态前完成专用分支，避免异常解引用且不增加
热路径分配、锁或网络往返。hardening 下 `test_mt_server` 定向回归通过（61.61s），
默认/C99 全量 CTest 保持全绿。

## Phase 94：集群少数派可用性门控

`CLUSTER INFO` 在既有 16384 槽覆盖扫描中同步统计持槽 master 总数和可达数，
不增加额外遍历或分配；状态判定为 `covered && !FAIL && reachable >=
masters / 2 + 1`。该路径属于低频管理命令，数据面 key 路由的 O(1) owner
缓存不变。两主、三主少数派场景均以 TDD 覆盖。

## Phase 95：C99 多线程原子降级

强制 C99 构建在 GCC/Clang 下使用 `__atomic` 内建，在 MSVC 下使用 Interlocked
原语；只有没有任何编译器原子内建的平台才退回单线程实现。这样不改变 C11
热路径指令，同时避免 mt 复制状态在 C99 构建中发生数据竞争。`test_cstd`
新增四线程百万次增量回归并通过。

## Phase 97：io_uring 固定发送缓冲与 SEND_ZC

io_uring op-mode 新增有界 64×256 KiB 注册发送缓冲池；发送路径仅在设置
`DDUP_IOU_SEND_ZC=1` 且槽位可用时启用，槽位不足或内核拒绝 `SEND_ZC` 时
回落现有 detached-buffer SEND，不增加失败重试或额外网络往返。固定缓冲
生命周期由初始发送 CQE 与 `IORING_CQE_F_NOTIF` 双事件共同驱动，避免在
内核仍持有 DMA 引用时复用内存。基准数字待在目标 NIC/内核上做 A/B 测量；
本阶段先以 `test_iouring_op` 的真实 loopback SEND_ZC 回归和 `test_server`
三后端集成回归锁定安全性。

## Phase 99：哈希表扩容阈值缓存

`rh_table` 在初始化和完成扩容时计算并缓存 85% load threshold，
`rh_maybe_grow()` 的插入热路径只保留一次整数比较，避免每次插入重复执行
除法/乘法。扩容时机、增量迁移、查找和删除语义不变；新增
`test_cached_growth_threshold` 锁定阈值更新行为。

本机 Release 构建（Linux，300k 命令/阶段，16B value）两次运行中位数：

| 测试 | ops/s |
| --- | ---: |
| SET（冷，含插入） | 3.90M |
| GET（热） | 4.85M |

该优化减少扩容判断的算术指令，收益会随插入比例增加而放大；基准受 CPU
频率和系统负载影响，数字用于同机 A/B 趋势比较。

## Phase 100：整数 RESP 写出优化

整数 RESP 编码使用两位数字查表，每轮处理两位并将除法从 `/10` 降为
`/100`；负数的符号和 `LLONG_MIN` 处理保持不变。新增边界 TDD 覆盖 `0`
和 `UINT64_MAX` 的无符号格式化结果。

本机 Release 构建（Linux，预热 32B 输出缓冲，1M 次）两次运行：

| 测试 | ops/s |
| --- | ---: |
| integer RESP writer | 106M–109M |

该基准隔离数字格式化和 RESP 小响应写入，实际命令吞吐仍受解析、存储和
网络系统调用影响；数字用于同机 A/B 趋势比较。

## Phase 101：RESP 长度解析热路径

bulk string、array、map、set 和 push 的长度字段改用专用非负解析器：以
`size_t` 累加并在 `RESP_MAX_ARRAY_LEN` 边界处提前拒绝，保留 `parse_ll`
处理整数/浮点等需要完整有符号语义的类型。这样避免每个 bulk 子项执行
符号与 `LLONG_MAX` 溢出分支，`*N` 命令数组的快路径也复用同一实现；协议
错误、`-1` null marker 与 1 GiB 安全上限不变。

本机 Release 构建（Linux，300k 命令/阶段，16B value，3 次）观测范围：

| 测试 | ops/s |
| --- | ---: |
| parse-only SET | 35.7M–39.2M |
| parse-only GET | 40.6M–44.6M |

该阶段数字与上一版本的 38M 级基线处于同一机器噪声区间，未宣称端到端
吞吐提升；收益目标是减少长度解析的指令数并强化超限输入的早拒绝。

## Phase 102：RESP 长度位数分层

在 Phase 101 的专用解析器上继续分层：长度字段超过 10 位时立即拒绝，
常见的 1–9 位长度走无逐位上限判断的紧凑累加，10 位长度在累加完成后
统一比较 1 GiB 上限。`-1` null marker、非法字符和所有 RESP 类型语义
保持不变；这样既减少正常命令的分支，也限制畸形输入的扫描成本。

本机 Release 构建（Linux，300k 命令/阶段，16B value，3 次）观测范围：

| 测试 | ops/s |
| --- | ---: |
| parse-only SET | 37.1M–39.6M |
| parse-only GET | 37.7M–44.3M |

与 Phase 101 的 35.7M–39.2M / 40.6M–44.6M 重叠，未宣称可归因的端到端
吞吐提升；主要收益是短长度分支减少和超长输入早拒绝。

## Phase 103：RESP 长度前导零兼容性修复

长度位数早拒绝现在只计算去除前导零后的有效数字位数，避免误伤历史上
可接受的填充格式（例如 `$000000000001`）。数值累加仍对 1 GiB 上限做
最终校验，超长有效数字继续在扫描前拒绝；新增回归覆盖填充长度与空值
边界，未改变热路径的分支结构。

## Phase 104：RESP 整数解析快速分支

`parse_ll` 现在先去除前导零，再按有效数字位数分层：少于 19 位的常见
整数使用无逐位溢出判断的累加，19 位整数继续逐位校验 `LLONG_MAX`/
`LLONG_MIN` 边界；超过 19 位直接拒绝。`-0`、填充整数和所有既有错误
语义保持兼容，避免在 TTL、计数和偏移参数上重复执行昂贵的边界分支。

本阶段没有新增独立整数解析微基准；现有 parse-only SET/GET 主要由 bulk
长度解析主导，无法将该分支的收益可靠分离，因此仅记录实现与回归结果。

## Phase 105：填充长度累加优化

bulk 长度解析在完成前导零识别后，从首个有效数字开始累加，不再对填充
零重复执行乘法；全零字段直接返回 0。该改动只影响非典型填充长度，普通
命令的分支和 1 GiB 上限检查不变。

## Phase 106：整数解析基准仪器

`bench_core` 新增 integer RESP parser 微基准，直接循环真实
`resp_parse()`，交替覆盖 `0`、短正/负数和 `LLONG_MAX`，并将 arena
重置保持在循环内以模拟命令解析生命周期。该仪器不改变服务端热路径，
用于后续整数解析 A/B 测量。

本机 Release 构建（Linux，1M 次）观测范围：

| 测试 | ops/s |
| --- | ---: |
| integer RESP parser | 73.8M–76.7M |
| integer RESP writer | 92.2M–104.1M |

基准随后校准为预计算样本长度，移除循环内的 `strlen()` 开销。重复 1M
次运行测得
parser `68.4M–80.6M ops/s`、writer `97.3M–100.1M ops/s`；较宽的范围
来自主机调度噪声。

## Phase 108：整数边界回归扩展

整数 parser TDD 进一步覆盖 `LLONG_MAX`、`LLONG_MIN`、正负溢出、`-0`
以及带前导零的极限输入。该阶段不改变算法，仅强化快速分支与严格 19 位
分支之间的行为契约；默认、C99 与 hardening 三套 parser 回归均通过。

## Phase 109：整数解析属性回归

新增确定性属性样本，使用完整 RESP wire（`:<integer>\r\n`）覆盖
`-128..128` 的步进值和 `LLONG_MIN/MAX` 附近边界，共 919 项检查。该套
测试不改变运行时路径，用于防止后续解析器优化只通过内部辅助函数而遗漏
协议层行为。

## Phase 110：解析基准多轮中位数

为避免单次运行误导，使用同一 Release 构建连续运行 5 次（每次 1M
样本）并取中位数：parse-only SET `36.8M ops/s`、parse-only GET
`42.4M ops/s`、integer RESP parser `79.7M ops/s`、integer RESP writer
`99.6M ops/s`。各项离散度仍受主机调度影响，数据仅用于同机趋势比较。

## Phase 111：PAL 哈希能力探测收口

将 wyhash 所需的宽乘法能力判断从 `src/core/rhtable.c` 移入
`src/pal/pal_platform.h` 的 `DDUP_HAS_WYHASH`。运行时哈希算法与数据面未改变：
支持宽乘法的目标继续使用 wyhash，其余目标继续使用 FNV-1a + fmix64 回退，
因此本阶段不宣称吞吐变化。新增能力宏布尔值测试，确保默认和 C99 构建保持
一致的编译路径。

## Phase 113：CRC64 校验表并发安全

`crc64()` 的 256 项反射 ECMA 表改为编译期 `static const` 数据，删除首次调用
的生成循环和 ready 分支。DUMP/RESTORE 的链式 CRC 结果保持不变，稳态仍为每个
输入字节一次表查找；本阶段的主要收益是并发首次校验无数据竞争，并缩短冷启动
路径。`test_dump` 增加 8 线程并发校验回归，完整 DUMP/RESTORE 测试保持通过。

## Phase 114：命令哈希一次性发布

`cmd_hash` 初始化改用 PAL 原子 compare-exchange 状态机（`0=未初始化`、
`1=构建中`、`2=已发布`），构建线程以 release store 发布，解析线程以 acquire
load 读取。这样消除多 worker 首次 `cmd_resolve()` 的并发写表竞态；稳态仅增加
一次 acquire load。`bench_core` 当前 Release 观测 `cmd_resolve (mixed)` 为
约 `100.6M ops/s`，与历史 83M 级基线相比仍在同机编译/调度波动范围内。

## Phase 115：Windows QPC 频率缓存并发安全

Windows PAL 的 `QueryPerformanceFrequency` 缓存改为原子 once-publish 状态机，
避免并发首用时的普通标志数据竞争；查询失败或异常非正频率时使用非零安全回落
值。POSIX 路径和稳态 QPC 读取未改变，本阶段不宣称吞吐变化；`test_pal` 新增
8 线程首次计时回归，默认/C99/hardening 构建均保持通过。

## Phase 112：CRC16 路由表并发安全

`hash_slot()` 使用的 CRC16 表改为编译期 `static const` 数据，移除首次调用
时的 256 项生成循环、可见初始化分支以及多 worker 并发写表的数据竞争。每个
字节仍保持一次表查找，稳态哈希指令数不变；收益是首请求路径更短且在 mt
首次路由时满足 C 内存模型的只读并发安全。测试增加 8 线程并发调用和 0–64
字节参考实现对照，共 104 项 hashslot 检查。

## Phase 117：Lua 黑名单词长固化

`redis.call/pcall` 的脚本禁调检查将每个静态命令名改为“指针 + 编译期词长”
条目，比较前不再对候选词执行 `strlen()`。命令数量、大小写不敏感匹配和错误
文本均不变；每次脚本命令调用仍是固定 10 项线性检查，减少重复长度扫描。
`test_eval` 增加全部禁调命令及混合大小写回归，默认构建通过。

## Phase 118：Lua 黑名单长度分桶

在固定词长基础上按输入长度分桶：常见非禁调命令（例如 `GET`）直接零次
候选比较；长度相同的黑名单最多比较两个字符串（`evalsha`/`eval_ro` 和
`psubscribe`/`evalsha_ro`）。大小写不敏感语义和拒绝文本不变。测试钩子统计
候选探测次数，锁定非匹配长度为 0，覆盖所有禁调命令及混合大小写回归。

## Phase 119：脚本命令桥接一次性发布

进程级 `g_cmd_fn` 从普通重复写入改为 PAL 原子 once-publish：首个
`db_init()` 发布命令桥接，其余初始化只进行 acquire 检查并等待发布完成。
脚本执行路径同样 acquire 检查后再读取函数指针，消除 mt/运行时临时 DB 初始化
与脚本执行并发时的数据竞争；稳态仅增加一次原子加载，不增加分配或锁。
`test_script` 以 8 线程、每线程 32 次 DB 初始化/销毁回归发布状态。

## Phase 120：RESP 流式解析 arena 回滚

`resp_parse()` 在解析前记录 arena checkpoint；遇到不完整输入或协议错误时
回滚本次聚合元素数组及递归临时分配，但保留已申请的块作为后续请求的 warm
容量。这样同一个恶意半包反复触发解析不会令连接 arena 的 `used` 累积，也不
增加完整命令的额外分配或释放。`test_resp_parser` 重复 100 次不完整聚合并
锁定块数量/已用字节稳定，`test_arena` 覆盖 mark/rewind 保留既有分配。

## Phase 121：RESP 超大聚合预分配门控

聚合解析在申请 `resp_value` 数组前检查当前输入是否至少包含每个子项的
3 字节最小 RESP 表示（如空 simple string）。对于 `*1000000\r\n` 这类只有
头部的半包，直接返回不完整，不申请百万级数组；输入继续到达并满足下界后
才进入原有解析与分配流程。该检查是整数除法和一次分支，不改变完整请求
的 wire 语义，并显著降低超大半包的瞬时内存风险。

## Phase 122：CLUSTER SLOT-STATS memory-bytes

`CLUSTER SLOT-STATS` 在一次 `rh_each()` 扫描中同时累计每个槽的 key 数和
`memory-bytes`（主表 entry 估算 + `obj_extra_mem` 对象负载），避免为第二个
指标重复遍历数据库。累加采用饱和逻辑，异常超大对象不会回绕成小值；
`SLOTSRANGE` 输出两个统计字段，`ORDERBY memory-bytes` 复用固定 16K 栈数组
和确定性槽位排序。CPU/network 指标在下一阶段通过独立逐槽计数器提供。

## Phase 123：CLUSTER SLOT-STATS CPU/network counters

在 session dispatch 收尾处增加逐槽 `cpu-usec`、`network-bytes-in` 和
`network-bytes-out` 累计。仅 cluster DB 启用该记账，并复用已有的
`cmd_keys_accum()` 提取单一 hash slot；无 key、跨槽或拓扑命令 fail-closed，
避免伪造归属。入站字节优先使用原始 RESP 帧长度，栈 session 回退为 argv
字节之和；出站字节使用本次命令追加到回复缓冲区的增量。计数器预置在
`db` 内，无热路径分配，使用饱和加法防止长时间运行溢出。`ORDERBY` 仍使用
固定 16K 项数组，故不会引入新的按请求堆分配。

本阶段同时统一脚本命令的 key 位置：`EVAL_RO/EVALSHA_RO` 和
`FCALL/FCALL_RO` 均按 `numkeys` 提取后续 key，避免把脚本/函数名误计为槽
或路由 key；`COMMAND GETKEYS` 复用同一位置约定。

集群 ownership 检查同步覆盖 `MSETNX`、`HIMPORT SET`、`SUNIONCARD` 和
`SDIFFCARD`，保持多节点路由与跨槽提取器一致；这些分支只增加参数解析，
不改变单节点命令热路径。

mt 路由器现在也按 `numkeys` 处理 `EVAL/EVALSHA/EVAL_RO/EVALSHA_RO` 和
`FCALL/FCALL_RO`，把脚本执行转发到 key owner；错误计数或缺失 key 参数
留在 home worker 交给核心命令做标准错误校验，避免路由层改变协议语义。

`SFLUSH` 和 `TRIMSLOTS` 在 mt 模式走 fail-closed 拒绝分支，不会在单个
home worker 上做部分槽删除；待跨 worker 广播事务具备原子提交/回滚后再启用。
### Phase 143: MT client ID allocation

MT client IDs use disjoint arithmetic sequences per worker (`first = worker + 1`,
`stride = worker_count`). Allocation remains a single increment on the accepting
worker with no shared lock or heap allocation. Connection migration preserves the
existing ID. The new uniqueness check adds no steady-state command-path cost.

### Phase 144: MT CLIENT KILL

`CLIENT KILL` uses the existing aggregate fan-out path. The filter is copied once
into the aggregate task; workers scan only their local connection arrays, so the
steady-state cost is O(workers + total connections) with no global lock. Only the
matching owner marks a connection for close, preserving proactor zombie safety.

### Phase 145: routed slowlog and LEN

Routed task timing adds one pair of `pal_now_us()` calls only when the target
worker executes a sessionless command. `SLOWLOG LEN` reads a worker-local counter
and aggregates integer replies without copying entries or allocating per-command
storage; `RESET` and `LEN` are excluded from recursive slowlog recording.

### Phase 146: aggregated SLOWLOG GET

Each worker contributes at most its configured slowlog window; the home worker
merges a bounded `workers * count` candidate set and truncates to `count`. Entries
are copied once as raw RESP and sorted with insertion sort, which is efficient for
the small default window and avoids comparator/index allocations. The final reply
is assembled in a temporary buffer and published only after all reserves succeed,
so allocation failure cannot emit a malformed partial array. Strict count parsing
rejects invalid requests before fan-out.

### Phase 147: TLS cluster bus

TLS is isolated to bus connections and adds no cost to the normal client data
path. Each connection owns one `pal_tls` object; handshake interest is toggled
only between read/write readiness states, and gossip parsing starts only after
handshake completion. Certificate verification is opt-in through `tls-ca-file`;
failed handshakes are closed before any topology mutation. Since the current
proactor contract cannot represent TLS WANT_READ/WANT_WRITE, selecting a
proactor with `tls-cluster` falls back to the readiness backend at startup.

### Phase 148: bounded ACL

ACL state is server-owned in a fixed 32-user table. Command permissions use
512-bit bitsets and key patterns use bounded storage with an allocation-free
glob matcher, so authorized data commands add one bit test and only inspect
provided key arguments. Password checks use a constant-time byte comparison.
Key authorization extracts only command-declared key positions for common
single- and multi-key commands, avoiding value/options scans on the hot path.
MT performs this check once on the home worker before batching or forwarding;
remote workers do not repeat the lookup or allocate an ACL context.
An empty key-pattern set is a constant-time deny for key-bearing commands;
the default user's unrestricted access is represented by one `*` pattern.
Sensitive ACL subcommand gating is a fixed username comparison before any
registry traversal, so rejected requests do not touch user metadata.
`SETUSER`/`DELUSER` are cold-path broadcasts; normal authorization remains a
local bitset/pattern check with no cross-worker synchronization.
Category rules are expanded once during `SETUSER`; data-path checks remain
single bit operations rather than category traversal.
`ACL LIST` formatting is confined to the cold management path and uses a
bounded stack buffer per user.
`ACL GETUSER` enumerates the fixed command bitset only on the management path;
no per-command allocations are introduced into dispatch.

### Phase 157: ACL CAT command listing

`ACL CAT` remains a management-only cold path. Category queries scan the static
command table without allocations, count before emitting the RESP array header,
and reject unknown categories before touching command metadata.

### Phase 158: ACL DRYRUN authorization simulation

`ACL DRYRUN` is implemented as a side-effect-free management path. It performs
one bounded user lookup, command-ID resolution, and the existing bitset/key
pattern authorization check; it never executes or allocates command state.

### Phase 159: secure ACL password generation

`ACL GENPASS` uses the platform secure RNG only on the cold management path.
Generation is bounded to 4096 bits and converts directly into a stack buffer;
normal command dispatch has no added work, locks, or allocations.

### Phase 160: bounded ACL failure log

ACL failures are recorded in a fixed 32-entry registry-local ring. Event writes
copy into bounded inline fields and never allocate; `ACL LOG` serializes only on
the cold management path, with count limiting before RESP emission.

### Phase 161: MT ACL denial audit parity

Remote-route denials append to the home worker's fixed ACL ring before the task
is rejected. The bounded inline copy adds no allocation and avoids any remote
worker synchronization or duplicate logging.

### Phase 162: bounded ACL channel authorization

Channel permissions use a fixed 16-pattern inline table and the existing
allocation-free glob matcher. Pub/Sub authorization checks each supplied
channel before registration or routing; unrestricted `&*` is a single flag,
and data commands add no channel metadata allocations.

### Phase 163: default all-channel compatibility

The default ACL user carries a single `all_channels` flag, avoiding pattern
iteration for the common unrestricted Pub/Sub path while preserving Redis
compatibility when no custom channel policy is configured.

### Phase 164: ACL channel metadata visibility

Channel rules are rendered only on the cold `ACL LIST/GETUSER` paths. Bounded
stack buffers and fixed arrays preserve the zero-allocation data-plane checks.

### Phase 165: ACL channel aliases

`allchannels` and `resetchannels` update the same inline flags as `&*` and an
empty channel set. Alias parsing is bounded and allocation-free; authorization
remains a single flag check or fixed-pattern scan.

### Phase 166: publish channel argument boundary

`PUBLISH` and `SPUBLISH` authorize only their channel argument; the message body
is never scanned. Subscription commands continue to check each supplied
channel/pattern with bounded iteration.

### Phase 167: ACL SETUSER aliases

Common Redis ACL aliases update the existing fixed user state in place. Parsing
uses bounded case-insensitive comparisons and preserves atomic copy-on-success;
the authorization hot path remains unchanged.

### Phase 168: complete ACL reset semantics

`reset` clears all bounded credential and permission fields in the temporary
user copy and disables the account. The final assignment remains atomic and
does not add work to normal authorization checks.

### Phase 169: ACL rule-line capacity

The cold `ACL LIST` formatter reserves stack capacity for both maximum key and
channel pattern tables plus bounded credentials. This avoids heap allocation and
prevents truncated RESP lines when a policy uses all inline slots.

### Phase 170: ACL log event coalescing

The fixed ACL log compares each new event only with the latest inline entry.
Identical reason/user/object triples increment a saturating counter in place,
avoiding allocation and retaining more distinct failures under repeated attacks.

### Phase 171: ACL GETUSER flag accuracy

The management-only flags renderer now emits the command-state label
`nocommands` rather than a channel-state label. This is a constant output
selection with no impact on authorization or data-plane latency.

### Phase 172: ACL key metadata prefixes

`ACL GETUSER` adds the `~` marker while serializing bounded key patterns. The
conversion uses a stack buffer on the management path and does not affect the
allocation-free key authorization matcher.

### Phase 173: safe ACL log null fields

The ACL log API normalizes absent user/object fields before coalescing. This is
a constant-time guard on the cold failure path and prevents null-pointer access
without changing the fixed-ring layout.

### Phase 174: ACL LOG negative count compatibility

Negative log counts are normalized to zero after bounded integer parsing, so the
query returns an empty RESP array without an error allocation or ring traversal.

### Phase 175: ACL nopass semantics

`nopass` is represented by one inline flag and short-circuits password
comparison for that user. `resetpass` clears the flag and restores normal
constant-time comparison; no heap work is added to ordinary authorization.

### Phase 176: complete nocommands reset

`nocommands` clears both fixed allow and deny bitsets in the temporary user
copy. The operation remains O(1) over the bounded bitset and preserves atomic
  rule replacement without affecting the hot authorization branch.

### Phase 177: resetkeys domain isolation

`resetkeys` updates only the bounded key-pattern fields in the temporary user
copy. No command, channel, credential, or enabled-state work is performed;
authorization remains unchanged outside key-bearing commands.

### Phase 178: ACL alias state normalization

`allkeys`/`~*` and `allchannels`/`&*` clear their bounded pattern arrays before
setting the corresponding O(1) all-access flag. `resetpass` clears the fixed
`nopass` bit before password checks resume. SETUSER still operates on a stack
copy and commits once, so the update is atomic with no hot-path allocation.

### Phase 179: ACL unrestricted-state cleanup

`reset` now clears the fixed `no_password` flag, while `allcommands` and
`+@all` clear the bounded deny bitset. These are constant-size updates on the
stack user copy; authorization remains a single bitset check with no added
allocation or synchronization.

### Phase 180: ACL multi-key authorization completeness

Double-key commands now perform two bounded glob checks instead of checking only
the first key. The fixed loop has no allocation and preserves fail-closed
behavior for source/destination operations while adding only one predictable
pattern scan on the common path.

### Phase 181: ACL multi-key read/write completeness

`EXISTS`, `TOUCH`, and set operations now scan every key operand through the
bounded pattern list. The implementation reuses the existing fixed loop and
adds no allocation, keeping authorization fail-closed for mixed-key requests.

### Phase 154: ACL generation invalidation

Each fixed ACL user slot carries a monotonic generation. Sessions cache the
generation at bind/auth time and perform a pointer-safe integer comparison on
each command; stale sessions are cleared without allocation or registry scans.
MT performs the same check before home-worker authorization, so routed tasks
cannot observe a deleted/recreated user's permissions. The normal path adds
one predictable branch and no synchronization or heap traffic.

### Phase 155: registry-local ACL generations

Generation counters now live inside each fixed ACL registry instead of a process
global. This removes cross-server/worker contention and data-race exposure while
preserving O(1) session validation: the hot path still performs only one cached
integer comparison and no lock or allocation.

### Phase 156: case-insensitive ACL categories

### Phase 182: ACL subcommand key extraction

Management and diagnostics commands use a constant-time subcommand dispatch for
keyless authorization. `MEMORY USAGE` and `DEBUG OBJECT` select their actual key
at the correct argument index, retaining bounded pattern checks without parsing
or allocation on unrelated subcommands.

### Phase 183: ACL operational argument boundaries

`OBJECT` and `XINFO` use their second argument as the key, while operational
commands such as `FLUSHDB`, `FLUSHALL`, and `SHUTDOWN` remain keyless regardless
of option tokens. Dispatch is a fixed branch sequence with no temporary storage.

### Phase 184: ACL store-command key completeness

Store-style commands validate destination and source keys using bounded glob
scans. `numkeys` is parsed with a checked 64-bit helper before any index math,
so malformed or truncated requests fail closed without allocation or overflow.

### Phase 185: ACL advanced multi-key coverage

HyperLogLog, set-cardinality, and sorted-set multi-key commands now scan every
declared source key. The bounded loops reuse the same glob matcher and validate
`numkeys` before indexing, preserving predictable latency and fail-closed
behavior without temporary allocations.

### Phase 186: ACL replication and control boundaries

Replication and cluster-control commands use the same constant-time keyless
branch as other administrative commands. Their host/offset/option arguments are
never glob-matched as data keys, avoiding false denials and preserving the
allocation-free authorization path.

### Phase 187: ACL script/function key positions

`EVAL`, `EVALSHA`, `FCALL`, and read-only variants parse a checked `numkeys` and
scan only the declared key slice. Zero-key scripts return after command-bitset
authorization; no script/function name or argument value is glob-matched.

### Phase 188: ACL blocking and option key positions

Blocking pop commands scan only their declared key list and exclude timeout or
count options. `BITOP` checks destination plus every source, and legacy
`GEORADIUS` variants check optional `STORE/STOREDIST` destinations. All paths use
bounded loops with no temporary allocation.

### Phase 189: ACL stream and multi-pop key positions

`XREAD/XREADGROUP` locate the `STREAMS` marker and scan only the key half of the
paired stream/id tail. `XGROUP` checks its stream argument, while `LMPOP/ZMPOP`
use checked `numkeys` slices. Group, consumer, timeout, direction, and count
  tokens never enter the glob matcher.

### Phase 190: ACL MIGRATE key positions

`MIGRATE` checks its primary key at the protocol-defined position and scans all
keys following the optional `KEYS` marker. Host, port, database, timeout, and
  authentication tokens bypass pattern matching; bounded scans remain allocation-free.

### Phase 191: ACL PUBSUB introspection channels

`PUBSUB CHANNELS` and `PUBSUB NUMSUB` validate each supplied channel against the
bounded channel pattern list. `NUMPAT` has no channel operands and remains a
  constant-time command-only authorization path.

### Phase 192: ACL cluster and sentinel containers

`CLUSTER` and `SENTINEL` container commands are treated as keyless control-plane
operations. Subcommand names, node addresses, slot numbers, and configuration
  tokens bypass key glob matching while command permissions remain enforced.

### Phase 193: ACL persistence and range-store key positions

Persistence and range-store commands use fixed argument positions for destination
and source keys. `MSETEX` scans only its alternating key slots after checked
  `numkeys`; payloads, TTL values, and options never enter the glob matcher.

### Phase 194: ACL stream shape validation

Stream reads now require an even key/ID tail after `STREAMS`; odd-length or
truncated requests fail closed before pattern scans. The validation is a single
  bounded parity check with no allocation.

### Phase 195: ACL extension-command key boundaries

`BACKUP` and `HOTKEYS` remain command-only control paths. `HIMPORT SET` checks
the hash key after its subcommand, while `PREPARE`, `DISCARD`, and `DISCARDALL`
  skip key matching. All branches are fixed-position and allocation-free.

### Phase 196: ACL malformed-key fail-closed

Generic key authorization now rejects non-bulk key values and odd-length
`MSET/MSETNX` pairs before pattern matching. The checks are constant-time shape
  guards that prevent malformed requests from bypassing the bounded glob scan.

### Phase 197: ACL subcommand help boundaries

`OBJECT HELP` and `XINFO HELP` take a constant-time keyless branch, while other
subcommands retain their explicit key position checks. No allocation or pattern
  scan is added to help requests.

### Phase 198: ACL lossless policy metadata

Rule rendering now emits `nocommands` for restricted command policies and an
explicit `nopass` token for unrestricted authentication. Rendering remains a
  bounded stack-buffer operation with no authorization-path cost.

### Phase 199: ACL GETUSER unrestricted metadata

`ACL GETUSER` emits the fixed `nopass` flag and keeps `~*`/`&*` entries for
unrestricted key/channel domains. The additional RESP fields are rendered from
existing inline state without allocations in authorization or authentication.

Category names are normalized with a bounded ASCII comparison while parsing
`SETUSER`; no temporary lowercase buffer or heap allocation is introduced.
The authorization hot path remains the same pre-expanded bitset lookup.

### Phase 200: ACL password-rule exclusivity

Adding a plaintext password rule clears the fixed `no_password` flag before the
atomic user commit. Authentication remains a constant-time comparison path and
does not add allocation or synchronization.

### Phase 201: MEMORY USAGE SAMPLES validation

`MEMORY USAGE` validates its optional `SAMPLES <count>` pair in-place before
the O(1) deterministic estimate. ddup has no sampled nested encoding, so the
validated hint has no allocation, scan, or data-path cost while malformed
requests fail closed.

### Phase 202: SLOWLOG negative-count compatibility

Negative `SLOWLOG GET` counts are normalized to the current fixed-ring length
before bounded serialization. The path remains O(N) in retained entries with no
additional allocation or copy beyond the existing RESP output.

### Phase 203: LATENCY GRAPH no-sample handling

`LATENCY GRAPH` now fails closed when ddup has no latency samples instead of
returning a synthetic success payload. Event names are formatted into a bounded
stack buffer; malformed or oversized names take a constant-time generic error
path with no heap allocation.

### Phase 204: LATENCY HISTORY no-sample handling

`LATENCY HISTORY` uses the same bounded no-sample error path as `GRAPH`, keeping
event formatting allocation-free and avoiding ambiguous empty-array success
responses for unknown events.

### Phase 205: DEBUG STRINGMATCH

`DEBUG STRINGMATCH` reuses the allocation-free, binary-safe glob matcher used by
`KEYS` and ACL patterns. Matching remains bounded by the input pattern/string and
does not add heap traffic to the diagnostic path.

### Phase 206: INFO section compatibility

`INFO` accepts one or more section tokens after validating them in-place. The
compact ddup snapshot is emitted from the same bounded renderer for each
selection, preserving O(1) section dispatch without per-section allocations.

### Phase 207: CLIENT REPLY suppression

Reply mode is a three-state integer stored directly in `session`; changing the
mode does not allocate. `session_execute_at()` records the existing output
length and truncates only the current command's newly appended RESP bytes when
`OFF` or `SKIP` applies. Command execution, persistence, monitoring, and
statistics still run, while suppression adds one length snapshot and a branch
on the connection hot path.

### Phase 209: CLIENT TRACKING state

Tracking metadata is stored inline in `session` (bounded prefix slots, no heap
allocation). Option parsing is a single bounded pass; incompatible modes and
invalid `CACHING` requests fail before state mutation. `TRACKINGINFO` serializes
the fixed state directly, so the control path remains O(options + prefixes) and
does not add work to ordinary key commands.

### Phase 210: CLIENT CACHING one-shot state

The caching hint remains an inline session bit. `session_execute_at()` clears it
after the next non-`CLIENT CACHING` command, adding only one command-level branch
and no allocation. Tracking OFF and RESET clear the bit together with the other
tracking metadata, preventing stale hints from affecting later requests.

### Phase 208: CLIENT NO-TOUCH

`NO-TOUCH` is a session-local integer flag. During command execution the
selected database receives a transient `no_touch_active` bit, so normal reads
use `rh_get()` instead of `rh_get_touch()` without changing command signatures
or allocating. The bit is cleared immediately after dispatch; the extra cost
is one predictable branch per lookup and one store per command.

### Phase 211: CLIENT GETREDIR consistency

`GETREDIR` reads the inline tracking redirect field in O(1) without allocation.
The command is handled before optional server client hooks, so embedded sessions
and network sessions expose the same state and disabled tracking deterministically
returns `-1`.

### Phase 212: CLIENT TRACKING redirect validation

Redirect validation uses a server-owned linear scan of the live connection table
before mutating session state. The check is O(number of connections), occurs
only on the control command, and adds no allocation or cost to ordinary requests.
Embedded sessions without a server hook retain the host-controlled compatibility
path.

### Phase 213: CLIENT TRACKING default mode

Mode-less `CLIENT TRACKING ON` now preserves `tracking_mode == 0`, representing
Redis' default tracking behavior. The state transition is a constant-time branch
with no allocation; `TRACKINGINFO` emits only the active mode flags, and
`CLIENT CACHING` rejects default-mode sessions before mutating state. Existing
OPTIN/OPTOUT sessions retain their mode when re-enabled without an explicit mode.

### Phase 214: CLIENT TRACKING atomic option commit

Tracking PREFIX parsing now uses a fixed `4 x 64` byte stack scratch buffer and
commits the complete bounded state only after all options pass validation. Failed
commands therefore perform no partial mutation; the steady-state cost is one
bounded copy and no heap allocation, while ordinary data commands are unchanged.

### Phase 215: CLIENT TRACKING mode transitions

Mode-less re-enable now performs a constant-time transition to the default mode
for OPTIN/OPTOUT sessions and clears their mode-specific redirect, caching, and
NOLOOP/PREFIX state. BCAST remains protected by the existing mode-switch guard.
Error lengths use `sizeof` on static literals, avoiding truncated RESP payloads
without adding allocation or data-path work.

### Phase 216: CLIENT TRACKING mode-specific cleanup

The mode-less transition to default tracking clears bounded mode-specific
metadata in the same constant-time state update. No heap allocation or ordinary
command-path work is added; the fixed PREFIX storage is reset by count only.

### Phase 217: CLIENT TRACKING PREFIX overlap validation

Each new bounded PREFIX is compared against the existing fixed slots up to the
shorter length. Duplicate and containing prefixes fail before session mutation,
with O(prefixes x prefix-length) control-path work, no heap allocation, and no
impact on ordinary command execution.

### Phase 218: CLIENT SETINFO metadata

`LIB-NAME` and `LIB-VER` use fixed 64-byte session fields. Validation is O(value
length) with no heap allocation; `CLIENT INFO` uses a bounded stack renderer,
and `CLIENT LIST` uses a larger fixed line buffer with an explicit truncation
guard. Metadata updates do not add work to ordinary data commands.

### Phase 219: CLIENT SETINFO error compatibility

Unknown SETINFO attributes are rendered into a fixed 128-byte stack buffer with
bounded precision before returning the Redis-compatible error. The path is O(1)
for the bounded attribute and adds no allocation or data-path overhead.

### Phase 220: bounded management diagnostics

Unknown subcommand errors for management containers use a shared fixed 128-byte
stack renderer and show at most 64 bytes of attacker-controlled input. The
diagnostic path performs no allocation and never passes an unbounded `snprintf`
result length to the RESP writer.

### Phase 221: bounded command-name diagnostics

Top-level and queued-command unknown-name errors now cap attacker-controlled
command text before formatting. SCRIPT subcommand diagnostics share the same
fixed stack renderer, keeping malformed requests allocation-free and bounded.

### Phase 222: bounded FUNCTION library diagnostics

Duplicate FUNCTION library errors cap the library name at 64 bytes and pass
only initialized bytes to the RESP writer. The validation remains allocation-free
and off the ordinary command hot path.

### Phase 223: bounded CONFIG diagnostics

Unknown `CONFIG SET` parameters are rendered with a fixed stack buffer and a
96-byte input cap. Truncated `snprintf` results are clamped before RESP output,
with no allocation or impact on recognized configuration updates.

### Phase 224: bounded hash-TTL diagnostics

Unknown options in hash-field expiration commands cap displayed input at 96
bytes and clamp the formatted length before RESP output. Recognized expiration
paths retain their existing allocation-free hot path.

### Phase 225: bounded ACL DRYRUN diagnostics

Unknown ACL users are rendered with a 96-byte input cap and checked output
length. The authorization and command-dispatch hot paths remain unchanged and
allocation-free.

### Phase 226: bounded LATENCY diagnostics

No-sample errors from `LATENCY GRAPH/HISTORY` cap event names at 96 bytes and
clamp the formatted length before RESP output. The latency query path remains
allocation-free for both valid and invalid events.

### Phase 227: bounded HSETEX option diagnostics

The shared `HSETEX` option parser caps unknown-option text at 96 bytes and
clamps the formatted result before writing RESP. Valid field writes retain the
existing allocation-free parsing path.

### Phase 228: bounded Pub/Sub context diagnostics

Commands rejected while subscribed now clamp the fixed-buffer diagnostic length
before RESP output. Command-name normalization remains bounded and allocation-free.

### Phase 229: bounded INFO and ACL metadata rendering

INFO snapshot renderers now append through a fixed-buffer helper that clamps
truncated `vsnprintf` results before the next offset calculation. ACL rule-line
rendering is covered with maximum key/channel pattern sets; both paths avoid
heap allocation in the normal metadata response.

### Phase 230: bounded ACL rule-line appends

ACL rule-line metadata now uses a fixed-buffer variadic append helper for every
optional rule segment. Once full, appends become constant-time no-ops and the
renderer reserves the final two bytes for `CRLF`, avoiding length poisoning or
heap allocation.

### Phase 231: transactional MONITOR appends

MONITOR quoted-argument rendering now checks `len * 2 + 2` overflow and
propagates reserve failures. A failed argument rolls the connection buffer back
to the message start, preserving protocol framing without extra allocations.

### Phase 232: bounded replication headers

Replication handshake headers validate `snprintf` success and non-truncation
before frame-size arithmetic. The checks are constant-time and add no work to
the steady-state command or backlog append paths.
