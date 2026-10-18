# ddup vs Garnet vs Redis — loopback benchmark

- date (UTC): 2026-10-18T08:35:05Z
- runner: Linux runnervm8k3b6 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux, CPUs: 4
- ddup commit: 5c1e203
- garnet commit: 8777588
- Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=d064f9f48c901a92
- client: redis-benchmark -t set|get -n 200000, median of 3; cells are SET / GET
- ddup rows: st select (7771), st io_uring readiness (7775), st io_uring op repost (7776), st io_uring op multishot-recv (7777), mt --io-threads 4 (7774)

| scenario | ddup-st | ddup-st-uring | ddup-st-uring-op | ddup-st-uring-ms | ddup-mt4 | garnet | redis |
|----------|---------|---------------|------------------|------------------|----------|--------|-------|

| c50 P16 d16 | 1234567.88 / 1197604.88 | 1183432.00 / 1190476.25 | 1190476.25 / 0 | 0 / 0 | 701754.38 / 1036269.44 | 763358.81 / 666666.62 | 1092896.12 / 1212121.12 |
| c500 P16 d16 | 537634.38 / 566572.25 | 531914.94 / 512820.53 | n/a | n/a | 409836.06 / 617283.94 | 884955.75 / 900900.88 | 1149425.38 / 1290322.62 |
| c500 P64 d16 | 913242.00 / 952381.00 | 934579.44 / 961538.44 | n/a | n/a | 497512.44 / 904977.38 | 2631579.00 / 3030303.00 | 1886792.50 / 2380952.50 |
| c50 P16 d1024 | 1123595.50 / 938967.12 | 1015228.44 / 934579.44 | n/a | n/a | 460829.50 / 980392.19 | 819672.12 / 749063.69 | 966183.56 / 896860.94 |

mt4 mid-run restarts (wedge mitigation): 0

internal consistency (ddup-bench on ddup-st): SET 1092896.17 req/s, GET 1183431.95 req/s
