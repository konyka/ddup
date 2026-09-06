# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-06T05:45:56Z
- runner: Linux runnervmejwal 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 4bc2c16
- garnet commit: c84bd7c
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 766283.50 / 843881.88 | 716845.81 / 763358.81 | 689655.19 / 0 | 0 / 0 | 466200.47 / 819672.12 | 289435.59 / 543478.25 | 660066.00 / 772200.75 |
| c500 P16 d16 | 340715.50 / 353356.91 | 330033.00 / 338983.06 | n/a | n/a | 352733.72 / 584795.31 | 600600.56 / 573065.88 | 653594.81 / 766283.50 |
| c500 P64 d16 | 466200.47 / 485436.91 | 438596.50 / 464037.12 | n/a | n/a | 471698.12 / 843881.88 | 1626016.25 / 1724138.00 | 956937.75 / 1169590.62 |
| c50 P16 d1024 | 706713.81 / 630914.81 | 675675.69 / 593471.81 | n/a | n/a | 341296.91 / 632911.38 | 506329.09 / 488997.53 | 569800.56 / 638977.62 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 722021.66 req/s, GET 803212.85 req/s
