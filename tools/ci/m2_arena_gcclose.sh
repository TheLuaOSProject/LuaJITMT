#!/bin/sh
# Build with internal assertions and run lua_close proto/closure churn.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
OUT=${TMPDIR:-/tmp}/lj_t_arena_gcclose

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" XCFLAGS="-DLUA_USE_ASSERT" -j"$JOBS" >/dev/null
"$CC" $CFLAGS -DLUA_USE_ASSERT -I"$ROOT/src" \
  "$ROOT/tests/t-arena-gcclose.c" "$ROOT/src/libluajit.a" -lm -ldl -o "$OUT"
"$OUT"
