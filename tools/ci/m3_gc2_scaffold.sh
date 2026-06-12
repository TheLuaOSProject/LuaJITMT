#!/bin/sh
# Run the focused M3 GC2 scaffold tests and dependent regression gates.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
TMP=${TMPDIR:-/tmp}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

for name in t-gc2-phase t-gc2-markbits; do
  out="$TMP/lj_${name}"
  "$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/$name.c" \
    "$ROOT/src/libluajit.a" -lm -ldl -o "$out"
  "$out"
done

"$ROOT/tools/ci/m3_gc2_paranoia.sh"
"$ROOT/tools/ci/m2_arena_all.sh"
"$ROOT/tools/ci/m2_gc_header_accessors.sh"

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null
make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" amalg -j"$JOBS" >/dev/null

"$ROOT/tools/ci/m0_matrix.sh"

echo "M3 GC2 scaffold tests passed"
