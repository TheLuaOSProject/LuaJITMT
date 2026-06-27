#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '(^|[^[:alnum:]_])(gen|ord)[[:space:]]*->[[:space:]]*next' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG next-link access is forbidden; use fin_*_next_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '(^|[^[:alnum:]_])(gen|head)[[:space:]]*->[[:space:]]*tab|&[[:space:]]*(gen|head)[[:space:]]*->[[:space:]]*tab' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG generation table access is forbidden; use fin_gen_tab_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'cts[[:space:]]*->[[:space:]]*fin_(head|order_head|order_retired)|&[[:space:]]*cts[[:space:]]*->[[:space:]]*fin_(head|order_head|order_retired)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG CTState root access is forbidden; use fin_*_head_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '(gcref_acq|setgcrefmt|setgcrefnullrel)[(].*(t|ft|headtab)[[:space:]]*->[[:space:]]*metatable' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG generation liveness access is forbidden; use fin_gen_tab_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'ord[[:space:]]*->[[:space:]]*(obj|tab|slot)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG ordered-node payload access is forbidden; use fin_order_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'ord[[:space:]]*->[[:space:]]*(retired_next|active)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG ordered-node retire state access is forbidden; use fin_order_* helpers' >&2
  exit 1
fi
for helper in gc2_finreg_cdata_order_seen_acq \
  gc2_finreg_cdata_order_seen_store_rlx \
  gc2_finreg_cdata_order_seen_add \
  gc2_finreg_cdata_order_claimed_acq \
  gc2_finreg_cdata_order_claimed_store_rlx \
  gc2_finreg_cdata_order_claimed_add \
  gc2_finreg_cdata_order_unlinked_acq \
  gc2_finreg_cdata_order_unlinked_store_rlx \
  gc2_finreg_cdata_order_unlinked_add \
  gc2_finreg_cdata_order_queued_acq \
  gc2_finreg_cdata_order_queued_store_rlx \
  gc2_finreg_cdata_order_queued_add \
  gc2_finreg_cdata_order_retired_acq \
  gc2_finreg_cdata_order_retired_store_rlx \
  gc2_finreg_cdata_order_retired_add \
  gc2_finreg_cdata_order_tombstones_acq \
  gc2_finreg_cdata_order_tombstones_store_rlx \
  gc2_finreg_cdata_order_tombstones_add \
  gc2_finreg_cdata_order_fallbacks_acq \
  gc2_finreg_cdata_order_fallbacks_store_rlx \
  gc2_finreg_cdata_order_fallbacks_add \
  gc2_finreg_cdata_pending_order_hits_acq \
  gc2_finreg_cdata_pending_order_hits_store_rlx \
  gc2_finreg_cdata_pending_order_hits_add; do
  if ! grep -qE "static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for FINREG ordered counters" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](finreg_cdata_order_(seen|claimed|unlinked|queued|retired|tombstones|fallbacks)|finreg_cdata_pending_order_hits)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](finreg_cdata_order_(seen|claimed|unlinked|queued|retired|tombstones|fallbacks)|finreg_cdata_pending_order_hits)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lib_base.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG ordered counter access is forbidden; use gc2_finreg_cdata_order_* helpers' >&2
  exit 1
fi
for helper in gc2_finreg_cdata_sets_acq \
  gc2_finreg_cdata_sets_store_rlx \
  gc2_finreg_cdata_sets_add \
  gc2_finreg_cdata_clears_acq \
  gc2_finreg_cdata_clears_store_rlx \
  gc2_finreg_cdata_clears_add \
  gc2_finreg_cdata_queued_acq \
  gc2_finreg_cdata_queued_store_rlx \
  gc2_finreg_cdata_queued_add \
  gc2_finreg_cdata_sweep_queued_acq \
  gc2_finreg_cdata_sweep_queued_store_rlx \
  gc2_finreg_cdata_sweep_queued_add \
  gc2_finreg_cdata_pweak_queued_acq \
  gc2_finreg_cdata_pweak_queued_store_rlx \
  gc2_finreg_cdata_pweak_queued_add \
  gc2_finreg_cdata_pweak_claimed_acq \
  gc2_finreg_cdata_pweak_claimed_store_rlx \
  gc2_finreg_cdata_pweak_claimed_add \
  gc2_finreg_cdata_preclaim_overflow_acq \
  gc2_finreg_cdata_preclaim_overflow_store_rlx \
  gc2_finreg_cdata_preclaim_overflow_add \
  gc2_finreg_cdata_preclaim_dispatched_acq \
  gc2_finreg_cdata_preclaim_dispatched_store_rlx \
  gc2_finreg_cdata_preclaim_dispatched_add; do
  if ! grep -qE "static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for FINREG cdata counters" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](finreg_cdata_(sets|clears|queued|sweep_queued|pweak_queued|pweak_claimed|preclaim_overflow|preclaim_dispatched))([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](finreg_cdata_(sets|clears|queued|sweep_queued|pweak_queued|pweak_claimed|preclaim_overflow|preclaim_dispatched))([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lj_cdata.c" \
    "$ROOT/src/lib_base.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG cdata counter access is forbidden; use gc2_finreg_cdata_* helpers' >&2
  exit 1
fi
if ! grep -qE 'LJ_FUNC void lj_gc2_finreg_cdata_finalizer_enqueue[[:space:]]*[(]' \
    "$ROOT/src/lj_gc2.h"; then
  printf '%s\n' 'lj_gc2_finreg_cdata_finalizer_enqueue declaration is required' >&2
  exit 1
fi
if ! grep -qE '^void lj_gc2_finreg_cdata_finalizer_enqueue[[:space:]]*[(]' \
    "$ROOT/src/lj_gc2.c"; then
  printf '%s\n' 'lj_gc2_finreg_cdata_finalizer_enqueue definition is required' >&2
  exit 1
fi
if hits=$(grep -nE -- 'lj_gc2_finreg_cdata_queue[[:space:]]*[(]' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_cdata.c" \
    "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'FINREG cdata finalizer publication must use lj_gc2_finreg_cdata_finalizer_enqueue()' >&2
  exit 1
fi
for helper in lj_gc2_finreg_cdata_finalize_pweak \
  lj_gc2_finreg_cdata_finalize_close \
  lj_gc2_finreg_cdata_dispatch \
  lj_gc2_finreg_cdata_disable \
  lj_gc2_finreg_cdata_pending; do
  if ! grep -qE "LJ_FUNC .*[[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_gc2.h"; then
    printf '%s\n' "${helper} declaration is required for GC2-owned close-time FINREG discovery" >&2
    exit 1
  fi
  if ! grep -qE "^(size_t|int|void)[[:space:]]+${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_gc2.c"; then
    printf '%s\n' "${helper} definition is required in lj_gc2.c" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- 'fin_gen_tab_disable_rel|gc_finalize_cdata_clear|gc_finalize_cdata_claim_preclaimed|gc_finalize_cdata_slot_owned|gc_finalize_cdata_preclaimed|gc_queue_cdata_finalizers_pweak|gc_cdata_finalizer_candidate_pweak|gc_order_cdata_object|gc_unlink_root_object|gc_separate_cdata_finalizers_ordered|gc_cdata_fin_pending_ordered|gc_cdata_finalizer_candidate_close' \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'ordered FINREG cdata discovery/dispatch/disable must stay in lj_gc2 helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '(makewhite|markfinalized|lj_gc_arena_markobj|lj_gc2_finreg_cdata_queue|lj_gc2_finreg_cdata_finalizer_enqueue|lj_gc2_finalizer_enqueue)[(].*obj2gco[(]cd[)]' \
    "$ROOT/src/lj_cdata.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'cdata sweep/free must not rescue finalizers; FINREG discovery owns finalizer queueing' >&2
  exit 1
fi
if hits=$(grep -nE -- '(setgcrefnullrel|setgcrefrel)[(](g->gc2[.]finreg_cdata_preclaim_obj\[[^]]+\]|newobj\[[^]]+\])|gcref_acq[(]oldobj\[[^]]+\][)]' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG preclaim object-slot access is forbidden; use gc2_queue_slot_* helpers' >&2
  exit 1
fi
for helper in gc2_finreg_cdata_preclaim_objvec_acq \
  gc2_finreg_cdata_preclaim_objvec_store_rlx \
  gc2_finreg_cdata_preclaim_objvec_rel \
  gc2_finreg_cdata_preclaim_finvec_acq \
  gc2_finreg_cdata_preclaim_finvec_store_rlx \
  gc2_finreg_cdata_preclaim_finvec_rel \
  gc2_finreg_cdata_preclaim_capacity_acq \
  gc2_finreg_cdata_preclaim_capacity_store_rlx \
  gc2_finreg_cdata_preclaim_capacity_rel \
  gc2_finreg_cdata_preclaim_head_acq \
  gc2_finreg_cdata_preclaim_head_store_rlx \
  gc2_finreg_cdata_preclaim_head_rel \
  gc2_finreg_cdata_preclaim_count_acq \
  gc2_finreg_cdata_preclaim_count_store_rlx \
  gc2_finreg_cdata_preclaim_count_rel; do
  if ! grep -qE "static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for FINREG preclaim state" >&2
    exit 1
  fi
done
for helper in gc2_finreg_cdata_preclaim_test_fail_acq \
  gc2_finreg_cdata_preclaim_test_fail_store_rlx \
  gc2_finreg_cdata_preclaim_test_fail_rel \
  gc2_finreg_cdata_preclaim_publish_pause_store_rlx \
  gc2_finreg_cdata_preclaim_publish_pause_rel \
  gc2_finreg_cdata_preclaim_publish_pause_xchg_acqrel \
  gc2_finreg_cdata_preclaim_publish_paused_acq \
  gc2_finreg_cdata_preclaim_publish_paused_store_rlx \
  gc2_finreg_cdata_preclaim_publish_paused_rel \
  gc2_finreg_cdata_preclaim_publish_release_acq \
  gc2_finreg_cdata_preclaim_publish_release_store_rlx \
  gc2_finreg_cdata_preclaim_publish_release_rel; do
  if ! grep -qE "static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for FINREG preclaim test hooks" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]finreg_cdata_preclaim_(obj|fin|capacity|head|count)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]finreg_cdata_preclaim_(obj|fin|capacity|head|count)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG preclaim state access is forbidden; use gc2_finreg_cdata_preclaim_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](finreg_cdata_preclaim_test_fail|finreg_cdata_preclaim_publish_(pause|paused|release))([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](finreg_cdata_preclaim_test_fail|finreg_cdata_preclaim_publish_(pause|paused|release))([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG preclaim test-hook access is forbidden; use gc2_finreg_cdata_preclaim_* helpers' >&2
  exit 1
fi
if hits=$(awk '
  /^LJLIB_CF\(ffi_gc\)/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in ffi.gc(); use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
"$ROOT/tools/ci/lua_test.sh" m7_ffi_finreg
cc -std=gnu99 -O2 -Wall -Wextra -Werror -mcx16 -I"$ROOT/src" \
  "$ROOT/tests/t-ffi-finreg-free-invariant.c" "$ROOT/src/libluajit.a" \
  -lm -ldl -pthread -o /tmp/lj_t-ffi-finreg-free-invariant
/tmp/lj_t-ffi-finreg-free-invariant
