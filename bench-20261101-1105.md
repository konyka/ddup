# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-11-01T11:05:50Z
- runner: Linux runnervm48cm3 7.0.0-1017-azure #17~24.04.1-Ubuntu SMP PREEMPT Thu Sep 24 18:46:57 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 5c1e203
- garnet commit: a5386cf
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=d064f9f48c901a92
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 1265822.75 / 1298701.25 | 1020408.19 / 1015228.44 | 0 / 0 | 0 / 0 | 497512.44 / 1156069.38 | 547945.19 / 732600.75 | 1123595.50 / 1282051.25 |
| c500 P16 d16 | 566572.25 / 574712.69 | 518134.72 / 549450.56 | n/a | n/a | 348432.06 / 526315.81 | 701754.38 / 724637.69 | 1092896.12 / 1052631.62 |
| c500 P64 d16 | 847457.62 / 854700.88 | 796812.81 / 813008.12 | n/a | n/a | 456621.00 / 847457.62 | 2666666.50 / 2702702.75 | 1851851.75 / 2222222.25 |
| c50 P16 d1024 | 1142857.12 / 995024.88 | 1030927.81 / 966183.56 | n/a | n/a | 468384.09 / 934579.44 | 787401.56 / 701754.38 | 956937.75 / 925925.88 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 943396.23 req/s, GET 1081081.08 req/s
