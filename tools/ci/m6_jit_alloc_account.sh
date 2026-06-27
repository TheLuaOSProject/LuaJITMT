#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -RIn -- 'gcnext(' "$ROOT/src" "$ROOT/tests" | \
    grep -vE -- '/src/lj_obj[.]h:[0-9]+:#define gcnext' || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw gcnext() traversal is forbidden; use lj_obj_gcw_acq()' >&2
  exit 1
fi
for helper in lj_tg_local_total_xchg_acqrel lj_tg_local_total_add_rlx
do
  if ! grep -q "$helper" "$ROOT/src/lj_tg.h"; then
    printf '%s\n' "missing TG allocation counter helper: $helper" >&2
    exit 1
  fi
done
if hits=$(grep -RInE -- '->[[:space:]]*local_total([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*local_total([^[:alnum:]_]|$)' \
    "$ROOT/src"/lj_*.c "$ROOT/src"/lib_*.c "$ROOT/src"/lj_*.h 2>/dev/null | \
    grep -vF "$ROOT/src/lj_tg.h:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TG allocation counter access is forbidden; use lj_tg_local_total_* helpers' >&2
  exit 1
fi
for helper in gc2_cycle_requests_acq \
  gc2_cycle_requests_store_rlx \
  gc2_cycle_requests_add \
  gc2_cycle_starts_acq \
  gc2_cycle_starts_store_rlx \
  gc2_cycle_starts_add \
  gc2_major_cycle_starts_acq \
  gc2_major_cycle_starts_store_rlx \
  gc2_major_cycle_starts_add \
  gc2_minor_cycle_requests_acq \
  gc2_minor_cycle_requests_store_rlx \
  gc2_minor_cycle_requests_add \
  gc2_minor_cycle_starts_acq \
  gc2_minor_cycle_starts_store_rlx \
  gc2_minor_cycle_starts_add \
  gc2_minor_sweep_deferred_acq \
  gc2_minor_sweep_deferred_store_rlx \
  gc2_minor_sweep_deferred_add \
  gc2_minor_roots_deferred_acq \
  gc2_minor_roots_deferred_store_rlx \
  gc2_minor_roots_deferred_add \
  gc2_major_root_scans_acq \
  gc2_major_root_scans_store_rlx \
  gc2_major_root_scans_add \
  gc2_minor_root_scans_acq \
  gc2_minor_root_scans_store_rlx \
  gc2_minor_root_scans_add; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 cycle/root telemetry" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*(gc2[.])?(cycle_requests|cycle_starts|major_cycle_starts|minor_cycle_requests|minor_cycle_starts|minor_sweep_deferred|minor_roots_deferred|major_root_scans|minor_root_scans)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*(gc2[.])?(cycle_requests|cycle_starts|major_cycle_starts|minor_cycle_requests|minor_cycle_starts|minor_sweep_deferred|minor_roots_deferred|major_root_scans|minor_root_scans)([^[:alnum:]_]|$)' \
    "$ROOT"/src/*.c || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 cycle/root telemetry access is forbidden; use gc2_* helpers' >&2
  exit 1
fi
for helper in gc2_minor_survival_base_live_acq \
  gc2_minor_survival_base_live_store_rlx \
  gc2_minor_survival_base_live_rel \
  gc2_minor_survival_bytes_acq \
  gc2_minor_survival_bytes_store_rlx \
  gc2_minor_survival_bytes_rel \
  gc2_minor_survival_major_requests_acq \
  gc2_minor_survival_major_requests_store_rlx \
  gc2_minor_survival_major_requests_add; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 minor-survival telemetry" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*(gc2[.])?(minor_survival_base_live|minor_survival_bytes|minor_survival_major_requests)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*(gc2[.])?(minor_survival_base_live|minor_survival_bytes|minor_survival_major_requests)([^[:alnum:]_]|$)' \
    "$ROOT"/src/*.c || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 minor-survival telemetry access is forbidden; use gc2_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'lj_gc2_update_minor_survival_policy[[:space:]]*[(]|LJ_FUNC .*lj_gc2_update_minor_survival_policy[[:space:]]*[(]' \
    "$ROOT"/src/*.c "$ROOT"/tests/*.c "$ROOT/src/lj_gc2.h" | \
    grep -v "$ROOT/src/lj_gc2.c:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'GC2 minor-survival policy updater must stay inside lj_gc2.c; use public cycle APIs or test wrappers' >&2
  exit 1
fi
if ! grep -qE '^[[:space:]]*static void lj_gc2_update_minor_survival_policy[[:space:]]*[(]' \
    "$ROOT/src/lj_gc2.c"; then
  printf '%s\n' 'lj_gc2_update_minor_survival_policy must stay static inside lj_gc2.c' >&2
  exit 1
fi
for helper in gc2_assist_runs_acq \
  gc2_assist_runs_store_rlx \
  gc2_assist_runs_add \
  gc2_assist_grey_drained_acq \
  gc2_assist_grey_drained_store_rlx \
  gc2_assist_grey_drained_add \
  gc2_assist_ssb_converted_acq \
  gc2_assist_ssb_converted_store_rlx \
  gc2_assist_ssb_converted_add \
  gc2_assist_weak_drained_acq \
  gc2_assist_weak_drained_store_rlx \
  gc2_assist_weak_drained_add \
  gc2_jit_hard_checks_acq \
  gc2_jit_hard_checks_store_rlx \
  gc2_jit_hard_checks_add \
  gc2_interp_hard_checks_acq \
  gc2_interp_hard_checks_store_rlx \
  gc2_interp_hard_checks_add \
  gc2_jit_scoped_slots_retired_acq \
  gc2_jit_scoped_slots_retired_store_rlx \
  gc2_jit_scoped_slots_retired_add; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 assist/hard-check telemetry" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*(gc2[.])?(assist_runs|assist_grey_drained|assist_ssb_converted|assist_weak_drained|jit_hard_checks|interp_hard_checks|jit_scoped_slots_retired)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*(gc2[.])?(assist_runs|assist_grey_drained|assist_ssb_converted|assist_weak_drained|jit_hard_checks|interp_hard_checks|jit_scoped_slots_retired)([^[:alnum:]_]|$)' \
    "$ROOT"/src/*.c || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 assist/hard-check telemetry access is forbidden; use gc2_* helpers' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m6_jit_alloc_account
