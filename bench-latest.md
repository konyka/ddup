# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-08-07T01:55:34Z
- runner: Linux runnervmvrwv9 6.17.0-1020-azure #20~24.04.1-Ubuntu SMP Fri Jun 19 20:09:14 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: e0bcb58
- garnet commit: 930e5f8
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000 -c 50 -P 16, median of 3

| server | SET req/s | GET req/s |
|--------|-----------|-----------|
| ddup   | 547945.19 | 790513.88 |
| garnet | 396039.59 | 490196.09 |
| redis  | 684931.50 | 854700.88 |

internal consistency (ddup-bench on ddup): SET 431034.48 req/s, GET 865800.87 req/s
