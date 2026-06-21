#!/bin/sh
# Run the Lua-defined M3 GC2 worker scheduler guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '(^|[^[:alnum:]_])(node|tail|fresh)[[:space:]]*->[[:space:]]*next' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 SSB next-link access is forbidden; use lj_gc2_ssb_next_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'ssb_node\[[^]]+\][.]next' \
    "$ROOT/src/lj_tg.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw embedded GC2 SSB next-link init is forbidden; use lj_gc2_ssb_next_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'setgcrefr?rel[(][*]lj_obj_gcwref[(](oldtail|tail)[)]' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 finalizer ring next-link release stores are forbidden; use lj_obj_setgcwrel' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]finalizer_(mpsc|tail|active|owner_tid|mpsc_drained)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]finalizer_(mpsc|tail|active|owner_tid|mpsc_drained)' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 finalizer queue/owner state access is forbidden; use gc2_finalizer_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]worker_active|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]worker_active' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 worker-active claim access is forbidden; use gc2_worker_active_* helpers' >&2
  exit 1
fi
for helper in gc2_grey_stack_acq gc2_grey_stack_store_rlx gc2_grey_stack_rel \
  gc2_grey_capacity_acq gc2_grey_capacity_store_rlx \
  gc2_grey_capacity_rel \
  gc2_grey_top_acq gc2_grey_top_store_rlx gc2_grey_top_cas \
  gc2_grey_bottom_acq gc2_grey_bottom_rlx gc2_grey_bottom_store_rlx \
  gc2_grey_bottom_rel; do
  if ! grep -qE "static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 grey deque state" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](grey_stack|grey_capacity|grey_top|grey_bottom)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](grey_stack|grey_capacity|grey_top|grey_bottom)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 grey deque state access is forbidden; use gc2_grey_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]assist_(active|shift)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]assist_(active|shift)' \
    "$ROOT/src/lj_api.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 assist state access is forbidden; use gc2_assist_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]cycle_leader|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]cycle_leader' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 cycle-leader token access is forbidden; use gc2_cycle_leader_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]cycle([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]cycle([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lj_safepoint.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 cycle epoch access is forbidden; use gc2_cycle_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](n_workers|worker_stop|worker_wake|worker_started|worker_exited)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](worker_wake|worker_started|worker_exited)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lib_base.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 worker lifecycle state access is forbidden; use gc2_worker_* lifecycle helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]phase|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]phase' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lj_tg.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 phase access is forbidden; use gc2_phase_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m3_gc2_worker_scheduler
