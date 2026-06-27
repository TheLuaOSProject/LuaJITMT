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
for helper in lj_tg_ssb_free_acq \
  lj_tg_ssb_free_store_rlx \
  lj_tg_ssb_free_cas \
  lj_tg_ssb_free_pop \
  lj_tg_ssb_free_push; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_tg.h"; then
    printf '%s\n' "${helper} helper is required for the TG SSB free-list head" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '(->|[.])[[:space:]]*ssb_free([^[:alnum:]_]|$)' \
    "$ROOT"/src/*.c || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TG SSB free-list head access is forbidden; use lj_tg_ssb_free_* helpers' >&2
  exit 1
fi
for helper in lj_tg_ssb_active_acq \
  lj_tg_ssb_active_rel \
  lj_tg_ssb_base_acq \
  lj_tg_ssb_base_rel \
  lj_tg_ssb_next_acq \
  lj_tg_ssb_next_rel \
  lj_tg_ssb_end_acq \
  lj_tg_ssb_end_rel; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_tg.h"; then
    printf '%s\n' "${helper} helper is required for TG SSB active cursors" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '(->|[.])[[:space:]]*ssb_(active|base|next|end)([^[:alnum:]_]|$)' \
    "$ROOT"/src/*.c || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TG SSB active cursor access is forbidden; use lj_tg_ssb_* helpers' >&2
  exit 1
fi
for helper in gc2_ssb_head_acq \
  gc2_ssb_head_store_rlx \
  gc2_ssb_head_cas \
  gc2_ssb_head_xchg_acqrel; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for the GC2 published SSB stack" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]ssb_head|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]ssb_head' \
    "$ROOT"/src/*.c || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 published SSB stack access is forbidden; use gc2_ssb_head_* helpers' >&2
  exit 1
fi
for helper in gc2_worker_thread_acq \
  gc2_worker_thread_store_rlx \
  gc2_worker_tg_acq \
  gc2_worker_tg_store_rlx \
  gc2_worker_tg_retired_acq \
  gc2_worker_tg_retired_store_rlx \
  gc2_worker_tg_retired_rel \
  gc2_worker_control_acq \
  gc2_worker_control_store_rlx \
  gc2_worker_control_rel \
  gc2_worker_control_cas \
  gc2_worker_control_futex_wake \
  gc2_worker_control_futex_wait; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 worker parking/control state" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](worker_thread|worker_tg)[[]|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](worker_thread|worker_tg)[[]' \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/tests/t-gc2-worker-scheduler.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 worker parking slot access is forbidden; use gc2_worker_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](worker_tg_retired|worker_control)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](worker_tg_retired|worker_control)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/tests/t-gc2-worker-scheduler.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 worker control/retire access is forbidden; use gc2_worker_* helpers' >&2
  exit 1
fi
for helper in gc2_ssb_published_acq \
  gc2_ssb_published_store_rlx \
  gc2_ssb_published_add \
  gc2_ssb_drained_acq \
  gc2_ssb_drained_store_rlx \
  gc2_ssb_drained_add \
  gc2_ssb_items_published_acq \
  gc2_ssb_items_published_store_rlx \
  gc2_ssb_items_published_add \
  gc2_ssb_items_drained_acq \
  gc2_ssb_items_drained_store_rlx \
  gc2_ssb_items_drained_add; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 SSB telemetry counters" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](ssb_published|ssb_drained|ssb_items_published|ssb_items_drained)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](ssb_published|ssb_drained|ssb_items_published|ssb_items_drained)([^[:alnum:]_]|$)' \
    "$ROOT"/src/*.c || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 SSB telemetry counter access is forbidden; use gc2_ssb_* helpers' >&2
  exit 1
fi
for helper in gc2_remembered_barriers_acq \
  gc2_remembered_barriers_store_rlx \
  gc2_remembered_barriers_add \
  gc2_remembered_pushed_acq \
  gc2_remembered_pushed_store_rlx \
  gc2_remembered_pushed_add \
  gc2_remembered_overflows_acq \
  gc2_remembered_overflows_store_rlx \
  gc2_remembered_overflows_add \
  gc2_remembered_filtered_acq \
  gc2_remembered_filtered_store_rlx \
  gc2_remembered_filtered_add \
  gc2_remembered_drained_acq \
  gc2_remembered_drained_store_rlx \
  gc2_remembered_drained_add; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 remembered telemetry counters" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](remembered_barriers|remembered_pushed|remembered_overflows|remembered_filtered|remembered_drained)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](remembered_barriers|remembered_pushed|remembered_overflows|remembered_filtered|remembered_drained)([^[:alnum:]_]|$)' \
    "$ROOT"/src/*.c || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 remembered telemetry counter access is forbidden; use gc2_remembered_* helpers' >&2
  exit 1
fi
for helper in gc2_fixpoint_rounds_acq \
  gc2_fixpoint_rounds_store_rlx \
  gc2_fixpoint_rounds_add \
  gc2_fixpoint_hits_acq \
  gc2_fixpoint_hits_store_rlx \
  gc2_fixpoint_hits_add \
  gc2_mark_complete_runs_acq \
  gc2_mark_complete_runs_store_rlx \
  gc2_mark_complete_runs_add \
  gc2_mark_complete_hits_acq \
  gc2_mark_complete_hits_store_rlx \
  gc2_mark_complete_hits_add \
  gc2_mark_complete_peer_waits_acq \
  gc2_mark_complete_peer_waits_store_rlx \
  gc2_mark_complete_peer_waits_add \
  gc2_mark_to_weak_acq \
  gc2_mark_to_weak_store_rlx \
  gc2_mark_to_weak_add \
  gc2_weak_complete_runs_acq \
  gc2_weak_complete_runs_store_rlx \
  gc2_weak_complete_runs_add \
  gc2_weak_complete_progress_acq \
  gc2_weak_complete_progress_store_rlx \
  gc2_weak_complete_progress_add \
  gc2_weak_to_sweep_acq \
  gc2_weak_to_sweep_store_rlx \
  gc2_weak_to_sweep_add; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 fixpoint/phase counters" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](fixpoint_rounds|fixpoint_hits|mark_complete_runs|mark_complete_hits|mark_complete_peer_waits|mark_to_weak|weak_complete_runs|weak_complete_progress|weak_to_sweep)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](fixpoint_rounds|fixpoint_hits|mark_complete_runs|mark_complete_hits|mark_complete_peer_waits|mark_to_weak|weak_complete_runs|weak_complete_progress|weak_to_sweep)([^[:alnum:]_]|$)' \
    "$ROOT"/src/*.c || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 fixpoint/phase counter access is forbidden; use gc2_* helpers' >&2
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
for helper in gc2_finalizer_queued_acq \
  gc2_finalizer_queued_store_rlx \
  gc2_finalizer_queued_add \
  gc2_finalizer_dequeued_acq \
  gc2_finalizer_dequeued_store_rlx \
  gc2_finalizer_dequeued_add \
  gc2_finalizer_mpsc_drained_acq \
  gc2_finalizer_mpsc_drained_store_rlx \
  gc2_finalizer_mpsc_drained_add \
  gc2_finalizer_enters_acq \
  gc2_finalizer_enters_store_rlx \
  gc2_finalizer_enters_add \
  gc2_finalizer_leaves_acq \
  gc2_finalizer_leaves_store_rlx \
  gc2_finalizer_leaves_add \
  gc2_finalizer_sweep_blocks_acq \
  gc2_finalizer_sweep_blocks_store_rlx \
  gc2_finalizer_sweep_blocks_add \
  gc2_finalizer_spawn_deferrals_acq \
  gc2_finalizer_spawn_deferrals_store_rlx \
  gc2_finalizer_spawn_deferrals_add \
  gc2_finalizer_spawn_release_wakes_acq \
  gc2_finalizer_spawn_release_wakes_store_rlx \
  gc2_finalizer_spawn_release_wakes_add; do
  if ! grep -qE "^[[:space:]]*${helper}[[:space:]]*[(]|static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 finalizer counters" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](finalizer_(queued|dequeued|mpsc_drained|enters|leaves|sweep_blocks|spawn_deferrals|spawn_release_wakes))([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](finalizer_(queued|dequeued|mpsc_drained|enters|leaves|sweep_blocks|spawn_deferrals|spawn_release_wakes))([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lib_base.c" \
    "$ROOT/src/lib_threading.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 finalizer counter access is forbidden; use gc2_finalizer_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]worker_active|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]worker_active' \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/tests/t-gc2-worker-scheduler.c" \
    "$ROOT/tests/t-gc2-traverse.c" \
    "$ROOT/tests/t-gc2-phase.c" \
    "$ROOT/tests/t-arena-gcsweep.c" || true); [ -n "$hits" ]; then
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
for helper in gc2_grey_pushed_acq \
  gc2_grey_pushed_store_rlx \
  gc2_grey_pushed_add \
  gc2_grey_drained_acq \
  gc2_grey_drained_store_rlx \
  gc2_grey_drained_add; do
  if ! grep -qE "static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 grey telemetry counters" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](grey_pushed|grey_drained)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](grey_pushed|grey_drained)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 grey telemetry counter access is forbidden; use gc2_grey_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]assist_(active|shift)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]assist_(active|shift)' \
    "$ROOT/src/lj_api.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 assist state access is forbidden; use gc2_assist_* helpers' >&2
  exit 1
fi
for helper in lj_tg_gc_assist_acq lj_tg_gc_assist_store_rlx
do
  if ! grep -q "$helper" "$ROOT/src/lj_tg.h"; then
    printf '%s\n' "missing TG GC assist helper: $helper" >&2
    exit 1
  fi
done
if hits=$(grep -RInE -- '->[[:space:]]*gc_assist([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc_assist([^[:alnum:]_]|$)' \
    "$ROOT/src"/lj_*.c "$ROOT/src"/lib_*.c "$ROOT/src"/lj_*.h 2>/dev/null | \
    grep -vF "$ROOT/src/lj_tg.h:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TG GC assist access is forbidden; use lj_tg_gc_assist_* helpers' >&2
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
for helper in gc2_sweep_legacy_ready_acq \
  gc2_sweep_legacy_ready_store_rlx \
  gc2_sweep_legacy_ready_rel; do
  if ! grep -qE "^[[:space:]]*${helper}[[:space:]]*[(]|static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 sweep close readiness" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]sweep_legacy_ready([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]sweep_legacy_ready([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 sweep close-readiness access is forbidden; use gc2_sweep_legacy_ready_* helpers' >&2
  exit 1
fi
for helper in gc2_sweep_to_idle_acq \
  gc2_sweep_to_idle_store_rlx \
  gc2_sweep_to_idle_add \
  gc2_preserve_abort_to_idle_acq \
  gc2_preserve_abort_to_idle_store_rlx \
  gc2_preserve_abort_to_idle_add; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 sweep-close telemetry" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](sweep_to_idle|preserve_abort_to_idle)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](sweep_to_idle|preserve_abort_to_idle)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 sweep-close telemetry access is forbidden; use gc2_sweep_to_idle_* or gc2_preserve_abort_to_idle_* helpers' >&2
  exit 1
fi
for helper in gc2_n_workers_acq \
  gc2_n_workers_store_rlx \
  gc2_n_workers_rel \
  gc2_worker_stop_acq \
  gc2_worker_stop_store_rlx \
  gc2_worker_stop_rel \
  gc2_worker_wake_acq \
  gc2_worker_wake_store_rlx \
  gc2_worker_wake_add \
  gc2_worker_started_acq \
  gc2_worker_started_store_rlx \
  gc2_worker_started_rel \
  gc2_worker_started_add \
  gc2_worker_exited_acq \
  gc2_worker_exited_store_rlx \
  gc2_worker_exited_rel \
  gc2_worker_exited_add; do
  if ! grep -qE "^[[:space:]]*${helper}[[:space:]]*[(]|static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 worker lifecycle state" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](n_workers|worker_stop|worker_wake|worker_started|worker_exited)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](worker_wake|worker_started|worker_exited)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lib_base.c" \
    "$ROOT/tests/t-gc2-worker-scheduler.c" \
    "$ROOT/tests/t-gc2-traverse.c" \
    "$ROOT/tests/t-gc2-phase.c" \
    "$ROOT/tests/t-arena-gcsweep.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 worker lifecycle state access is forbidden; use gc2_worker_* lifecycle helpers' >&2
  exit 1
fi
for helper in gc2_worker_runs_acq \
  gc2_worker_runs_store_rlx \
  gc2_worker_runs_add \
  gc2_worker_grey_drained_acq \
  gc2_worker_grey_drained_store_rlx \
  gc2_worker_grey_drained_add \
  gc2_worker_ssb_converted_acq \
  gc2_worker_ssb_converted_store_rlx \
  gc2_worker_ssb_converted_add \
  gc2_worker_weak_drained_acq \
  gc2_worker_weak_drained_store_rlx \
  gc2_worker_weak_drained_add \
  gc2_worker_idle_declares_acq \
  gc2_worker_idle_declares_store_rlx \
  gc2_worker_idle_declares_add \
  gc2_worker_busy_retries_acq \
  gc2_worker_busy_retries_store_rlx \
  gc2_worker_busy_retries_add \
  gc2_worker_wakes_acq \
  gc2_worker_wakes_store_rlx \
  gc2_worker_wakes_add \
  gc2_worker_parks_acq \
  gc2_worker_parks_store_rlx \
  gc2_worker_parks_add \
  gc2_worker_async_progress_acq \
  gc2_worker_async_progress_store_rlx \
  gc2_worker_async_progress_add; do
  if ! grep -qE "^[[:space:]]*${helper}[[:space:]]*[(]|static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 worker counters" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](worker_(runs|grey_drained|ssb_converted|weak_drained|idle_declares|busy_retries|wakes|parks|async_progress))([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](worker_(runs|grey_drained|ssb_converted|weak_drained|idle_declares|busy_retries|wakes|parks|async_progress))([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lib_base.c" \
    "$ROOT/tests/t-gc2-worker-scheduler.c" \
    "$ROOT/tests/t-gc2-traverse.c" \
    "$ROOT/tests/t-gc2-phase.c" \
    "$ROOT/tests/t-arena-gcsweep.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 worker counter access is forbidden; use gc2_worker_* counter helpers' >&2
  exit 1
fi
for helper in gc2_minor_sweep_arenas_acq \
  gc2_minor_sweep_arenas_store_rlx \
  gc2_minor_sweep_arenas_add \
  gc2_sweep_owner_runs_acq \
  gc2_sweep_owner_runs_store_rlx \
  gc2_sweep_owner_runs_add \
  gc2_sweep_owner_arenas_acq \
  gc2_sweep_owner_arenas_store_rlx \
  gc2_sweep_owner_arenas_add \
  gc2_sweep_owner_live_cells_acq \
  gc2_sweep_owner_live_cells_store_rlx \
  gc2_sweep_owner_live_cells_add; do
  if ! grep -qE "^[[:space:]]*${helper}[[:space:]]*[(]|static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 sweep telemetry counters" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*(gc2[.])?(minor_sweep_arenas|sweep_owner_runs|sweep_owner_arenas|sweep_owner_live_cells)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*(gc2[.])?(minor_sweep_arenas|sweep_owner_runs|sweep_owner_arenas|sweep_owner_live_cells)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lib_base.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 sweep telemetry counter access is forbidden; use gc2_* helpers' >&2
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
