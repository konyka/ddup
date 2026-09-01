# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-01T03:29:48Z
- runner: Linux runnervmgx7h7 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 2b150d3
- garnet commit: c960760
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 1149425.38 / 1156069.38 | 1176470.62 / 1315789.50 | 0 / 0 | 0 / 0 | 692041.50 / 1273885.25 | 555555.56 / 800000.00 | 1081081.12 / 1265822.75 |
| c500 P16 d16 | 561797.75 / 597014.94 | 566572.25 / 581395.31 | n/a | n/a | 643086.81 / 975609.75 | 781249.94 / 740740.69 | 980392.19 / 1000000.00 |
| c500 P64 d16 | 881057.25 / 909090.94 | 862069.00 / 896860.94 | n/a | n/a | 847457.62 / 1785714.25 | 2666666.50 / 2857142.75 | 1886792.50 / 2272727.25 |
| c50 P16 d1024 | 1098901.12 / 966183.56 | 1000000.00 / 877193.00 | n/a | n/a | 660066.00 / 1000000.00 | 803212.88 / 729927.06 | 1000000.00 / 970873.81 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 1162790.70 req/s, GET 1290322.58 req/s
