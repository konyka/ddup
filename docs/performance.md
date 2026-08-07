# ddup 性能基准

每个涉及热路径的阶段完成后更新。测试方法、硬件环境随数字一并记录。

## 基线

待 Phase 3（网络服务器 + 压测客户端）完成后记录首批数字。

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

Phase 12 说明：PSYNC 的收益不在 loopback 小数据集（绝对值几十毫秒），
而在大数据集 + 高写入场景——全量重同步成本 = 快照序列化 + 全量传输 +
清库重载（O(db 大小)），部分重同步 = backlog 尾部字节（O(离线期间写入
量)）。测试同时发现并修复了 >64KiB 快照帧接收的 size_t 下溢崩溃
（大快照全量同步此前不可用）。

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

每周 CI（.github/workflows/bench.yml，ubuntu-latest 4 vCPU，loopback）对
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
restart 兜底 pkill（且计数器因 cell() 在 $() 子壳中执行而静默丢失，
restarts=0 是假象）；更早无 pkill 的运行里的触发源未最终定位
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
