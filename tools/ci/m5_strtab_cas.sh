#!/bin/sh
# Run the Lua-defined M5 string table CAS publication guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
for helper in lj_str_tabh_acq \
  lj_str_tabh_store_rlx \
  lj_str_tabh_rel \
  lj_str_tabh_xchg_acqrel \
  lj_str_retired_head_acq \
  lj_str_retired_head_store_rlx \
  lj_str_retired_head_cas \
  lj_str_retired_head_xchg_acqrel \
  lj_str_retire_epoch_acq \
  lj_str_retire_epoch_rel; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_str.h"; then
    printf '%s\n' "${helper} helper is required for string-table publication" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '(^|[^[:alnum:]_])g[[:space:]]*->[[:space:]]*str[.](tabh|retired)([^[:alnum:]_]|$)|&[[:space:]]*g[[:space:]]*->[[:space:]]*str[.](tabh|retired)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_str.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lj_state.c" \
    "$ROOT/tests/t-strtab-cas.c" \
    "$ROOT/tests/t-strtab-prep.c" \
    "$ROOT/tests/t-strtab-rehash.c" \
    "$ROOT/tests/t-arena-gcmark.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw string-table head access is forbidden; use lj_str_tabh_* or lj_str_retired_head_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*retire_epoch([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_str.c" \
    "$ROOT/tests/t-strtab-cas.c" \
    "$ROOT/tests/t-strtab-prep.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw StrTabHdr retire_epoch access is forbidden; use lj_str_retire_epoch_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*retired_next' \
    "$ROOT/src/lj_str.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/tests/t-strtab-cas.c" \
    "$ROOT/tests/t-strtab-prep.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw StrTabHdr retired_next access is forbidden; use lj_str_retired_next_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'gcrefu[[:space:]]*[(]' \
    "$ROOT/src/lj_str.h" \
    "$ROOT/tests/t-strtab-prep.c" \
    "$ROOT/tests/t-strtab-rehash.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw string-table GCRef metadata reads are forbidden; use lj_str_ref_load_acq() helpers' >&2
  exit 1
fi
for helper in gc2_smr_reclaim_runs_acq \
  gc2_smr_reclaim_runs_store_rlx \
  gc2_smr_reclaim_runs_add \
  gc2_smr_reclaimed_acq \
  gc2_smr_reclaimed_store_rlx \
  gc2_smr_reclaimed_add; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 SMR reclaim counters" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](smr_reclaim_runs|smr_reclaimed)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](smr_reclaim_runs|smr_reclaimed)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/tests/t-strtab-cas.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 SMR reclaim counter access is forbidden; use gc2_smr_* helpers' >&2
  exit 1
fi
if ! grep -qE '^[[:space:]]*static void strtab_wait_no_l[[:space:]]*[(]void[)]' \
    "$ROOT/src/lj_str.c"; then
  printf '%s\n' 'strtab resize/active waits must use strtab_wait_no_l()' >&2
  exit 1
fi
if hits=$(grep -nE -- 'la_cpu_pause[[:space:]]*[(]' \
    "$ROOT/src/lj_str.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'string-table resize/active waits must yield via strtab_wait_no_l(), not spin on la_cpu_pause()' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_strtab_cas
