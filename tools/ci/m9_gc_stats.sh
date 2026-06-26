#!/bin/sh
# Run the Lua-defined M9 GC stats guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

for helper in gc2_sweep_live_updates_acq gc2_sweep_live_updates_store_rlx \
  gc2_sweep_live_updates_add gc2_sweep_live_huge_bytes_acq \
  gc2_sweep_live_huge_bytes_store_rlx gc2_sweep_live_huge_bytes_rel \
  gc2_live_estimate_acq gc2_live_estimate_store_rlx gc2_live_estimate_rel; do
  if ! grep -q "static LJ_AINLINE .* ${helper}" "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 sweep-live stats" >&2
    exit 1
  fi
done

if hits=$(grep -nE -- '->[[:space:]]*gc2[.](sweep_live_updates|sweep_live_huge_bytes|live_estimate)([^[:alnum:]_]|$)|(^|[^[:alnum:]_])gc2->[[:space:]]*(sweep_live_updates|sweep_live_huge_bytes|live_estimate)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lib_base.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 sweep-live estimate access is forbidden; use gc2_* helpers' >&2
  exit 1
fi

if hits=$(grep -nE -- '->[[:space:]]*gc2[.](remembered_barriers|remembered_pushed|remembered_overflows|remembered_filtered|remembered_drained)([^[:alnum:]_]|$)|(^|[^[:alnum:]_])gc2->[[:space:]]*(remembered_barriers|remembered_pushed|remembered_overflows|remembered_filtered|remembered_drained)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lib_base.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 remembered stat access is forbidden; use gc2_remembered_* helpers' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m9_gc_stats
