# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-04T11:47:27Z
- runner: Linux runnervmejwal 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: c06e8eb
- garnet commit: 13413ff
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 873362.44 / 873362.44 | 722021.62 / 823045.25 | 0 / 0 | 0 / 0 | 447427.28 / 826446.31 | 297619.06 / 505050.50 | 706713.81 / 816326.50 |
| c500 P16 d16 | 358422.91 / 388349.53 | 362318.84 / 379506.62 | n/a | n/a | 378071.84 / 632911.38 | 544959.12 / 558659.19 | 682593.81 / 793650.75 |
| c500 P64 d16 | 539083.56 / 593471.81 | 552486.19 / 576368.88 | n/a | n/a | 464037.12 / 938967.12 | 1550387.62 / 1562499.88 | 1020408.19 / 1273885.25 |
| c50 P16 d1024 | 680272.12 / 583090.38 | 621118.00 / 554016.62 | n/a | n/a | 349040.12 / 597014.94 | 507614.22 / 472813.22 | 591716.00 / 595238.12 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 732600.73 req/s, GET 790513.83 req/s
