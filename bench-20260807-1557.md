# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-08-07T15:57:55Z
- runner: Linux runnervmvrwv9 6.17.0-1020-azure #20~24.04.1-Ubuntu SMP Fri Jun 19 20:09:14 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: f5057d4
- garnet commit: 4ba5ebf
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=e53ff17674aa6190
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring (7775), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-mt4 | garnet | redis |
|----------|---------|---------------|----------|--------|-------|

| c50 P16 d16 | 493827.16 / 554016.62 | 530504.00 / 549450.56 | 459770.12 / 583090.38 | 404040.41 / 583090.38 | 677966.12 / 732600.75 |
| c500 P16 d16 | 470588.22 / 598802.44 | 485436.91 / 574712.69 | 444444.47 / 550964.19 | 647249.25 / 684931.50 | 995024.88 / 947867.31 |
| c500 P64 d16 | 673400.69 / 813008.12 | 668896.31 / 826446.31 | 607902.75 / 793650.75 | 1904762.00 / 2061855.62 | 1639344.25 / 1886792.50 |
| c50 P16 d1024 | 162733.94 / 550964.19 | 161550.89 / 546448.06 | 155763.23 / 524934.38 | 531914.94 / 563380.31 | 858369.12 / 769230.81 |

internal consistency (ddup-bench on ddup-st): SET 450450.45 req/s, GET 563380.28 req/s
