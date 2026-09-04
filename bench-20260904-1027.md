# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-04T10:27:32Z
- runner: Linux runnervmgx7h7 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 1731dec
- garnet commit: 13413ff
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 687285.19 / 847457.62 | 829875.50 / 751879.69 | 763358.81 / 0 | 0 / 0 | 491400.50 / 873362.44 | 372439.47 / 508905.84 | 696864.12 / 760456.25 |
| c500 P16 d16 | 414937.75 / 435729.84 | 391389.44 / 416666.69 | n/a | n/a | 375234.53 / 607902.75 | 568181.81 / 557103.06 | 615384.62 / 696864.12 |
| c500 P64 d16 | 569800.56 / 600600.56 | 544959.12 / 591716.00 | n/a | n/a | 516795.88 / 980392.19 | 1739130.38 / 1612903.25 | 961538.44 / 1190476.25 |
| c50 P16 d1024 | 727272.69 / 623052.94 | 645161.31 / 607902.75 | n/a | n/a | 349040.12 / 651465.75 | 511508.94 / 483091.78 | 576368.88 / 645161.31 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 746268.66 req/s, GET 813008.13 req/s
