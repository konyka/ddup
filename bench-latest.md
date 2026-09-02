# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-02T01:35:36Z
- runner: Linux runnervmgx7h7 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 91205f2
- garnet commit: 4f40ee9
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 660066.00 / 760456.25 | 617283.94 / 714285.69 | 0 / 0 | 749063.69 / 0 | 514138.81 / 900900.88 | 490196.09 / 461893.78 | 668896.31 / 735294.06 |
| c500 P16 d16 | 362318.84 / 386100.38 | 359066.44 / 355871.91 | n/a | n/a | 387596.91 / 647249.25 | 566572.25 / 554016.62 | 653594.81 / 740740.69 |
| c500 P64 d16 | 530504.00 / 564971.75 | 511508.94 / 537634.38 | n/a | n/a | 526315.81 / 1025641.06 | 1526717.62 / 1785714.25 | 956937.75 / 1183432.00 |
| c50 P16 d1024 | 694444.50 / 615384.62 | 626959.25 / 579710.12 | n/a | n/a | 383877.12 / 598802.44 | 477326.97 / 462962.94 | 561797.75 / 588235.31 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 706713.78 req/s, GET 760456.27 req/s
