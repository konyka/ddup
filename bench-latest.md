# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-02T20:11:58Z
- runner: Linux runnervmgx7h7 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 3ebe689
- garnet commit: 4f40ee9
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 787401.56 / 847457.62 | 763358.81 / 790513.88 | n/a | 0 / 0 | 455580.84 / 803212.88 | 392156.88 / 536193.06 | 673400.69 / 772200.75 |
| c500 P16 d16 | 386847.19 / 390624.97 | 371747.22 / 396825.38 | n/a | n/a | 389863.53 / 645161.31 | 598802.44 / 609756.06 | 673400.69 / 772200.75 |
| c500 P64 d16 | 542005.44 / 571428.56 | 523560.22 / 566572.25 | n/a | n/a | 514138.81 / 985221.69 | 1680672.25 / 1724138.00 | 1010101.00 / 1226993.88 |
| c50 P16 d1024 | 714285.69 / 626959.25 | 632911.38 / 576368.88 | n/a | n/a | 347222.25 / 634920.62 | 507614.22 / 493827.16 | 566572.25 / 636942.62 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 760456.27 req/s, GET 881057.27 req/s
