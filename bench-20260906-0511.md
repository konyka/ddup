# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-06T05:11:44Z
- runner: Linux runnervmejwal 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 0d08517
- garnet commit: c84bd7c
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 655737.69 / 754717.00 | 706713.81 / 675675.69 | 0 / 0 | 0 / 0 | 433839.47 / 840336.12 | 417536.53 / 486618.00 | 662251.69 / 754717.00 |
| c500 P16 d16 | 375234.53 / 404040.41 | 379506.62 / 389863.53 | n/a | n/a | 367647.03 / 621118.00 | 586510.25 / 586510.25 | 653594.81 / 743494.44 |
| c500 P64 d16 | 554016.62 / 591716.00 | 576368.88 / 611620.81 | n/a | n/a | 498753.12 / 956937.75 | 1574803.12 / 1626016.25 | 947867.31 / 1111111.12 |
| c50 P16 d1024 | 709219.88 / 607902.75 | 630914.81 / 588235.31 | n/a | n/a | 328947.38 / 638977.62 | 484261.53 / 479616.31 | 564971.75 / 630914.81 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 701754.39 req/s, GET 836820.08 req/s
