# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-02T21:52:45Z
- runner: Linux runnervmgx7h7 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 6035f45
- garnet commit: 4f40ee9
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 763358.81 / 829875.50 | 740740.69 / 740740.69 | 0 / 0 | 0 / 0 | 493827.16 / 836820.12 | 323101.78 / 515463.91 | 706713.81 / 793650.75 |
| c500 P16 d16 | 380952.41 / 404858.31 | 370370.34 / 390624.97 | n/a | n/a | 377358.50 / 632911.38 | 568181.81 / 534759.31 | 694444.50 / 699300.69 |
| c500 P64 d16 | 536193.06 / 573065.88 | 531914.94 / 563380.31 | n/a | n/a | 527704.50 / 975609.75 | 1587301.50 / 1709401.75 | 1069518.62 / 1265822.75 |
| c50 P16 d1024 | 675675.69 / 581395.31 | 636942.62 / 561797.75 | n/a | n/a | 380952.41 / 609756.06 | 483091.78 / 464037.12 | 598802.44 / 598802.44 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 722021.66 req/s, GET 796812.75 req/s
