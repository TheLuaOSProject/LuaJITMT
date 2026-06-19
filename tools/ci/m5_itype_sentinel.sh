#!/bin/sh
# Build and run M5 internal table sentinel TValue guards.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
OUT=${TMPDIR:-/tmp}/lj_t-itype-sentinel

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-itype-sentinel.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

for needle in \
  'LJ_LIGHTUD_INTERNAL_SEG' \
  'LJ_TFORWARD_BITS' \
  'LJ_TKEYLOCK_BITS' \
  'tvisforward' \
  'tviskeylock' \
  'tvistabinternal' \
  'setforwardV' \
  'setkeylockV'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_obj.h"; then
    echo "guardrail: missing internal table sentinel marker: $needle" >&2
    exit 1
  fi
done

echo "M5 internal table sentinel tag tests passed"
