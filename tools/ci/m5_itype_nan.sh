#!/bin/sh
# Build and run M5 NaN TValue tag guards.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
OUT=${TMPDIR:-/tmp}/lj_t-itype-nan

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-itype-nan.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

echo "M5 NaN tag tests passed"
