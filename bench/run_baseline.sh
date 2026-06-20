#!/bin/sh
# Record the M0 single-thread benchmark baseline as bench/baseline_<host>.csv.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${1:-"$ROOT/src/luajit"}
HOST=${BASELINE_HOST:-$(hostname | tr -c 'A-Za-z0-9_.-' '_')}
OUT=${BASELINE_OUT:-"$ROOT/bench/baseline_${HOST}.csv"}
BENCH_LUA=${BASELINE_BENCH_LUA:-"$ROOT/aux/bench/bench.lua"}

if [ ! -x "$BIN" ]; then
  echo "baseline: LuaJIT binary is not executable: $BIN" >&2
  exit 2
fi
if [ ! -f "$BENCH_LUA" ]; then
  echo "baseline: benchmark harness not found: $BENCH_LUA" >&2
  exit 2
fi

LUA_BIN=${BASELINE_LUA:-${LUA:-$BIN}}
jit_tmp=$(mktemp)
interp_tmp=$(mktemp)
trap 'rm -f "$jit_tmp" "$interp_tmp"' EXIT HUP INT TERM

"$BIN" "$BENCH_LUA" > "$jit_tmp"
"$BIN" -joff "$BENCH_LUA" > "$interp_tmp"

LUA_PATH="$ROOT/tests/lib/?.lua;$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$LUA_BIN" "$ROOT/bench/bench_csv_cli.lua" baseline-csv \
  "$jit_tmp" "$interp_tmp" > "$OUT"

echo "wrote $OUT"
