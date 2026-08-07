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
