# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-10-11T10:33:55Z
- runner: Linux runnervmmprz5 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 5c1e203
- garnet commit: b528df7
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=d064f9f48c901a92
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 660066.00 / 711743.81 | 714285.69 / 778210.12 | 0 / 0 | 680272.12 / 0 | 386100.38 / 671140.94 | 293255.12 / 462962.94 | 655737.69 / 757575.75 |
| c500 P16 d16 | 357142.84 / 371057.53 | 346020.75 / 356506.25 | n/a | n/a | 231749.72 / 352112.66 | 588235.31 / 584795.31 | 662251.69 / 760456.25 |
| c500 P64 d16 | 501253.16 / 540540.56 | 492610.84 / 522193.22 | n/a | n/a | 277777.78 / 470588.22 | 1666666.75 / 1818181.88 | 966183.56 / 1197604.88 |
| c50 P16 d1024 | 651465.75 / 615384.62 | 625000.00 / 579710.12 | n/a | n/a | 281690.16 / 619195.00 | 492610.84 / 469483.56 | 569800.56 / 630914.81 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 666666.67 req/s, GET 751879.70 req/s
