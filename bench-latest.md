# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-09-04T11:09:52Z
- runner: Linux runnervmejwal 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 8c3702d
- garnet commit: 13413ff
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 1250000.00 / 1298701.25 | 1298701.25 / 1234567.88 | n/a | 0 / 0 | 766283.50 / 1315789.50 | 578034.69 / 668896.31 | 1156069.38 / 1315789.50 |
| c500 P16 d16 | 597014.94 / 662251.69 | 609756.06 / 623052.94 | n/a | n/a | 607902.75 / 966183.56 | 833333.38 / 873362.44 | 1169590.62 / 1183432.00 |
| c500 P64 d16 | 896860.94 / 925925.88 | 851063.81 / 873362.44 | n/a | n/a | 826446.31 / 1459854.12 | 2222222.25 / 2325581.25 | 1680672.25 / 2105263.25 |
| c50 P16 d1024 | 1092896.12 / 921659.00 | 985221.69 / 888888.94 | n/a | n/a | 583090.38 / 1041666.69 | 749063.69 / 722021.62 | 947867.31 / 1010101.00 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 1142857.14 req/s, GET 1324503.31 req/s
