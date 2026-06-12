#!/bin/sh
# Build and run M4 C unit drivers under ThreadSanitizer.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O1 -g -Wall -Wextra -mcx16 -fsanitize=thread"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
TMP=${TMPDIR:-/tmp}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

TSAN_OPTIONS=${TSAN_OPTIONS:-"halt_on_error=1 second_deadlock_stack=1"}
export TSAN_OPTIONS

THR_OUT="$TMP/lj_t-thr-substrate-tsan"
CHAN_OUT="$TMP/lj_t-chan-stress-tsan"

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-thr-substrate.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$THR_OUT"
"$THR_OUT"

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-chan-stress.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$CHAN_OUT"
"$CHAN_OUT"

echo "M4 TSAN driver tests passed"
