#!/bin/sh
# Build and run the runtime traversable arena sweep bridge test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
OUT=${TMPDIR:-/tmp}/lj_t_arena_gcsweep

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-arena-gcsweep.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -o "$OUT"
"$OUT"

for needle in \
  'gc_arena_sweep_pending(global_State *g)' \
  'gc_arena_finish_sweep_boundary(global_State *g, int drain)' \
  'lj_gc2_sweep_owner_progress(g, tg, LJ_GC2_SWEEP_BATCH)' \
  '05 section 5.8 boundary-lazy traversable sweep bridge' \
  'assert(g->gc.state == GCSsweep)' \
  'assert(delta <= LJ_GC2_SWEEP_BATCH)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc.c" "$ROOT/tests/t-arena-gcsweep.c"; then
    echo "guardrail: missing arena GC sweep marker: $needle" >&2
    exit 1
  fi
done
