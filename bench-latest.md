# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-02T21:43:57Z
- runner: Linux runnervmejwal 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 44a576f
- garnet commit: 4f40ee9
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 1149425.38 / 1369863.00 | 1149425.38 / 1324503.38 | 0 / 0 | 0 / 0 | 441501.09 / 985221.69 | 392927.31 / 719424.44 | 803212.88 / 952381.00 |
| c500 P16 d16 | 455580.84 / 506329.09 | 455580.84 / 510204.09 | n/a | n/a | 379506.62 / 729927.06 | 833333.38 / 796812.81 | 809716.62 / 947867.31 |
| c500 P64 d16 | 626959.25 / 680272.12 | 625000.00 / 677966.12 | n/a | n/a | 464037.12 / 1015228.44 | 1980198.00 / 2298850.75 | 1069518.62 / 1315789.50 |
| c50 P16 d1024 | 884955.75 / 917431.19 | 862069.00 / 847457.62 | n/a | n/a | 348432.06 / 847457.62 | 854700.88 / 781249.94 | 709219.88 / 790513.88 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 843881.86 req/s, GET 1015228.43 req/s
