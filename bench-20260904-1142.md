# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-04T11:42:51Z
- runner: Linux runnervmgx7h7 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 94e6009
- garnet commit: 13413ff
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 675675.69 / 819672.12 | 763358.81 / 836820.12 | 655737.69 / 0 | 716845.81 / 0 | 475059.38 / 843881.88 | 391389.44 / 505050.50 | 666666.62 / 766283.50 |
| c500 P16 d16 | 327332.25 / 353356.91 | 323624.62 / 343642.59 | n/a | n/a | 361010.81 / 589970.50 | 589970.50 / 593471.81 | 668896.31 / 754717.00 |
| c500 P64 d16 | 454545.47 / 477326.97 | 440528.62 / 464037.12 | n/a | n/a | 475059.38 / 881057.25 | 1526717.62 / 1694915.25 | 961538.44 / 1197604.88 |
| c50 P16 d1024 | 692041.50 / 634920.62 | 687285.19 / 595238.12 | n/a | n/a | 342465.75 / 632911.38 | 500000.00 / 484261.53 | 571428.56 / 636942.62 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 740740.74 req/s, GET 823045.27 req/s
