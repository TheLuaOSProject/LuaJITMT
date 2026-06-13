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
  'gc_arena_sweep_tg_ready(TGState *tg)' \
  'gc_arena_finish_sweep_boundary(global_State *g, int drain)' \
  'la_loadptr_acq((void *const *)&g->gc2.tg_list)' \
  'lj_gc2_worker_drain_progress(g, LJ_GC2_SWEEP_BATCH)' \
  '05 section 5.6.3 worker-owned sweep bridge' \
  'assert(lj_gc2_worker_drain(g, 1) == 1u)' \
  'assert(lj_gc2_worker_drain(g, 1) == 0)' \
  'worker_runs0 = la_load64_acq(&g->gc2.worker_runs)' \
  'lj_arena_alloc_restore_sweep_kind(&extra_tg.alloc, LJ_ARENAK_PLAIN)' \
  'lj_gc2_handshake(g, LJ_GC2_HS_RESET_ALLOC)' \
  'seed_traversable_needsweep(&extra_tg, seeded)' \
  'assert(g->gc.state == GCSsweep)' \
  'assert(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE] != NULL)' \
  'arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_PLAIN]' \
  'arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_TRAVERSABLE]' \
  'assert(extra_trav_a->hdr.sweep_epoch == sweep_cycle)' \
  'assert(delta <= LJ_GC2_SWEEP_BATCH)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc.c" "$ROOT/tests/t-arena-gcsweep.c"; then
    echo "guardrail: missing arena GC sweep marker: $needle" >&2
    exit 1
  fi
done
