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
  'test_weak_key_write_barrier' \
  'test_vm_weak_key_write_barrier' \
  'test_vm_weak_value_array_barrier' \
  'test_weak_clear_marks_string_slots' \
  'test_weak_post_clear_resurrection_write' \
  'test_weak_complete_bridge' \
  'lj_gc2_weak_complete(g, gcref(g->gc.weak), 1)' \
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
  'gc_finalize_cdata_call_owned(lua_State *L, GCobj *o,' \
  'gc_finalize_cdata_slot_owned(lua_State *L, GCobj *o, GCtab *t,' \
  'lj_ctype_fin_claim_wait(cts)' \
  '09 section 9.6: finalizer may spawn while GC is paused.' \
  'lj_state_tryclaim(cbL, lj_thr_current_id(g), &claim)' \
  'lua_State *oldL' \
  'gc_call_finalizer must not use shared vmthread callback stack'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc2.c" "$ROOT/src/lj_gc2.h" \
      "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc.h" "$ROOT/tests/t-gc2-phase.c" \
      "$ROOT/tests/t-gc2-traverse.c"; then
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
  /static void gc_finalize_cdata_call_owned\(lua_State \*L, GCobj \*o,/ {
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
