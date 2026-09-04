# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-04T11:13:44Z
- runner: Linux runnervmgx7h7 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 58c0156
- garnet commit: 13413ff
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 641025.62 / 704225.31 | 632911.38 / 623052.94 | 0 / 0 | 0 / 0 | 389863.53 / 787401.56 | 411522.62 / 554016.62 | 664451.81 / 760456.25 |
| c500 P16 d16 | 340715.50 / 347826.09 | 332225.91 / 348432.06 | n/a | n/a | 328407.22 / 566572.25 | 546448.06 / 560224.12 | 600600.56 / 701754.38 |
| c500 P64 d16 | 476190.50 / 488997.53 | 446428.56 / 467289.72 | n/a | n/a | 493827.16 / 892857.12 | 1612903.25 / 1769911.50 | 985221.69 / 1204819.38 |
| c50 P16 d1024 | 709219.88 / 615384.62 | 571428.56 / 540540.56 | n/a | n/a | 346620.44 / 643086.81 | 512820.53 / 486618.00 | 573065.88 / 643086.81 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 757575.76 req/s, GET 877192.98 req/s
