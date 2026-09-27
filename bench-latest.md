# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-27T09:52:16Z
- runner: Linux runnervmtr4k5 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 5c1e203
- garnet commit: d2d71f6
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 763358.81 / 823045.25 | 625000.00 / 660066.00 | 0 / 0 | 0 / 0 | 367647.03 / 626959.25 | 279329.59 / 473933.66 | 638977.62 / 772200.75 |
| c500 P16 d16 | 362318.84 / 375234.53 | 351493.84 / 375939.84 | n/a | n/a | 235571.27 / 362976.41 | 607902.75 / 600600.56 | 655737.69 / 769230.81 |
| c500 P64 d16 | 516795.88 / 555555.56 | 520833.34 / 547945.19 | n/a | n/a | 281293.94 / 496277.91 | 1562499.88 / 1769911.50 | 961538.44 / 1212121.12 |
| c50 P16 d1024 | 664451.81 / 611620.81 | 664451.81 / 537634.38 | n/a | n/a | 288184.44 / 619195.00 | 492610.84 / 461893.78 | 549450.56 / 593471.81 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 651465.80 req/s, GET 740740.74 req/s
