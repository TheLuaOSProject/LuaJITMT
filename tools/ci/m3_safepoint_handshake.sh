#!/bin/sh
# Build and run the focused C-level safepoint handshake test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
OUT=${TMPDIR:-/tmp}/lj_t_safepoint_handshake

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-safepoint-handshake.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -o "$OUT"
"$OUT"

for needle in \
  'TGState *self = lj_thr_get_tg()' \
  'Leader self-ack is a real poll' \
  'remote native ack' \
  'lj_gc2_reclaim_retired(g, epoch)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_safepoint.c"; then
    echo "guardrail: missing safepoint handshake marker: $needle" >&2
    exit 1
  fi
done

if rg -F -q 'Deterministic single-mutator scaffold' "$ROOT/src/lj_safepoint.c"; then
  echo "guardrail: non-native TGs must not be remotely acked" >&2
  exit 1
fi

echo "M3 safepoint handshake tests passed"
