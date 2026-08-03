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
