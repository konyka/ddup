# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-11-22T12:11:29Z
- runner: Linux runnervm0hi8g 7.0.0-1020-azure #21-Ubuntu SMP PREEMPT Tue Oct 27 22:34:38 UTC 2026 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 5c1e203
- garnet commit: ef3c20d
- Redis server v=8.0.5 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=d47fc40702b1fad8
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 1020408.19 / 1142857.12 | 985221.69 / 1149425.38 | 0 / 0 | 0 / 0 | 402414.47 / 816326.50 | 424628.47 / 732600.75 | 865800.81 / 1025641.06 |
| c500 P16 d16 | 445434.31 / 467289.72 | 437636.78 / 464037.12 | n/a | n/a | 257069.41 / 428265.53 | 892857.12 / 829875.50 | 862069.00 / 1015228.44 |
| c500 P64 d16 | 569800.56 / 606060.56 | 571428.56 / 602409.69 | n/a | n/a | 274348.44 / 547945.19 | 2127659.75 / 2500000.00 | 1219512.12 / 1470588.12 |
| c50 P16 d1024 | 769230.81 / 884955.75 | 793650.75 / 847457.62 | n/a | n/a | 318979.28 / 704225.31 | 865800.81 / 888888.94 | 775193.81 / 819672.12 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 729927.01 req/s, GET 865800.87 req/s
