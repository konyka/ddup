#!/bin/bash
# run_matrix.sh - Phase 27 benchmark matrix: single-thread vs mt, connection
# count, pipeline depth, value size, and a PING RTT/throughput ceiling.
#
# Usage: bench/run_matrix.sh [requests] [port_base]
# Prints one CSV line per measurement:
#   scenario,threads,clients,pipeline,dbytes,cmd,qps,p50_us,p99_us
set -u
N="${1:-100000}"
PB="${2:-7800}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRV="$DIR/build/ddup-server.exe"
BENCH="$DIR/build/ddup-bench.exe"
[ -x "$SRV" ] || SRV="$DIR/build/ddup-server"
[ -x "$BENCH" ] || BENCH="$DIR/build/ddup-bench"
[ -x "$SRV" ] || { echo "no server binary" >&2; exit 1; }
[ -x "$BENCH" ] || { echo "no bench binary" >&2; exit 1; }

# Windows Defender transiently quarantines fresh unsigned binaries in the
# build tree (execution fails with ENOENT while a scan runs), so run from
# a scratch copy; harmless on POSIX.
BIN=/tmp/ddup-matrix-bin
mkdir -p "$BIN"
cp "$SRV" "$BIN/server-bin"
cp "$BENCH" "$BIN/bench-bin"
SRV="$BIN/server-bin"
BENCH="$BIN/bench-bin"

PORT=$PB

# one_scenario name threads c P d cmd
one_scenario() {
    local name="$1" thr="$2" c="$3" P="$4" d="$5" cmd="$6"
    local extra="" out qps lat p50 p99
    if [ "$thr" -gt 1 ]; then extra="--io-threads $thr"; fi
    PORT=$((PORT + 1))
    "$SRV" --port "$PORT" $extra >/tmp/ddup-matrix-server.log 2>&1 &
    local pid=$!
    sleep 1
    if [ "$cmd" != "set" ]; then
        # populate the keyspace first (same key sequence as the GET run)
        "$BENCH" -p "$PORT" -n "$N" -c "$c" -P "$P" -t set -d "$d" >/dev/null 2>&1
    fi
    out="$("$BENCH" -p "$PORT" -n "$N" -c "$c" -P "$P" -t "$cmd" -d "$d" 2>&1)"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    qps="$(printf '%s\n' "$out" | grep 'requests per second' | awk '{print $1}')"
    lat="$(printf '%s\n' "$out" | grep 'latency (us)')"
    p50="$(printf '%s\n' "$lat" | sed -n 's/.*p50=\([0-9]*\).*/\1/p')"
    p99="$(printf '%s\n' "$lat" | sed -n 's/.*p99=\([0-9]*\).*/\1/p')"
    if [ -z "$qps" ]; then qps="FAILED"; printf '%s\n' "$out" >&2; fi
    echo "$name,$thr,$c,$P,$d,$cmd,$qps,$p50,$p99"
}

echo "scenario,threads,clients,pipeline,dbytes,cmd,qps,p50_us,p99_us"
#           name            thr c    P   d    cmd
for cmd in set get; do
  one_scenario base-st      1   50   16  16   "$cmd"
  one_scenario base-mt4     4   50   16  16   "$cmd"
  one_scenario base-mt8     8   50   16  16   "$cmd"
  one_scenario c500-st      1   500  16  16   "$cmd"
  one_scenario c500-mt4     4   500  16  16   "$cmd"
  one_scenario c1000-st     1   1000 16  16   "$cmd"
  one_scenario c1000-mt4    4   1000 16  16   "$cmd"
  one_scenario p1-st        1   50   1   16   "$cmd"
  one_scenario p1-mt4       4   50   1   16   "$cmd"
  one_scenario p64-st       1   50   64  16   "$cmd"
  one_scenario p64-mt4      4   50   64  16   "$cmd"
  one_scenario d1k-st       1   50   16  1024 "$cmd"
  one_scenario d1k-mt4      4   50   16  1024 "$cmd"
done
one_scenario ping-p1-st     1   50   1   16   ping
one_scenario ping-p1-mt4    4   50   1   16   ping
one_scenario ping-p64-st    1   50   64  16   ping
one_scenario ping-p64-mt4   4   50   64  16   ping
one_scenario ping-c1000-st  1   1000 16  16   ping
one_scenario ping-c1000-mt4 4   1000 16  16   ping
