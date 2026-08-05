#!/bin/sh
# run_bench.sh - repeatable server benchmark for ddup (Windows Git Bash /
# POSIX). Starts the server, warms up, measures SET and GET, kills it.
#
# Usage: bench/run_bench.sh [port] [requests] [io_threads]
set -e
PORT="${1:-7777}"
N="${2:-100000}"
IO_THREADS="${3:-1}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRV="$DIR/build/ddup-server.exe"
BENCH="$DIR/build/ddup-bench.exe"
[ -x "$SRV" ] || SRV="$DIR/build/ddup-server"
[ -x "$BENCH" ] || BENCH="$DIR/build/ddup-bench"

EXTRA_ARGS=""
if [ "$IO_THREADS" -gt 1 ] 2>/dev/null; then
    EXTRA_ARGS="--io-threads $IO_THREADS"
fi

"$SRV" --port "$PORT" $EXTRA_ARGS >/tmp/ddup-bench-server.log 2>&1 &
SRV_PID=$!
trap 'kill $SRV_PID 2>/dev/null || true' EXIT
sleep 2

echo "== warmup (10k SET) =="
"$BENCH" -p "$PORT" -n 10000 -c 10 -P 16 -t set | tail -1
echo "== SET  (-n $N -c 50 -P 16) =="
"$BENCH" -p "$PORT" -n "$N" -c 50 -P 16 -t set | tail -1
echo "== GET  (-n $N -c 50 -P 16) =="
"$BENCH" -p "$PORT" -n "$N" -c 50 -P 16 -t get | tail -1

kill $SRV_PID 2>/dev/null || true
wait $SRV_PID 2>/dev/null || true
trap - EXIT
