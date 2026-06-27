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

if hits=$(grep -nE -- '(^|[^[:alnum:]_])gc2->[[:space:]]*(finreg_cdata_order_(seen|claimed|unlinked|queued|retired|tombstones|fallbacks)|finreg_cdata_pending_order_hits)([^[:alnum:]_]|$)' \
    "$ROOT/src/lib_base.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 FINREG ordered stat access is forbidden; use gc2_finreg_cdata_order_* helpers' >&2
  exit 1
fi

if ! grep -qE '^typedef struct GC2StatsSnapshot[[:space:]]*[{]' \
    "$ROOT/src/lj_gc2.h"; then
  printf '%s\n' 'GC2StatsSnapshot is required for GC stats ownership' >&2
  exit 1
fi
if ! grep -qE 'LJ_FUNC void lj_gc2_stats_snapshot[[:space:]]*[(]' \
    "$ROOT/src/lj_gc2.h"; then
  printf '%s\n' 'lj_gc2_stats_snapshot declaration is required for GC stats ownership' >&2
  exit 1
fi
if ! grep -qE '^void lj_gc2_stats_snapshot[[:space:]]*[(]' \
    "$ROOT/src/lj_gc2.c"; then
  printf '%s\n' 'lj_gc2_stats_snapshot definition is required in lj_gc2.c' >&2
  exit 1
fi
for pattern in 'GC2StatsSnapshot s;' 'lj_gc2_stats_snapshot(g, &s);'; do
  if ! grep -qF "$pattern" "$ROOT/src/lib_base.c"; then
    printf '%s\n' "collectgarbage('stats') must use ${pattern}" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- 'gc2_[[:alnum:]_]+_acq[[:space:]]*[(]|lj_gc2_(alloc_since|cycle_alloc|trigger|hard)_load[[:space:]]*[(]|lj_gc2_minor_roots_active[[:space:]]*[(]' \
    "$ROOT/src/lib_base.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'collectgarbage("stats") must read GC2 state through lj_gc2_stats_snapshot' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m9_gc_stats
