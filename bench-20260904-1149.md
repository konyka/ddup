# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-04T11:49:52Z
- runner: Linux runnervmgx7h7 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: e08bb34
- garnet commit: 13413ff
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 826446.31 / 854700.88 | 655737.69 / 709219.88 | 0 / 0 | 0 / 0 | 461893.78 / 793650.75 | 484261.53 / 531914.94 | 657894.75 / 769230.81 |
| c500 P16 d16 | 380228.12 / 420168.06 | 378787.88 / 383141.75 | n/a | n/a | 369685.75 / 625000.00 | 578034.69 / 574712.69 | 647249.25 / 716845.81 |
| c500 P64 d16 | 583090.38 / 619195.00 | 550964.19 / 595238.12 | n/a | n/a | 508905.84 / 980392.19 | 1503759.38 / 1709401.75 | 975609.75 / 1176470.62 |
| c50 P16 d1024 | 694444.50 / 626959.25 | 628930.81 / 583090.38 | n/a | n/a | 334448.16 / 623052.94 | 491400.50 / 478468.88 | 574712.69 / 621118.00 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 738007.38 req/s, GET 836820.08 req/s
