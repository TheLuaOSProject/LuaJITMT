#!/bin/sh
# Record the M0 single-thread benchmark baseline as bench/baseline_<host>.csv.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${1:-"$ROOT/src/luajit"}
HOST=${BASELINE_HOST:-$(hostname | tr -c 'A-Za-z0-9_.-' '_')}
OUT=${BASELINE_OUT:-"$ROOT/bench/baseline_${HOST}.csv"}
BENCH_LUA=${BASELINE_BENCH_LUA:-"$ROOT/plan/aux/bench/bench.lua"}

if [ ! -x "$BIN" ]; then
  echo "baseline: LuaJIT binary is not executable: $BIN" >&2
  exit 2
fi
if [ ! -f "$BENCH_LUA" ]; then
  echo "baseline: benchmark harness not found: $BENCH_LUA" >&2
  exit 2
fi

jit_tmp=$(mktemp)
interp_tmp=$(mktemp)
trap 'rm -f "$jit_tmp" "$interp_tmp"' EXIT HUP INT TERM

"$BIN" "$BENCH_LUA" > "$jit_tmp"
"$BIN" -joff "$BENCH_LUA" > "$interp_tmp"

awk '
  FILENAME == ARGV[1] && FNR > 1 {
    order[++n] = $1
    jit_total[$1] = $2
    jit_ns[$1] = $3
    next
  }
  FILENAME == ARGV[2] && FNR > 1 {
    interp_total[$1] = $2
    interp_ns[$1] = $3
    next
  }
  END {
    print "benchmark,jit_total_s,jit_ns_per_op,interp_total_s,interp_ns_per_op"
    for (i = 1; i <= n; i++) {
      name = order[i]
      printf "%s,%s,%s,%s,%s\n", name, jit_total[name], jit_ns[name], interp_total[name], interp_ns[name]
    }
  }
' "$jit_tmp" "$interp_tmp" > "$OUT"

echo "wrote $OUT"
