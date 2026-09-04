# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-04T11:10:20Z
- runner: Linux runnervmejwal 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: b330087
- garnet commit: 13413ff
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 772200.75 / 699300.69 | 687285.19 / 680272.12 | 0 / 0 | 0 / 0 | 483091.78 / 836820.12 | 344234.09 / 451467.28 | 651465.75 / 749063.69 |
| c500 P16 d16 | 308641.97 / 350877.19 | 312012.50 / 334448.16 | n/a | n/a | 357781.75 / 593471.81 | 576368.88 / 579710.12 | 645161.31 / 757575.75 |
| c500 P64 d16 | 465116.28 / 483091.78 | 409836.06 / 462962.94 | n/a | n/a | 455580.84 / 854700.88 | 1574803.12 / 1754386.00 | 961538.44 / 1183432.00 |
| c50 P16 d1024 | 709219.88 / 628930.81 | 606060.56 / 539083.56 | n/a | n/a | 338409.47 / 641025.62 | 488997.53 / 481927.72 | 576368.88 / 643086.81 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 778210.12 req/s, GET 873362.45 req/s
