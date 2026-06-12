#!/bin/sh
# Run all focused M2 arena scaffold tests.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

"$ROOT/tools/ci/m2_arena_bitmap.sh"
"$ROOT/tools/ci/m2_arena_map.sh"
"$ROOT/tools/ci/m2_arena_alloc.sh"
"$ROOT/tools/ci/m2_arena_hugetab.sh"
"$ROOT/tools/ci/m2_arena_sweep.sh"
"$ROOT/tools/ci/m2_arena_state.sh"
"$ROOT/tools/ci/m2_arena_gcmark.sh"
"$ROOT/tools/ci/m2_arena_gcverify.sh"
"$ROOT/tools/ci/m2_arena_gcsweep.sh"
"$ROOT/tools/ci/m2_arena_gcphase.sh"

echo "M2 arena focused tests passed"
