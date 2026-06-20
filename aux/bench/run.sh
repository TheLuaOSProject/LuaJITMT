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
mode=${1:?mode}; shift

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

bench_text() {
  bin=$1
  jitflag=$2
  filter=${BENCH_FILTER:-}
  if [ -n "$jitflag" ]; then
    if [ -n "$filter" ]; then
      "$bin" "$jitflag" "$HERE/bench.lua" "$filter"
    else
      "$bin" "$jitflag" "$HERE/bench.lua"
    fi
  else
    if [ -n "$filter" ]; then
      "$bin" "$HERE/bench.lua" "$filter"
    else
      "$bin" "$HERE/bench.lua"
    fi
  fi
}

case "$mode" in
baseline)
  bin=${1:?binary}
  jit_tmp=$(mktemp)
  interp_tmp=$(mktemp)
  trap 'rm -f "$jit_tmp" "$interp_tmp"' EXIT HUP INT TERM
  bench_text "$bin" "" > "$jit_tmp"
  bench_text "$bin" "-joff" > "$interp_tmp"
  bench_lua ns-csv "$jit_tmp" jit_ns_per_op \
    > "$HERE/baseline_jit_$(hostname).csv"
  bench_lua ns-csv "$interp_tmp" interp_ns_per_op \
    > "$HERE/baseline_interp_$(hostname).csv"
  echo "wrote baseline CSVs for $(hostname)"
  ;;
compare)
  old=${1:?old}; new=${2:?new}
  old_tmp=$(mktemp)
  new_tmp=$(mktemp)
  trap 'rm -f "$old_tmp" "$new_tmp"' EXIT HUP INT TERM
  bench_text "$old" "" > "$old_tmp"
  bench_text "$new" "" > "$new_tmp"
  bench_lua compare-bench-text "$old_tmp" "$new_tmp"
  ;;
scaling)
  bin=${1:?binary}
  threads=${BENCH_THREADS:-"1 2 4 8"}
  filter=${BENCH_FILTER:-}
  for n in $threads; do
    echo "== $n threads =="
    "$bin" "$HERE/bench_mt.lua" "$n" "$filter"
  done
  ;;
*) echo "unknown mode $mode" >&2; exit 2 ;;
esac
