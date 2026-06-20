#!/bin/sh
# Compare two bench/run_baseline.sh CSVs with the M6/M9 JIT geomean gate.
set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: $0 <baseline.csv> <current.csv>" >&2
  exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE=$1
CUR=$2
LUA_BIN=${BENCH_LUA:-${LUA:-}}
if [ -z "$LUA_BIN" ]; then
  if [ -x "$ROOT/src/luajit" ]; then
    LUA_BIN=$ROOT/src/luajit
  elif command -v lua >/dev/null 2>&1; then
    LUA_BIN=lua
  else
    echo "baseline: no Lua interpreter found" >&2
    exit 2
  fi
fi

LUA_PATH="$ROOT/tests/lib/?.lua;$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$LUA_BIN" "$ROOT/bench/bench_csv_cli.lua" compare-csv "$BASE" "$CUR"
