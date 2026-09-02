# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-02T21:34:19Z
- runner: Linux runnervmgx7h7 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: a67ea70
- garnet commit: 4f40ee9
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 826446.31 / 854700.88 | 732600.75 / 803212.88 | 0 / 0 | 0 / 0 | 493827.16 / 840336.12 | 343642.59 / 496277.91 | 709219.88 / 813008.12 |
| c500 P16 d16 | 378787.88 / 409836.06 | 378071.84 / 384615.41 | n/a | n/a | 382409.19 / 617283.94 | 544959.12 / 524934.38 | 643086.81 / 682593.81 |
| c500 P64 d16 | 550964.19 / 578034.69 | 549450.56 / 578034.69 | n/a | n/a | 508905.84 / 961538.44 | 1587301.50 / 1652892.62 | 1052631.62 / 1250000.00 |
| c50 P16 d1024 | 666666.62 / 604229.62 | 628930.81 / 576368.88 | n/a | n/a | 374531.84 / 609756.06 | 497512.44 / 471698.12 | 597014.94 / 606060.56 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 754716.98 req/s, GET 803212.85 req/s
