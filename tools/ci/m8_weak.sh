#!/bin/sh
# Run the current M8 weak-table/finalizer semantic gates.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
TMP=${TMPDIR:-/tmp}

for needle in \
  'gc2_weak_mayclear(global_State *g, cTValue *o, int val,' \
  'g->gc.state == GCSatomic && iswhite(gcV(o))' \
  'lj_gc2_barrier_weak_key(lua_State *L, GCtab *t' \
  'lj_gc2_barrier_weak_write(lua_State *L, GCtab *t' \
  'gc2_tab_weak_barrier_mode(global_State *g, GCtab *t)' \
  'use captured P_WEAK mode' \
  'test_weak_key_write_barrier' \
  'test_vm_weak_key_write_barrier' \
  'test_peer_weak_key_write_barrier' \
  'Peer TG performs the P_WEAK table write' \
  'test_jit_weak_table_store_helper_barrier' \
  'existing weak table stores trace through a P_WEAK-aware helper' \
  'test_jit_weak_array_store_helper_barrier' \
  'existing weak-value array stores trace through the array helper' \
  'test_vm_weak_value_hash_key_barrier' \
  'test_vm_weak_value_array_barrier' \
  'test_table_insert_weak_value_array_barrier' \
  'table.insert weak-value array write' \
  'lj_meta_tset_owner(lua_State *L, cTValue *o, cTValue *k,' \
  'test_capi_weak_newindex_target_write_barrier' \
  'lj_meta_tset_owner(L, tv, base+1, &owner)' \
  'test_ffi_weak_newindex_target_write_barrier' \
  't-m8-ffi-weak-newindex OK' \
  'ffi_register_module(lua_State *L)' \
  'lj_gc2_barrier_weak_write(L, t, &key, L->top-1);' \
  'test_ffi_loaded_weak_value_barrier' \
  'lib_weak_write_str(lua_State *L, GCtab *tab' \
  'test_lib_register_weak_value_barrier' \
  'test_jit_profile_registry_weak_barrier' \
  'lj_gc2_barrier_weak_write(L, registry, &key, &tv);' \
  'test_weak_clear_marks_string_slots' \
  'lj_obj_cleargcflags_atomic(gcV(o), LJ_GC_WHITES);' \
  'assert(!iswhite(obj2gco(modestr)));' \
  'test_weak_drain_uses_captured_mode' \
  'test_weak_pre_clear_late_write_survives_drain' \
  'P_WEAK late write before weak drain' \
  'GC2 late weak write mark wins over legacy white' \
  'test_weak_post_clear_resurrection_write' \
  'test_vm_weak_post_clear_existing_key_write' \
  'weak-kv table kept a one-cycle hash entry' \
  'test_capi_rawset_weak_write_barrier' \
  'C API raw hash weak write' \
  'C API raw array weak write' \
  'lj_gc2_barrier_weak_key(L, t, k);' \
  'weak-table key write' \
  'test_weak_complete_bridge' \
  'lj_gc2_weak_complete(g, gcref(g->gc.weak), 1)' \
  'gc2_weak_backfill_legacy(global_State *g, GCobj *legacy)' \
  'weak_legacy_backfills' \
  'weak_legacy_backfill_tables' \
  'int weak = lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAK' \
  'capture traversal-time weak mode' \
  'gc2_weak_paranoia_zero_diff(global_State *g, GCobj *legacy)' \
  'lj_gc2_finalizer_try_enter(global_State *g)' \
  'peer finalizer dispatch backs off' \
  'void *finalizer_tail' \
  'lj_gc_cdata_fin_pending(global_State *g)' \
  '!lj_gc_cdata_fin_pending(g)' \
  'gc_cdata_finalizer_candidate_close(gcV(&key))' \
  'gc_queue_cdata_finalizer(global_State *g, GCobj *o)' \
  'gc_cdata_finalizer_candidate_close(GCobj *o)' \
  '!(lj_obj_gcflags(o) & LJ_GC_FINALIZED)' \
  'gc_separate_cdata_finalizers(global_State *g)' \
  'lj_gc_finalize_cdata_disable(global_State *g)' \
  'm8_close_chain_cdata' \
  'cdata_finalized == 4' \
  'ALTERNATING_CLOSE_CHAIN' \
  'm8_close_chain_alternating_cdata' \
  'alternating_cdata_finalized == ALTERNATING_CLOSE_CHAIN' \
  'CLOSE_FINREG_GROWTH_BATCH' \
  'm8_close_register_growth_cdata' \
  'growth_cdata_finalized == CLOSE_FINREG_GROWTH_BATCH' \
  'close_error_cdata_finalizer' \
  'close_error_udata_finalizer' \
  'error_cdata_finalized == 1' \
  'after_error_cdata_finalized == 1' \
  'error_udata_finalized == 1' \
  'after_error_udata_finalized == 1' \
  'close_clear_suppressed_cdata_finalizer' \
  'm8_close_clear_suppressed_cdata' \
  'ffi.gc(keep.suppressed, nil)' \
  'suppressed_cdata_finalized == 0' \
  'close_nested_gc_cdata_finalizer' \
  'close_nested_gc_udata_finalizer' \
  'run_nested_collect' \
  'nested_gc_cdata_finalized == 1' \
  'nested_gc_udata_finalized == 1' \
  'm8_close_probe_shutdown_spawn' \
  'close_shutdown_spawn_cdata_finalizer' \
  'close_shutdown_spawn_udata_finalizer' \
  'VM shutdown in progress' \
  'shutdown_spawn_cdata_finalized == 1' \
  'shutdown_spawn_udata_finalized == 1' \
  'threading_rehome_unstarted_stack(lua_State *L, lua_State *L1,' \
  'if (la_load32_acq(&G(L)->mt_shutdown) != 0)' \
  'threading_rehome_unstarted_stack(L, L1, tg);' \
  'threading_thread___gc' \
  'threading_tg_is_registered(global_State *g, TGState *target)' \
  'lj_tg_fini_thread(g, th->tg);' \
  'lj_gc2_finreg_udata_register(L, g, obj2gco(ud));' \
  'order_cdata_finalized[0] == 3' \
  'order_cdata_finalized[1] == 2' \
  'order_cdata_finalized[2] == 1' \
  'lua_close drains alternating cdata/userdata finalizers to fixed point' \
  'lj_gc2_finalizer_queue_pending(global_State *g)' \
  'lj_gc2_finalizer_pending(global_State *g)' \
  'lj_gc2_finalizer_sweep_pending(global_State *g)' \
  'lj_gc2_finalizer_enqueue(global_State *g, GCobj *o)' \
  'lj_gc2_finalizer_drain(global_State *g)' \
  'lj_gc2_finalizer_dequeue(global_State *g)' \
  'void *finalizer_mpsc' \
  'uint64_t finalizer_queued' \
  'uint64_t finalizer_dequeued' \
  'uint64_t finalizer_mpsc_drained' \
  'la_casptr((void **)&g->gc2.finalizer_mpsc' \
  'la_xchgptr_acqrel((void **)&g->gc2.finalizer_mpsc' \
  'la_storeptr_rel((void **)&g->gc2.finalizer_tail' \
  'gc_mark_finalizers(global_State *g)' \
  'gc2_mark_finalizer_stack(global_State *g, GCobj *o)' \
  'gc2_mark_finalizer_ring(global_State *g, GCobj *tail)' \
  'g->gc2.finalizer_mpsc));' \
  'g->gc2.finalizer_tail));' \
  'test_finalizer_consumer_ring' \
  'la_loadptr_acq((void *const *)&g->gc2.finalizer_tail) != NULL' \
  'assert(lj_gc2_finalizer_dequeue(g) == a)' \
  'runtime finalizer queues must not use legacy mmudata' \
  'finalizer_queued0 = la_load64_acq(&g->gc2.finalizer_queued)' \
  'finalizer_dequeued0 = la_load64_acq(&g->gc2.finalizer_dequeued)' \
  'assert(!lj_gc2_finalizer_queue_pending(g))' \
  'assert(lj_gc2_finalizer_queue_pending(g))' \
  'assert(la_load32_acq(&g->gc2.finalizer_owner_tid) ==' \
  'finalizer_enters0 + 1u' \
  'finalizer_leaves0 + 1u' \
  'gc_finalizer_mt_release_exclusive(global_State *g)' \
  'gc_finalizer_mt_reclaim_exclusive(global_State *g)' \
  'gc_finalizer_restore_threshold(global_State *g, GCSize oldt)' \
  'gc_fullgc_deferred_by_finalizer(global_State *g)' \
  'uint64_t finalizer_spawn_deferrals' \
  'finalizer_spawn_deferrals, 0' \
  'test_finalizer_spawn_deferred_state' \
  'gc2_spawn_defer_t' \
  'finalizer_spawn_deferrals) > deferrals0' \
  'finrc <= 0' \
  'Keep GCSfinalize open until spawned TG exits.' \
  'finalizer-spawn outlived callback' \
  'oldt = la_load32_acq(&g->mt_live) != 0 ? lj_gc_mt_threshold_load(g) :' \
  'lj_gc_mt_threshold_store(g, oldt)' \
  'threshold == LJ_MAX_MEM && g->gc.state == GCSfinalize' \
  'collectgarbage("step", 1000000)' \
  'newproxy(true)' \
  'userdata explicit step' \
  'timeout 10s "$ROOT/src/luajit" "$ROOT/tests/t-m8-finalizer-spawn-live.lua"' \
  'GC step completed while finalizer-spawned worker was live' \
  'finalizer-spawned worker can outlive callback' \
  'worker finalizer notification timed out' \
  'worker finalizer fired more times than registered' \
  'finalized_by_thread[tid] == expected_by_thread[tid]' \
  'gc_finalize_cdata_slot_owned(lua_State *L, GCobj *o, cTValue *key)' \
  'lj_state_checkstack(cbL, 2+LJ_FR2+LUA_MINSTACK);' \
  'oldtop = savestack(cbL, cbL->top);' \
  'cbL->top = restorestack(cbL, oldtop);' \
  'lj_ctype_fin_get(L, cts, key, &t)' \
  'gc_queue_cdata_finalizers_pweak(lua_State *L, global_State *g)' \
  'gc_preclaim_cdata_finalizers_pweak_finreg(lua_State *L,' \
  'FINREG P_WEAK cdata scan' \
  'gc_separate_cdata_finalizers_ordered(global_State *g,' \
  'FINREG ordered close-time cdata scan' \
  'typedef struct FinRegOrderNode' \
  'fin_order_head' \
  'FINREG ordered P_WEAK cdata scan' \
  'root unlink after ordered FINREG claim' \
  'queue claimed cdata in FINREG order' \
  'ordered FINREG identity is enough for P_WEAK' \
  'ordered FINREG identity is enough for close-time' \
  'gc2_rootless_order_fin_t' \
  'gc2_close_rootless_order_fin_t' \
  'uint64_t finreg_cdata_order_queued' \
  'uint64_t finreg_cdata_order_fallbacks' \
  'uint64_t finreg_cdata_pweak_root_fallbacks' \
  'uint64_t finreg_cdata_close_root_fallbacks' \
  'uint64_t finreg_cdata_pending_order_hits' \
  'gc_cdata_fin_pending_ordered(global_State *g, CTState *cts)' \
  'FINREG ordered close-time cdata pending scan' \
  'finreg_cdata_pending_order_hits, 0' \
  'ordered_fallback && ordered_queued == 0' \
  'if (!ordered_fallback)' \
  'gc_cdata_finreg_pending_scan(CTState *cts)' \
  'collectgarbage('\''collect'\'')' \
  'finreg_cdata_close_root_fallbacks, 1' \
  'gc_claim_cdata_finalizer_pweak(lua_State *L, global_State *g,' \
  'GCRef obj;' \
  'setgcref(ord->obj, o);' \
  'gc_order_cdata_object(FinRegOrderNode *ord, GCtab *t,' \
  'gc_marktv(g, &fin);' \
  'lj_gc2_finreg_cdata_preclaim(L, g, o, &fin)' \
  'gc_finalize_cdata_claim_preclaimed(global_State *g, GCobj *o)' \
  'P_WEAK preclaim suppressed by later ffi.gc(cd, nil)' \
  'gc2_preclaim_clear_fin_t' \
  'gc2_finclaim_next_capacity(cap, count + 1u)' \
  'lj_gc2_finreg_cdata_preclaim_take(L, g, o, &fin)' \
  'dispatch order may differ from FINREG scan' \
  'gc_mark_finreg_cdata_preclaims(global_State *g)' \
  'gc2_mark_finreg_cdata_preclaims(global_State *g)' \
  'P_WEAK cdata finalizer discovery bridge' \
  'uint64_t finreg_cdata_pweak_queued' \
  'uint64_t finreg_cdata_sweep_queued' \
  'la_add64_rlx(&g->gc2.finreg_cdata_sweep_queued, 1)' \
  'uint64_t finreg_cdata_pweak_claimed' \
  'uint64_t finreg_cdata_preclaim_dispatched' \
  'finreg_cdata_pweak_queued) == pweak1 + 1u' \
  'finreg_cdata_pweak_claimed) == claimed1 + 1u' \
  'finreg_cdata_sweep_queued) ==' \
  'finreg_cdata_pweak_queued) == pweak2 + 3u' \
  'finreg_cdata_pweak_claimed) == claimed2 + 3u' \
  'finreg_cdata_order_queued) == orderq2 + 3u' \
  'finreg_cdata_order_fallbacks) ==' \
  'finreg_cdata_pweak_root_fallbacks) ==' \
  'finreg_cdata_close_root_fallbacks) ==' \
  'finreg_cdata_pending_order_hits) == pendingorder2 + 1u' \
  'gc2_regorder_fin_t' \
  'gc2_cdata_order[1] == 1' \
  'gc2_rereg_fin_t' \
  'gc2_close_order_fin_t' \
  'lj_gc_finalize_cdata(L)' \
  'finreg_cdata_order_queued) == orderq2 + 2u' \
  'gc2_cdata_order[0] == 3' \
  'gc2_cdata_order[1] == 2' \
  'gc2_cdata_order[2] == 1' \
  'finalizer_dequeued) == finalizerd1 + 1u' \
  'finalizer_mpsc_drained) == mpscd1 + 1u' \
  'finalizer_mpsc_drained) == mpscd2 + 3u' \
  'finalizer_mpsc_drained) == mpscd2 + 2u' \
  'finreg_cdata_preclaim_dispatched) ==' \
  'gc2_cdata_bulk_finalizer' \
  'gc2_bulk_fin_t' \
  'finreg_cdata_preclaim_overflow) == overflow2' \
  'finreg_cdata_preclaim_capacity >= (MSize)bulk_n' \
  'gc2_cdata_finalized == finalized0 + 1' \
  'LJ_GC_UDATA_FINREG == LJ_GC_WEAKVAL' \
  'old | LJ_GC_UDATA_FINREG' \
  'void *finreg_udata_head' \
  'lj_gc2_finreg_udata_register_mt(lua_State *L, global_State *g,' \
  'lj_gc2_finreg_udata_register_mt(L, G(L), ud' \
  'debug.getmetatable(m).__gc = gc2_internal_udata_finalizer' \
  'immediate + lazy' \
  'ffi.C default namespace' \
  'lj_gc2_finreg_udata_register(lua_State *L, global_State *g,' \
  'GC2-owned userdata FINREG discovery' \
  'GC2 userdata FINREG identity is enough' \
  'test_unlink_udata_object' \
  'Side-list no-finalizer userdata is done' \
  'return gc_separateudata_registered(g, all);' \
  'lj_gc2_finreg_udata_set(g, o, 1);' \
  'lj_gc2_finreg_udata_set(g, o, 0);' \
  'test_finreg_internal_userdata_telemetry' \
  'finreg_udata_registered) == registered0 + 4u' \
  'finreg_udata_discovered) == discovered0 + 2u' \
  'sets0 + 4u' \
  'finreg_udata_queued) == queued0 + 2u' \
  'finreg_udata_clears) == clears0 + 4u' \
  'test_finreg_userdata_inplace_finalizer_behavior' \
  'gc2_counting_finalizer' \
  '09 section 9.6: finalizer may spawn while GC is paused.' \
  'lj_state_tryclaim(cbL, lj_thr_current_id(g), &claim)' \
  'lua_State *oldL' \
  'gc_call_finalizer must not use shared vmthread callback stack'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc2.c" "$ROOT/src/lj_gc2.h" \
	      "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc.h" "$ROOT/tests/t-gc2-phase.c" \
	      "$ROOT/src/lj_meta.c" "$ROOT/src/lib_ffi.c" "$ROOT/src/lj_obj.h" \
	      "$ROOT/tests/t-gc2-traverse.c" \
	      "$ROOT/tests/t-m8-ffi-weak-newindex.c" \
	      "$ROOT/tests/t-m8-finalizer-spawn-live.lua" "$ROOT/src/lj_state.c" \
	      "$ROOT/src/lib_threading.c" \
	      "$ROOT/tests/t-m8-close-finalizers.c" "$ROOT/tools/ci/m8_weak.sh"; then
    echo "guardrail: missing M8 weak/finalizer marker: $needle" >&2
    exit 1
  fi
done

if awk '
  /static void gc_call_finalizer\(global_State \*g, lua_State \*L,/ {
    infn = 1
  }
  infn && /lua_State \*[^=]+=[[:space:]]*vmthread\(g\)/ {
    bad = 1
  }
  infn && /^}/ {
    infn = 0
  }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_gc.c"; then
  echo "guardrail: gc_call_finalizer must not assign vmthread(g) as its callback stack" >&2
  exit 1
fi

if awk '
  /LUA_API void lua_close\(lua_State \*L\)/ {
    inclose = 1
    incp = 0
    sawudata = 0
    sawcdata = 0
  }
  inclose && /lj_vm_cpcall\(L, NULL, NULL, cpfinalize\) == LUA_OK/ {
    incp = 1
  }
  incp && /!lj_gc2_finalizer_queue_pending\(g\)/ {
    sawudata = 1
  }
  incp && /!lj_gc_cdata_fin_pending\(g\)/ {
    sawcdata = 1
  }
  incp && /break;/ && !(sawudata && sawcdata) {
    bad = 1
  }
  incp && /^    }/ { incp = 0 }
  inclose && /^}/ { inclose = 0 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_state.c"; then
  echo "guardrail: lua_close finalizer drain may only break after both pending-queue checks" >&2
  exit 1
fi

if awk '
  /static int gc_finalize_cdata_slot_owned\(lua_State \*L, GCobj \*o,/ {
    inhelper = 1
  }
  inhelper && /^}/ {
    inhelper = 0
  }
  /if \(o->gch.gct == ~LJ_TCDATA\)/ {
    incdata = 1
  }
  incdata && /lj_gc2_finalizer_leave\(g\);/ {
    incdata = 0
  }
  /void lj_gc_finalize_cdata\(lua_State \*L\)/ {
    inclose = 1
  }
  inclose && /^}/ {
    inclose = 0
  }
  !inhelper && (incdata || inclose) && /gc_call_finalizer\(g, L,/ {
    bad = 1
    print
  }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_gc.c"; then
  echo "guardrail: cdata finalizer dispatch must route through owned slot helper" >&2
  exit 1
fi

if rg -n 'mmudata' "$ROOT/src"; then
  echo "guardrail: runtime finalizer queues must not use legacy mmudata" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-weak-modes.lua"
"$ROOT/src/luajit" "$ROOT/tests/t-weak-modes.lua"
timeout 10s "$ROOT/src/luajit" -joff "$ROOT/tests/t-m8-finalizer-spawn-live.lua"
timeout 10s "$ROOT/src/luajit" "$ROOT/tests/t-m8-finalizer-spawn-live.lua"
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-gc-finreg.lua" 3 72
"$ROOT/src/luajit" "$ROOT/tests/t-ffi-gc-finreg.lua" 3 72

out="$TMP/lj_t-gc2-phase_m8"
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-gc2-phase.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$out"
"$out"

out="$TMP/lj_t-gc2-traverse_m8"
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-gc2-traverse.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$out"
"$out"

out="$TMP/lj_t-m8-ffi-weak-newindex"
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-m8-ffi-weak-newindex.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$out"
"$out"

out="$TMP/lj_t-m8-close-finalizers"
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-m8-close-finalizers.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$out"
"$out"

out="$TMP/lj_t-m8-finalizer-state"
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-m8-finalizer-state.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$out"
"$out"

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" XCFLAGS="-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1" \
  -j"$JOBS" >/dev/null

LJ_M8_WEAK_RACE_ITERS=0 LJ_M8_FINALIZER_SPAWN=0 \
  "$ROOT/src/luajit" -joff "$ROOT/tests/t-weak-modes.lua"

out="$TMP/lj_t-gc2-phase_m8_paranoia"
"$CC" $CFLAGS -DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1 -I"$ROOT/src" \
  "$ROOT/tests/t-gc2-phase.c" "$ROOT/src/libluajit.a" \
  -lm -ldl -pthread -o "$out"
"$out"

out="$TMP/lj_t-gc2-traverse_m8_paranoia"
"$CC" $CFLAGS -DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1 -I"$ROOT/src" \
  "$ROOT/tests/t-gc2-traverse.c" "$ROOT/src/libluajit.a" \
  -lm -ldl -pthread -o "$out"
"$out"

out="$TMP/lj_t-m8-ffi-weak-newindex_paranoia"
"$CC" $CFLAGS -DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1 -I"$ROOT/src" \
  "$ROOT/tests/t-m8-ffi-weak-newindex.c" "$ROOT/src/libluajit.a" \
  -lm -ldl -pthread -o "$out"
"$out"

out="$TMP/lj_t-m8-close-finalizers_paranoia"
"$CC" $CFLAGS -DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1 -I"$ROOT/src" \
  "$ROOT/tests/t-m8-close-finalizers.c" "$ROOT/src/libluajit.a" \
  -lm -ldl -pthread -o "$out"
"$out"

out="$TMP/lj_t-m8-finalizer-state_paranoia"
"$CC" $CFLAGS -DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1 -I"$ROOT/src" \
  "$ROOT/tests/t-m8-finalizer-state.c" "$ROOT/src/libluajit.a" \
  -lm -ldl -pthread -o "$out"
"$out"

echo "M8 weak/finalizer semantic gates passed"
