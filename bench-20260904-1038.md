# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-04T10:38:19Z
- runner: Linux runnervmejwal 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 33e3a8f
- garnet commit: 13413ff
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 952381.00 / 1020408.19 | 1015228.44 / 917431.19 | 0 / 0 | 0 / 0 | 606060.56 / 1063829.88 | 422833.00 / 613496.94 | 900900.88 / 1000000.00 |
| c500 P16 d16 | 413223.16 / 438596.50 | 389863.53 / 415800.41 | n/a | n/a | 446428.56 / 724637.69 | 694444.50 / 660066.00 | 847457.62 / 970873.81 |
| c500 P64 d16 | 600600.56 / 630914.81 | 598802.44 / 625000.00 | n/a | n/a | 600600.56 / 1081081.12 | 1941747.62 / 2061855.62 | 1265822.75 / 1538461.62 |
| c50 P16 d1024 | 873362.44 / 760456.25 | 781249.94 / 719424.44 | n/a | n/a | 444444.47 / 757575.75 | 626959.25 / 579710.12 | 751879.69 / 740740.69 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 865800.87 req/s, GET 913242.01 req/s
