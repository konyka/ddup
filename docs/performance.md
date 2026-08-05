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
说明 ddup-bench 客户端本身无法打满 50 并发下的服务器，后续应增强
bench 客户端并发能力。

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
升高后差距消失（c200 持平）。本 bench 客户端为顺序连接，未压出
select 的 1024 fd 上限与 FD_SET 重建成本——IOCP 的真正优势场景
（海量并发长连接）当前基准无法体现，如实记录。

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
