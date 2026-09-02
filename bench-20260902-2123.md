# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-02T21:23:58Z
- runner: Linux runnervmgx7h7 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 24e6b3a
- garnet commit: 4f40ee9
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 840336.12 / 884955.75 | 790513.88 / 722021.62 | n/a | 746268.62 / 0 | 471698.12 / 836820.12 | 462962.94 / 454545.47 | 645161.31 / 757575.75 |
| c500 P16 d16 | 378787.88 / 398406.41 | 346020.75 / 375234.53 | n/a | n/a | 374531.84 / 613496.94 | 573065.88 / 563380.31 | 643086.81 / 749063.69 |
| c500 P64 d16 | 547945.19 / 581395.31 | 547945.19 / 576368.88 | n/a | n/a | 519480.53 / 990099.00 | 1550387.62 / 1652892.62 | 970873.81 / 1197604.88 |
| c50 P16 d1024 | 722021.62 / 606060.56 | 563380.31 / 534759.31 | n/a | n/a | 349650.34 / 632911.38 | 503778.31 / 486618.00 | 547945.19 / 630914.81 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 751879.70 req/s, GET 836820.08 req/s
