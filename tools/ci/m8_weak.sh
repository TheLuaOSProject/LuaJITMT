#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

for helper in gc2_weak_count_acq gc2_weak_count_store_rlx \
  gc2_weak_stack_acq gc2_weak_stack_store_rlx gc2_weak_stack_rel \
  gc2_weak_ready_acq gc2_weak_ready_store_rlx gc2_weak_ready_rel \
  gc2_weak_capacity_acq gc2_weak_capacity_store_rlx \
  gc2_weak_capacity_rel \
  gc2_weak_count_add gc2_weak_scan_cursor_acq \
  gc2_weak_scan_cursor_store_rlx gc2_weak_scan_cursor_cas \
  gc2_weak_clear_cursor_acq gc2_weak_clear_cursor_store_rlx \
  gc2_weak_clear_cursor_cas; do
  if ! grep -qE "static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 weak snapshot state" >&2
    exit 1
  fi
done

if hits=$(grep -nE -- '->[[:space:]]*gc2[.](weak_count|weak_scan_cursor|weak_clear_cursor)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](weak_count|weak_scan_cursor|weak_clear_cursor)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 weak snapshot state access is forbidden; use gc2_weak_* helpers' >&2
  exit 1
fi

if hits=$(grep -nE -- '->[[:space:]]*gc2[.](weak_stack|weak_ready|weak_capacity)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](weak_stack|weak_ready|weak_capacity)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 weak snapshot vector access is forbidden; use gc2_weak_* helpers' >&2
  exit 1
fi

for helper in gc2_weak_tables_seen_acq \
  gc2_weak_tables_seen_store_rlx \
  gc2_weak_tables_seen_add \
  gc2_weak_tables_weakkey_acq \
  gc2_weak_tables_weakkey_store_rlx \
  gc2_weak_tables_weakkey_add \
  gc2_weak_tables_weakval_acq \
  gc2_weak_tables_weakval_store_rlx \
  gc2_weak_tables_weakval_add \
  gc2_weak_tables_allweak_acq \
  gc2_weak_tables_allweak_store_rlx \
  gc2_weak_tables_allweak_add \
  gc2_weak_tables_queued_acq \
  gc2_weak_tables_queued_store_rlx \
  gc2_weak_tables_queued_add \
  gc2_weak_tables_overflow_acq \
  gc2_weak_tables_overflow_store_rlx \
  gc2_weak_tables_overflow_add; do
  if ! grep -qE "static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 weak discovery counters" >&2
    exit 1
  fi
done

if hits=$(grep -nE -- '->[[:space:]]*gc2[.](weak_tables_(seen|weakkey|weakval|allweak|queued|overflow))([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](weak_tables_(seen|weakkey|weakval|allweak|queued|overflow))([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 weak discovery counter access is forbidden; use gc2_weak_* helpers' >&2
  exit 1
fi

for helper in gc2_weak_scan_runs_acq \
  gc2_weak_scan_runs_store_rlx \
  gc2_weak_scan_runs_add \
  gc2_weak_scan_tables_acq \
  gc2_weak_scan_tables_store_rlx \
  gc2_weak_scan_tables_add \
  gc2_weak_scan_slots_acq \
  gc2_weak_scan_slots_store_rlx \
  gc2_weak_scan_slots_add \
  gc2_weak_scan_clearable_acq \
  gc2_weak_scan_clearable_store_rlx \
  gc2_weak_scan_clearable_add \
  gc2_weak_clear_runs_acq \
  gc2_weak_clear_runs_store_rlx \
  gc2_weak_clear_runs_add \
  gc2_weak_clear_tables_acq \
  gc2_weak_clear_tables_store_rlx \
  gc2_weak_clear_tables_add \
  gc2_weak_clear_slots_acq \
  gc2_weak_clear_slots_store_rlx \
  gc2_weak_clear_slots_add \
  gc2_weak_clear_cleared_acq \
  gc2_weak_clear_cleared_store_rlx \
  gc2_weak_clear_cleared_add \
  gc2_weak_bridge_skipped_acq \
  gc2_weak_bridge_skipped_store_rlx \
  gc2_weak_bridge_skipped_add \
  gc2_weak_bridge_fallbacks_acq \
  gc2_weak_bridge_fallbacks_store_rlx \
  gc2_weak_bridge_fallbacks_add \
  gc2_weak_bridge_backfills_acq \
  gc2_weak_bridge_backfills_store_rlx \
  gc2_weak_bridge_backfills_add \
  gc2_weak_bridge_backfill_tables_acq \
  gc2_weak_bridge_backfill_tables_store_rlx \
  gc2_weak_bridge_backfill_tables_add \
  gc2_weak_bridge_backfill_slots_acq \
  gc2_weak_bridge_backfill_slots_store_rlx \
  gc2_weak_bridge_backfill_slots_add \
  gc2_weak_bridge_backfill_cleared_acq \
  gc2_weak_bridge_backfill_cleared_store_rlx \
  gc2_weak_bridge_backfill_cleared_add; do
  if ! grep -qE "static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 weak scan counters" >&2
    exit 1
  fi
done

weak_bridge_counters='weak_bridge_(skipped|fallbacks|backfills)'
weak_bridge_counters="${weak_bridge_counters}|weak_bridge_backfill_(tables|slots|cleared)"
weak_scan_counters='weak_scan_(runs|tables|slots|clearable)'
weak_scan_counters="${weak_scan_counters}|weak_clear_(runs|tables|slots|cleared)"
weak_scan_counters="${weak_scan_counters}|${weak_bridge_counters}"
weak_scan_raw_re="->[[:space:]]*gc2[.](${weak_scan_counters})"
weak_scan_raw_re="${weak_scan_raw_re}([^[:alnum:]_]|$)"
weak_scan_addr_re="&[[:space:]]*[^)]*->[[:space:]]*gc2[.]"
weak_scan_addr_re="${weak_scan_addr_re}(${weak_scan_counters})([^[:alnum:]_]|$)"
if hits=$(grep -nE -- "${weak_scan_raw_re}|${weak_scan_addr_re}" \
    "$ROOT/src/lj_gc2.c" "$ROOT/src/lib_base.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' \
    'raw GC2 weak scan counter access is forbidden; use gc2_weak_* helpers' >&2
  exit 1
fi

for helper in gc2_weak_keys_marked_acq \
  gc2_weak_keys_marked_store_rlx \
  gc2_weak_keys_marked_add \
  gc2_weak_values_marked_acq \
  gc2_weak_values_marked_store_rlx \
  gc2_weak_values_marked_add; do
  if ! grep -qE "static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 weak mark counters" >&2
    exit 1
  fi
done

weak_mark_counters='weak_(keys|values)_marked'
weak_mark_raw_re="->[[:space:]]*gc2[.]${weak_mark_counters}"
weak_mark_raw_re="${weak_mark_raw_re}([^[:alnum:]_]|$)"
weak_mark_addr_re="&[[:space:]]*[^)]*->[[:space:]]*gc2[.]"
weak_mark_addr_re="${weak_mark_addr_re}${weak_mark_counters}([^[:alnum:]_]|$)"
if hits=$(grep -nE -- "${weak_mark_raw_re}|${weak_mark_addr_re}" \
    "$ROOT/src/lj_gc2.c" "$ROOT/src/lib_base.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' \
    'raw GC2 weak mark counter access is forbidden; use gc2_weak_* helpers' >&2
  exit 1
fi

for helper in gc2_finalizer_drain_test_pause_store_rlx \
  gc2_finalizer_drain_test_pause_rel \
  gc2_finalizer_drain_test_pause_xchg_acqrel \
  gc2_finalizer_drain_test_paused_acq \
  gc2_finalizer_drain_test_paused_store_rlx \
  gc2_finalizer_drain_test_paused_rel \
  gc2_finalizer_drain_test_release_acq \
  gc2_finalizer_drain_test_release_store_rlx \
  gc2_finalizer_drain_test_release_rel; do
  if ! grep -qE "static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 finalizer test hooks" >&2
    exit 1
  fi
done

finalizer_test_hooks='finalizer_drain_test_(pause|paused|release)'
if hits=$(grep -nE -- "->[[:space:]]*gc2[.]${finalizer_test_hooks}([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]${finalizer_test_hooks}([^[:alnum:]_]|$)" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' \
    'raw GC2 finalizer test-hook access is forbidden; use gc2_finalizer_* helpers' >&2
  exit 1
fi

if hits=$(awk '
  /^static int gc2_call_finalizer\(global_State \*g, lua_State \*L,/ {
    in_fn = 1
  }
  in_fn && (/while[[:space:]]*\(!lj_state_tryclaim/ || /la_cpu_pause[[:space:]]*\(/) {
    print FILENAME ":" FNR ":" $0
  }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_gc.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' \
    'GC2 finalizer callback dispatch must defer on a busy lua_State instead of spinning' >&2
  exit 1
fi

if ! awk '
  /^static int lj_gc2_finalizer_dispatch_one\(lua_State \*L,/ { in_fn = 1 }
  in_fn && /lj_state_tryclaim\(L,/ { claimed = 1 }
  in_fn && /lj_gc2_finalizer_dequeue_owned\(g\)/ && !claimed { bad = 1 }
  in_fn && /^}/ { in_fn = 0 }
  END { exit(claimed && !bad ? 0 : 1) }
' "$ROOT/src/lj_gc2.c"; then
  printf '%s\n' \
    'GC2 finalizer dispatch must claim callback lua_State before dequeuing finalizers' >&2
  exit 1
fi

if hits=$(grep -nE -- '^static int gc_dispatch_finalizer_obj[[:space:]]*[(]' \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' \
    'legacy finalizer object routing must stay in GC2 finalizer dispatch helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'lj_gc2_finreg_(cdata|udata)_dispatch[[:space:]]*[(]' \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' \
    'legacy finalizer dispatch must call GC2 finalizer dispatch APIs, not FINREG dispatch directly' >&2
  exit 1
fi
if hits=$(grep -nE -- 'GC2FinalizerCallFunc' \
    "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.h" "$ROOT/src/lj_gc2.c" || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' \
    'GC2 finalizer callback runner must stay internal, not a public function-pointer bridge' >&2
  exit 1
fi
if hits=$(awk '
  /^[[:space:]]*#[[:space:]]*if[[:space:]]+defined[(]lj_gc2_c[)]/ &&
      /LJ_GC2_TEST_HELPERS/ {
    in_internal = 1
    depth = 0
    next
  }
  in_internal && /^[[:space:]]*#[[:space:]]*if/ { depth++ }
  in_internal && /^[[:space:]]*#[[:space:]]*endif/ {
    if (depth == 0) {
      in_internal = 0
      next
    }
    depth--
    next
  }
  /GC2FinalizerDispatchFunc/ && !in_internal {
    print FILENAME ":" FNR ":" $0
  }
' "$ROOT/src/lj_gc2.h"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' \
    'raw finalizer object-dispatch type must stay internal/test-only in lj_gc2.h' >&2
  exit 1
fi
if hits=$(grep -nE -- 'LJ_FUNC .*[[:space:]]lj_gc2_finalizer_(queue_pending|sweep_pending)[[:space:]]*[(]' \
    "$ROOT/src/lj_gc2.h" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw finalizer pending predicates must stay private to lj_gc2.c' >&2
  exit 1
fi
for helper in gc2_finalizer_queue_pending \
  gc2_finalizer_sweep_pending; do
  if ! grep -qE "^[[:space:]]*static int ${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_gc2.c"; then
    printf '%s\n' "${helper} must stay static inside lj_gc2.c" >&2
    exit 1
  fi
done
for helper in lj_gc2_test_finalizer_queue_pending \
  lj_gc2_test_finalizer_sweep_pending; do
  if ! grep -qE "^[[:space:]]*LJ_FUNC int ${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_gc2.h"; then
    printf '%s\n' "${helper} declaration is required for finalizer fixture predicates" >&2
    exit 1
  fi
done
if ! grep -qF 'lj_gc2_finalizer_dispatch_all(L)' "$ROOT/src/lj_state.c"; then
  printf '%s\n' 'lua_close finalizer drain must call lj_gc2_finalizer_dispatch_all(L)' >&2
  exit 1
fi
if ! grep -qF 'lj_gc2_finalizer_step(L, GCFINALIZECOST,' "$ROOT/src/lj_gc.c"; then
  printf '%s\n' 'legacy GCSfinalize step must call lj_gc2_finalizer_step' >&2
  exit 1
fi
if ! grep -qF 'lj_gc2_finalizer_fullgc_deferred(g)' "$ROOT/src/lj_gc.c"; then
  printf '%s\n' 'lj_gc_fullgc finalizer-spawn deferral must use lj_gc2_finalizer_fullgc_deferred' >&2
  exit 1
fi
if ! grep -qE '^static int gc2_call_finalizer[[:space:]]*[(]' \
    "$ROOT/src/lj_gc2.c"; then
  printf '%s\n' 'GC2-owned finalizer callback runner is required' >&2
  exit 1
fi
for helper in gc2_finalizer_pause_threshold \
  gc2_finalizer_restore_threshold \
  gc2_finalizer_pcall \
  gc2_finalizer_mt_release_exclusive \
  gc2_finalizer_mt_reclaim_exclusive; do
  if ! grep -qE "^static .*[[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_gc2.c"; then
    printf '%s\n' "${helper} must stay static inside lj_gc2.c" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- 'gc_call_finalizer|lj_gc2_finalizer_(pause_threshold|restore_threshold|pcall)[[:space:]]*[(]|lj_gc2_finalizer_mt_(release|reclaim)_exclusive[[:space:]]*[(]|lj_vm_pcall[[:space:]]*[(]|lj_vmevent_send[[:space:]]*[(]|hook_(save|entergc|restore)[[:space:]]*[(]|lj_state_checkstack[[:space:]]*[(]|save(stack)?[[:space:]]*[(]|restorestack[[:space:]]*[(]' \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'legacy finalizer callback path must not own callback stack, event, or protected-call policy' >&2
  exit 1
fi
if hits=$(grep -nE -- 'LJ_FUNC .*[[:space:]]lj_gc2_finalizer_(pause_threshold|restore_threshold|pcall|mt_(release|reclaim)_exclusive)[[:space:]]*[(]' \
    "$ROOT/src/lj_gc2.h" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw finalizer callback helpers must stay private to lj_gc2.c' >&2
  exit 1
fi
if hits=$(grep -nE -- 'LJ_FUNC .*[[:space:]]lj_gc2_finalizer_spawn_deferred[[:space:]]*[(]' \
    "$ROOT/src/lj_gc2.h" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw finalizer-spawn deferral predicate must stay private to lj_gc2.c' >&2
  exit 1
fi
if ! grep -qE '^static int gc2_finalizer_spawn_deferred[[:space:]]*[(]' \
    "$ROOT/src/lj_gc2.c"; then
  printf '%s\n' 'private gc2_finalizer_spawn_deferred implementation is required' >&2
  exit 1
fi
if hits=$(grep -nE -- 'lj_gc2_finalizer_spawn_deferred[[:space:]]*[(]' \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'legacy GC must not call raw finalizer-spawn deferral predicate' >&2
  exit 1
fi
if ! grep -qF 'lj_gc2_finalizer_close_pending(g)' "$ROOT/src/lj_state.c"; then
  printf '%s\n' 'lua_close finalizer fixed-point loop must use lj_gc2_finalizer_close_pending' >&2
  exit 1
fi
if hits=$(grep -nE -- 'lj_gc2_finalizer_queue_pending[[:space:]]*[(]|lj_gc_cdata_fin_pending[[:space:]]*[(]' \
    "$ROOT/src/lj_state.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'lua_close must not open-code finalizer queue or FINREG pending checks' >&2
  exit 1
fi
if hits=$(grep -nE -- 'lj_gc_finalize_(udata|cdata|cdata_disable)[[:space:]]*[(]|lj_gc_cdata_fin_pending[[:space:]]*[(]|lj_gc_separateudata[[:space:]]*[(]|LJ_FUNC .*[[:space:]]lj_gc_finalize_(udata|cdata|cdata_disable)[[:space:]]*[(]|LJ_FUNC .*[[:space:]]lj_gc_cdata_fin_pending[[:space:]]*[(]|LJ_FUNC .*[[:space:]]lj_gc_separateudata[[:space:]]*[(]' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc.h" \
    "$ROOT/src/lj_state.c" \
    "$ROOT/tests/t-gc2-traverse.c" \
    "$ROOT/tests/t-gc2-worker-scheduler.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'legacy close-time finalizer wrappers are forbidden; use GC2 finalizer/FINREG APIs directly' >&2
  exit 1
fi

if hits=$(awk '
  /^static void gc2_finreg_dispatch_requeue\(global_State \*g,/ { in_fn = 1 }
  in_fn && /lj_gc_linkobj[[:space:]]*[(]/ { cdata_link = 1 }
  in_fn && /lj_gc_linkobj_after[[:space:]]*[(]/ { udata_link = 1 }
  in_fn && /makewhite[[:space:]]*[(]/ { white = 1 }
  in_fn && /lj_gc_arena_markobj[[:space:]]*[(]/ { arena = 1 }
  in_fn && /^}/ { in_fn = 0 }
  END {
    if (!(cdata_link && udata_link && white && arena))
      print "missing requeue/rewhite state mutation"
  }
' "$ROOT/src/lj_gc2.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' \
    'GC2 FINREG dispatch helper must own finalizer object requeue/rewhite state mutation' >&2
  exit 1
fi

if ! grep -qE '^[[:space:]]*static void gc2_finreg_dispatch_requeue[[:space:]]*[(]' \
    "$ROOT/src/lj_gc2.c"; then
  printf '%s\n' 'gc2_finreg_dispatch_requeue helper is required for finalizer dispatch ownership' >&2
  exit 1
fi
for fn in lj_gc2_finreg_cdata_dispatch lj_gc2_finreg_udata_dispatch; do
  if ! awk -v fn="$fn" '
    $0 ~ ("^static int[[:space:]]+" fn "[[:space:]]*[(]") { in_fn = 1 }
    in_fn && /gc2_finreg_dispatch_requeue[[:space:]]*[(]g,[[:space:]]*o[)]/ {
      found = 1
    }
    in_fn && /^}/ { in_fn = 0 }
    END { exit(found ? 0 : 1) }
  ' "$ROOT/src/lj_gc2.c"; then
    printf '%s\n' "${fn} must requeue/rewhite finalizer objects through gc2_finreg_dispatch_requeue" >&2
    exit 1
  fi
done

if hits=$(awk '
  /^static void lj_gc2_finalizer_enter\(global_State \*g\)/ ||
  /^static void lj_gc2_finalizer_leave\(global_State \*g\)/ ||
  /^void lj_gc2_finalizer_dispatch_all\(lua_State \*L,/ {
    in_fn = 1
  }
  in_fn && /la_cpu_pause[[:space:]]*\(/ { print FILENAME ":" FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_gc2.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' \
    'GC2 finalizer owner waits must use native sleep slices instead of CPU spinning' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m8_weak
