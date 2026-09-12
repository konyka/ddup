# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-12T15:50:17Z
- runner: Linux runnervmlun5p 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: c2abaf6
- garnet commit: 09f0554
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 749063.69 / 840336.12 | 757575.75 / 751879.69 | 0 / 0 | 809716.62 / 0 | 467289.72 / 823045.25 | 357142.84 / 488997.53 | 729927.06 / 816326.50 |
| c500 P16 d16 | 384615.41 / 399201.59 | 366300.38 / 386100.38 | n/a | n/a | 377358.50 / 611620.81 | 566572.25 / 568181.81 | 704225.31 / 803212.88 |
| c500 P64 d16 | 549450.56 / 589970.50 | 526315.81 / 561797.75 | n/a | n/a | 451467.28 / 925925.88 | 1562499.88 / 1694915.25 | 1092896.12 / 1342281.88 |
| c50 P16 d1024 | 675675.69 / 584795.31 | 632911.38 / 569800.56 | n/a | n/a | 327332.25 / 606060.56 | 496277.91 / 467289.72 | 597014.94 / 584795.31 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 757575.76 req/s, GET 873362.45 req/s
