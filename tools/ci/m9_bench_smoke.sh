#!/bin/sh
# Fast M9 benchmark harness smoke. Not a performance gate.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

make -C "$ROOT/src" >/dev/null

for needle in \
  'LJLIB_CF(threading_now)' \
  'CLOCK_MONOTONIC' \
  'local wall = assert(th.now, "bench_mt.lua requires threading.now()")' \
  'local scale = tonumber(getenv("BENCH_SCALE")) or 1' \
  'if dt <= 0 then dt = 1e-9 end' \
  'requires an even thread count >= 2' \
  'BENCH_THREADS="1 2 4 8"' \
  'BENCH_FILTER=<substring>'
do
  if ! rg -F -q "$needle" "$ROOT/src/lib_threading.c" \
      "$ROOT/plan/aux/bench/bench_mt.lua" \
      "$ROOT/plan/aux/bench/run.sh"; then
    echo "guardrail: missing M9 benchmark marker: $needle" >&2
    exit 1
  fi
done

if ! rg -F -q 'm9_bench_smoke.sh' "$ROOT/tools/ci/m9_m10_gc.sh"; then
  echo "guardrail: m9_bench_smoke.sh is not wired into the M9/M10 aggregate" >&2
  exit 1
fi

if rg -n 'os[.]clock' "$ROOT/plan/aux/bench/bench_mt.lua"; then
  echo "guardrail: bench_mt.lua must use monotonic wall time, not CPU time" >&2
  exit 1
fi

"$ROOT/src/luajit" "$ROOT/tests/t-threading-api.lua"

BENCH_SCALE=0.0001 "$ROOT/src/luajit" \
  "$ROOT/plan/aux/bench/bench_mt.lua" 1 chan_pingpong |
  rg -F 'chan_pingpong' |
  rg -F 'skipped: requires an even thread count >= 2' >/dev/null

BENCH_SCALE=0.0001 "$ROOT/src/luajit" \
  "$ROOT/plan/aux/bench/bench_mt.lua" 2 chan_pingpong |
  rg -F 'chan_pingpong' |
  rg -F 'ops/s' >/dev/null

BENCH_SCALE=0.0001 BENCH_THREADS="1 2" BENCH_FILTER=arith-MT \
  "$ROOT/plan/aux/bench/run.sh" scaling "$ROOT/src/luajit" |
  rg -F 'GC stats:' >/dev/null

echo "M9 benchmark smoke guard passed"
