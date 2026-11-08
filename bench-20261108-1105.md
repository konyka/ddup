# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-11-08T11:05:25Z
- runner: Linux runnervmwuk1x 7.0.0-1019-azure #20-Ubuntu SMP PREEMPT Mon Oct 19 22:41:16 UTC 2026 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 5c1e203
- garnet commit: 5cc8f3f
- Redis server v=8.0.5 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=d47fc40702b1fad8
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 655737.69 / 865800.81 | 573065.88 / 813008.12 | 0 / 0 | 0 / 0 | 365630.72 / 684931.50 | 316455.69 / 495049.50 | 719424.44 / 778210.12 |
| c500 P16 d16 | 369003.69 / 390624.97 | 359066.44 / 377358.50 | n/a | n/a | 218579.23 / 349040.12 | 536193.06 / 534759.31 | 701754.38 / 793650.75 |
| c500 P64 d16 | 515463.91 / 550964.19 | 511508.94 / 536193.06 | n/a | n/a | 279329.59 / 491400.50 | 1470588.12 / 1769911.50 | 1111111.12 / 1360544.25 |
| c50 P16 d1024 | 675675.69 / 657894.75 | 660066.00 / 604229.62 | n/a | n/a | 287356.34 / 638977.62 | 486618.00 / 495049.50 | 619195.00 / 657894.75 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 581395.35 req/s, GET 689655.17 req/s
