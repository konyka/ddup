# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-12T08:55:43Z
- runner: Linux runnervmlun5p 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 2316bff
- garnet commit: 09f0554
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 714285.69 / 751879.69 | 746268.62 / 699300.69 | n/a | 0 / 0 | 338409.47 / 796812.81 | 438596.50 / 557103.06 | 673400.69 / 816326.50 |
| c500 P16 d16 | 451467.28 / 451467.28 | 479616.31 / 440528.62 | 460829.50 / 480769.22 | n/a | 286944.03 / 632911.38 | 597014.94 / 611620.81 | 687285.19 / 749063.69 |
| c500 P64 d16 | 602409.69 / 680272.12 | 649350.62 / 666666.62 | 772200.75 / 757575.75 | n/a | 484261.53 / 947867.31 | 1818181.88 / 1869158.88 | 1176470.62 / 1739130.38 |
| c50 P16 d1024 | 684931.50 / 796812.81 | 636942.62 / 724637.69 | 0 / 0 | n/a | 332778.69 / 623052.94 | 492610.84 / 468384.09 | 557103.06 / 581395.31 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 787401.57 req/s, GET 884955.75 req/s
