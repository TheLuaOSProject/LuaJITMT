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
  'GCRef *weak_stack' \
  'uint8_t *weak_ready' \
  'uint64_t weak_count' \
  'uint64_t weak_tables_seen' \
  'uint64_t weak_tables_weakkey' \
  'uint64_t weak_tables_weakval' \
  'uint64_t weak_tables_allweak' \
  'uint64_t weak_tables_queued' \
  'uint64_t weak_tables_overflow' \
  'uint64_t weak_scan_cursor' \
  'uint64_t weak_scan_runs' \
  'uint64_t weak_scan_tables' \
  'uint64_t weak_scan_slots' \
  'uint64_t weak_scan_clearable' \
  'uint64_t weak_clear_cursor' \
  'uint64_t weak_clear_runs' \
  'uint64_t weak_clear_tables' \
  'uint64_t weak_clear_slots' \
  'uint64_t weak_clear_cleared' \
  'uint64_t worker_weak_drained' \
  'uint64_t sweep_owner_runs' \
  'uint64_t sweep_owner_arenas' \
  'uint64_t sweep_owner_live_cells' \
  'LJ_GC2_WEAK_DRAIN_BATCH' \
  'LJ_GC2_SWEEP_BATCH' \
  'uint64_t finreg_cdata_sets' \
  'uint64_t finreg_cdata_clears' \
  'uint64_t finreg_cdata_queued' \
  'uint64_t finreg_udata_sets' \
  'uint64_t finreg_udata_clears' \
  'uint64_t finreg_udata_queued' \
  'uint64_t weak_keys_marked' \
  'uint64_t weak_values_marked' \
  'gc2_weak_record(global_State *g, GCtab *t)' \
  'gc2_weak_next_capacity(MSize cap, uint64_t need)' \
  '05 section 5.8 adaptive weak snapshot' \
  'lj_gc2_weak_snapshot_count(global_State *g)' \
  'lj_gc2_weak_snapshot_tab(global_State *g' \
  'lj_gc2_weak_snapshot_scan(global_State *g, uint32_t limit)' \
  'lj_gc2_weak_snapshot_clear(global_State *g, uint32_t limit)' \
  'lj_gc2_weak_drain(global_State *g, uint32_t limit)' \
  'la_store8_rel(&g->gc2.weak_ready' \
  'la_load8_acq(&g->gc2.weak_ready' \
  'la_cas64(&g->gc2.weak_scan_cursor' \
  'la_cas64(&g->gc2.weak_clear_cursor' \
  'lj_gc2_finreg_cdata_set(global_State *g, GCobj *o, int enabled)' \
  'gc2_finreg_queue_mark(global_State *g, GCobj *o)' \
  '05 section 5.8 FINREG resurrection' \
  'lj_gc2_finreg_cdata_queue(global_State *g, GCobj *o)' \
  'lj_gc2_finreg_cdata_set(G(L), obj2gco(cd), 1)' \
  'lj_gc2_finreg_cdata_set(G(L), obj2gco(cd), 0)' \
  'lj_gc2_finreg_cdata_set(g, o, 0)' \
  'lj_gc2_finreg_cdata_queue(g, obj2gco(cd))' \
  'lj_gc2_finreg_udata_set(global_State *g, GCobj *o, int enabled)' \
  'lj_gc2_finreg_udata_queue(global_State *g, GCobj *o)' \
  'lj_gc2_finreg_udata_set(g, obj2gco(ud), 1)' \
  'lj_gc2_finreg_udata_set(g, obj2gco(ud), 0)' \
  'lj_gc2_finreg_udata_queue(g, o)' \
  'gc2_weak_mayclear(global_State *g, cTValue *o, int val,' \
  'int markstr)' \
  'gc2_tab_is_ffi_fin(global_State *g, GCtab *t)' \
  'FFI finalizer registry is owned by FINREG' \
  'lj_gc2_markobj(g, gcV(o))' \
  'gc2_note_weak_table(global_State *g, GCtab *t, int weak)' \
  'lj_gc2_barrier_tvn_g(global_State *g, cTValue *tv' \
  'lj_gc2_legacy_weak_begin(global_State *g)' \
  'lj_gc2_barrier_weak_key(lua_State *L, GCtab *t' \
  'lj_gc2_barrier_weak_write(lua_State *L, GCtab *t' \
  'gc2_tab_weak_mode(global_State *g, GCtab *t' \
  'lj_gc2_fixpoint_round(global_State *g, lua_State *L' \
  'lj_gc2_fixpoint_run(global_State *g, lua_State *L' \
  'la_xchg64_acqrel(&g->gc2.marks_this_round, 0)' \
  'LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_FLUSH_SSB' \
  'lj_gc2_worker_drain_progress(g, LJ_GC2_WEAK_DRAIN_BATCH)' \
  '05 section 5.8 worker-owned weak-drain bridge' \
  'lj_gc2_worker_drain(g' \
  'lj_gc2_worker_drain_progress(global_State *g, uint32_t limit)' \
  'weak = lj_gc2_weak_drain(g, limit - work)' \
  'la_add64_rlx(&g->gc2.worker_weak_drained' \
  'lj_gc2_sweep_owner_progress(global_State *g, TGState *tg' \
  'la_add64_rlx(&g->gc2.sweep_owner_arenas' \
  '05 section 5.8 bounded traversable sweep bridge' \
  'lj_gc2_ssb_empty(g)' \
  'la_loadptr_acq((void *const *)&tg->ssb_next)' \
  'la_storeptr_rel((void **)&tg->ssb_next' \
  'lj_gc2_fixpoint_run(g, L, 64, ~(uint32_t)0)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc.h" \
      "$ROOT/src/lj_gc2.c" "$ROOT/src/lj_gc2.h" "$ROOT/src/lj_obj.h" \
      "$ROOT/src/lj_tab.c" "$ROOT/src/lj_cdata.c" "$ROOT/src/lib_ffi.c" \
      "$ROOT/src/lj_api.c"; then
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
