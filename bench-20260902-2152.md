# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-02T21:52:41Z
- runner: Linux runnervmejwal 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: b3394c0
- garnet commit: 4f40ee9
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 766283.50 / 858369.12 | 673400.69 / 727272.69 | 0 / 0 | 0 / 0 | 487804.88 / 843881.88 | 304878.03 / 485436.91 | 722021.62 / 775193.81 |
| c500 P16 d16 | 382409.19 / 396039.59 | 375939.84 / 399201.59 | n/a | n/a | 399201.59 / 645161.31 | 552486.19 / 561797.75 | 699300.69 / 800000.00 |
| c500 P64 d16 | 540540.56 / 571428.56 | 554016.62 / 588235.31 | n/a | n/a | 461893.78 / 975609.75 | 1680672.25 / 1694915.25 | 1075268.75 / 1290322.62 |
| c50 P16 d1024 | 680272.12 / 574712.69 | 638977.62 / 573065.88 | n/a | n/a | 373831.75 / 602409.69 | 486618.00 / 449438.22 | 593471.81 / 600600.56 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 719424.46 req/s, GET 806451.61 req/s
