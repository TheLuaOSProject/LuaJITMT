#!/bin/sh
# run.sh — benchmark driver for LuaJIT-MT (13_testing_and_benchmarks.md).
# Usage:
#   ./run.sh baseline <luajit-binary>          # record single-thread CSVs
#   ./run.sh compare  <old-binary> <new-binary># geomean regression check
#   ./run.sh scaling  <luajit-mt-binary>       # 1/2/4/8-thread curves (M4+)
# Environment:
#   BENCH_SCALE=<factor>       reduce/expand iteration counts in Lua harnesses
#   BENCH_THREADS="1 2 4 8"    override scaling thread counts
#   BENCH_FILTER=<substring>   run matching bench_mt.lua cases only
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

bench_lua() {
  lua_bin=${BENCH_LUA:-${LUA:-}}
  if [ -z "$lua_bin" ]; then
    if [ -x "$ROOT/src/luajit" ]; then
      lua_bin=$ROOT/src/luajit
    elif command -v lua >/dev/null 2>&1; then
      lua_bin=lua
    else
      echo "benchmark: no Lua interpreter found" >&2
      exit 2
    fi
  fi
  LUA_PATH="$ROOT/tests/lib/?.lua;$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
    "$lua_bin" "$ROOT/bench/bench_csv_cli.lua" "$@"
}

bench_lua aux-run "$HERE" "$@"
