# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-02T20:30:16Z
- runner: Linux runnervmejwal 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 77cba21
- garnet commit: 4f40ee9
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 836820.12 / 862069.00 | 647249.25 / 711743.81 | 729927.06 / 0 | 0 / 0 | 481927.72 / 847457.62 | 375234.53 / 512820.53 | 662251.69 / 775193.81 |
| c500 P16 d16 | 393700.78 / 416666.69 | 369003.69 / 395256.94 | n/a | n/a | 371057.53 / 623052.94 | 550964.19 / 546448.06 | 638977.62 / 719424.44 |
| c500 P64 d16 | 529100.56 / 566572.25 | 514138.81 / 550964.19 | n/a | n/a | 515463.91 / 985221.69 | 1724138.00 / 1834862.38 | 980392.19 / 1197604.88 |
| c50 P16 d1024 | 711743.81 / 625000.00 | 628930.81 / 579710.12 | n/a | n/a | 340715.50 / 641025.62 | 498753.12 / 470588.22 | 569800.56 / 606060.56 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 732600.73 req/s, GET 881057.27 req/s
