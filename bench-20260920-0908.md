# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-20T09:08:02Z
- runner: Linux runnervmlun5p 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 5c1e203
- garnet commit: 5c6e595
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 727272.69 / 840336.12 | 772200.75 / 769230.81 | 0 / 0 | n/a | 392927.31 / 668896.31 | 288184.44 / 450450.44 | 628930.81 / 766283.50 |
| c500 P16 d16 | 347826.09 / 383877.12 | 359712.22 / 363636.34 | n/a | n/a | 235017.62 / 353982.31 | 578034.69 / 579710.12 | 653594.81 / 757575.75 |
| c500 P64 d16 | 484261.53 / 539083.56 | 495049.50 / 527704.50 | n/a | n/a | 284495.03 / 473933.66 | 1587301.50 / 1754386.00 | 947867.31 / 1197604.88 |
| c50 P16 d1024 | 647249.25 / 625000.00 | 626959.25 / 530504.00 | n/a | n/a | 286532.94 / 621118.00 | 508905.84 / 464037.12 | 554016.62 / 623052.94 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 682593.86 req/s, GET 772200.77 req/s
