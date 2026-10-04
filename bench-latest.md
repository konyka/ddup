# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-10-04T10:20:04Z
- runner: Linux runnervm8df0l 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 5c1e203
- garnet commit: fc97f95
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 724637.69 / 819672.12 | 660066.00 / 655737.69 | 689655.19 / 0 | 694444.50 / 0 | 363636.34 / 675675.69 | 452488.69 / 523560.22 | 657894.75 / 760456.25 |
| c500 P16 d16 | 359066.44 / 388349.53 | 350262.72 / 373134.31 | n/a | n/a | 235571.27 / 353982.31 | 593471.81 / 597014.94 | 668896.31 / 769230.81 |
| c500 P64 d16 | 500000.00 / 536193.06 | 492610.84 / 520833.34 | n/a | n/a | 280112.06 / 478468.88 | 1526717.62 / 1754386.00 | 975609.75 / 1197604.88 |
| c50 P16 d1024 | 651465.75 / 628930.81 | 653594.81 / 542005.44 | n/a | n/a | 290697.66 / 621118.00 | 503778.31 / 478468.88 | 564971.75 / 607902.75 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 653594.77 req/s, GET 719424.46 req/s
