#!/bin/sh
# Guard canonical M9 benchmark CSV/geomean accounting.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BASE="$ROOT/bench/baseline_372b369b9afd_.csv"
TMP=${TMPDIR:-/tmp}
CUR=$(mktemp "$TMP/lj-bench-current.XXXXXX")
BAD=$(mktemp "$TMP/lj-bench-bad.XXXXXX")
trap 'rm -f "$CUR" "$BAD"' EXIT HUP INT TERM

make -C "$ROOT/src" >/dev/null

for needle in \
  'BENCH_LUA=${BASELINE_BENCH_LUA:-"$ROOT/aux/bench/bench.lua"}' \
  'BENCH_SCALE' \
  'BENCH_GC_MODE' \
  'COLUMN=${BENCH_COLUMN:-jit_ns_per_op}' \
  'MAX=${BENCH_GEOMEAN_MAX:-1.10}' \
  'geomean' \
  'PASS: geomean' \
  'FAIL: geomean'
do
  if ! rg -F -q "$needle" "$ROOT/bench/run_baseline.sh" \
      "$ROOT/bench/compare_baseline.sh" "$ROOT/aux/bench/bench.lua"; then
    echo "guardrail: missing M9 benchmark accounting marker: $needle" >&2
    exit 1
  fi
done

if ! rg -F -q 'm9_bench_regression.sh' "$ROOT/tools/ci/m9_m10_gc.sh"; then
  echo "guardrail: m9_bench_regression.sh is not wired into the M9/M10 aggregate" >&2
  exit 1
fi

"$ROOT/bench/compare_baseline.sh" "$BASE" "$BASE" |
  rg -F 'PASS: geomean 1.000000 <= 1.100000' >/dev/null

awk -F, 'BEGIN { OFS="," }
  NR == 1 { print; next }
  { $3 = sprintf("%.2f", $3 * 2) }
  { print }
' "$BASE" > "$BAD"
if "$ROOT/bench/compare_baseline.sh" "$BASE" "$BAD" >/dev/null 2>&1; then
  echo "guardrail: benchmark regression checker accepted a known bad CSV" >&2
  exit 1
fi

BENCH_SCALE=0.001 BASELINE_OUT="$CUR" "$ROOT/bench/run_baseline.sh" "$ROOT/src/luajit" >/dev/null
"$ROOT/bench/compare_baseline.sh" "$CUR" "$CUR" |
  rg -F 'PASS: geomean 1.000000 <= 1.100000' >/dev/null

echo "M9 benchmark regression accounting guard passed"
