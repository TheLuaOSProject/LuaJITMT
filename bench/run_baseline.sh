#!/bin/sh
# Record the M0 single-thread benchmark baseline as bench/baseline_<host>.csv.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${1:-"$ROOT/src/luajit"}
HOST=${BASELINE_HOST:-$(hostname | tr -c 'A-Za-z0-9_.-' '_')}
OUT=${BASELINE_OUT:-"$ROOT/bench/baseline_${HOST}.csv"}
BENCH_LUA=${BASELINE_BENCH_LUA:-"$ROOT/aux/bench/bench.lua"}
LUA_BIN=${BASELINE_LUA:-${LUA:-$BIN}}

LUA_PATH="$ROOT/tests/lib/?.lua;$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$LUA_BIN" "$ROOT/bench/bench_csv_cli.lua" run-baseline \
  "$BIN" "$BENCH_LUA" "$OUT"
