# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-10-25T09:01:22Z
- runner: Linux runnervmtai0g 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 5c1e203
- garnet commit: a1c9862
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=d064f9f48c901a92
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 740740.69 / 826446.31 | 738007.38 / 793650.75 | 0 / 0 | 675675.69 / 0 | 373134.31 / 621118.00 | 304414.00 / 484261.53 | 673400.69 / 754717.00 |
| c500 P16 d16 | 353982.31 / 361010.81 | 343053.16 / 362976.41 | n/a | n/a | 230680.52 / 352112.66 | 613496.94 / 589970.50 | 657894.75 / 760456.25 |
| c500 P64 d16 | 495049.50 / 526315.81 | 490196.09 / 522193.22 | n/a | n/a | 282485.88 / 481927.72 | 1526717.62 / 1652892.62 | 970873.81 / 1190476.25 |
| c50 P16 d1024 | 657894.75 / 617283.94 | 636942.62 / 584795.31 | n/a | n/a | 279720.28 / 609756.06 | 484261.53 / 450450.44 | 561797.75 / 583090.38 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 593471.81 req/s, GET 664451.83 req/s
