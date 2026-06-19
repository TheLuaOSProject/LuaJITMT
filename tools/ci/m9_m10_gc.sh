#!/bin/sh
# Run the current M9/M10 GC telemetry and generational gates.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

"$ROOT/tools/ci/m9_gc_stats.sh"
"$ROOT/tools/ci/m9_bench_smoke.sh"
"$ROOT/tools/ci/m9_bench_regression.sh"
"$ROOT/tools/ci/m10_generational.sh"

echo "M9/M10 GC gates passed"
