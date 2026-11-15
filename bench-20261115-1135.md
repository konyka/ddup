# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-11-15T11:35:11Z
- runner: Linux ip-10-0-142-184 7.0.0-1016-aws #17-Ubuntu SMP PREEMPT Mon Oct 19 17:36:33 UTC 2026 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 5c1e203
- garnet commit: 3412254
- Redis server v=8.0.5 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=d47fc40702b1fad8
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 1600000.00 / 1709401.75 | 1562499.88 / 1600000.00 | 0 / 0 | 1398601.38 / 1801801.75 | 505050.50 / 1388889.00 | 819672.12 / 1010101.00 | 1290322.62 / 1980198.00 |
| c500 P16 d16 | 793650.75 / 636942.62 | 787401.56 / 623052.94 | n/a | 0 / 0 | 396039.59 / 729927.06 | 1369863.00 / 1492537.25 | 1515151.50 / 1666666.75 |
| c500 P64 d16 | 1052631.62 / 1015228.44 | 995024.88 / 1058201.12 | n/a | 0 / 0 | 450450.44 / 1020408.19 | 3571428.50 / 4255319.50 | 2857142.75 / 3636363.75 |
| c50 P16 d1024 | 1315789.50 / 1298701.25 | 1298701.25 / 1282051.25 | n/a | 1063829.88 / 0 | 389863.53 / 975609.75 | 1123595.50 / 1081081.12 | 1136363.62 / 1226993.88 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 1063829.79 req/s, GET 1197604.79 req/s
