#!/bin/sh
# Run the Lua-defined M5 string table CAS publication guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
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
exec "$ROOT/tools/ci/lua_test.sh" m5_strtab_cas
