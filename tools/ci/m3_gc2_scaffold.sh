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

for needle in \
  'uint64_t fixpoint_rounds' \
  'uint64_t fixpoint_hits' \
  'lj_gc2_fixpoint_round(global_State *g, lua_State *L' \
  'lj_gc2_fixpoint_run(global_State *g, lua_State *L' \
  'la_xchg64_acqrel(&g->gc2.marks_this_round, 0)' \
  'LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_FLUSH_SSB' \
  'lj_gc2_worker_drain(g' \
  'lj_gc2_ssb_empty(g)' \
  'lj_gc2_fixpoint_run(g, L, 64, ~(uint32_t)0)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c" \
      "$ROOT/src/lj_gc2.h" "$ROOT/src/lj_obj.h"; then
    echo "guardrail: missing GC2 fixpoint-round marker: $needle" >&2
    exit 1
  fi
done

for name in t-gc2-phase t-gc2-markbits t-gc2-traverse; do
  out="$TMP/lj_${name}"
  "$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/$name.c" \
    "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$out"
  "$out"
done

"$ROOT/tools/ci/m3_safepoint_handshake.sh"
"$ROOT/tools/ci/m3_vm_safepoint.sh"
"$ROOT/tools/ci/m3_gc2_paranoia.sh"
"$ROOT/tools/ci/m2_arena_all.sh"
"$ROOT/tools/ci/m2_gc_header_accessors.sh"

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null
make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" amalg -j"$JOBS" >/dev/null

"$ROOT/tools/ci/m0_matrix.sh"

echo "M3 GC2 scaffold tests passed"
