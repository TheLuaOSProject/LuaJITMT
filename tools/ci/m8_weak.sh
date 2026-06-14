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
  'test_vm_weak_value_hash_key_barrier' \
  'test_vm_weak_value_array_barrier' \
  'test_weak_clear_marks_string_slots' \
  'test_weak_drain_uses_captured_mode' \
  'test_weak_post_clear_resurrection_write' \
  'test_vm_weak_post_clear_existing_key_write' \
  'lj_gc2_barrier_weak_key(L, t, k);' \
  'weak-table key write' \
  'test_weak_complete_bridge' \
  'lj_gc2_weak_complete(g, gcref(g->gc.weak), 1)' \
  'int weak = lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAK' \
  'capture traversal-time weak mode' \
  'gc2_weak_paranoia_zero_diff(global_State *g, GCobj *legacy)' \
  'lj_gc2_finalizer_try_enter(global_State *g)' \
  'peer finalizer dispatch backs off' \
  'gcref_acq(g->gc.mmudata) == NULL' \
  'lj_gc2_finalizer_pending(global_State *g)' \
  'lj_gc2_finalizer_sweep_pending(global_State *g)' \
  'assert(la_load32_acq(&g->gc2.finalizer_owner_tid) ==' \
  'finalizer_enters0 + 1u' \
  'finalizer_leaves0 + 1u' \
  'gc_finalizer_mt_release_exclusive(global_State *g)' \
  'gc_finalizer_mt_reclaim_exclusive(global_State *g)' \
  'gc_fullgc_deferred_by_finalizer(global_State *g)' \
  'finrc <= 0' \
  'Keep GCSfinalize open until spawned TG exits.' \
  'finalizer-spawn outlived callback' \
  'lj_gc_mt_threshold_store(g, oldt)' \
  'collectgarbage("step", 1000000)' \
  'GC step completed while finalizer-spawned worker was live' \
  'finalizer-spawned worker can outlive callback' \
  'gc_finalize_cdata_call_owned(lua_State *L, GCobj *o,' \
  'gc_finalize_cdata_slot_owned(lua_State *L, GCobj *o, cTValue *key)' \
  'lj_ctype_fin_get(L, cts, key, &t)' \
  'LJ_GC_UDATA_FINREG == LJ_GC_WEAKVAL' \
  'old | LJ_GC_UDATA_FINREG' \
  'lj_gc2_finreg_udata_set(g, o, 0);' \
  'sets0 + 3u' \
  'finreg_udata_clears) == clears0 + 3u' \
  '09 section 9.6: finalizer may spawn while GC is paused.' \
  'lj_state_tryclaim(cbL, lj_thr_current_id(g), &claim)' \
  'lua_State *oldL' \
  'gc_call_finalizer must not use shared vmthread callback stack'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc2.c" "$ROOT/src/lj_gc2.h" \
      "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc.h" "$ROOT/tests/t-gc2-phase.c" \
      "$ROOT/src/lj_meta.c" "$ROOT/tests/t-gc2-traverse.c" \
      "$ROOT/tests/t-m8-finalizer-spawn-live.lua"; then
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
  /static int gc_finalize_cdata_call_owned\(lua_State \*L, GCobj \*o,/ {
    inhelper = 1
  }
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
  echo "guardrail: cdata finalizer dispatch must route through gc_finalize_cdata_call_owned" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-weak-modes.lua"
"$ROOT/src/luajit" "$ROOT/tests/t-weak-modes.lua"
timeout 10s "$ROOT/src/luajit" -joff "$ROOT/tests/t-m8-finalizer-spawn-live.lua"

out="$TMP/lj_t-gc2-phase_m8"
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-gc2-phase.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$out"
"$out"

out="$TMP/lj_t-gc2-traverse_m8"
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-gc2-traverse.c" \
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
