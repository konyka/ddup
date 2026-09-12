# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-12T20:03:37Z
- runner: Linux runnervmlun5p 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 16b3712
- garnet commit: 09f0554
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 869565.19 / 947867.31 | 975609.75 / 966183.56 | 0 / 0 | 938967.12 / 0 | 501253.16 / 900900.88 | 410677.62 / 595238.12 | 896860.94 / 1010101.00 |
| c500 P16 d16 | 445434.31 / 478468.88 | 445434.31 / 468384.09 | n/a | n/a | 286123.03 / 428265.53 | 657894.75 / 684931.50 | 800000.00 / 943396.25 |
| c500 P64 d16 | 630914.81 / 660066.00 | 574712.69 / 638977.62 | n/a | n/a | 333889.84 / 576368.88 | 1739130.38 / 1769911.50 | 1162790.62 / 1470588.12 |
| c50 P16 d1024 | 775193.81 / 684931.50 | 760456.25 / 638977.62 | n/a | n/a | 361010.81 / 680272.12 | 579710.12 / 555555.56 | 694444.50 / 701754.38 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 823045.27 req/s, GET 917431.19 req/s
