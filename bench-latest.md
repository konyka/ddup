# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-12T21:07:54Z
- runner: Linux runnervmlun5p 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 3dc2b2e
- garnet commit: 09f0554
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 769230.81 / 833333.38 | 757575.75 / 684931.50 | 0 / 0 | 751879.69 / 0 | 410677.62 / 696864.12 | 514138.81 / 447427.28 | 699300.69 / 823045.25 |
| c500 P16 d16 | 348432.06 / 364963.53 | 351493.84 / 357142.84 | n/a | n/a | 238379.03 / 362976.41 | 563380.31 / 557103.06 | 689655.19 / 803212.88 |
| c500 P64 d16 | 537634.38 / 561797.75 | 518134.72 / 537634.38 | n/a | n/a | 292825.75 / 485436.91 | 1526717.62 / 1626016.25 | 1063829.88 / 1298701.25 |
| c50 P16 d1024 | 689655.19 / 593471.81 | 664451.81 / 554016.62 | n/a | n/a | 303951.38 / 617283.94 | 493827.16 / 460829.50 | 595238.12 / 588235.31 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 687285.22 req/s, GET 760456.27 req/s
