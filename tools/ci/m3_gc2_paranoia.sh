#!/bin/sh
# Build with GC2 paranoia enabled and run the GC2 oracle tests.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
TMP=${TMPDIR:-/tmp}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" XCFLAGS="-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1" \
  -j"$JOBS" >/dev/null
for name in t-gc2-paranoia t-gc2-phase t-gc2-markbits; do
  out="$TMP/lj_${name}_paranoia"
  "$CC" $CFLAGS -DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1 -I"$ROOT/src" \
    "$ROOT/tests/$name.c" "$ROOT/src/libluajit.a" -lm -ldl -o "$out"
  "$out"
done
"$ROOT/tools/ci/run_stock_tests.sh" "$ROOT/src/luajit" --quiet

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" \
  XCFLAGS="-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1 -DLUAJIT_DISABLE_JIT" \
  -j"$JOBS" >/dev/null
"$ROOT/tools/ci/run_stock_tests.sh" "$ROOT/src/luajit" --quiet -jit
