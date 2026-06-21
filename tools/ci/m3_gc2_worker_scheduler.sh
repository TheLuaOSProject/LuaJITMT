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
