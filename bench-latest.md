# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-08-25T02:02:51Z
- runner: Linux runnervm76f27 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 24a7af5
- garnet commit: 1322207
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 796812.81 / 793650.75 | 722021.62 / 727272.69 | 0 / 0 | 0 / 0 | 483091.78 / 836820.12 | 421940.94 / 475059.38 | 727272.69 / 819672.12 |
| c500 P16 d16 | 394477.28 / 778210.12 | 397614.31 / 722021.62 | n/a | n/a | 277392.50 / 769230.81 | 566572.25 / 566572.25 | 699300.69 / 781249.94 |
| c500 P64 d16 | 613496.94 / 2380952.50 | 579710.12 / 2222222.25 | n/a | n/a | 336700.34 / 1923076.88 | 1680672.25 / 1666666.75 | 1069518.62 / 1290322.62 |
| c50 P16 d1024 | 666666.62 / 625000.00 | 606060.56 / 578034.69 | n/a | n/a | 386847.19 / 595238.12 | 507614.22 / 466200.47 | 583090.38 / 600600.56 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 840336.13 req/s, GET 961538.46 req/s
