/*
** Concurrent GC scaffold.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_gc2_c
#define LUA_CORE

#if LJ_GC2_PARANOIA
#include <stdio.h>
#include <stdlib.h>
#endif

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_chan.h"
#include "lj_gc2.h"
#include "lj_gc.h"
#include "lj_thr.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_safepoint.h"
#include "lj_arena.h"
#include "lj_tg.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#include "lj_clib.h"
#endif
#include "lj_trace.h"
#include "lj_mcode.h"
#include "lj_dispatch.h"

#define GC2_GREY_INIT	256u
#define GC2_GREY_LIMIT	((MSize)(LJ_MAX_MEM32 / sizeof(GCRef)))
#define GC2_WEAK_INIT	128u
#define GC2_WEAK_LIMIT	((MSize)(LJ_MAX_MEM32 / sizeof(GCRef)))
#define GC2_FINCLAIM_INIT	128u
#define GC2_FINCLAIM_LIMIT \
  ((MSize)(LJ_MAX_MEM32 / (sizeof(GCRef) + sizeof(TValue))))

static int gc2_grey_grow(global_State *g);
static int gc2_grey_empty(global_State *g);
static int gc2_weak_resize(global_State *g, MSize cap);
static void gc2_weak_reset(global_State *g);
static int gc2_finclaim_resize(global_State *g, MSize cap);
static void gc2_finclaim_reset(global_State *g);
static int gc2_tab_weak_mode(global_State *g, GCtab *t, GCtab *mt);
static void *gc2_worker_main(void *arg);
static void gc2_mark_tv_worker(global_State *g, cTValue *tv);

static uint32_t gc2_flush_and_drain_ssb(global_State *g)
{
  if (!g)
    return 0;
  if (gc2_phase_acq(g) == LJ_GC2_IDLE)
    return 0;
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  return lj_gc2_drain_ssb(g);
}

void lj_gc2_init(global_State *g)
{
  uint32_t i;
  gc2_gcpause_pct_store_rlx(g, 100);
  gc2_assist_shift_store_rlx(g,
    lj_gc2_assist_shift_from_stepmul(g->gc.stepmul));
  gc2_phase_store_rlx(g, LJ_GC2_IDLE);
  gc2_cycle_store_rlx(g, 0);
  gc2_cycle_leader_store_rlx(g, 0);
  gc2_hs_epoch_store_rlx(g, 0);
  gc2_hs_pending_store_rlx(g, 0);
  gc2_hs_actions_store_rlx(g, 0);
  gc2_hs_leader_store_rlx(g, 0);
  gc2_hs_signal_ns_store_rlx(g, 0);
  gc2_hs_ack_latency_samples_store_rlx(g, 0);
  gc2_hs_ack_latency_sum_store_rlx(g, 0);
  gc2_hs_ack_latency_max_store_rlx(g, 0);
  for (i = 0; i < LJ_GC2_HS_LATENCY_BUCKETS; i++)
    gc2_hs_ack_latency_bucket_store_rlx(g, i, 0);
  gc2_smr_reclaim_runs_store_rlx(g, 0);
  gc2_smr_reclaimed_store_rlx(g, 0);
  gc2_cycle_requests_store_rlx(g, 0);
  gc2_cycle_starts_store_rlx(g, 0);
  gc2_major_cycle_starts_store_rlx(g, 0);
  gc2_minor_cycle_requests_store_rlx(g, 0);
  gc2_minor_cycle_starts_store_rlx(g, 0);
  gc2_cycle_minor_requested_store_rlx(g, 0);
  gc2_cycle_sweep_minor_store_rlx(g, 0);
  gc2_minor_sweep_enabled_store_rlx(g, 0);
  gc2_cycle_roots_minor_store_rlx(g, 0);
  gc2_minor_roots_enabled_store_rlx(g, 0);
  gc2_minor_sweep_deferred_store_rlx(g, 0);
  gc2_minor_sweep_arenas_store_rlx(g, 0);
  gc2_minor_roots_deferred_store_rlx(g, 0);
  gc2_major_root_scans_store_rlx(g, 0);
  gc2_minor_root_scans_store_rlx(g, 0);
  gc2_minor_survival_base_live_store_rlx(g, 0);
  gc2_minor_survival_bytes_store_rlx(g, 0);
  gc2_minor_survival_pct_store_rlx(g, 0);
  gc2_minor_survival_threshold_pct_store_rlx(
    g, LJ_GC2_MINOR_SURVIVAL_MAJOR_PCT);
  gc2_minor_survival_major_requests_store_rlx(g, 0);
  gc2_force_major_store_rlx(g, 0);
  gc2_remembered_barriers_store_rlx(g, 0);
  gc2_remembered_pushed_store_rlx(g, 0);
  gc2_remembered_overflows_store_rlx(g, 0);
  gc2_remembered_filtered_store_rlx(g, 0);
  gc2_remembered_drained_store_rlx(g, 0);
  gc2_marks_this_round_store_rlx(g, 0);
  gc2_ssb_head_store_rlx(g, NULL);
  gc2_ssb_published_store_rlx(g, 0);
  gc2_ssb_drained_store_rlx(g, 0);
  gc2_ssb_items_published_store_rlx(g, 0);
  gc2_ssb_items_drained_store_rlx(g, 0);
  gc2_fixpoint_rounds_store_rlx(g, 0);
  gc2_fixpoint_hits_store_rlx(g, 0);
  gc2_mark_complete_runs_store_rlx(g, 0);
  gc2_mark_complete_hits_store_rlx(g, 0);
  gc2_mark_complete_peer_waits_store_rlx(g, 0);
  gc2_mark_to_weak_store_rlx(g, 0);
  gc2_weak_complete_runs_store_rlx(g, 0);
  gc2_weak_complete_progress_store_rlx(g, 0);
  gc2_weak_to_sweep_store_rlx(g, 0);
  gc2_sweep_legacy_ready_store_rlx(g, 0);
  gc2_sweep_to_idle_store_rlx(g, 0);
  gc2_preserve_abort_to_idle_store_rlx(g, 0);
  lj_gc2_alloc_since_store(g, 0);
  lj_gc2_cycle_alloc_store(g, 0);
  lj_gc2_trigger_store(g, 0);
  lj_gc2_hard_store(g, 0);
  gc2_assist_runs_store_rlx(g, 0);
  gc2_assist_grey_drained_store_rlx(g, 0);
  gc2_assist_ssb_converted_store_rlx(g, 0);
  gc2_assist_weak_drained_store_rlx(g, 0);
  gc2_jit_hard_checks_store_rlx(g, 0);
  gc2_interp_hard_checks_store_rlx(g, 0);
  gc2_jit_scoped_slots_retired_store_rlx(g, 0);
  gc2_assist_active_store_rlx(g, 0);
  gc2_generational_store_rlx(g, 0);
  gc2_grey_stack_store_rlx(g, NULL);
  gc2_grey_capacity_store_rlx(g, 0);
  gc2_grey_top_store_rlx(g, 0);
  gc2_grey_bottom_store_rlx(g, 0);
  gc2_grey_pushed_store_rlx(g, 0);
  gc2_grey_drained_store_rlx(g, 0);
  for (i = 0; i < LJ_GC2_WORKER_MAX; i++)
    g->gc2.worker_thread[i] = NULL;
  for (i = 0; i < LJ_GC2_WORKER_MAX; i++)
    g->gc2.worker_tg[i] = NULL;
  gc2_n_workers_store_rlx(g, 0);
  gc2_worker_stop_store_rlx(g, 0);
  gc2_worker_wake_store_rlx(g, 0);
  gc2_worker_started_store_rlx(g, 0);
  gc2_worker_exited_store_rlx(g, 0);
  gc2_worker_active_store_rlx(g, 0);
  gc2_worker_runs_store_rlx(g, 0);
  gc2_worker_grey_drained_store_rlx(g, 0);
  gc2_worker_ssb_converted_store_rlx(g, 0);
  gc2_worker_weak_drained_store_rlx(g, 0);
  gc2_worker_idle_declares_store_rlx(g, 0);
  gc2_worker_busy_retries_store_rlx(g, 0);
  gc2_worker_wakes_store_rlx(g, 0);
  gc2_worker_parks_store_rlx(g, 0);
  gc2_worker_async_progress_store_rlx(g, 0);
  gc2_tg_thread_roots_store_rlx(g, 0);
  gc2_tg_cur_roots_store_rlx(g, 0);
  gc2_tg_trace_roots_store_rlx(g, 0);
  gc2_thread_scan_claims_store_rlx(g, 0);
  gc2_thread_scan_busy_store_rlx(g, 0);
  gc2_thread_scan_requeues_store_rlx(g, 0);
  gc2_thread_scan_owner_scans_store_rlx(g, 0);
  gc2_thread_scan_needscan_store_rlx(g, 0);
  gc2_thread_scan_owner_needscans_store_rlx(g, 0);
  gc2_thread_scan_dirty_misses_store_rlx(g, 0);
  gc2_sweep_owner_runs_store_rlx(g, 0);
  gc2_sweep_owner_arenas_store_rlx(g, 0);
  gc2_sweep_owner_live_cells_store_rlx(g, 0);
  gc2_sweep_live_updates_store_rlx(g, 0);
  gc2_sweep_live_huge_bytes_store_rlx(g, 0);
  gc2_live_estimate_store_rlx(g, 0);
  gc2_weak_stack_store_rlx(g, NULL);
  gc2_weak_ready_store_rlx(g, NULL);
  gc2_weak_capacity_store_rlx(g, 0);
  gc2_weak_count_store_rlx(g, 0);
  gc2_weak_tables_seen_store_rlx(g, 0);
  gc2_weak_tables_weakkey_store_rlx(g, 0);
  gc2_weak_tables_weakval_store_rlx(g, 0);
  gc2_weak_tables_allweak_store_rlx(g, 0);
  gc2_weak_tables_queued_store_rlx(g, 0);
  gc2_weak_tables_overflow_store_rlx(g, 0);
  gc2_weak_scan_cursor_store_rlx(g, 0);
  gc2_weak_scan_runs_store_rlx(g, 0);
  gc2_weak_scan_tables_store_rlx(g, 0);
  gc2_weak_scan_slots_store_rlx(g, 0);
  gc2_weak_scan_clearable_store_rlx(g, 0);
  gc2_weak_clear_cursor_store_rlx(g, 0);
  gc2_weak_clear_runs_store_rlx(g, 0);
  gc2_weak_clear_tables_store_rlx(g, 0);
  gc2_weak_clear_slots_store_rlx(g, 0);
  gc2_weak_clear_cleared_store_rlx(g, 0);
  gc2_weak_legacy_skipped_store_rlx(g, 0);
  gc2_weak_legacy_fallbacks_store_rlx(g, 0);
  gc2_weak_legacy_backfills_store_rlx(g, 0);
  gc2_weak_legacy_backfill_tables_store_rlx(g, 0);
  gc2_weak_legacy_backfill_slots_store_rlx(g, 0);
  gc2_weak_legacy_backfill_cleared_store_rlx(g, 0);
  gc2_finreg_cdata_sets_store_rlx(g, 0);
  gc2_finreg_cdata_clears_store_rlx(g, 0);
  gc2_finreg_cdata_queued_store_rlx(g, 0);
  gc2_finreg_cdata_sweep_queued_store_rlx(g, 0);
  gc2_finreg_cdata_pweak_queued_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_objvec_store_rlx(g, NULL);
  gc2_finreg_cdata_preclaim_finvec_store_rlx(g, NULL);
  gc2_finreg_cdata_preclaim_capacity_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_head_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_count_store_rlx(g, 0);
  gc2_finreg_cdata_pweak_claimed_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_overflow_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_dispatched_store_rlx(g, 0);
  gc2_finreg_cdata_order_seen_store_rlx(g, 0);
  gc2_finreg_cdata_order_claimed_store_rlx(g, 0);
  gc2_finreg_cdata_order_unlinked_store_rlx(g, 0);
  gc2_finreg_cdata_order_queued_store_rlx(g, 0);
  gc2_finreg_cdata_order_retired_store_rlx(g, 0);
  gc2_finreg_cdata_order_tombstones_store_rlx(g, 0);
  gc2_finreg_cdata_order_fallbacks_store_rlx(g, 0);
  gc2_finreg_cdata_pending_order_hits_store_rlx(g, 0);
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  gc2_finreg_cdata_preclaim_test_fail_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_publish_pause_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_publish_paused_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_publish_release_store_rlx(g, 0);
#endif
  gc2_finreg_udata_sets_store_rlx(g, 0);
  gc2_finreg_udata_clears_store_rlx(g, 0);
  gc2_finreg_udata_queued_store_rlx(g, 0);
  gc2_finreg_udata_head_store_rlx(g, NULL);
  gc2_finreg_udata_retired_store_rlx(g, NULL);
  gc2_finreg_udata_registered_store_rlx(g, 0);
  gc2_finreg_udata_retired_nodes_store_rlx(g, 0);
  gc2_finreg_udata_discovered_store_rlx(g, 0);
  gc2_finreg_udata_forgets_store_rlx(g, 0);
  gc2_finalizer_mpsc_store_rlx(g, NULL);
  gc2_finalizer_tail_store_rlx(g, NULL);
  gc2_finalizer_active_store_rlx(g, 0);
  gc2_finalizer_owner_store_rlx(g, 0);
  gc2_finalizer_queued_store_rlx(g, 0);
  gc2_finalizer_dequeued_store_rlx(g, 0);
  gc2_finalizer_mpsc_drained_store_rlx(g, 0);
  gc2_finalizer_enters_store_rlx(g, 0);
  gc2_finalizer_leaves_store_rlx(g, 0);
  gc2_finalizer_sweep_blocks_store_rlx(g, 0);
  gc2_finalizer_spawn_deferrals_store_rlx(g, 0);
  gc2_finalizer_spawn_release_wakes_store_rlx(g, 0);
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  gc2_finalizer_drain_test_pause_store_rlx(g, 0);
  gc2_finalizer_drain_test_paused_store_rlx(g, 0);
  gc2_finalizer_drain_test_release_store_rlx(g, 0);
#endif
  gc2_weak_keys_marked_store_rlx(g, 0);
  gc2_weak_values_marked_store_rlx(g, 0);
  gc2_tg_list_store_rlx(g, NULL);
  gc2_n_threads_store_rlx(g, 0);
  lj_gc2_update_pacing(g);
  lj_tg_attach(g, G2TG(g));  /* 05 section 5.4.1 main TG registration. */
}

void lj_gc2_fini(global_State *g)
{
  lj_gc2_worker_stop(g);
  (void)lj_tg_reclaim_dead(g);
  if (g) {
    GCRef *grey_stack = gc2_grey_stack_acq(g);
    if (grey_stack) {
      lj_mem_freevec(g, grey_stack, gc2_grey_capacity_acq(g), GCRef);
      gc2_grey_stack_store_rlx(g, NULL);
      gc2_grey_capacity_store_rlx(g, 0);
      gc2_grey_top_store_rlx(g, 0);
      gc2_grey_bottom_store_rlx(g, 0);
    }
  }
  if (g) {
    GCRef *weak_stack = gc2_weak_stack_acq(g);
    if (weak_stack) {
      lj_mem_freevec(g, weak_stack, gc2_weak_capacity_acq(g), GCRef);
      gc2_weak_stack_store_rlx(g, NULL);
    }
  }
  if (g) {
    uint8_t *weak_ready = gc2_weak_ready_acq(g);
    if (weak_ready) {
      lj_mem_freevec(g, weak_ready, gc2_weak_capacity_acq(g), uint8_t);
      gc2_weak_ready_store_rlx(g, NULL);
    }
  }
  if (g) {
    gc2_weak_capacity_store_rlx(g, 0);
    gc2_weak_count_store_rlx(g, 0);
  }
  if (g) {
    GCRef *obj = gc2_finreg_cdata_preclaim_objvec_acq(g);
    if (obj) {
      lj_mem_freevec(g, obj, gc2_finreg_cdata_preclaim_capacity_acq(g), GCRef);
      gc2_finreg_cdata_preclaim_objvec_store_rlx(g, NULL);
    }
  }
  if (g) {
    TValue *fin = gc2_finreg_cdata_preclaim_finvec_acq(g);
    if (fin) {
      lj_mem_freevec(g, fin, gc2_finreg_cdata_preclaim_capacity_acq(g), TValue);
      gc2_finreg_cdata_preclaim_finvec_store_rlx(g, NULL);
    }
  }
  if (g) {
    gc2_finreg_cdata_preclaim_capacity_store_rlx(g, 0);
    gc2_finreg_cdata_preclaim_head_store_rlx(g, 0);
    gc2_finreg_cdata_preclaim_count_store_rlx(g, 0);
  }
  if (g) {
    GC2FinRegUDataNode *node =
      gc2_finreg_udata_head_xchg_acqrel(g, NULL);
    while (node) {
      GC2FinRegUDataNode *next = gc2_finreg_udata_next_acq(node);
      if (gc2_finreg_udata_active_acq(node))
	lj_mem_freet(g, node);
      node = next;
    }
    node = gc2_finreg_udata_retired_xchg_acqrel(g, NULL);
    while (node) {
      GC2FinRegUDataNode *next = gc2_finreg_udata_retired_next_acq(node);
      lj_mem_freet(g, node);
      node = next;
    }
  }
}

void lj_gc2_worker_wake(global_State *g)
{
  uint32_t n;
  if (!g)
    return;
  n = gc2_n_workers_acq(g);
  if (n == 0)
    return;
  gc2_worker_wakes_add(g, 1);
  (void)gc2_worker_wake_add(g, 1);
  gc2_worker_wake_futex_wake(g, (int)n);
}

static int gc2_worker_arena_internal(global_State *g)
{
  return g && g->allocf == lj_arena_allocf && g->main_tg &&
	 lj_tg_flags_test_acq(g->main_tg, TGF_ARENA_INTERNAL);
}

static int gc2_worker_tg_registered(global_State *g, TGState *target)
{
  TGState *tg;
  if (!g || !target)
    return 0;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg))
    if (tg == target)
      return 1;
  return 0;
}

static int gc2_worker_release_tg_slot(global_State *g, uint32_t i)
{
  TGState *tg;
  if (!g || i >= LJ_GC2_WORKER_MAX)
    return 0;
  tg = (TGState *)g->gc2.worker_tg[i];
  if (!tg)
    return 1;
  if (gc2_worker_tg_registered(g, tg))
    return 0;
  lj_tg_fini_thread(g, tg);
  lj_mem_free(g, tg, sizeof(TGState));
  g->gc2.worker_tg[i] = NULL;
  return 1;
}

static int gc2_worker_release_tg_slots(global_State *g)
{
  uint32_t i;
  int ok = 1;
  if (!g)
    return 0;
  (void)lj_tg_reclaim_dead(g);
  for (i = 0; i < LJ_GC2_WORKER_MAX; i++)
    ok &= gc2_worker_release_tg_slot(g, i);
  return ok;
}

static int gc2_worker_prepare_tg_slots(global_State *g)
{
  return gc2_worker_release_tg_slots(g);
}

static int gc2_worker_start_count(global_State *g, uint32_t n)
{
  GCobj *mainobj;
  lua_State *L;
  uint32_t i;
  int rc, wait;
  if (!g || n == 0)
    return 1;
  if (n > LJ_GC2_WORKER_MAX)
    n = LJ_GC2_WORKER_MAX;
  if (gc2_n_workers_acq(g) != 0)
    return 1;
  mainobj = gcref_acq(g->mainthref);
  L = mainobj ? &mainobj->th : NULL;
  if (!L)
    return 0;
  if (!gc2_worker_prepare_tg_slots(g))
    return 0;
  gc2_worker_stop_rel(g, 0);
  gc2_worker_started_rel(g, 0);
  gc2_worker_exited_rel(g, 0);
  gc2_n_workers_rel(g, n);  /* 05 section 5.6.3 parked pool. */
  for (i = 0; i < n; i++) {
    LJThr *thr = (LJThr *)lj_mem_new(L, sizeof(LJThr));
    TGState *tg = lj_mem_newt(L, sizeof(TGState), TGState);
    thr->tid = 0;
    lj_tg_init_thread(g, tg, NULL, gc2_worker_arena_internal(g));
    thr->tid = lj_thr_newid();
    tg->tid = thr->tid;
    tg->alloc.owner_tid = thr->tid;
    g->gc2.worker_thread[i] = thr;
    g->gc2.worker_tg[i] = tg;
    rc = lj_thr_create(thr, gc2_worker_main, tg);
    if (rc != 0) {
      g->gc2.worker_thread[i] = NULL;
      g->gc2.worker_tg[i] = NULL;
      lj_tg_fini_thread(g, tg);
      lj_mem_free(g, tg, sizeof(TGState));
      lj_mem_free(g, thr, sizeof(LJThr));
      lj_gc2_worker_stop(g);
      return 0;
    }
  }
  for (wait = 0; wait < 1000; wait++) {
    uint32_t started = gc2_worker_started_acq(g);
    if (started >= n)
      return 1;
    gc2_worker_started_futex_wait(g, started, 1000000);
  }
  if (gc2_worker_started_acq(g) >= n)
    return 1;
  lj_gc2_worker_stop(g);
  return 0;
}

int lj_gc2_workers_set(global_State *g, uint32_t n)
{
  uint32_t old;
  if (!g)
    return 0;
  if (n > LJ_GC2_WORKER_MAX)
    n = LJ_GC2_WORKER_MAX;
  old = gc2_n_workers_acq(g);
  if (old == n)
    return 1;
  if (old != 0)
    lj_gc2_worker_stop(g);
  if (n == 0)
    return 1;
  return gc2_worker_start_count(g, n);
}

int lj_gc2_worker_start(global_State *g)
{
  if (!g)
    return 0;
  if (gc2_n_workers_acq(g) != 0)
    return 1;
  return gc2_worker_start_count(g, 1);
}

void lj_gc2_worker_stop(global_State *g)
{
  uint32_t i, any = 0;
  TGState *self;
  uint8_t was_native = 0;
  if (!g)
    return;
  for (i = 0; i < LJ_GC2_WORKER_MAX; i++)
    any |= g->gc2.worker_thread[i] != NULL;
  if (!any) {
    (void)gc2_worker_release_tg_slots(g);
    gc2_n_workers_rel(g, 0);
    return;
  }
  gc2_worker_stop_rel(g, 1);
  lj_gc2_worker_wake(g);
  self = lj_thr_get_tg_fallback(g);
  if (self) {
    was_native = lj_tg_in_native_acq(self);
    lj_tg_in_native_rel(self, 1);  /* Join wait can remote-ack workers. */
  }
  for (i = 0; i < LJ_GC2_WORKER_MAX; i++) {
    LJThr *thr = (LJThr *)g->gc2.worker_thread[i];
    if (!thr)
      continue;
    (void)lj_thr_join(thr, NULL);
    g->gc2.worker_thread[i] = NULL;
    lj_mem_free(g, thr, sizeof(LJThr));
  }
  if (self)
    lj_tg_in_native_rel(self, was_native);
  (void)gc2_worker_release_tg_slots(g);
  gc2_n_workers_rel(g, 0);
}

static void *gc2_worker_main(void *arg)
{
  TGState *tg = (TGState *)arg;
  global_State *g = tg ? tg->gl : NULL;
  if (!g)
    return NULL;
  lj_thr_set_tg(tg);
  lj_native_enter(tg);  /* No Lua stack: remote handshakes may ack this TG. */
  lj_tg_attach(g, tg);
  (void)gc2_worker_started_add(g, 1);
  gc2_worker_started_futex_wake(g, LJ_GC2_WORKER_MAX);
  while (gc2_worker_stop_acq(g) == 0) {
    uint32_t total = 0;
    for (;;) {
      uint32_t step;
      if (gc2_worker_stop_acq(g) != 0)
	break;
      step = lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH);
      if (step == 0)
	break;
      total = step > ~(uint32_t)0 - total ? ~(uint32_t)0 : total + step;
    }
    if (total == 0 && gc2_phase_acq(g) == LJ_GC2_SWEEP &&
	lj_gc2_sweep_to_idle(g))
      total = 1;  /* 05 section 5.8: scheduler-owned idle close. */
    if (total)
      gc2_worker_async_progress_add(g, total);
    if (gc2_worker_stop_acq(g) != 0)
      break;
    gc2_worker_parks_add(g, 1);
    {
      uint32_t wake = gc2_worker_wake_acq(g);
      if (gc2_worker_stop_acq(g) != 0)
	break;
      gc2_worker_wake_futex_wait(g, wake, -1);
    }
  }
  lj_tg_detach(g, tg);
  lj_thr_set_tg(NULL);
  (void)gc2_worker_exited_add(g, 1);
  gc2_worker_exited_futex_wake(g, LJ_GC2_WORKER_MAX);
  return NULL;
}

uint64_t lj_gc2_flush_alloc(global_State *g, TGState *tg)
{
  uint64_t bytes;
  if (!g || !tg)
    return 0;
  bytes = lj_tg_local_total_xchg_acqrel(tg, 0);
  if (bytes != 0)
    lj_gc2_alloc_since_add(g, bytes);  /* 05 section 5.11. */
  return bytes;
}

static int gc2_request_cycle(global_State *g, TGState *tg)
{
  uint32_t expect = 0;
  uint32_t tid = tg ? la_load32_acq(&tg->tid) : 0;
  if (tid == 0)
    return 0;
  if (gc2_phase_acq(g) != LJ_GC2_IDLE)
    return 0;
  if (lj_gc_threshold_load(g) == LJ_MAX_MEM)
    return 0;  /* Honor collectgarbage("stop"). */
  if (!gc2_cycle_leader_cas(g, &expect, tid))
    return 0;  /* 05 section 5.11 nonblocking cycle-request token. */
  gc2_cycle_requests_add(g, 1);  /* 05 section 5.11 telemetry. */
  lj_gc_threshold_store(g, lj_gc_total_load(g));  /* Legacy cycle-driver bridge. */
  return 1;
}

static void gc2_maybe_trigger_cycle(global_State *g, TGState *tg)
{
  if (gc2_phase_acq(g) != LJ_GC2_IDLE)
    return;
  if (lj_gc2_alloc_since_load(g) <=
      lj_gc2_trigger_load(g))  /* 05 section 5.11 trigger. */
    return;
  (void)gc2_request_cycle(g, tg);
}

void lj_gc2_account_alloc(global_State *g, TGState *tg, GCSize bytes)
{
  uint64_t old;
  if (!g || !tg || bytes == 0)
    return;
  old = lj_tg_local_total_add_rlx(tg, (uint64_t)bytes);
  if (old + (uint64_t)bytes < old || old + (uint64_t)bytes >= LJ_GC2_ACCT_FLUSH)
    (void)lj_gc2_flush_alloc(g, tg);
  gc2_maybe_trigger_cycle(g, tg);
  if (lj_gc2_hard_limit_reached(g))  /* 05 section 5.11 hard limit. */
    (void)lj_gc2_assist(g, tg);
}

uint32_t lj_gc2_assist_shift_from_stepmul(uint32_t stepmul)
{
  uint32_t shift = 0;
  uint32_t work = stepmul < 100 ? 1u : stepmul / 100u;
  while (work > 1u && shift < 8u) {
    work = (work + 1u) >> 1;
    shift++;
  }
  return shift;
}

void lj_gc2_update_pacing(global_State *g)
{
  uint64_t live, legacy_live, gc2_live, trigger, hard;
  uint32_t pct;
  if (!g)
    return;
  legacy_live = g->gc.estimate ? g->gc.estimate : lj_gc_total_load(g);
  gc2_live = gc2_live_estimate_acq(g);
  live = gc2_live > legacy_live ? gc2_live : legacy_live;
  if (live < LJ_GC2_ACCT_FLUSH)
    live = LJ_GC2_ACCT_FLUSH;
  pct = gc2_gcpause_pct_acq(g);
  if (pct == 0)
    pct = 100;
  trigger = (live / 100u) * (uint64_t)pct +
	    ((live % 100u) * (uint64_t)pct) / 100u;
  if (trigger < LJ_GC2_ACCT_FLUSH)
    trigger = LJ_GC2_ACCT_FLUSH;
  hard = trigger > ~(uint64_t)0 / 2u ? ~(uint64_t)0 : trigger * 2u;
  lj_gc2_trigger_store(g, trigger);  /* 05 section 5.11. */
  lj_gc2_hard_store(g, hard);  /* 05 section 5.11. */
}

static void gc2_reset_alloc_trigger(global_State *g)
{
  TGState *tg;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg))
    (void)lj_gc2_flush_alloc(g, tg);
  lj_gc2_cycle_alloc_store(g, lj_gc2_alloc_since_xchg(g, 0));
}

static TGState *gc2_tg_for_mem(global_State *g, const void *p)
{
  if (p) {
    uint32_t owner_tid = lj_arena_of(p)->hdr.owner_tid;
    TGState *owner = lj_tg_find_owner(g, owner_tid);
    if (owner)
      return owner;
  }
  return G2TG(g);
}

static void gc2_clear_marks(TGState *tg)
{
  if (tg && lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL)) {
    lj_arena_alloc_clear_marks(&tg->alloc);
    if (lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      lj_arena_hugetab_clear_marks(&tg->huge);
  }
}

static void gc2_clear_marks_all(global_State *g)
{
  TGState *tg;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg))
    gc2_clear_marks(tg);
}

static void gc2_mark_strtab_mem(global_State *g)
{
  StrTabHdr *hdr;
  hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.tabh);
  if (hdr)
    lj_gc2_markmem(g, hdr);
  for (hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.retired);
       hdr != NULL;
       hdr = lj_str_retired_next_acq(hdr))
    lj_gc2_markmem(g, hdr);
}

static void gc2_mark_tab_retired_mem(global_State *g)
{
  TabNodeRetire *ret;
  for (ret = (TabNodeRetire *)la_loadptr_acq(
	 (void *const *)&g->tab.retired_nodes);
       ret != NULL;
       ret = lj_tab_node_retired_next_acq(ret)) {
    lj_gc2_markmem(g, ret);
    if (la_load32_acq(&ret->armed))
      lj_gc2_markmem(g, lj_tab_node_hdrw(ret->node));
  }
}

#if LJ_HASJIT
static void gc2_traverse_trace(global_State *g, GCtrace *T);
static int gc2_mark_trace_root(global_State *g, TraceNo traceno)
{
  GCtrace *T;
  if (traceno == 0)
    return 0;
  T = traceref(G2J(g), traceno);
  if (!T)
    return 0;
  lj_gc2_markobj(g, obj2gco(T));
  return 1;
}
#endif

void lj_gc2_legacy_mark_begin(global_State *g)
{
  TGState *tg;
  uint32_t leader;
  uint32_t forced_major, minor_requested, sweep_minor, roots_minor, drained;
  if (!g)
    return;
  tg = G2TG(g);
  forced_major = gc2_force_major_xchg_acqrel(g, 0);
  minor_requested = !forced_major && gc2_generational_acq(g) != 0;
  sweep_minor = minor_requested &&
    gc2_minor_sweep_enabled_acq(g) != 0;
  roots_minor = sweep_minor &&
    gc2_minor_roots_enabled_acq(g) != 0;
  gc2_cycle_minor_requested_rel(g, minor_requested);
  gc2_cycle_sweep_minor_rel(g, sweep_minor);
  gc2_cycle_roots_minor_rel(g, roots_minor);
  if (minor_requested)
    gc2_minor_cycle_requests_add(g, 1);
  if (minor_requested && !sweep_minor)
    gc2_minor_sweep_deferred_add(g, 1);
  if (minor_requested && !roots_minor)
    gc2_minor_roots_deferred_add(g, 1);
  if (roots_minor)
    gc2_minor_cycle_starts_add(g, 1);
  else
    gc2_major_cycle_starts_add(g, 1);
  /* Publish MARK before clearing the request token, so late allocators stop. */
  gc2_phase_rel(g, LJ_GC2_MARK);
  leader = gc2_cycle_leader_xchg_acqrel(g, 0);
  if (gc2_tg_list_acq(g) == NULL && tg != NULL)
    lj_tg_attach(g, tg);
  (void)gc2_cycle_inc_acqrel(g);
  if (leader)
    gc2_cycle_starts_add(g, 1);
  gc2_marks_this_round_store_rlx(g, 0);
  if (!minor_requested)
    (void)gc2_flush_and_drain_ssb(g);  /* Discard remembered roots for majors. */
  (void)lj_tg_reclaim_dead(g);
  lj_assertG(gc2_grey_empty(g), "gc2 grey deque not empty at mark begin");
  gc2_grey_top_store_rlx(g, 0);
  gc2_grey_bottom_store_rlx(g, 0);
  if (gc2_grey_capacity_acq(g) == 0)
    (void)gc2_grey_grow(g);
  gc2_weak_reset(g);
  gc2_finclaim_reset(g);
  gc2_reset_alloc_trigger(g);
  /* Minor cycles keep old black marks; remembered SSB entries still traverse. */
  if (!sweep_minor)
    gc2_clear_marks_all(g);
  if (minor_requested) {
    lj_gc2_handshake(g, LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK|
		     LJ_GC2_HS_FLUSH_SSB|LJ_GC2_HS_RESET_ALLOC);
    drained = lj_gc2_drain_ssb(g);
    if (drained)
      gc2_remembered_drained_add(g, drained);
  } else {
    lj_gc2_handshake(g, LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK|
		     LJ_GC2_HS_RESET_ALLOC);
  }
  lj_gc2_worker_wake(g);  /* 05 section 5.6.3 parked worker scheduler. */
}

void lj_gc2_force_major(global_State *g)
{
  if (g)
    gc2_force_major_rel(g, 1);
}

static uint32_t gc2_idle_barrier_actions(global_State *g, int flush_ssb)
{
  uint32_t actions = LJ_GC2_HS_ALLOC_WHITE;
  if (flush_ssb)
    actions |= LJ_GC2_HS_FLUSH_SSB;
  if (gc2_generational_acq(g))
    actions |= LJ_GC2_HS_ENABLE_BARRIER;
  else
    actions |= LJ_GC2_HS_DISABLE_BARRIER;
  return actions;
}

static void gc2_update_public_minor_gates(global_State *g)
{
  uint32_t enabled;
  if (!g)
    return;
  enabled = gc2_generational_acq(g) != 0;
  gc2_minor_sweep_enabled_rel(g, enabled);
  gc2_minor_roots_enabled_rel(g, enabled);
}

static uint32_t gc2_ratio_pct(uint64_t num, uint64_t den)
{
  if (num == 0 || den == 0)
    return 0;
  if (num >= den)
    return 100;
  while (num > ~(uint64_t)0 / 100u) {
    num >>= 1;
    den = (den + 1u) >> 1;
    if (den == 0 || num >= den)
      return 100;
  }
  return (uint32_t)((num * 100u) / den);
}

void lj_gc2_update_minor_survival_policy(global_State *g, uint64_t live)
{
  uint64_t base, alloc, survived = 0;
  uint32_t pct = 0, threshold;
  int minor;
  if (!g)
    return;
  base = gc2_minor_survival_base_live_acq(g);
  alloc = lj_gc2_cycle_alloc_load(g);
  minor = gc2_cycle_sweep_minor_acq(g) != 0;
  if (minor && live > base && alloc != 0) {
    survived = live - base;
    pct = gc2_ratio_pct(survived, alloc);
  }
  gc2_minor_survival_bytes_rel(g, survived);
  gc2_minor_survival_pct_rel(g, pct);
  gc2_minor_survival_base_live_rel(g, live);
  threshold = gc2_minor_survival_threshold_pct_acq(g);
  if (threshold == 0)
    threshold = LJ_GC2_MINOR_SURVIVAL_MAJOR_PCT;
  if (minor && pct >= threshold &&
      gc2_generational_acq(g) != 0) {
    gc2_minor_survival_major_requests_add(g, 1);
    lj_gc2_force_major(g);
  }
}

void lj_gc2_set_generational(global_State *g, int enabled)
{
  uint32_t want = enabled ? 1u : 0u;
  if (!g)
    return;
  if (gc2_generational_acq(g) == want)
    return;
  gc2_generational_rel(g, want);
  if (want)
    lj_gc2_force_major(g);  /* First generational cycle establishes old marks. */
  else {
    gc2_force_major_rel(g, 0);
    gc2_minor_survival_pct_rel(g, 0);
    gc2_minor_survival_bytes_rel(g, 0);
    gc2_update_public_minor_gates(g);
  }
  if (gc2_phase_acq(g) == LJ_GC2_IDLE)
    lj_gc2_handshake(g, gc2_idle_barrier_actions(g, 0));
}

void lj_gc2_legacy_weak_begin(global_State *g)
{
  lj_gc2_mark_to_weak(g);
}

void lj_gc2_mark_to_weak(global_State *g)
{
  uint32_t expect = LJ_GC2_MARK;
  if (!g || !gc2_phase_cas(g, &expect, LJ_GC2_WEAK))
    return;
  gc2_mark_to_weak_add(g, 1);
  lj_gc2_worker_wake(g);  /* 05 section 5.6.3 parked worker scheduler. */
}

void lj_gc2_legacy_sweep_begin(global_State *g)
{
  lj_gc2_weak_to_sweep(g);
}

void lj_gc2_weak_to_sweep(global_State *g)
{
  uint32_t expect = LJ_GC2_WEAK;
  if (!g)
    return;
  if (!gc2_phase_cas(g, &expect, LJ_GC2_SWEEP))
    return;
  gc2_sweep_legacy_ready_rel(g, 0);
  gc2_weak_to_sweep_add(g, 1);
  lj_gc2_handshake(g, LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_FLUSH_SSB|
		   (gc2_cycle_sweep_minor_acq(g) ?
		    LJ_GC2_HS_ALLOC_WHITE : LJ_GC2_HS_ALLOC_BLACK));
  (void)lj_gc2_drain_ssb(g);  /* Temporary worker-consume stand-in. */
  (void)lj_tg_reclaim_dead(g);
  lj_gc2_worker_wake(g);  /* 05 section 5.6.3 parked worker scheduler. */
}

static TGState *gc2_finalizer_current_tg(global_State *g)
{
  TGState *tg = lj_thr_get_tg();
  if (!g || !tg || tg->gl == g)
    return tg;
  /* Missing TLS can be an unattached helper; only stale TLS can fallback. */
  return g->main_tg;
}

static uint32_t gc2_finalizer_current_owner(global_State *g)
{
  TGState *tg = gc2_finalizer_current_tg(g);
  uint32_t tid = tg ? la_load32_acq(&tg->tid) : 0;
  return tid != 0 ? tid : ~(uint32_t)0;
}

static int gc2_finalizer_owned_by_current(global_State *g)
{
  TGState *tg;
  uint32_t owner;
  if (!g)
    return 0;
  owner = gc2_finalizer_owner_acq(g);
  if (owner == 0)
    return 0;
  tg = gc2_finalizer_current_tg(g);
  return owner == (tg ? la_load32_acq(&tg->tid) : ~(uint32_t)0);
}

void lj_gc2_finalizer_enqueue(global_State *g, GCobj *o)
{
  GCobj *head;
  if (!g || !o)
    return;
  do {
    head = gc2_finalizer_mpsc_acq(g);
    if (head)
      lj_obj_setgcwrel(o, head);
    else
      lj_obj_setgcwnullrel(o);
  } while (!gc2_finalizer_mpsc_cas(g, &head, o));
  gc2_finalizer_queued_add(g, 1);
  if (head == NULL)
    lj_gc2_worker_wake(g);  /* 05 section 5.8: finalizer work became visible. */
}

void lj_gc2_finalizer_drain_owned(global_State *g)
{
  GCobj *stack, *rev = NULL, *newtail = NULL, *oldtail;
  size_t n = 0;
  if (!g)
    return;
  lj_assertG(gc2_finalizer_owned_by_current(g),
	     "gc2 finalizer drain requires owner");
  stack = gc2_finalizer_mpsc_xchg_acqrel(g, NULL);
  while (stack) {
    GCobj *next = lj_obj_gcw_acq(stack);
    if (newtail == NULL)
      newtail = stack;
    if (rev)
      lj_obj_setgcwrel(stack, rev);
    else
      lj_obj_setgcwnullrel(stack);
    rev = stack;
    stack = next;
    n++;
  }
  if (!rev)
    return;
  oldtail = gc2_finalizer_tail_acq(g);
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  if (gc2_finalizer_drain_test_pause_xchg_acqrel(g, 0) != 0) {
    gc2_finalizer_drain_test_paused_rel(g, 1);
    while (gc2_finalizer_drain_test_release_acq(g) == 0)
      la_cpu_pause();
    gc2_finalizer_drain_test_paused_rel(g, 0);
  }
#endif
  if (oldtail) {
    GCobj *head = lj_obj_gcw_acq(oldtail);
    lj_obj_setgcwrel(newtail, head);
    lj_obj_setgcwrel(oldtail, rev);
    gc2_finalizer_tail_rel(g, newtail);
  } else {
    lj_obj_setgcwrel(newtail, rev);
    gc2_finalizer_tail_rel(g, newtail);
  }
  gc2_finalizer_mpsc_drained_add(g, n);
}

void lj_gc2_finalizer_drain(global_State *g)
{
  if (!g)
    return;
  lj_gc2_finalizer_enter(g);
  lj_gc2_finalizer_drain_owned(g);
  lj_gc2_finalizer_leave(g);
}

GCobj *lj_gc2_finalizer_dequeue_owned(global_State *g)
{
  GCobj *tail, *o;
  if (!g)
    return NULL;
  lj_assertG(gc2_finalizer_owned_by_current(g),
	     "gc2 finalizer dequeue requires owner");
  tail = gc2_finalizer_tail_acq(g);
  if (!tail) {
    lj_gc2_finalizer_drain_owned(g);
    tail = gc2_finalizer_tail_acq(g);
    if (!tail)
      return NULL;
  }
  o = lj_obj_gcw_acq(tail);
  lj_assertG(o != NULL, "broken gc2 finalizer queue");
  if (!o)
    return NULL;
  if (o == tail) {
    gc2_finalizer_tail_rel(g, NULL);
  } else {
    lj_obj_setgcwrel(tail, lj_obj_gcw_acq(o));
  }
  lj_obj_setgcwnullrel(o);
  gc2_finalizer_dequeued_add(g, 1);
  return o;  /* 05 section 5.8: GC2-owned finalizer queue bridge. */
}

GCobj *lj_gc2_finalizer_dequeue(global_State *g)
{
  GCobj *o;
  if (!g)
    return NULL;
  lj_gc2_finalizer_enter(g);
  o = lj_gc2_finalizer_dequeue_owned(g);
  lj_gc2_finalizer_leave(g);
  return o;
}

int lj_gc2_finalizer_try_enter(global_State *g)
{
  uint32_t owner, old;
  if (!g)
    return 0;
  owner = gc2_finalizer_current_owner(g);
  for (;;) {
    old = gc2_finalizer_active_acq(g);
    if (old != 0) {
      if (gc2_finalizer_owner_acq(g) != owner)
	return 0;  /* 05 section 5.8: peer finalizer dispatch backs off. */
      if (old == ~(uint32_t)0)
	return 0;
      if (gc2_finalizer_active_cas(g, &old, old + 1)) {
	gc2_finalizer_enters_add(g, 1);
	return 1;
      }
      continue;
    }
    if (gc2_finalizer_active_cas(g, &old, 1)) {
      gc2_finalizer_owner_rel(g, owner);
      gc2_finalizer_enters_add(g, 1);
      return 1;
    }
  }
}

void lj_gc2_finalizer_enter(global_State *g)
{
  if (!g)
    return;
  while (!lj_gc2_finalizer_try_enter(g))
    la_cpu_pause();
}

void lj_gc2_finalizer_leave(global_State *g)
{
  uint32_t old;
  int wake_worker = 0;
  if (!g)
    return;
  for (;;) {
    old = gc2_finalizer_active_acq(g);
    lj_assertG(old != 0, "gc2 finalizer active underflow");
    if (old == 0)
      return;
    if (old == 1) {
      uint32_t expect = 1;
      if (gc2_finalizer_active_cas(g, &expect, ~(uint32_t)0)) {
	gc2_finalizer_owner_rel(g, 0);
	gc2_finalizer_active_rel(g, 0);
	wake_worker = gc2_finalizer_mpsc_acq(g) != NULL;
	break;  /* 05 section 5.8: close finalizer owner after last leave. */
      }
      continue;
    }
    if (old == ~(uint32_t)0) {
      la_cpu_pause();
      continue;
    }
    if (gc2_finalizer_active_cas(g, &old, old - 1))
      break;  /* 05 section 5.8: nested owner leave. */
  }
  gc2_finalizer_leaves_add(g, 1);
  if (wake_worker)
    lj_gc2_worker_wake(g);  /* 05 section 5.8: owner release exposes work. */
}

static int gc2_finalizer_pending_for_sweep(global_State *g, int owner_ok)
{
  if (!g)
    return 0;
  if (lj_gc2_finalizer_queue_pending(g))
    return 1;
  if (gc2_finalizer_active_acq(g) == 0)
    return 0;
  return !(owner_ok && gc2_finalizer_owned_by_current(g));
}

int lj_gc2_finalizer_queue_pending(global_State *g)
{
  if (!g)
    return 0;
  return gc2_finalizer_tail_acq(g) != NULL ||
	 gc2_finalizer_mpsc_acq(g) != NULL;
}

int lj_gc2_finalizer_pending(global_State *g)
{
  return gc2_finalizer_pending_for_sweep(g, 0);
}

int lj_gc2_finalizer_sweep_pending(global_State *g)
{
  return gc2_finalizer_pending_for_sweep(g, 1);
}

static int gc2_sweep_blocked_by_finalizer(global_State *g)
{
  if (!lj_gc2_finalizer_sweep_pending(g))
    return 0;
  gc2_finalizer_sweep_blocks_add(g, 1);
  return 1;  /* 05 section 5.8 finalizer drain before traversable sweep. */
}

int lj_gc2_sweep_tg_ready(TGState *tg)
{
  uint8_t flags;
  if (!tg)
    return 0;
  flags = lj_tg_flags_acq(tg);
  return !(flags & TGF_DEAD) && (flags & TGF_ARENA_INTERNAL);
}

int lj_gc2_sweep_needs_prepare(global_State *g)
{
  TGState *tg;
  uint32_t cycle;
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP)
    return 0;
  cycle = gc2_cycle_acq(g);
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg))
    if (lj_gc2_sweep_tg_ready(tg) && tg->alloc.prepare_epoch != cycle)
      return 1;
  return 0;
}

int lj_gc2_sweep_pending(global_State *g)
{
  TGState *tg;
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP)
    return 0;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg))
    if (lj_gc2_sweep_tg_ready(tg) &&
	tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] != NULL)
      return 1;
  return 0;
}

uint32_t lj_gc2_sweep_owner_progress(global_State *g, TGState *tg,
				      uint32_t limit)
{
  uint32_t n = 0, epoch;
  uint64_t live = 0;
  int minor;
  if (!g || !tg || limit == 0 ||
      !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return 0;
  if (gc2_phase_acq(g) != LJ_GC2_SWEEP)
    return 0;
  if (gc2_sweep_blocked_by_finalizer(g))
    return 0;
  epoch = gc2_cycle_acq(g);
  minor = gc2_cycle_sweep_minor_acq(g) != 0;
  if (minor)
    (void)lj_gc_sweep_gc2_unmarked(g);
  tg->alloc.sweep_epoch = epoch;
  while (n < limit) {
    GCArena *next = tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE];
    if (next)
      (void)lj_gc_sweep_gc2_arena_unmarked(g, next);
    {
      GCArena *a = lj_arena_sweep_one(&tg->alloc, LJ_ARENAK_TRAVERSABLE,
				      epoch, minor);
      if (!a)
	break;
      live += a->hdr.live_cells;
    }
    n++;
  }
  if (n) {
    gc2_sweep_owner_runs_add(g, 1);
    gc2_sweep_owner_arenas_add(g, n);
    gc2_sweep_owner_live_cells_add(g, live);
    if (minor)
      gc2_minor_sweep_arenas_add(g, n);
  }
  return n;
}

static uint64_t gc2_sweep_live_cells(GCArena *a, uint32_t epoch)
{
  uint64_t cells = 0;
  for (; a != NULL; a = lj_arena_next_acq(a))
    if (a->hdr.sweep_epoch == epoch)
      cells += a->hdr.live_cells;
  return cells;
}

static uint64_t gc2_saturating_add64(uint64_t a, uint64_t b)
{
  return a > ~(uint64_t)0 - b ? ~(uint64_t)0 : a + b;
}

uint64_t lj_gc2_sweep_live_aggregate(global_State *g)
{
  TGState *tg;
  uint64_t cells = 0, huge_bytes = 0, bytes;
  uint32_t epoch;
  if (!g)
    return 0;
  epoch = gc2_cycle_acq(g);
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    uint8_t flags = lj_tg_flags_acq(tg);
    if ((flags & (TGF_DEAD|TGF_ARENA_INTERNAL)) != TGF_ARENA_INTERNAL)
      continue;
    cells += gc2_sweep_live_cells(tg->alloc.owned[LJ_ARENAK_TRAVERSABLE],
				  epoch);
    if (flags & TGF_HUGETAB)
      huge_bytes = gc2_saturating_add64(huge_bytes,
	lj_arena_hugetab_live_bytes(&tg->huge,
	  LJ_HUGEF_MARK|LJ_HUGEF_TRAVERSABLE));
  }
  bytes = cells > (~(uint64_t)0 >> LJ_CELL_SHIFT) ?
	  ~(uint64_t)0 : cells << LJ_CELL_SHIFT;
  bytes = gc2_saturating_add64(bytes, huge_bytes);
  gc2_sweep_live_huge_bytes_rel(g, huge_bytes);
  gc2_live_estimate_rel(g, bytes);
  gc2_sweep_live_updates_add(g, 1);
  return bytes;
}

void lj_gc2_sweep_legacy_ready(global_State *g)
{
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP)
    return;
  gc2_sweep_legacy_ready_rel(g, 1);
  lj_gc2_worker_wake(g);  /* 05 section 5.8: legacy roots reached close. */
}

void lj_gc2_legacy_preserve_abort(global_State *g)
{
  uint32_t phase;
  if (!g)
    return;
  gc2_cycle_leader_rel(g, 0);
  gc2_sweep_legacy_ready_rel(g, 0);
  (void)gc2_flush_and_drain_ssb(g);
  phase = gc2_phase_xchg_acqrel(g, LJ_GC2_IDLE);
  if (phase != LJ_GC2_IDLE)
    gc2_preserve_abort_to_idle_add(g, 1);
  lj_gc2_handshake(g, gc2_idle_barrier_actions(g, 0));
  (void)lj_tg_reclaim_dead(g);
}

int lj_gc2_sweep_to_idle(global_State *g)
{
  uint32_t expect = 0, phase;
  if (!g)
    return 0;
  if (!gc2_worker_active_cas(g, &expect, 1))
    return 0;  /* 05 section 5.8 scheduler close waits for worker owner. */
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_SWEEP || gc2_sweep_blocked_by_finalizer(g) ||
      !gc2_sweep_legacy_ready_acq(g) ||
      lj_gc2_sweep_needs_prepare(g) || lj_gc2_sweep_pending(g)) {
    gc2_worker_active_rel(g, 0);
    return 0;
  }
  gc2_cycle_leader_rel(g, 0);
  (void)gc2_flush_and_drain_ssb(g);
  phase = gc2_phase_xchg_acqrel(g, LJ_GC2_IDLE);
  if (phase != LJ_GC2_SWEEP) {
    gc2_worker_active_rel(g, 0);
    return 0;
  }
  gc2_sweep_to_idle_add(g, 1);
  lj_gc2_update_minor_survival_policy(g, lj_gc2_sweep_live_aggregate(g));
  gc2_update_public_minor_gates(g);
  lj_gc2_handshake(g, gc2_idle_barrier_actions(g, 0));
  (void)lj_tg_reclaim_dead(g);
  lj_gc2_update_pacing(g);
  gc2_worker_active_rel(g, 0);
  return 1;
}

void lj_gc2_legacy_cycle_end(global_State *g)
{
  uint32_t phase;
  if (!g)
    return;
  gc2_cycle_leader_rel(g, 0);
  (void)gc2_flush_and_drain_ssb(g);
  phase = gc2_phase_xchg_acqrel(g, LJ_GC2_IDLE);
  if (phase == LJ_GC2_SWEEP) {
    gc2_sweep_to_idle_add(g, 1);
    lj_gc2_update_minor_survival_policy(g, lj_gc2_sweep_live_aggregate(g));
  }
  gc2_sweep_legacy_ready_rel(g, 0);
  gc2_update_public_minor_gates(g);
  lj_gc2_handshake(g, gc2_idle_barrier_actions(g, 0));
  (void)lj_tg_reclaim_dead(g);
  lj_gc2_update_pacing(g);
}

uint32_t lj_gc2_handshake(global_State *g, uint32_t actions)
{
  return lj_safepoint_handshake(g, actions);
}

uint32_t lj_gc2_reclaim_retired(global_State *g, uint64_t epoch)
{
  uint32_t n = 0;
  if (!g)
    return 0;
  n += lj_str_reclaim_retired(g, epoch);  /* 05 section 5.9 SMR drain. */
  n += lj_tab_reclaim_retired(g, epoch);  /* 06 section 6.3.5 SMR drain. */
#if LJ_HASFFI
  n += lj_ctype_reclaim_retired(g, epoch);  /* 11.2 CTState table SMR drain. */
#endif
  n += lj_mcode_reclaim_retired(g, epoch);  /* 08 section 8.7 SMR drain. */
  n += lj_trace_reclaim_retired(g, epoch);  /* 08 section 8.3/8.7 drain. */
  if (n) {
    gc2_smr_reclaim_runs_add(g, 1);
    gc2_smr_reclaimed_add(g, n);
  }
  return n;
}

static void gc2_mark_tv(global_State *g, cTValue *tv)
{
  if (tvisgcv(tv))
    lj_gc2_markobj(g, gcV(tv));
}

static void gc2_mark_fixedstr(global_State *g)
{
  MSize i;
  StrTabHdr *hdr;
  GCRef *strtab;
  hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.tabh);
  if (!hdr)
    return;
  strtab = hdr->bucket;
  for (i = 0; i <= hdr->mask; i++) {
    GCobj *o;
    for (o = lj_str_hashhead_acq(&strtab[i]); o != NULL;
	 o = lj_str_next_acq(o))
      if (lj_obj_gcflags(o) & (LJ_GC_FIXED|LJ_GC_SFIXED))
	lj_gc2_markobj(g, o);
  }
}

static TValue *gc2_stack_scan_top(global_State *g, lua_State *L)
{
  TValue *frame, *bot = tvref(L->stack);
  TValue *top = L->top, *ctop = curr_top(L), *max = tvref(L->maxstack);
  for (frame = L->base - 1; frame > bot + LJ_FR2; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    if (!LJ_FR2)
      lj_gc2_markobj(g, obj2gco(fn));
  }
  if (ctop > top)
    top = ctop;
  return top > max ? max : top;
}

static LJ_AINLINE uint8_t *gc2_thread_flagp(lua_State *L)
{
  return lj_obj_gcflags_ref(obj2gco(L));
}

static void gc2_thread_set_needscan(global_State *g, lua_State *L)
{
  uint8_t old = la_or8_rlx(gc2_thread_flagp(L), LJ_GC_NEEDSCAN);
  if (!(old & LJ_GC_NEEDSCAN))
    gc2_thread_scan_needscan_add(g, 1);
}

static void gc2_thread_clear_needscan(lua_State *L)
{
  la_and8_rlx(gc2_thread_flagp(L), (uint8_t)~LJ_GC_NEEDSCAN);
}

static int gc2_thread_needscan(lua_State *L)
{
  return (la_load8_acq(gc2_thread_flagp(L)) & LJ_GC_NEEDSCAN) != 0;
}

static uint64_t gc2_thread_owner_dirty(global_State *g, lua_State *L,
				       TGState **ptg)
{
  uint32_t owner;
  TGState *tg;
  if (ptg)
    *ptg = NULL;
  if (!g || !L)
    return 0;
  owner = la_load32_acq(&L->thr_owner);
  if (owner == 0 || owner == LJ_THREAD_GCSCAN)
    return 0;
  tg = lj_tg_find_owner(g, owner);
  if (!tg || lj_tg_flags_test_acq(tg, TGF_DEAD))
    return 0;
  if (ptg)
    *ptg = tg;
  return lj_tg_stack_dirty_epoch_acq(tg);
}

static void gc2_scan_thread_stack(global_State *g, lua_State *L)
{
  GCobj *mt, *uv;
  TValue *o, *top;
  TValue tv;
  uint64_t dirty_epoch;
  uint32_t cycle;
  if (!L || tvref(L->stack) == NULL)
    return;
  cycle = gc2_cycle_acq(g);
  lj_gc2_markobj(g, obj2gco(L));
  lj_gc2_markmem(g, tvref(L->stack));
  top = gc2_stack_scan_top(g, L);
  for (o = tvref(L->stack) + 1 + LJ_FR2; o < top; o++) {
    lj_tv_load_acq(&tv, o);
    gc2_mark_tv(g, &tv);
  }
  {
    GCtab *env = tabref_acq(L->env);
    if (env)
      lj_gc2_markobj(g, obj2gco(env));
  }
  mt = gcref_acq(L->mt_thread);
  if (mt != NULL)
    lj_gc2_markobj(g, mt);
  for (uv = gcref_acq(L->openupval); uv != NULL;
       uv = lj_obj_gcw_acq(uv)) {
    lj_gc2_markobj(g, uv);
    if (uv->gch.gct == ~LJ_TUPVAL) {
      TValue tv;
      lj_tv_load_acq(&tv, uvval(gco2uv(uv)));
      gc2_mark_tv(g, &tv);
    }
  }
  dirty_epoch = gc2_thread_owner_dirty(g, L, NULL);
  la_store64_rel(&L->scan_dirty_epoch, dirty_epoch);
  la_store64_rel(&L->scan_epoch, cycle);
  gc2_thread_clear_needscan(L);
}

static void gc2_scan_owned_needscan(global_State *g, lua_State *owner_L)
{
  TGState *tg;
  GCobj *o;
  uint32_t tid;
  if (!owner_L)
    return;
  tg = L2TG(owner_L);
  if (!tg)
    return;
  tid = la_load32_acq(&tg->tid);
  if (tid == 0 || tid == LJ_THREAD_GCSCAN)
    return;
  for (o = gcref_acq(g->gc.root); o != NULL; o = lj_obj_gcw_acq(o)) {
    lua_State *th;
    if (o->gch.gct != ~LJ_TTHREAD)
      continue;
    th = gco2th(o);
    if (th == owner_L || !gc2_thread_needscan(th))
      continue;
    if (la_load32_acq(&th->thr_owner) != tid)
      continue;
    gc2_scan_thread_stack(g, th);
    gc2_thread_scan_owner_needscans_add(g, 1);
  }
}

static void gc2_scan_thread_roots(global_State *g, lua_State *L)
{
  if (!L || tvref(L->stack) == NULL)
    return;
  gc2_scan_thread_stack(g, L);
  gc2_scan_owned_needscan(g, L);
}

#if LJ_HASFFI
static void gc2_finreg_markobj(global_State *g, GCobj *o)
{
  (void)lj_gc2_markobj(g, o);
}

static void gc2_finreg_markmem(global_State *g, void *p)
{
  (void)lj_gc2_markmem(g, p);
}

static void gc2_mark_finreg_cdata_preclaims(global_State *g)
{
  MSize i, head, count;
  if (!gc2_finreg_cdata_preclaim_ready(g))
    return;
  head = gc2_finreg_cdata_preclaim_head_acq(g);
  count = gc2_finreg_cdata_preclaim_count_acq(g);
  for (i = head; i < count; i++) {
    GCobj *o = gc2_finreg_cdata_preclaim_obj_acq(g, i);
    if (o) {
      TValue fin;
      lj_gc2_markobj(g, o);
      gc2_finreg_cdata_preclaim_fin_acq(g, i, &fin);
      gc2_mark_tv(g, &fin);
    }
  }
}
#endif

static void gc2_mark_finalizer_ring(global_State *g, GCobj *tail)
{
  GCobj *o = tail;
  if (!o)
    return;
  do {
    o = lj_obj_gcw_acq(o);
    lj_gc2_markobj(g, o);
  } while (o != tail);
}

static void gc2_scan_threading_live_roots(global_State *g)
{
  LJThreadLive *node;
  for (node = (LJThreadLive *)
	 la_loadptr_acq((void *const *)&g->threading_live);
       node != NULL;
       node = lj_thread_live_next_acq(node)) {
    GCobj *o = gcref_acq(node->ud);
    if (o && o->gch.gct == ~LJ_TUDATA &&
	lj_udata_udtype_acq(gco2ud(o)) == UDTYPE_THREAD)
      lj_gc2_markobj(g, o);
  }
}

static void gc2_scan_pending_roots(global_State *g)
{
  if (!g)
    return;
  lj_gc2_finalizer_enter(g);
  lj_gc2_finalizer_drain_owned(g);
  gc2_mark_finalizer_ring(g, gc2_finalizer_tail_acq(g));
  lj_gc2_finalizer_leave(g);
  gc2_scan_threading_live_roots(g);
#if LJ_HASFFI
  gc2_mark_finreg_cdata_preclaims(g);
#endif
}

static void gc2_scan_tg_roots(global_State *g)
{
  TGState *tg;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    lua_State *thread_L, *cur_L;
    lj_gc2_markmem(g, tg->tmpbuf.b);
    if (lj_tg_flags_test_acq(tg, TGF_DEAD))
      continue;
    thread_L = lj_tg_load_thread_L(tg);
    cur_L = lj_tg_load_cur_L(tg);
    if (thread_L) {
      lj_gc2_markobj(g, obj2gco(thread_L));  /* 05 section 5.7.4 TG root. */
      gc2_tg_thread_roots_add(g, 1);
    }
    if (cur_L && cur_L != thread_L) {
      lj_gc2_markobj(g, obj2gco(cur_L));  /* 05 section 5.7.4 TG root. */
      gc2_tg_cur_roots_add(g, 1);
    }
#if LJ_HASJIT
    {
      int32_t vmstate = lj_tg_vmstate_load_acq(tg);
      if (vmstate > 0 && gc2_mark_trace_root(g, (TraceNo)vmstate))
	gc2_tg_trace_roots_add(g, 1);
    }
#endif
  }
}

#if LJ_HASJIT
static void gc2_scan_current_trace_root(global_State *g)
{
  jit_State *J = G2J(g);
  gc2_traverse_trace(g, &J->cur);  /* 05 section 5.7.4 current trace root. */
}
#endif

static void gc2_scan_global_roots(global_State *g)
{
  lua_State *mainL = mainthread_acq(g);
  lua_State *vmL = vmthread_acq(g);
  ptrdiff_t i;
  lj_gc2_markobj(g, obj2gco(mainL));
  {
    GCtab *env = tabref_acq(mainL->env);
    if (env)
      lj_gc2_markobj(g, obj2gco(env));
  }
  lj_gc2_markobj(g, obj2gco(vmL));
  gc2_mark_tv(g, &g->registrytv);
  for (i = 0; i < GCROOT_MAX; i++) {
    GCobj *o = gcref_acq(g->gcroot[i]);
    if (o != NULL)
      lj_gc2_markobj(g, o);
  }
  gc2_scan_pending_roots(g);
  gc2_mark_fixedstr(g);
  gc2_mark_strtab_mem(g);
  gc2_mark_tab_retired_mem(g);
#if LJ_64
  lj_gc2_markmem(g, mref(g->gc.lightudseg, uint32_t));
#endif
  lj_gc2_markmem(g, g->tmpbuf.b);
  gc2_scan_tg_roots(g);
#if LJ_HASFFI
  {
    CTState *cts = ctype_ctsG(g);
    if (cts) {
      CTypeTab *ret;
      GCRef *meta = ctype_metamap_acq(cts);
      uint64_t *cbblack = ctype_cbblack_acq(cts);
      TValue *func;
      lua_State **owner;
      lj_gc2_markmem(g, cts);
      lj_gc2_markmem(g, ctype_tabh_acq(cts));
      for (ret = ctype_retiredtab_acq(cts);
	   ret != NULL;
	   ret = ctype_tab_retired_next_acq(ret)) {
	lj_gc2_markmem(g, ret);
      }
      lj_gc2_markmem(g, meta);
      lj_gc2_markmem(g, cbblack);
      if (meta) {
	MSize i, n = ctype_metamap_size_acq(cts);
	for (i = 0; i < n; i++) {
	  GCobj *o = ctype_metamap_obj_acq(meta, i);
	  if (o)
	    lj_gc2_markobj(g, o);
	}
      }
      {
	GCtab *pinmt = ctype_pinmt_acq(cts);
	if (pinmt)
	  lj_gc2_markobj(g, obj2gco(pinmt));
      }
      lj_ctype_fin_mark(g, gc2_finreg_markobj, gc2_finreg_markmem);
      lj_gc2_markmem(g, ctype_cb_cbid_acq(cts));
      owner = ctype_cb_owner_acq(cts);
      lj_gc2_markmem(g, owner);
      if (owner) {
	MSize i, n = ctype_cb_sizeid_acq(cts);
	for (i = 0; i < n; i++) {
	  lua_State *th = ctype_cb_owner_slot_acq(owner, i);
	  if (th)
	    lj_gc2_markobj(g, obj2gco(th));
	}
      }
      func = ctype_cb_func_acq(cts);
      lj_gc2_markmem(g, func);
      if (func) {
	MSize i, n = ctype_cb_sizeid_acq(cts);
	for (i = 0; i < n; i++) {
	  TValue tv;
	  lj_tv_load_acq(&tv, &func[i]);
	  gc2_mark_tv_worker(g, &tv);
	}
      }
    }
  }
#endif
#if LJ_HASJIT
  {
    jit_State *J = G2J(g);
    lj_trace_markvecs(g, 1);
    gc2_scan_current_trace_root(g);
    lj_mcode_markretired(g, 1);
    lj_gc2_markmem(g, J->irbuf ? J->irbuf + J->irbotlim : NULL);
    lj_gc2_markmem(g, J->snapbuf);
    lj_gc2_markmem(g, J->snapmapbuf);
  }
#endif
}

void lj_gc2_scan_roots(global_State *g, lua_State *L)
{
  if (!g)
    return;
  gc2_major_root_scans_add(g, 1);
  gc2_scan_global_roots(g);
  gc2_scan_thread_roots(g, L);
}

void lj_gc2_scan_minor_roots(global_State *g, lua_State *L)
{
  if (!g || gc2_cycle_roots_minor_acq(g) == 0)
    return;
  gc2_minor_root_scans_add(g, 1);
  gc2_scan_pending_roots(g);
  gc2_scan_tg_roots(g);
  gc2_scan_thread_roots(g, L);
#if LJ_HASJIT
  gc2_scan_current_trace_root(g);
#endif
}

void lj_gc2_scan_cycle_roots(global_State *g, lua_State *L)
{
  if (!g)
    return;
  if (gc2_cycle_roots_minor_acq(g))
    lj_gc2_scan_minor_roots(g, L);
  else
    lj_gc2_scan_roots(g, L);
}

static void *gc2_mark_base(GCobj *o);
static int gc2_mark_base_traversable(global_State *g, void *p);
static int gc2_grey_push(global_State *g, GCobj *o);
static uint32_t gc2_drain_grey(global_State *g, uint32_t limit);
static void gc2_traverse_udata(global_State *g, GCudata *ud);

static LJ_AINLINE void gc2_queue_slot_store_rel(GCRef *slot, GCobj *o)
{
  setgcrefrel(*slot, o);
}

static LJ_AINLINE GCobj *gc2_queue_slot_load_acq(const GCRef *slot)
{
  return gcref_acq(*slot);
}

static LJ_AINLINE void gc2_queue_slot_clear_rel(GCRef *slot)
{
  setgcrefnullrel(*slot);
}

static int gc2_grey_grow(global_State *g)
{
  GCRef *oldstack = gc2_grey_stack_acq(g);
  GCRef *newstack;
  MSize oldcap = gc2_grey_capacity_acq(g);
  MSize newcap = oldcap ? oldcap << 1 : GC2_GREY_INIT;
  uint64_t top = gc2_grey_top_acq(g);
  uint64_t bottom = gc2_grey_bottom_rlx(g);
  MSize count = bottom > top ? (MSize)(bottom - top) : 0;
  lua_State *L = lj_tg_cur_L(g);
  if (!L) {
    GCobj *mainobj = gcref_acq(g->mainthref);
    if (mainobj)
      L = &mainobj->th;
  }
  if (!L || oldcap >= GC2_GREY_LIMIT)
    return 0;
  if (newcap < oldcap || newcap > GC2_GREY_LIMIT)
    newcap = GC2_GREY_LIMIT;
  if (newcap <= oldcap || count > newcap)
    return 0;
  newstack = lj_mem_newvec(L, newcap, GCRef);
  if (oldstack && oldcap) {
    MSize i;
    for (i = 0; i < count; i++) {
      GCobj *o = gc2_queue_slot_load_acq(&oldstack[(MSize)((top + i) % oldcap)]);
      if (o)
	gc2_queue_slot_store_rel(&newstack[i], o);
      else
	gc2_queue_slot_clear_rel(&newstack[i]);
    }
  }
  gc2_grey_stack_rel(g, newstack);
  gc2_grey_capacity_rel(g, newcap);
  gc2_grey_top_store_rlx(g, 0);
  gc2_grey_bottom_store_rlx(g, count);
  if (oldstack)
    lj_mem_freevec(g, oldstack, oldcap, GCRef);
  return 1;
}

static int gc2_grey_push(global_State *g, GCobj *o)
{
  GCRef *stack;
  uint64_t top, bottom;
  MSize cap;
  if (!g || !o)
    return 0;
  bottom = gc2_grey_bottom_rlx(g);
  top = gc2_grey_top_acq(g);
  cap = gc2_grey_capacity_acq(g);
  if ((cap == 0 || bottom - top >= cap) && !gc2_grey_grow(g))
    return 0;
  bottom = gc2_grey_bottom_rlx(g);
  cap = gc2_grey_capacity_acq(g);
  stack = gc2_grey_stack_acq(g);
  if (!stack || cap == 0)
    return 0;
  gc2_queue_slot_store_rel(&stack[(MSize)(bottom % cap)], o);
  /* 05 section 5.6.3: slot release-published before bottom. */
  gc2_grey_bottom_rel(g, bottom + 1);
  gc2_grey_pushed_add(g, 1);
  return 1;
}

static int gc2_grey_empty(global_State *g)
{
  if (!g)
    return 1;
  return gc2_grey_top_acq(g) == gc2_grey_bottom_acq(g);
}

static int gc2_weak_ensure(global_State *g)
{
  MSize cap;
  GCRef *stack;
  uint8_t *ready;
  if (!g)
    return 0;
  cap = gc2_weak_capacity_acq(g);
  stack = gc2_weak_stack_acq(g);
  ready = gc2_weak_ready_acq(g);
  if (stack && ready && cap > 0)
    return 1;
  return gc2_weak_resize(g, GC2_WEAK_INIT);
}

static MSize gc2_weak_next_capacity(MSize cap, uint64_t need)
{
  MSize n = cap ? cap : GC2_WEAK_INIT;
  if (n < GC2_WEAK_INIT)
    n = GC2_WEAK_INIT;
  if (need > (uint64_t)GC2_WEAK_LIMIT)
    need = (uint64_t)GC2_WEAK_LIMIT;
  while ((uint64_t)n < need && n < GC2_WEAK_LIMIT) {
    if (n > (GC2_WEAK_LIMIT >> 1)) {
      n = GC2_WEAK_LIMIT;
      break;
    }
    n <<= 1;
  }
  return n;
}

static int gc2_weak_resize(global_State *g, MSize cap)
{
  lua_State *L;
  GCRef *oldstack, *newstack;
  uint8_t *oldready, *newready;
  MSize oldcap;
  if (!g || cap == 0)
    return 0;
  L = mainthread_acq(g);
  if (!L)
    return 0;
  oldcap = gc2_weak_capacity_acq(g);
  oldstack = gc2_weak_stack_acq(g);
  oldready = gc2_weak_ready_acq(g);
  newstack = lj_mem_newvec(L, cap, GCRef);
  newready = lj_mem_newvec(L, cap, uint8_t);
  gc2_weak_stack_rel(g, newstack);
  gc2_weak_ready_rel(g, newready);
  gc2_weak_capacity_rel(g, cap);
  if (oldstack)
    lj_mem_freevec(g, oldstack, oldcap, GCRef);
  if (oldready)
    lj_mem_freevec(g, oldready, oldcap, uint8_t);
  return 1;
}

static void gc2_weak_reset(global_State *g)
{
  MSize i;
  MSize cap;
  uint8_t *ready;
  uint64_t prior_count;
  if (!g)
    return;
  prior_count = gc2_weak_count_acq(g);
  (void)gc2_weak_ensure(g);
  cap = gc2_weak_capacity_acq(g);
  if (prior_count > (uint64_t)cap) {
    MSize ncap = gc2_weak_next_capacity(cap, prior_count);
    if (ncap > cap)
      (void)gc2_weak_resize(g, ncap);  /* 05 section 5.8 adaptive weak snapshot. */
  }
  cap = gc2_weak_capacity_acq(g);
  ready = gc2_weak_ready_acq(g);
  if (ready)
    for (i = 0; i < cap; i++)
      la_store8_rlx(&ready[i], 0);
  gc2_weak_count_store_rlx(g, 0);  /* 05 section 5.8 side vector. */
  gc2_weak_scan_cursor_store_rlx(g, 0);
  gc2_weak_clear_cursor_store_rlx(g, 0);
}

static MSize gc2_finclaim_next_capacity(MSize cap, MSize need)
{
  MSize n = cap ? cap : GC2_FINCLAIM_INIT;
  if (n < GC2_FINCLAIM_INIT)
    n = GC2_FINCLAIM_INIT;
  if (need > GC2_FINCLAIM_LIMIT)
    need = GC2_FINCLAIM_LIMIT;
  while (n < need && n < GC2_FINCLAIM_LIMIT) {
    if (n > (GC2_FINCLAIM_LIMIT >> 1)) {
      n = GC2_FINCLAIM_LIMIT;
      break;
    }
    n <<= 1;
  }
  return n;
}

static void gc2_finclaim_publish(lua_State *L, global_State *g, MSize idx,
				 GCobj *o, cTValue *fin)
{
  GCRef *objv = gc2_finreg_cdata_preclaim_objvec_acq(g);
  TValue *finv = gc2_finreg_cdata_preclaim_finvec_acq(g);
  copyTVrel(L, &finv[idx], fin);
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  if (gc2_finreg_cdata_preclaim_publish_pause_xchg_acqrel(g, 0) != 0) {
    gc2_finreg_cdata_preclaim_publish_paused_rel(g, 1);
    while (gc2_finreg_cdata_preclaim_publish_release_acq(g) == 0)
      la_cpu_pause();
    gc2_finreg_cdata_preclaim_publish_paused_rel(g, 0);
  }
#endif
  gc2_queue_slot_store_rel(&objv[idx], o);
  /* 05 section 5.8: finalizer value is visible before object ready marker. */
}

static void gc2_finclaim_clear(lua_State *L, global_State *g, MSize idx)
{
  GCRef *objv = gc2_finreg_cdata_preclaim_objvec_acq(g);
  TValue *finv = gc2_finreg_cdata_preclaim_finvec_acq(g);
  TValue nilv;
  gc2_queue_slot_clear_rel(&objv[idx]);
  setnilV(&nilv);
  copyTVrel(L, &finv[idx], &nilv);
}

static void gc2_finclaim_copy_slot(lua_State *L, GCRef *newobj, TValue *newfin,
				   MSize dst, GCRef *oldobj,
				   TValue *oldfin, MSize src)
{
  GCobj *o = gc2_queue_slot_load_acq(&oldobj[src]);
  if (o) {
    TValue fin;
    lj_tv_load_acq(&fin, &oldfin[src]);
    copyTVrel(L, &newfin[dst], &fin);
    gc2_queue_slot_store_rel(&newobj[dst], o);
  } else {
    TValue nilv;
    setnilV(&nilv);
    copyTVrel(L, &newfin[dst], &nilv);
    gc2_queue_slot_clear_rel(&newobj[dst]);
  }
}

static int gc2_finclaim_resize(global_State *g, MSize cap)
{
  lua_State *L;
  GCRef *oldobj, *newobj;
  TValue *oldfin, *newfin;
  MSize oldcap, head, count, pending, i;
  if (!g || cap == 0)
    return 0;
  L = mainthread_acq(g);
  if (!L)
    return 0;
  oldobj = gc2_finreg_cdata_preclaim_objvec_acq(g);
  oldfin = gc2_finreg_cdata_preclaim_finvec_acq(g);
  oldcap = gc2_finreg_cdata_preclaim_capacity_acq(g);
  head = gc2_finreg_cdata_preclaim_head_acq(g);
  count = gc2_finreg_cdata_preclaim_count_acq(g);
  pending = count > head ? count - head : 0;
  if (cap < pending)
    return 0;
  newobj = lj_mem_newvec(L, cap, GCRef);
  newfin = lj_mem_newvec(L, cap, TValue);
  for (i = 0; i < pending; i++)
    gc2_finclaim_copy_slot(L, newobj, newfin, i, oldobj, oldfin, head + i);
  gc2_finreg_cdata_preclaim_objvec_rel(g, newobj);
  gc2_finreg_cdata_preclaim_finvec_rel(g, newfin);
  gc2_finreg_cdata_preclaim_capacity_rel(g, cap);
  gc2_finreg_cdata_preclaim_head_rel(g, 0);
  gc2_finreg_cdata_preclaim_count_rel(g, pending);
  if (oldobj)
    lj_mem_freevec(g, oldobj, oldcap, GCRef);
  if (oldfin)
    lj_mem_freevec(g, oldfin, oldcap, TValue);
  return 1;
}

static void gc2_finclaim_reset(global_State *g)
{
  lua_State *L;
  GCRef *obj;
  TValue *fin;
  MSize head, count, pending, cap, i;
  if (!g)
    return;
  L = mainthread_acq(g);
  if (!L)
    return;
  obj = gc2_finreg_cdata_preclaim_objvec_acq(g);
  fin = gc2_finreg_cdata_preclaim_finvec_acq(g);
  cap = gc2_finreg_cdata_preclaim_capacity_acq(g);
  head = gc2_finreg_cdata_preclaim_head_acq(g);
  count = gc2_finreg_cdata_preclaim_count_acq(g);
  pending = count > head ? count - head : 0;
  if (!obj || !fin || cap == 0) {
    (void)gc2_finclaim_resize(g, GC2_FINCLAIM_INIT);
    return;
  }
  if (head != 0 && pending != 0) {
    for (i = 0; i < pending; i++)
      gc2_finclaim_copy_slot(L, obj, fin, i, obj, fin, head + i);
  }
  gc2_finreg_cdata_preclaim_head_rel(g, 0);
  gc2_finreg_cdata_preclaim_count_rel(g, pending);
  if (pending >= cap && cap < GC2_FINCLAIM_LIMIT) {
    MSize ncap = gc2_finclaim_next_capacity(cap, pending + 1u);
    if (ncap > cap)
      (void)gc2_finclaim_resize(g, ncap);
  }
}

static void gc2_weak_record(global_State *g, GCtab *t)
{
  MSize cap;
  GCRef *stack;
  uint8_t *ready;
  uint64_t idx;
  if (!g || !t) {
    if (g)
      gc2_weak_tables_overflow_add(g, 1);
    return;
  }
  cap = gc2_weak_capacity_acq(g);
  stack = gc2_weak_stack_acq(g);
  ready = gc2_weak_ready_acq(g);
  if (!stack || !ready || cap == 0) {
    gc2_weak_tables_overflow_add(g, 1);
    return;
  }
  idx = gc2_weak_count_add(g, 1);  /* 05 section 5.8 MPSC slot. */
  if (idx < (uint64_t)cap) {
    gc2_queue_slot_store_rel(&stack[(MSize)idx], obj2gco(t));
    /* 05 section 5.8: publish weak snapshot slot before ready byte. */
    la_store8_rel(&ready[(MSize)idx], 1);
    gc2_weak_tables_queued_add(g, 1);
  } else {
    gc2_weak_tables_overflow_add(g, 1);
  }
}

uint32_t lj_gc2_weak_snapshot_count(global_State *g)
{
  uint64_t reserved, count;
  MSize cap;
  GCRef *stack;
  uint8_t *ready;
  if (!g)
    return 0;
  cap = gc2_weak_capacity_acq(g);
  stack = gc2_weak_stack_acq(g);
  ready = gc2_weak_ready_acq(g);
  if (!stack || !ready)
    return 0;
  reserved = gc2_weak_count_acq(g);
  if (reserved > (uint64_t)cap)
    reserved = (uint64_t)cap;
  for (count = 0; count < reserved; count++)
    if (la_load8_acq(&ready[(MSize)count]) == 0)
      break;
  return count > ~(uint32_t)0 ? ~(uint32_t)0 : (uint32_t)count;
}

GCtab *lj_gc2_weak_snapshot_tab(global_State *g, uint32_t idx)
{
  GCobj *o;
  GCRef *stack;
  if (!g || idx >= lj_gc2_weak_snapshot_count(g))
    return NULL;
  stack = gc2_weak_stack_acq(g);
  if (!stack)
    return NULL;
  o = gc2_queue_slot_load_acq(&stack[idx]);
  return (o && o->gch.gct == ~LJ_TTAB) ? gco2tab(o) : NULL;
}

static int gc2_weak_mayclear(global_State *g, cTValue *o, int val,
			     int markstr)
{
  if (tvisgcv(o)) {
    if (tvisstr(o)) {
      if (markstr) {
	(void)lj_gc2_markobj(g, gcV(o));
	lj_obj_cleargcflags_atomic(gcV(o), LJ_GC_WHITES);
      }
      return 0;  /* 05 section 5.8: strings are not weak-cleared. */
    }
    if (lj_gc2_ismarked(g, gcV(o)) == 0) {
      if (g->gc.state == GCSatomic && iswhite(gcV(o)))
	return 1;  /* 05 section 5.8: legacy-color weak oracle bridge. */
      return 1;
    }
    /* GC2 late weak write mark wins over legacy white during GCSatomic. */
    if (tvisudata(o) && val &&
	(lj_obj_gcflags(obj2gco(udataV(o))) & LJ_GC_FINALIZED))
      return 1;
  }
  return 0;
}

static int gc2_tab_is_ffi_fin(global_State *g, GCtab *t)
{
#if LJ_HASFFI
  return lj_ctype_fin_istab(g, t);
#else
  UNUSED(g); UNUSED(t);
  return 0;
#endif
}

static void gc2_weak_process_tab(global_State *g, GCtab *t, int clear,
				 uint64_t *slots, uint64_t *clearable)
{
  int weak = lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAK;
  if (!weak)
    return;
  if (weak & LJ_GC_WEAKVAL) {
    TValue *array;
    MSize i, asize = lj_tab_array_snapshot_acq(t, &array);
    for (i = 0; i < asize; i++) {
      TValue val;
      lj_tv_load_acq(&val, &array[i]);
      if (!tvisnil(&val)) {
	(*slots)++;
	if (gc2_weak_mayclear(g, &val, 1, clear)) {
	  (*clearable)++;
	  if (clear)
	    lj_tab_storenilraw(&array[i]);
	}
      }
    }
  }
  {
    MSize i, hmask;
    Node *node = lj_tab_node_snapshot_acq(t, &hmask);
    for (i = 0; i <= hmask; i++) {
      Node *n = &node[i];
      TValue key, val;
      lj_tv_load_acq(&val, &n->val);
      if (!tvisnil(&val)) {
	lj_tv_load_acq(&key, &n->key);
	(*slots)++;
	if (gc2_weak_mayclear(g, &key, 0, clear) ||
	    gc2_weak_mayclear(g, &val, 1, clear)) {
	  (*clearable)++;
	  if (clear)
	    lj_tab_storenilraw(&n->val);
	}
      }
    }
  }
}

#if LJ_GC2_PARANOIA
static void gc2_weak_paranoia_zero_diff(global_State *g, GCobj *legacy)
{
  uint64_t tables = 0, slots = 0, clearable = 0;
  while (legacy) {
    GCtab *t;
    if (legacy->gch.gct != ~LJ_TTAB) {
      fprintf(stderr, "GC2 weak paranoia: non-table legacy weak node %p\n",
	      (void *)legacy);
      abort();
    }
    t = gco2tab(legacy);
    gc2_weak_process_tab(g, t, 0, &slots, &clearable);
    tables++;
    legacy = gcref_acq(t->gclist);
  }
  if (clearable != 0) {
    fprintf(stderr, "GC2 weak paranoia: %llu/%llu clearable weak slots "
	    "after GC2 skip across %llu tables\n",
	    (unsigned long long)clearable, (unsigned long long)slots,
	    (unsigned long long)tables);
    abort();
  }
}
#endif

uint32_t lj_gc2_weak_snapshot_scan(global_State *g, uint32_t limit)
{
  uint64_t start, end;
  uint32_t i, n, scanned = 0;
  uint64_t slots = 0, clearable = 0;
  if (!g || limit == 0)
    return 0;
  n = lj_gc2_weak_snapshot_count(g);
  do {
    start = gc2_weak_scan_cursor_acq(g);
    if (start >= (uint64_t)n)
      return 0;
    end = start + limit;
    if (end < start || end > (uint64_t)n)
      end = (uint64_t)n;
  } while (!gc2_weak_scan_cursor_cas(g, &start, end));
  /* 05 section 5.8 bounded scan cursor. */
  for (i = (uint32_t)start; (uint64_t)i < end; i++) {
    GCtab *t = lj_gc2_weak_snapshot_tab(g, i);
    if (!t)
      continue;
    gc2_weak_process_tab(g, t, 0, &slots, &clearable);
    scanned++;
  }
  if (scanned) {
    gc2_weak_scan_runs_add(g, 1);
    gc2_weak_scan_tables_add(g, scanned);
    gc2_weak_scan_slots_add(g, slots);
    gc2_weak_scan_clearable_add(g, clearable);
  }
  return scanned;
}

uint32_t lj_gc2_weak_snapshot_clear(global_State *g, uint32_t limit)
{
  uint64_t start, end;
  uint32_t i, n, scanned = 0;
  uint64_t slots = 0, cleared = 0;
  if (!g || limit == 0)
    return 0;
  n = lj_gc2_weak_snapshot_count(g);
  do {
    start = gc2_weak_clear_cursor_acq(g);
    if (start >= (uint64_t)n)
      return 0;
    end = start + limit;
    if (end < start || end > (uint64_t)n)
      end = (uint64_t)n;
  } while (!gc2_weak_clear_cursor_cas(g, &start, end));
  /* 05 section 5.8 bounded clear cursor. */
  for (i = (uint32_t)start; (uint64_t)i < end; i++) {
    GCtab *t = lj_gc2_weak_snapshot_tab(g, i);
    if (!t)
      continue;
    gc2_weak_process_tab(g, t, 1, &slots, &cleared);
    scanned++;
  }
  if (scanned) {
    gc2_weak_clear_runs_add(g, 1);
    gc2_weak_clear_tables_add(g, scanned);
    gc2_weak_clear_slots_add(g, slots);
    gc2_weak_clear_cleared_add(g, cleared);
  }
  return scanned;
}

uint32_t lj_gc2_weak_drain(global_State *g, uint32_t limit)
{
  if (!g || limit == 0 || gc2_phase_acq(g) != LJ_GC2_WEAK)
    return 0;
  return lj_gc2_weak_snapshot_clear(g, limit);
}

static int gc2_weak_snapshot_complete(global_State *g, uint32_t *pn)
{
  uint64_t reserved, cleared;
  MSize cap;
  uint32_t n;
  GCRef *stack;
  uint8_t *ready;
  if (!g)
    return 0;
  if (gc2_phase_acq(g) != LJ_GC2_WEAK)
    return 0;
  cap = gc2_weak_capacity_acq(g);
  stack = gc2_weak_stack_acq(g);
  ready = gc2_weak_ready_acq(g);
  if (!stack || !ready) {
    if (pn)
      *pn = 0;
    return gc2_weak_count_acq(g) == 0;
  }
  reserved = gc2_weak_count_acq(g);
  if (reserved > (uint64_t)cap)
    return 0;  /* Overflowed snapshots are handled by the owner-clear bridge. */
  n = lj_gc2_weak_snapshot_count(g);
  if ((uint64_t)n != reserved)
    return 0;  /* Reserved slots must all be published in the ready prefix. */
  cleared = gc2_weak_clear_cursor_acq(g);
  if (cleared < (uint64_t)n)
    return 0;  /* The bounded GC2 clear cursor has not drained the prefix. */
  if (pn)
    *pn = n;
  return 1;
}

static int gc2_weak_snapshot_has_tab(global_State *g, GCtab *t, uint32_t n)
{
  GCRef *stack = gc2_weak_stack_acq(g);
  uint32_t i;
  if (!stack)
    return 0;
  for (i = 0; i < n; i++) {
    GCobj *o = gc2_queue_slot_load_acq(&stack[i]);
    if (o == obj2gco(t))
      return 1;
  }
  return 0;
}

int lj_gc2_weak_snapshot_covers_legacy(global_State *g, GCobj *legacy)
{
  uint32_t n;
  uint64_t legacy_count = 0;
  if (!gc2_weak_snapshot_complete(g, &n))
    return 0;
  while (legacy) {
    GCtab *t;
    int found = 0;
    uint8_t flags;
    if (legacy->gch.gct != ~LJ_TTAB)
      return 0;
    t = gco2tab(legacy);
    flags = lj_obj_gcflags(obj2gco(t));
    if ((flags & LJ_GC_WEAK) == 0)
      return 0;
    if (legacy_count >= (uint64_t)n)
      return 0;  /* Conservative guard against duplicates/corruption. */
    legacy_count++;
    found = gc2_weak_snapshot_has_tab(g, t, n);
    if (!found)
      return 0;
    legacy = gcref_acq(t->gclist);
  }
  return 1;  /* 05 section 5.8: GC2-cleared snapshot covers legacy weak list. */
}

static int gc2_weak_backfill_legacy(global_State *g, GCobj *legacy)
{
  uint32_t n;
  uint64_t tables = 0, slots = 0, cleared = 0;
  if (!gc2_weak_snapshot_complete(g, &n))
    return 0;
  while (legacy) {
    GCtab *t;
    uint8_t flags;
    if (legacy->gch.gct != ~LJ_TTAB)
      return 0;
    t = gco2tab(legacy);
    flags = lj_obj_gcflags(obj2gco(t));
    if ((flags & LJ_GC_WEAK) == 0)
      return 0;
    if (!gc2_weak_snapshot_has_tab(g, t, n)) {
      gc2_weak_process_tab(g, t, 1, &slots, &cleared);
      tables++;
    }
    legacy = gcref_acq(t->gclist);
  }
  if (tables) {
    gc2_weak_legacy_backfills_add(g, 1);
    gc2_weak_legacy_backfill_tables_add(g, tables);
    gc2_weak_legacy_backfill_slots_add(g, slots);
    gc2_weak_legacy_backfill_cleared_add(g, cleared);
  }
  return 1;  /* 05 section 5.8: owner-cleared legacy weak snapshot gaps. */
}

static int gc2_weak_overflow_clear_legacy(global_State *g, GCobj *legacy)
{
  uint64_t reserved, tables = 0, slots = 0, cleared = 0;
  MSize cap;
  GCRef *stack;
  uint8_t *ready;
  if (!g || gc2_phase_acq(g) != LJ_GC2_WEAK)
    return 0;
  cap = gc2_weak_capacity_acq(g);
  stack = gc2_weak_stack_acq(g);
  ready = gc2_weak_ready_acq(g);
  if (!stack || !ready)
    return 0;
  reserved = gc2_weak_count_acq(g);
  if (reserved <= (uint64_t)cap)
    return 0;
  while (legacy) {
    GCtab *t;
    uint8_t flags;
    if (legacy->gch.gct != ~LJ_TTAB)
      return 0;
    t = gco2tab(legacy);
    flags = lj_obj_gcflags(obj2gco(t));
    if ((flags & LJ_GC_WEAK) == 0)
      return 0;
    gc2_weak_process_tab(g, t, 1, &slots, &cleared);
    tables++;
    legacy = gcref_acq(t->gclist);
  }
  if (tables) {
    gc2_weak_legacy_backfills_add(g, 1);
    gc2_weak_legacy_backfill_tables_add(g, tables);
    gc2_weak_legacy_backfill_slots_add(g, slots);
    gc2_weak_legacy_backfill_cleared_add(g, cleared);
  }
  return 1;  /* 05 section 5.8: overflowed weak snapshots stay GC2-owned. */
}

void lj_gc2_weak_legacy_result(global_State *g, int skipped)
{
  if (!g)
    return;
  if (skipped)
    gc2_weak_legacy_skipped_add(g, 1);
  else
    gc2_weak_legacy_fallbacks_add(g, 1);
}

int lj_gc2_weak_complete(global_State *g, GCobj *legacy, uint32_t drain_limit)
{
  uint32_t weakdrain;
  uint64_t progress = 0;
  if (!g || drain_limit == 0 || gc2_phase_acq(g) != LJ_GC2_WEAK)
    return 0;
  gc2_weak_complete_runs_add(g, 1);
  for (;;) {
    weakdrain = lj_gc2_worker_drain(g, drain_limit);
    if (weakdrain) {
      progress += (uint64_t)weakdrain;
      continue;
    }
    if (gc2_worker_active_acq(g) == 0)
      break;
    la_cpu_pause();  /* 05 section 5.8: peer drain must finish before fallback. */
  }
  if (progress)
    gc2_weak_complete_progress_add(g, progress);
  if (lj_gc2_weak_snapshot_covers_legacy(g, legacy)) {
#if LJ_GC2_PARANOIA
    gc2_weak_paranoia_zero_diff(g, legacy);
#endif
    lj_gc2_weak_legacy_result(g, 1);
    return 1;  /* 05 section 5.8 scheduler-owned weak completion bridge. */
  }
  if (gc2_weak_overflow_clear_legacy(g, legacy)) {
#if LJ_GC2_PARANOIA
    gc2_weak_paranoia_zero_diff(g, legacy);
#endif
    lj_gc2_weak_legacy_result(g, 1);
    return 1;  /* 05 section 5.8 owner-cleared overflow bridge. */
  }
  if (gc2_weak_backfill_legacy(g, legacy)) {
#if LJ_GC2_PARANOIA
    gc2_weak_paranoia_zero_diff(g, legacy);
#endif
    lj_gc2_weak_legacy_result(g, 1);
    return 1;  /* 05 section 5.8 owner-cleared legacy weak gaps. */
  }
  lj_gc2_weak_legacy_result(g, 0);
  return 0;  /* 05 section 5.8 conditional legacy weak fallback. */
}

void lj_gc2_finreg_cdata_set(global_State *g, GCobj *o, int enabled)
{
#if LJ_HASFFI
  if (!g || !o || o->gch.gct != ~LJ_TCDATA)
    return;
  if (enabled)
    gc2_finreg_cdata_sets_add(g, 1);
  else
    gc2_finreg_cdata_clears_add(g, 1);
#else
  UNUSED(g); UNUSED(o); UNUSED(enabled);
#endif
}

static void gc2_finreg_queue_mark(global_State *g, GCobj *o)
{
  uint32_t phase;
  if (!g || !o)
    return;
  phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK)
    (void)lj_gc2_markobj(g, o);  /* 05 section 5.8 FINREG resurrection. */
}

void lj_gc2_finreg_cdata_queue(global_State *g, GCobj *o)
{
#if LJ_HASFFI
  if (!g || !o || o->gch.gct != ~LJ_TCDATA)
    return;
  gc2_finreg_queue_mark(g, o);
  gc2_finreg_cdata_queued_add(g, 1);
#else
  UNUSED(g); UNUSED(o);
#endif
}

#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
void lj_gc2_test_finreg_cdata_preclaim_fail(global_State *g, uint32_t n)
{
  if (g)
    gc2_finreg_cdata_preclaim_test_fail_rel(g, n);
}

void lj_gc2_test_finreg_cdata_preclaim_publish_pause(global_State *g)
{
  if (!g)
    return;
  gc2_finreg_cdata_preclaim_publish_release_rel(g, 0);
  gc2_finreg_cdata_preclaim_publish_paused_rel(g, 0);
  gc2_finreg_cdata_preclaim_publish_pause_rel(g, 1);
}

void lj_gc2_test_finalizer_drain_pause(global_State *g)
{
  if (!g)
    return;
  gc2_finalizer_drain_test_release_rel(g, 0);
  gc2_finalizer_drain_test_paused_rel(g, 0);
  gc2_finalizer_drain_test_pause_rel(g, 1);
}
#endif

int lj_gc2_finreg_cdata_preclaim(lua_State *L, global_State *g, GCobj *o,
				 cTValue *fin)
{
#if LJ_HASFFI
  MSize count, cap;
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  uint32_t test_fail;
#endif
  if (!L || !g || !o || !fin || o->gch.gct != ~LJ_TCDATA)
    return 0;
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  test_fail = gc2_finreg_cdata_preclaim_test_fail_acq(g);
  if (test_fail) {
    gc2_finreg_cdata_preclaim_test_fail_rel(g, test_fail - 1u);
    gc2_finreg_cdata_preclaim_overflow_add(g, 1);
    return 0;  /* Test-only side-vector failure injection. */
  }
#endif
  count = gc2_finreg_cdata_preclaim_count_acq(g);
  cap = gc2_finreg_cdata_preclaim_capacity_acq(g);
  if (!gc2_finreg_cdata_preclaim_ready(g)) {
    if (!gc2_finclaim_resize(g, GC2_FINCLAIM_INIT)) {
      gc2_finreg_cdata_preclaim_overflow_add(g, 1);
      return 0;
    }
    count = gc2_finreg_cdata_preclaim_count_acq(g);
    cap = gc2_finreg_cdata_preclaim_capacity_acq(g);
  }
  if (count >= cap) {
    MSize ncap = gc2_finclaim_next_capacity(cap, count + 1u);
    if (ncap <= cap || !gc2_finclaim_resize(g, ncap)) {
      gc2_finreg_cdata_preclaim_overflow_add(g, 1);
      return 0;
    }
    count = gc2_finreg_cdata_preclaim_count_acq(g);
    cap = gc2_finreg_cdata_preclaim_capacity_acq(g);
  }
  if (count >= cap) {
    gc2_finreg_cdata_preclaim_overflow_add(g, 1);
    return 0;
  }
  gc2_finclaim_publish(L, g, count, o, fin);
  gc2_finreg_cdata_preclaim_count_rel(g, count + 1u);
  gc2_finreg_cdata_pweak_claimed_add(g, 1);
  return 1;  /* 05 section 5.8: P_WEAK owns claimed cdata finalizer. */
#else
  UNUSED(L); UNUSED(g); UNUSED(o); UNUSED(fin);
  return 0;
#endif
}

int lj_gc2_finreg_cdata_preclaim_take(lua_State *L, global_State *g,
				      GCobj *o, TValue *fin)
{
#if LJ_HASFFI
  MSize head, count, i;
  GCobj *claimed;
  if (!L || !g || !o || !fin || o->gch.gct != ~LJ_TCDATA ||
      !gc2_finreg_cdata_preclaim_ready(g))
    return 0;
  head = gc2_finreg_cdata_preclaim_head_acq(g);
  count = gc2_finreg_cdata_preclaim_count_acq(g);
  if (head >= count)
    return 0;
  for (i = head; i < count; i++) {
    claimed = gc2_finreg_cdata_preclaim_obj_acq(g, i);
    if (claimed != o)
      continue;
    gc2_finreg_cdata_preclaim_fin_acq(g, i, fin);
    gc2_finclaim_clear(L, g, i);
    while (head < count &&
	   gc2_finreg_cdata_preclaim_obj_acq(g, head) == NULL)
      head++;
    if (head == count) {
      gc2_finreg_cdata_preclaim_head_rel(g, 0);
      gc2_finreg_cdata_preclaim_count_rel(g, 0);
    } else {
      gc2_finreg_cdata_preclaim_head_rel(g, head);
    }
    gc2_finreg_cdata_preclaim_dispatched_add(g, 1);
    return 1;  /* 05 section 5.8: dispatch order may differ from FINREG scan. */
  }
  if (head == count) {
    gc2_finreg_cdata_preclaim_head_rel(g, 0);
    gc2_finreg_cdata_preclaim_count_rel(g, 0);
  }
  return 0;
#else
  UNUSED(L); UNUSED(g); UNUSED(o); UNUSED(fin);
  return 0;
#endif
}

int lj_gc2_finreg_udata_set(global_State *g, GCobj *o, int enabled)
{
  uint8_t old;
  if (!g || !o || o->gch.gct != ~LJ_TUDATA)
    return 0;
  old = la_load8_acq(lj_obj_gcflags_ref(o));
  for (;;) {
    uint8_t next = enabled ? (uint8_t)(old | LJ_GC_UDATA_FINREG) :
			     (uint8_t)(old & (uint8_t)~LJ_GC_UDATA_FINREG);
    if (next == old)
      return 0;
    if (la_cas8(lj_obj_gcflags_ref(o), &old, next, LA_ACQ_REL, LA_ACQ))
      break;
  }
  if (enabled) {
    gc2_finreg_udata_sets_add(g, 1);
    return 1;
  } else {
    gc2_finreg_udata_clears_add(g, 1);
    return -1;
  }
}

void lj_gc2_finreg_udata_register(lua_State *L, global_State *g, GCobj *o)
{
  GC2FinRegUDataNode *node, *head;
  if (!L || !g || !o || o->gch.gct != ~LJ_TUDATA)
    return;
  for (node = gc2_finreg_udata_head_acq(g);
       node != NULL;
       node = gc2_finreg_udata_next_acq(node)) {
    if (gc2_finreg_udata_active_acq(node) &&
	gc2_finreg_udata_obj_acq(node) == o)
      return;
  }
  node = lj_mem_newt(L, sizeof(GC2FinRegUDataNode), GC2FinRegUDataNode);
  gc2_finreg_udata_obj_rel(node, o);
  gc2_finreg_udata_retired_next_rel(node, NULL);
  gc2_finreg_udata_active_rel(node, 1);
  do {
    head = gc2_finreg_udata_head_acq(g);
    gc2_finreg_udata_next_rel(node, head);
  } while (!gc2_finreg_udata_head_cas(g, &head, node));
  gc2_finreg_udata_registered_add(g, 1);
}

void lj_gc2_finreg_udata_register_mt(lua_State *L, global_State *g,
				     GCudata *ud, GCtab *mt)
{
  TValue mmv;
  if (!L || !g || !ud || !mt)
    return;
  lj_gc2_finreg_udata_register(L, g, obj2gco(ud));
  if (lj_meta_fasttv(g, mt, MM_gc, &mmv))
    (void)lj_gc2_finreg_udata_set(g, obj2gco(ud), 1);
}

static int gc2_finreg_udata_retire(global_State *g,
				   GC2FinRegUDataNode *node)
{
  GC2FinRegUDataNode *head;
  if (!g || !node)
    return 0;
  lj_assertG(gc2_finreg_udata_obj_acq(node) == NULL,
	     "retiring live userdata FINREG node");
  if (!gc2_finreg_udata_active_retire(node))
    return 0;
  do {
    head = gc2_finreg_udata_retired_acq(g);
    gc2_finreg_udata_retired_next_rel(node, head);
  } while (!gc2_finreg_udata_retired_cas(g, &head, node));
  gc2_finreg_udata_retired_nodes_add(g, 1);
  return 1;
}

int lj_gc2_finreg_udata_unlink(global_State *g, GC2FinRegUDataNode *prev,
			       GC2FinRegUDataNode *node,
			       GC2FinRegUDataNode *next)
{
  GC2FinRegUDataNode *expect;
  if (!g || !node)
    return 0;
  if (!gc2_finreg_udata_retire(g, node))
    return 1;
  if (prev) {
    expect = node;
    if (gc2_finreg_udata_active_acq(prev))
      (void)gc2_finreg_udata_next_cas(prev, &expect, next);
  } else {
    expect = node;
    (void)gc2_finreg_udata_head_cas(g, &expect, next);
  }
  return 1;  /* 05 section 5.8: logical retire plus best-effort CAS unlink. */
}

void lj_gc2_finreg_udata_forget(global_State *g, GCobj *o)
{
  GC2FinRegUDataNode *prev, *node;
  int cleared = 0;
  if (!g || !o || o->gch.gct != ~LJ_TUDATA)
    return;
  prev = NULL;
  node = gc2_finreg_udata_head_acq(g);
  while (node) {
    GC2FinRegUDataNode *next = gc2_finreg_udata_next_acq(node);
    if (!gc2_finreg_udata_active_acq(node)) {
      node = next;
      continue;
    }
    if (gc2_finreg_udata_obj_acq(node) == o) {
      gc2_finreg_udata_obj_clear(node);
      cleared = 1;
      if (lj_gc2_finreg_udata_unlink(g, prev, node, next)) {
	node = next;
	continue;
      }
      prev = NULL;
      node = gc2_finreg_udata_head_acq(g);
      continue;
    }
    prev = node;
    node = next;
  }
  if (cleared)
    gc2_finreg_udata_forgets_add(g, 1);
}

void lj_gc2_finreg_udata_queue(global_State *g, GCobj *o)
{
  if (!g || !o || o->gch.gct != ~LJ_TUDATA)
    return;
  gc2_finreg_queue_mark(g, o);
  gc2_finreg_udata_queued_add(g, 1);
}

static GCobj *gc2_grey_pop(global_State *g)
{
  GCRef *stack;
  uint64_t top, bottom;
  GCobj *o;
  MSize cap;
  if (!g)
    return NULL;
  stack = gc2_grey_stack_acq(g);
  cap = gc2_grey_capacity_acq(g);
  if (!stack || cap == 0)
    return NULL;
  bottom = gc2_grey_bottom_rlx(g);
  if (bottom == 0)
    return NULL;
  bottom--;
  gc2_grey_bottom_store_rlx(g, bottom);
  la_fence_seq();  /* Chase-Lev owner pop: order bottom before top load. */
  top = gc2_grey_top_acq(g);
  if (top <= bottom) {
    o = gc2_queue_slot_load_acq(&stack[(MSize)(bottom % cap)]);
    if (top == bottom) {
      uint64_t expect = top;
      /* 05 section 5.6.3: single item is claimed through top. */
      if (!gc2_grey_top_cas(g, &expect, top + 1)) {
	o = NULL;
      }
      gc2_grey_bottom_rel(g, top + 1);
    }
    return o;
  }
  gc2_grey_bottom_rel(g, top);
  return NULL;
}

GCobj *lj_gc2_grey_steal(global_State *g)
{
  GCRef *stack;
  uint64_t top, bottom, expect;
  GCobj *o;
  MSize cap;
  if (!g)
    return NULL;
  stack = gc2_grey_stack_acq(g);
  cap = gc2_grey_capacity_acq(g);
  if (!stack || cap == 0)
    return NULL;
  /* 05 section 5.6.3: non-owner steal; deque growth is owner-quiesced. */
  top = gc2_grey_top_acq(g);
  la_fence_seq();  /* 05 section 5.6.3: order top before bottom snapshot. */
  bottom = gc2_grey_bottom_acq(g);
  if (top >= bottom)
    return NULL;
  o = gc2_queue_slot_load_acq(&stack[(MSize)(top % cap)]);
  expect = top;
  /* 05 section 5.6.3: steal claim linearizes through top. */
  if (!gc2_grey_top_cas(g, &expect, top + 1))
    return NULL;
  return o;
}

static void gc2_ssb_activate(TGState *tg, GC2SSBNode *node)
{
  lj_gc2_ssb_next_rel(node, NULL);
  node->n = 0;
  lj_tg_ssb_active_rel(tg, node);
  /* 05 section 5.6.2: publish active SSB cursor reset. */
  lj_tg_ssb_base_rel(tg, node->slot);
  lj_tg_ssb_end_rel(tg, node->slot + TG_GC2_SSB_SLOTS);
  lj_tg_ssb_next_rel(tg, node->slot);
}

static void gc2_ssb_publish(global_State *g, GC2SSBNode *node)
{
  GC2SSBNode *head = gc2_ssb_head_acq(g);
  do {
    lj_gc2_ssb_next_rel(node, head);
  } while (!gc2_ssb_head_cas(g, &head, node));
  lj_gc2_worker_wake(g);  /* 05 section 5.6.3 parked worker scheduler. */
}

static uint32_t gc2_flush_ssb(global_State *g, TGState *tg, int allow_drain)
{
  GC2SSBNode *node, *fresh;
  GCRef *base, *next;
  uint32_t n;
  if (!g || !tg)
    return 0;
  node = lj_tg_ssb_active_acq(tg);
  if (!node)
    return 0;
  base = lj_tg_ssb_base_acq(tg);
  next = lj_tg_ssb_next_acq(tg);
  if (!base || !next)
    return 0;
  n = (uint32_t)(next - base);
  if (n == 0)
    return 0;
  fresh = lj_tg_ssb_free_pop(tg);
  if (!fresh && allow_drain) {
    (void)lj_gc2_drain_ssb(g);  /* Temporary scaffold until workers recycle. */
    fresh = lj_tg_ssb_free_pop(tg);
  }
  if (!fresh)
    return 0;
  node->n = n;
  gc2_ssb_publish(g, node);
  gc2_ssb_published_add(g, 1);
  gc2_ssb_items_published_add(g, n);
  gc2_ssb_activate(tg, fresh);
  return n;
}

uint32_t lj_gc2_flush_ssb(global_State *g, TGState *tg)
{
  return gc2_flush_ssb(g, tg, 1);
}

static int gc2_ssb_push(global_State *g, GCobj *o, int allow_drain)
{
  TGState *tg;
  GCRef *next, *end;
  if (!g || !o)
    return 0;
  tg = G2TG(g);
  if (!tg)
    return 0;
  next = lj_tg_ssb_next_acq(tg);
  end = lj_tg_ssb_end_acq(tg);
  if (!next || !end)
    return 0;
  if (next == end) {
    if (gc2_flush_ssb(g, tg, allow_drain) == 0)
      return 0;
    next = lj_tg_ssb_next_acq(tg);
    end = lj_tg_ssb_end_acq(tg);
    if (!next || !end || next == end)
      return 0;
  }
  gc2_queue_slot_store_rel(next, o);
  /* 05 section 5.6.2: slot release-published before cursor advance. */
  lj_tg_ssb_next_rel(tg, next + 1);
  return 1;
}

int lj_gc2_ssb_push(global_State *g, GCobj *o)
{
  return gc2_ssb_push(g, o, 1);
}

static void gc2_ssb_mark_one(global_State *g, GCobj *o)
{
  if (o) {
    void *base = gc2_mark_base(o);
    int marked = lj_gc2_ismarkedmem(g, base);
    if (marked < 0 || o->gch.gct == 0)
      return;
    if (marked == 0)
      (void)lj_gc2_markmem(g, base);
    if (gc2_mark_base_traversable(g, base)) {
      int pushed = gc2_grey_push(g, o);
      lj_assertG(pushed, "gc2 grey push failed for SSB object");
      UNUSED(pushed);
    }
  }
}

static void gc2_ssb_recycle_node(GC2SSBNode *node)
{
  TGState *owner = node->owner;
  node->n = 0;
  if (owner) {
    lj_tg_ssb_free_push(owner, node);
  } else {
    lj_gc2_ssb_next_rel(node, NULL);
  }
}

static void gc2_ssb_publish_list(global_State *g, GC2SSBNode *head)
{
  GC2SSBNode *tail, *next;
  GC2SSBNode *oldhead;
  if (!g || !head)
    return;
  tail = head;
  while ((next = lj_gc2_ssb_next_acq(tail)) != NULL)
    tail = next;
  oldhead = gc2_ssb_head_acq(g);
  do {
    lj_gc2_ssb_next_rel(tail, oldhead);
  } while (!gc2_ssb_head_cas(g, &oldhead, head));  /* 05 section 5.6.2. */
}

static LJ_NOINLINE uint32_t gc2_drain_published_ssb_to_grey(global_State *g,
							    uint32_t limit)
{
  GC2SSBNode *node;
  uint32_t nitems = 0, nnodes = 0;
  if (!g || limit == 0)
    return 0;
  node = gc2_ssb_head_xchg_acqrel(g, NULL);
  while (node && nitems < limit) {
    GC2SSBNode *next = lj_gc2_ssb_next_acq(node);
    while (node->n > 0 && nitems < limit) {
      GCRef *slot = &node->slot[node->n - 1u];
      GCobj *o = gc2_queue_slot_load_acq(slot);
      gc2_queue_slot_clear_rel(slot);
      node->n--;
      gc2_ssb_mark_one(g, o);
      nitems++;
    }
    if (node->n == 0) {
      nnodes++;
      gc2_ssb_recycle_node(node);
      node = next;
    } else {
      lj_gc2_ssb_next_rel(node, next);
      break;
    }
  }
  if (node)
    gc2_ssb_publish_list(g, node);
  if (nnodes) {
    gc2_ssb_drained_add(g, nnodes);
  }
  if (nitems)
    gc2_ssb_items_drained_add(g, nitems);
  return nitems;
}

static uint32_t gc2_drain_active_ssb_to_grey(global_State *g, TGState *tg,
					     uint32_t limit)
{
  GCRef *base, *next;
  uint32_t n = 0;
  if (!g || !tg || limit == 0)
    return 0;
  base = lj_tg_ssb_base_acq(tg);
  next = lj_tg_ssb_next_acq(tg);
  if (!base || !next)
    return 0;
  while (n < limit && next > base) {
    GCRef *slot = next - 1;
    GCobj *o = gc2_queue_slot_load_acq(slot);
    gc2_queue_slot_clear_rel(slot);
    gc2_ssb_mark_one(g, o);
    next = slot;
    /* 05 section 5.7.1: publish slot processed before cursor retreat. */
    lj_tg_ssb_next_rel(tg, next);
    n++;
  }
  return n;
}

uint32_t lj_gc2_drain_ssb(global_State *g)
{
  uint32_t nitems;
  if (!g)
    return 0;
  nitems = gc2_drain_published_ssb_to_grey(g, ~(uint32_t)0);
  (void)gc2_drain_grey(g, ~(uint32_t)0);  /* Temporary single-worker scaffold. */
  return nitems;
}

uint32_t lj_gc2_assist(global_State *g, TGState *tg)
{
  uint32_t phase, shift, limit, expect = 0, n = 0, converted = 0, weak = 0;
  if (!g || !tg || lj_tg_gc_assist_acq(tg))
    return 0;
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK)
    return 0;
  if (!lj_gc2_hard_limit_reached(g))
    return 0;
  if (!gc2_assist_active_cas(g, &expect, 1))
    return 0;  /* Current global grey deque has one owner side. */
  lj_tg_gc_assist_store_rlx(tg, 1);
  gc2_assist_runs_add(g, 1);  /* 05 section 5.11 telemetry. */
  shift = gc2_assist_shift_acq(g);
  if (shift > 8u)
    shift = 8u;
  limit = 1u << shift;
  (void)lj_gc2_flush_alloc(g, tg);
  while (n < limit) {
    uint32_t left = limit - n;
    uint32_t drained = gc2_drain_grey(g, left);
    if (drained) {
      n += drained;
      continue;
    }
    if (converted >= limit)
      break;
    if (!gc2_drain_active_ssb_to_grey(g, tg, 1) &&
	!gc2_drain_published_ssb_to_grey(g, 1))
      break;
    converted++;
  }
  if (phase == LJ_GC2_WEAK) {
    uint32_t work = n + converted;
    if (work < limit)
      weak = lj_gc2_weak_drain(g, limit - work);  /* 05 section 5.11. */
  }
  if (n)
    gc2_assist_grey_drained_add(g, n);
  if (converted)
    gc2_assist_ssb_converted_add(g, converted);
  if (weak)
    gc2_assist_weak_drained_add(g, weak);
  lj_tg_gc_assist_store_rlx(tg, 0);
  gc2_assist_active_rel(g, 0);
  return n + weak;
}

int lj_gc2_ssb_empty(global_State *g)
{
  TGState *tg;
  if (!g)
    return 1;
  if (gc2_ssb_head_acq(g) != NULL)
    return 0;  /* 05 section 5.7.1 SSB-empty fixpoint predicate. */
  if (!gc2_grey_empty(g))
    return 0;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    GCRef *next, *base;
    if (lj_tg_flags_test_acq(tg, TGF_DEAD))
      continue;
    next = lj_tg_ssb_next_acq(tg);
    base = lj_tg_ssb_base_acq(tg);
    if (next != base)
      return 0;
  }
  return 1;
}

static int gc2_barrier_active_g(global_State *g)
{
  TGState *tg;
  uint32_t phase;
  if (!g)
    return 0;
  tg = G2TG(g);
  if (!tg || !lj_tg_mark_active_acq(tg))
    return 0;
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK)
    return 0;
  return 1;
}

static int gc2_remember_active_g(global_State *g)
{
  TGState *tg;
  if (!g || gc2_phase_acq(g) != LJ_GC2_IDLE ||
      gc2_generational_acq(g) == 0)
    return 0;
  tg = G2TG(g);
  return tg && lj_tg_mark_active_acq(tg);
}

static void gc2_remember_obj(global_State *g, GCobj *o)
{
  if (!g || !o)
    return;
  if (gc2_ssb_push(g, o, 0)) {
    gc2_remembered_pushed_add(g, 1);
  } else {
    gc2_remembered_overflows_add(g, 1);
    lj_gc2_force_major(g);
    (void)gc2_request_cycle(g, G2TG(g));
  }
}

static int gc2_remember_pair_match(global_State *g, GCobj *parent,
				   GCobj *child)
{
  if (gc2_minor_sweep_enabled_acq(g) == 0 || child == NULL)
    return 1;
  if (parent && lj_gc2_ismarked(g, parent) <= 0)
    return 0;
  return lj_gc2_ismarked(g, child) == 0;
}

static void gc2_remember_pair(global_State *g, GCobj *parent, GCobj *child)
{
  GCobj *root = parent ? parent : child;
  if (!g || !root || !gc2_remember_active_g(g))
    return;
  gc2_remembered_barriers_add(g, 1);
  if (!gc2_remember_pair_match(g, parent, child)) {
    gc2_remembered_filtered_add(g, 1);
    return;
  }
  gc2_remember_obj(g, root);
}

static int gc2_tab_weak_mode(global_State *g, GCtab *t, GCtab *mt)
{
  int weak = 0;
  TValue modev;
  cTValue *mode = lj_meta_fasttv(g, mt, MM_mode, &modev);
  if (mode && tvisstr(mode)) {
    const char *modestr = strVdata(mode);
    int c;
    (void)lj_gc2_markobj(g, gcV(mode));  /* Weak mode metadata is strong. */
    while ((c = *modestr++)) {
      if (c == 'k') weak |= LJ_GC_WEAKKEY;
      else if (c == 'v') weak |= LJ_GC_WEAKVAL;
    }
  #if LJ_HASFFI
    if (weak && gc2_tab_is_ffi_fin(g, t))
      weak = (int)(~0u & ~LJ_GC_WEAKVAL);
  #endif
  }
  return weak;
}

static int gc2_tab_weak_barrier_mode(global_State *g, GCtab *t)
{
  int weak = lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAK;
  if (weak)
    return weak;  /* 05 section 5.8: use captured P_WEAK mode. */
  return gc2_tab_weak_mode(g, t, tabref_acq(t->metatable));
}

void lj_gc2_barrier_tv(lua_State *L, cTValue *tv)
{
  global_State *g;
  TValue snap;
  if (L && tv) {
    lj_tv_load_acq(&snap, tv);
    if (tvisgcv(&snap)) {
      g = G(L);
      if (gc2_barrier_active_g(g))
	lj_gc2_markobj(g, gcV(&snap));
      else
	gc2_remember_pair(g, NULL, gcV(&snap));
    }
  }
}

void lj_gc2_barrier_tv_g(global_State *g, cTValue *tv)
{
  TValue snap;
  if (tv) {
    lj_tv_load_acq(&snap, tv);
    if (tvisgcv(&snap)) {
      if (gc2_barrier_active_g(g))
	lj_gc2_markobj(g, gcV(&snap));
      else
	gc2_remember_pair(g, NULL, gcV(&snap));
    }
  }
}

void lj_gc2_barrier_tvn_g(global_State *g, cTValue *tv, uint32_t n)
{
  uint32_t i;
  if (!tv)
    return;
  if (gc2_barrier_active_g(g)) {
    for (i = 0; i < n; i++)
      lj_gc2_barrier_tv_g(g, &tv[i]);
  } else if (gc2_remember_active_g(g)) {
    for (i = 0; i < n; i++) {
      TValue snap;
      lj_tv_load_acq(&snap, &tv[i]);
      if (tvisgcv(&snap))
	gc2_remember_pair(g, NULL, gcV(&snap));
    }
  }
}

void lj_gc2_barrier_tvn_pair_g(global_State *g, GCobj *parent,
			       cTValue *tv, uint32_t n)
{
  uint32_t i;
  if (!tv)
    return;
  if (gc2_barrier_active_g(g)) {
    for (i = 0; i < n; i++)
      lj_gc2_barrier_tv_g(g, &tv[i]);
  } else if (gc2_remember_active_g(g)) {
    for (i = 0; i < n; i++) {
      TValue snap;
      lj_tv_load_acq(&snap, &tv[i]);
      if (tvisgcv(&snap))
	gc2_remember_pair(g, parent, gcV(&snap));
    }
  }
}

void lj_gc2_barrier_uv(global_State *g, cTValue *tv)
{
  lj_gc2_barrier_tv_g(g, tv);
}

void lj_gc2_barrier_obj(lua_State *L, GCobj *o)
{
  global_State *g;
  if (!o || !L)
    return;
  g = G(L);
  if (gc2_barrier_active_g(g))
    lj_gc2_markobj(g, o);
  else
    gc2_remember_pair(g, NULL, o);
}

void lj_gc2_barrier_obj_pair(lua_State *L, GCobj *parent, GCobj *child)
{
  global_State *g;
  if (!child || !L)
    return;
  g = G(L);
  if (gc2_barrier_active_g(g))
    lj_gc2_markobj(g, child);
  else
    gc2_remember_pair(g, parent, child);
}

void lj_gc2_barrier_tv_pair_g(global_State *g, GCobj *parent, cTValue *tv)
{
  TValue snap;
  if (tv) {
    lj_tv_load_acq(&snap, tv);
    if (tvisgcv(&snap)) {
      if (gc2_barrier_active_g(g))
	lj_gc2_markobj(g, gcV(&snap));
      else
	gc2_remember_pair(g, parent, gcV(&snap));
    }
  }
}

void lj_gc2_barrier_tv_pair(lua_State *L, GCobj *parent, cTValue *tv)
{
  if (L)
    lj_gc2_barrier_tv_pair_g(G(L), parent, tv);
}

static void gc2_barrier_tab_mark(global_State *g, GCtab *t)
{
  GCobj *o;
  int marked;
  o = obj2gco(t);
  marked = lj_gc2_ismarked(g, o);
  if (marked > 0) {
    int pushed = lj_gc2_ssb_push(g, o);
    lj_assertG(pushed, "gc2 table barrier SSB push failed");
    UNUSED(pushed);
  } else if (marked == 0) {
    (void)lj_gc2_markobj(g, o);
  }
}

void lj_gc2_barrier_tab_g(global_State *g, GCtab *t)
{
  if (!t)
    return;
  if (gc2_barrier_active_g(g))
    gc2_barrier_tab_mark(g, t);
  else
    gc2_remember_pair(g, obj2gco(t), NULL);
}

void lj_gc2_barrier_tab(lua_State *L, GCtab *t)
{
  global_State *g;
  if (!t || !L)
    return;
  g = G(L);
  if (gc2_barrier_active_g(g))
    gc2_barrier_tab_mark(g, t);
  else
    gc2_remember_pair(g, obj2gco(t), NULL);
}

void lj_gc2_barrier_weak_key(lua_State *L, GCtab *t, cTValue *key)
{
  global_State *g;
  int weak;
  if (!L || !t || !key || !tvisgcv(key))
    return;
  g = G(L);
  if (gc2_phase_acq(g) != LJ_GC2_WEAK)
    return;
  weak = gc2_tab_weak_barrier_mode(g, t);
  /* 05 section 5.8 weak-table key write. */
  if (weak && lj_gc2_markobj(g, gcV(key)))
    gc2_weak_keys_marked_add(g, 1);
}

void lj_gc2_barrier_weak_write(lua_State *L, GCtab *t, cTValue *key,
			       cTValue *val)
{
  global_State *g;
  if (!L || !t)
    return;
  g = G(L);
  if (gc2_phase_acq(g) != LJ_GC2_WEAK)
    return;
  if (gc2_tab_weak_barrier_mode(g, t) == 0)
    return;
  if (key && tvisgcv(key) && lj_gc2_markobj(g, gcV(key)))
    gc2_weak_keys_marked_add(g, 1);
  if (val && tvisgcv(val) && lj_gc2_markobj(g, gcV(val)))
    gc2_weak_values_marked_add(g, 1);
}

static int gc2_mark_base_traversable(global_State *g, void *p)
{
  TGState *tg = gc2_tg_for_mem(g, p);
  GCArena *a;
  if (!p || !tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return 0;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    LJHugeInfo hi;
    if (!lj_tg_flags_test_acq(tg, TGF_HUGETAB) ||
	lj_arena_hugetab_lookup(&tg->huge, p, &hi) != 1)
      return 0;
    return (hi.flags & LJ_HUGEF_TRAVERSABLE) != 0;
  }
  return (a->hdr.flags & LJ_AF_TRAVERSABLE) != 0;
}

static void *gc2_mark_base(GCobj *o)
{
#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    GCcdata *cd = gco2cd(o);
    if (cdataisv(cd))
      return memcdatav(cd);
  }
#endif
  return o;
}

int lj_gc2_markmem(global_State *g, void *p)
{
  TGState *tg = gc2_tg_for_mem(g, p);
  GCArena *a;
  uint32_t cell;
  int marked;
  if (!p || !tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return 0;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    if (!lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      return 0;
    marked = lj_arena_hugetab_mark(&tg->huge, p, NULL);
    if (marked == 1)
      gc2_marks_this_round_add(g, 1);  /* 05 section 5.7.1. */
    return marked == 1;
  }
  cell = lj_arena_cellof(p);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
      !lj_arena_bm_get(a->block, cell))
    return 0;
  marked = !la_bit_test_and_set64(&a->mark[cell >> 6],
				  cell & 63);  /* 05 section 5.6.1. */
  if (marked)
    gc2_marks_this_round_add(g, 1);  /* 05 section 5.7.1. */
  return marked;
}

int lj_gc2_markobj(global_State *g, GCobj *o)
{
  void *base;
  int marked;
  int traversable;
  if (!o)
    return 0;
  base = gc2_mark_base(o);
  marked = lj_gc2_markmem(g, base);
  if (marked) {
    uint32_t phase = gc2_phase_acq(g);
    traversable = gc2_mark_base_traversable(g, base);
    if ((phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK) &&
	(traversable || o->gch.gct == ~LJ_TUDATA)) {
      if (traversable) {
	int pushed = lj_gc2_ssb_push(g, o);  /* 05 section 5.6.1. */
	lj_assertG(pushed, "gc2 SSB push failed for marked traversable object");
	UNUSED(pushed);
      } else {
	gc2_traverse_udata(g, gco2ud(o));
      }
    }
  }
  return marked;
}

static int gc2_markobj_worker(global_State *g, GCobj *o)
{
  void *base;
  int marked;
  int traversable;
  if (!o)
    return 0;
  base = gc2_mark_base(o);
  marked = lj_gc2_markmem(g, base);
  traversable = gc2_mark_base_traversable(g, base);
  if (marked && (traversable || o->gch.gct == ~LJ_TUDATA)) {
    if (traversable) {
      int pushed = gc2_grey_push(g, o);  /* 05 section 5.6.3. */
      lj_assertG(pushed, "gc2 worker grey push failed for marked object");
      UNUSED(pushed);
    } else {
      gc2_traverse_udata(g, gco2ud(o));
    }
  }
  return marked;
}

static void gc2_mark_tv_worker(global_State *g, cTValue *tv)
{
  lj_assertG(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct),
	     "TValue and GC type mismatch");
  if (tvisgcv(tv))
    gc2_markobj_worker(g, gcV(tv));
}

static void gc2_note_weak_table(global_State *g, GCtab *t, int weak)
{
  if (!weak)
    return;
  if (gc2_tab_is_ffi_fin(g, t))
    return;  /* FFI finalizer registry is owned by FINREG, not weak clear. */
  /* 05 section 5.8: capture traversal-time weak mode. */
  lj_obj_masksetgcflags(obj2gco(t), LJ_GC_WEAK, weak);
  gc2_weak_tables_seen_add(g, 1);
  if (weak & LJ_GC_WEAKKEY)
    gc2_weak_tables_weakkey_add(g, 1);
  if (weak & LJ_GC_WEAKVAL)
    gc2_weak_tables_weakval_add(g, 1);
  if (weak == LJ_GC_WEAK)
    gc2_weak_tables_allweak_add(g, 1);
  gc2_weak_record(g, t);
}

#if LJ_HASJIT
static void gc2_marktrace_worker(global_State *g, TraceNo traceno)
{
  if (traceno) {
    GCtrace *T = traceref(G2J(g), traceno);
    if (T)
      gc2_markobj_worker(g, obj2gco(T));
  }
}
#endif

static int gc2_traverse_tab(global_State *g, GCtab *t)
{
  GCtab *mt = tabref_acq(t->metatable);
  int weak = gc2_tab_weak_mode(g, t, mt);
  int ffi_fin = gc2_tab_is_ffi_fin(g, t);
  void *arraymem;
  gc2_note_weak_table(g, t, weak);  /* 05 section 5.8 discovery scaffold. */
  arraymem = lj_tab_array_mem_acq(t);
  if (arraymem)
    lj_gc2_markmem(g, arraymem);
  {
    MSize hmask;
    Node *node = lj_tab_node_snapshot_acq(t, &hmask);
    if (hmask > 0)
      lj_gc2_markmem(g, lj_tab_node_hdrw(node));
  }
  if (mt)
    gc2_markobj_worker(g, obj2gco(mt));
  if (weak == LJ_GC_WEAK)
    return weak;
  if (!(weak & LJ_GC_WEAKVAL)) {
    TValue *array;
    MSize i, asize = lj_tab_array_snapshot_acq(t, &array);
    for (i = 0; i < asize; i++) {
      TValue val;
      lj_tv_load_acq(&val, &array[i]);
      gc2_mark_tv_worker(g, &val);
    }
  }
  {
    MSize i, hmask;
    Node *node = lj_tab_node_snapshot_acq(t, &hmask);
    if (hmask > 0) {
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	TValue key, val;
	int key_loaded = 0;
	lj_tv_load_acq(&val, &n->val);
#if LJ_HASFFI
	if (ffi_fin) {
	  lj_tv_load_acq(&key, &n->key);
	  key_loaded = 1;
	  while (lj_cdata_fin_isclaim(&val) || tviskeylock(&key)) {
	    la_cpu_pause();
	    lj_tv_load_acq(&val, &n->val);
	    lj_tv_load_acq(&key, &n->key);
	  }
	}
#endif
	if (!tvisnil(&val)) {
	  if (!key_loaded)
	    lj_tv_load_acq(&key, &n->key);
	  lj_assertG(!tvisnil(&key), "mark of nil key in non-empty slot");
	  lj_assertG(!tviskeylock(&key),
		     "mark of key lock in non-empty slot");
	  if (!(weak & LJ_GC_WEAKKEY)) gc2_mark_tv_worker(g, &key);
	  if (!(weak & LJ_GC_WEAKVAL)) gc2_mark_tv_worker(g, &val);
	}
      }
    }
  }
  return weak;
}

#if LJ_HASFFI
static void gc2_traverse_clib_cache(global_State *g, CLibrary *cl)
{
  CLibCacheEntry *e;
  for (e = lj_clib_cache_head_acq(cl);
       e != NULL;
       e = lj_clib_cache_next_acq(e)) {
    GCstr *name = lj_clib_cache_name_acq(e);
    TValue tv;
    lj_gc2_markmem(g, e);
    if (name)
      gc2_markobj_worker(g, obj2gco(name));
    lj_clib_cache_val_acq(&tv, e);
    gc2_mark_tv_worker(g, &tv);
  }
}
#endif

static void gc2_traverse_udata(global_State *g, GCudata *ud)
{
  GCtab *mt = tabref_acq(ud->metatable);
  GCtab *env = tabref_acq(ud->env);
  uint8_t udtype = lj_udata_udtype_acq(ud);
  if (mt)
    gc2_markobj_worker(g, obj2gco(mt));
  if (env)
    gc2_markobj_worker(g, obj2gco(env));
#if LJ_HASFFI
  if (udtype == UDTYPE_FFI_CLIB)
    gc2_traverse_clib_cache(g, (CLibrary *)uddata(ud));
  if (udtype == UDTYPE_FFI_PIN) {
    TValue tv;
    lj_tv_load_acq(&tv, (TValue *)uddata(ud));
    gc2_mark_tv_worker(g, &tv);  /* 11.6 ffi.pin() root. */
  }
#endif
  if (LJ_HASBUFFER && udtype == UDTYPE_BUFFER) {
    SBufExt *sbx = (SBufExt *)uddata(ud);
    GCobj *ref;
    if (!sbufiscoworborrow(sbx))
      lj_gc2_markmem(g, lj_buf_bptr_acq((SBuf *)sbx));
    ref = gcref_acq(sbx->cowref);
    if (sbufiscow(sbx) && ref)
      gc2_markobj_worker(g, ref);
    ref = gcref_acq(sbx->dict_str);
    if (ref)
      gc2_markobj_worker(g, ref);
    ref = gcref_acq(sbx->dict_mt);
    if (ref)
      gc2_markobj_worker(g, ref);
  }
  if (udtype == UDTYPE_CHANNEL) {
    LJChan *ch = (LJChan *)uddata(ud);
    uint32_t i;
    for (i = 0; i < ch->cap; i++) {
      TValue tv;
      lj_tv_load_acq(&tv, &ch->slot[i].tv);
      gc2_mark_tv_worker(g, &tv);  /* 09 section 9.5. */
    }
  }
  if (udtype == UDTYPE_THREAD) {
    LJThread *th = (LJThread *)uddata(ud);
    lua_State *child = lj_thread_state_load_acq(th);
    if (child)
      gc2_markobj_worker(g, obj2gco(child));  /* 09 section 9.2. */
  }
}

static void gc2_traverse_upval(global_State *g, GCupval *uv)
{
  TValue tv;
  lj_tv_load_acq(&tv, uvval(uv));
  gc2_mark_tv_worker(g, &tv);
}

static void gc2_traverse_func(global_State *g, GCfunc *fn)
{
  GCtab *env = tabref_acq(fn->c.env);
  if (env)
    gc2_markobj_worker(g, obj2gco(env));
  if (isluafunc(fn)) {
    uint32_t i;
    lj_assertG(fn->l.nupvalues <= funcproto(fn)->sizeuv,
	       "function upvalues out of range");
    gc2_markobj_worker(g, obj2gco(funcproto(fn)));
    for (i = 0; i < fn->l.nupvalues; i++)
      gc2_markobj_worker(g, obj2gco(func_uv_acq(&fn->l, i)));
  } else {
    uint32_t i;
    for (i = 0; i < fn->c.nupvalues; i++) {
      TValue tv;
      lj_tv_load_acq(&tv, &fn->c.upvalue[i]);
      gc2_mark_tv_worker(g, &tv);
    }
  }
}

#if LJ_HASJIT
static void gc2_traverse_trace(global_State *g, GCtrace *T)
{
  IRIns *irbase;
  IRRef ref;
  if (trace_traceno_acq(T) == 0)
    return;
  irbase = trace_ir_acq(T);
  for (ref = trace_nk_acq(T); ref < REF_TRUE; ref++) {
    IRIns *ir = &irbase[ref];
    IRIns irs = ir_load_acq(ir);
    if (irs.o == IR_KGC)
      gc2_markobj_worker(g, ir_kgc_load_acq(ir));
    if (irt_is64(irs.t) && irs.o != IR_KNULL)
      ref++;
  }
  gc2_marktrace_worker(g, trace_link_acq(T));
  gc2_marktrace_worker(g, trace_nextroot_acq(T));
  gc2_marktrace_worker(g, trace_nextside_acq(T));
  gc2_markobj_worker(g, trace_startptgco_acq(T));
}
#endif

static void gc2_traverse_proto(global_State *g, GCproto *pt)
{
  ptrdiff_t i;
  gc2_markobj_worker(g, obj2gco(proto_chunkname_acq(pt)));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)
    gc2_markobj_worker(g, proto_kgc_acq(pt, i));
#if LJ_HASJIT
  gc2_marktrace_worker(g, proto_trace_acq(pt));
#endif
}

static TValue *gc2_stack_scan_top_worker(global_State *g, lua_State *L)
{
  TValue *frame, *bot = tvref(L->stack);
  TValue *top = L->top, *ctop = curr_top(L), *max = tvref(L->maxstack);
  for (frame = L->base - 1; frame > bot + LJ_FR2; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    if (!LJ_FR2)
      gc2_markobj_worker(g, obj2gco(fn));
  }
  if (ctop > top)
    top = ctop;
  return top > max ? max : top;
}

static int gc2_thread_owner_scans(global_State *g, lua_State *th)
{
  TGState *tg;
  uint64_t scan_epoch, scanned_dirty, owner_dirty;
  uint32_t cycle;
  if (!g || !th)
    return 0;
  owner_dirty = gc2_thread_owner_dirty(g, th, &tg);
  if (!tg)
    return 0;
  cycle = gc2_cycle_acq(g);
  scan_epoch = la_load64_acq(&th->scan_epoch);
  if (scan_epoch != cycle)
    return 0;
  scanned_dirty = la_load64_acq(&th->scan_dirty_epoch);
  if (scanned_dirty != owner_dirty) {
    gc2_thread_scan_dirty_misses_add(g, 1);
    return 0;
  }
  return 1;
}

static int gc2_thread_has_live_owner(global_State *g, lua_State *th)
{
  uint32_t owner;
  TGState *tg;
  if (!g || !th)
    return 0;
  owner = la_load32_acq(&th->thr_owner);
  if (owner == 0 || owner == LJ_THREAD_GCSCAN)
    return 0;
  tg = lj_tg_find_owner(g, owner);
  return tg && !lj_tg_flags_test_acq(tg, TGF_DEAD);
}

static void gc2_traverse_thread(global_State *g, lua_State *th)
{
  LJStateClaim claim;
  GCobj *mt, *uv;
  TValue *o, *top;
  TValue tv;
  uint32_t cycle;
  if (!th || tvref(th->stack) == NULL)
    return;
  if (!lj_state_gcscan_claim(th, &claim)) {
    gc2_thread_scan_busy_add(g, 1);
    if (gc2_thread_owner_scans(g, th)) {
      gc2_thread_clear_needscan(th);
      gc2_thread_scan_owner_scans_add(g, 1);
    } else {
      int pushed = gc2_grey_push(g, obj2gco(th));
      lj_assertG(pushed, "gc2 busy thread requeue failed");
      UNUSED(pushed);
      if (gc2_thread_has_live_owner(g, th))
	gc2_thread_set_needscan(g, th);
      gc2_thread_scan_requeues_add(g, 1);
    }
    return;  /* 05 section 5.7.2: owner scan or retry preserves work. */
  }
  cycle = gc2_cycle_acq(g);
  gc2_thread_scan_claims_add(g, 1);
  lj_gc2_markmem(g, tvref(th->stack));
  top = gc2_stack_scan_top_worker(g, th);
  for (o = tvref(th->stack) + 1 + LJ_FR2; o < top; o++) {
    lj_tv_load_acq(&tv, o);
    gc2_mark_tv_worker(g, &tv);
  }
  {
    GCtab *env = tabref_acq(th->env);
    if (env)
      gc2_markobj_worker(g, obj2gco(env));
  }
  mt = gcref_acq(th->mt_thread);
  if (mt != NULL)
    gc2_markobj_worker(g, mt);
  for (uv = gcref_acq(th->openupval); uv != NULL;
       uv = lj_obj_gcw_acq(uv)) {
    gc2_markobj_worker(g, uv);
    if (uv->gch.gct == ~LJ_TUPVAL) {
      TValue tv;
      lj_tv_load_acq(&tv, uvval(gco2uv(uv)));
      gc2_mark_tv_worker(g, &tv);
    }
  }
  la_store64_rel(&th->scan_dirty_epoch, 0);
  la_store64_rel(&th->scan_epoch, cycle);
  gc2_thread_clear_needscan(th);
  lj_state_dropclaim(&claim);
}

static void gc2_traverse_obj(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    (void)gc2_traverse_tab(g, gco2tab(o));
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    gc2_traverse_func(g, gco2func(o));
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    gc2_traverse_proto(g, gco2pt(o));
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    gc2_traverse_thread(g, gco2th(o));
  } else if (gct == ~LJ_TUPVAL) {
    gc2_traverse_upval(g, gco2uv(o));
  } else if (gct == ~LJ_TUDATA) {
    gc2_traverse_udata(g, gco2ud(o));
#if LJ_HASJIT
  } else if (gct == ~LJ_TTRACE) {
    gc2_traverse_trace(g, gco2trace(o));
#endif
  } else {
    lj_assertG(gct == ~LJ_TSTR || gct == ~LJ_TCDATA,
	       "bad GC type %d", gct);
  }
}

static uint32_t gc2_drain_grey(global_State *g, uint32_t limit)
{
  uint32_t n = 0;
  while (g && n < limit && !gc2_grey_empty(g)) {
    GCobj *o = gc2_grey_pop(g);
    if (o) {
      gc2_traverse_obj(g, o);
      n++;
    }
  }
  if (n)
    gc2_grey_drained_add(g, n);
  return n;
}

static uint32_t gc2_worker_sweep_progress(global_State *g, uint32_t limit)
{
  TGState *tg;
  uint32_t n = 0;
  if (gc2_sweep_blocked_by_finalizer(g))
    return 0;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL && n < limit;
       tg = lj_tg_next_acq(tg)) {
    uint8_t flags = lj_tg_flags_acq(tg);
    if ((flags & (TGF_DEAD|TGF_ARENA_INTERNAL)) != TGF_ARENA_INTERNAL)
      continue;
    n += lj_gc2_sweep_owner_progress(g, tg, limit - n);
  }
  return n;  /* 05 section 5.6.3 worker-owned sweep bridge. */
}

static uint32_t gc2_worker_progress_add(uint32_t a, uint32_t b)
{
  return b > ~(uint32_t)0 - a ? ~(uint32_t)0 : a + b;
}

static uint32_t gc2_worker_finalizer_drain(global_State *g, uint32_t limit)
{
  uint32_t expect = 0;
  uint64_t before, after, delta;
  if (!g || limit == 0 || gc2_phase_acq(g) != LJ_GC2_IDLE ||
      gc2_finalizer_mpsc_acq(g) == NULL)
    return 0;
  if (!gc2_worker_active_cas(g, &expect, 1)) {
    gc2_worker_busy_retries_add(g, 1);
    return 0;
  }
  if (gc2_phase_acq(g) != LJ_GC2_IDLE ||
      gc2_finalizer_mpsc_acq(g) == NULL) {
    gc2_worker_active_rel(g, 0);
    return 0;
  }
  if (!lj_gc2_finalizer_try_enter(g)) {
    gc2_worker_active_rel(g, 0);
    return 0;
  }
  before = gc2_finalizer_mpsc_drained_acq(g);
  lj_gc2_finalizer_drain_owned(g);
  after = gc2_finalizer_mpsc_drained_acq(g);
  lj_gc2_finalizer_leave(g);
  gc2_worker_active_rel(g, 0);
  delta = after - before;
  return delta > ~(uint32_t)0 ? ~(uint32_t)0 : (uint32_t)delta;
}

static uint32_t gc2_worker_drain_inner(global_State *g, uint32_t limit,
				       uint32_t *progress)
{
  uint32_t phase, expect = 0, n = 0, converted = 0, weak = 0, sweep = 0;
  uint32_t finalizer = 0;
  uint32_t total;
  if (progress)
    *progress = 0;
  if (!g || limit == 0)
    return 0;
  finalizer = gc2_worker_finalizer_drain(g, limit);
  if (finalizer) {
    gc2_worker_runs_add(g, 1);
    if (progress)
      *progress = finalizer;
    return finalizer;  /* 05 section 5.8: worker drains finalizer queue work. */
  }
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK &&
      phase != LJ_GC2_SWEEP)
    return 0;
  if (!gc2_worker_active_cas(g, &expect, 1)) {
    gc2_worker_busy_retries_add(g, 1);
    return 0;  /* 05 section 5.6.3 temporary single-worker bridge. */
  }
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK &&
      phase != LJ_GC2_SWEEP) {
    gc2_worker_active_rel(g, 0);
    return 0;
  }
  if (phase == LJ_GC2_SWEEP) {
    sweep = gc2_worker_sweep_progress(g, limit);
  } else {
    while (gc2_worker_progress_add(n, converted) < limit) {
      uint32_t work;
      GCobj *o = lj_gc2_grey_steal(g);
      if (o) {
	gc2_traverse_obj(g, o);  /* 05 section 5.6.3 worker steal+trace. */
	n++;
	continue;
      }
      work = gc2_worker_progress_add(n, converted);
      if (work >= limit)
	break;
      {
	uint32_t moved = gc2_drain_published_ssb_to_grey(g, limit - work);
	if (!moved)
	  break;
	converted += moved;
      }
    }
    if (phase == LJ_GC2_WEAK) {
      uint32_t work = gc2_worker_progress_add(n, converted);
      if (work < limit)
	weak = lj_gc2_weak_drain(g, limit - work);  /* 05 section 5.8. */
    }
  }
  total = gc2_worker_progress_add(n, converted);
  total = gc2_worker_progress_add(total, weak);
  total = gc2_worker_progress_add(total, sweep);
  if (total)
    gc2_worker_runs_add(g, 1);
  if (n) {
    gc2_grey_drained_add(g, n);
    gc2_worker_grey_drained_add(g, n);
  }
  if (converted)
    gc2_worker_ssb_converted_add(g, converted);
  if (weak)
    gc2_worker_weak_drained_add(g, weak);
  if (!total)
    gc2_worker_idle_declares_add(g, 1);
  if (progress)
    *progress = total;
  gc2_worker_active_rel(g, 0);
  return total;  /* 05 section 5.6.3 total worker progress contract. */
}

uint32_t lj_gc2_worker_drain(global_State *g, uint32_t limit)
{
  return gc2_worker_drain_inner(g, limit, NULL);
}

static uint32_t gc2_worker_drain_budget(global_State *g, uint32_t limit)
{
  uint32_t n = 0;
  while (n < limit && !lj_gc2_ssb_empty(g)) {
    uint32_t step = lj_gc2_worker_drain(g, limit - n);
    if (step == 0)
      break;
    if (step > limit - n)
      n = limit;
    else
      n += step;
  }
  return n;
}

uint32_t lj_gc2_fixpoint_round(global_State *g, lua_State *L, uint32_t limit)
{
  uint32_t phase, acked, fixpoint;
  if (!g || limit == 0)
    return 0;
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK)
    return 0;
  (void)gc2_marks_this_round_xchg_acqrel(g, 0);
  (void)gc2_worker_drain_budget(g, limit);  /* 05 section 5.7.1 pre-round drain. */
  acked = lj_gc2_handshake(g, LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_FLUSH_SSB);
  if (acked == 0 && L) {
    lj_gc2_scan_cycle_roots(g, L);
    (void)lj_gc2_flush_ssb(g, L2TG(L));
  }
  (void)gc2_worker_drain_budget(g, limit);  /* 05 section 5.7.1 post-root drain. */
  fixpoint = gc2_marks_this_round_acq(g) == 0 &&
	     lj_gc2_ssb_empty(g);
  gc2_fixpoint_rounds_add(g, 1);
  if (fixpoint)
    gc2_fixpoint_hits_add(g, 1);
  return fixpoint;
}

uint32_t lj_gc2_fixpoint_run(global_State *g, lua_State *L,
			     uint32_t max_rounds, uint32_t limit)
{
  uint32_t i;
  if (!g || max_rounds == 0 || limit == 0)
    return 0;
  for (i = 0; i < max_rounds; i++)
    if (lj_gc2_fixpoint_round(g, L, limit))
      return 1;
  return 0;
}

uint32_t lj_gc2_mark_complete(global_State *g, lua_State *L,
			      uint32_t max_rounds, uint32_t limit)
{
  uint32_t hit;
  if (!g || gc2_phase_acq(g) != LJ_GC2_MARK)
    return 0;
  gc2_mark_complete_runs_add(g, 1);
  for (;;) {
    hit = lj_gc2_fixpoint_run(g, L, max_rounds, limit);
    if (hit || gc2_worker_active_acq(g) == 0)
      break;
    gc2_mark_complete_peer_waits_add(g, 1);
    while (gc2_worker_active_acq(g) != 0)
      la_cpu_pause();  /* 05 section 5.7.1 peer drain before P_WEAK. */
  }
  if (hit)
    gc2_mark_complete_hits_add(g, 1);
  return hit;  /* 05 section 5.7.1 scheduler-owned mark completion bridge. */
}

int lj_gc2_ismarkedmem(global_State *g, void *p)
{
  TGState *tg = gc2_tg_for_mem(g, p);
  GCArena *a;
  uint32_t cell;
  if (!p || !tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return -1;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    LJHugeInfo hi;
    if (!lj_tg_flags_test_acq(tg, TGF_HUGETAB) ||
	lj_arena_hugetab_lookup(&tg->huge, p, &hi) != 1)
      return -1;
    return (hi.flags & LJ_HUGEF_MARK) != 0;
  }
  cell = lj_arena_cellof(p);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
      !lj_arena_bm_get(a->block, cell))
    return -1;
  return lj_arena_bm_get(a->mark, cell);
}

int lj_gc2_ismarked(global_State *g, GCobj *o)
{
  return o ? lj_gc2_ismarkedmem(g, gc2_mark_base(o)) : -1;
}

#if LJ_GC2_PARANOIA
static int gc2_legacy_liveobj(GCobj *o)
{
  uint8_t flags = lj_obj_gcflags(o);
  return !iswhite(o) || (flags & (LJ_GC_FIXED|LJ_GC_SFIXED));
}

static int gc2_legacy_has_base(global_State *g, void *p)
{
  GCobj *o;
  for (o = gcref_acq(g->gc.root); o != NULL; o = lj_obj_gcw_acq(o)) {
    if (gc2_legacy_liveobj(o) && gc2_mark_base(o) == p)
      return 1;
    if (o->gch.gct == ~LJ_TTHREAD) {
      GCobj *uv;
      for (uv = gcref_acq(gco2th(o)->openupval); uv != NULL;
	   uv = lj_obj_gcw_acq(uv))
	if (gc2_legacy_liveobj(uv) && gc2_mark_base(uv) == p)
	  return 1;
    }
  }
  return 0;
}

static uint32_t gc2_paranoia_scan_arena(global_State *g, GCArena *a)
{
  uint32_t w, bad = 0;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t m = a->block[w] & a->mark[w];
    while (m) {
      uint32_t bit = (uint32_t)__builtin_ctzll(m);
      uint32_t cell = (w << 6) + bit;
      m &= m - 1u;
      if (cell >= LJ_AFIRST_CELL &&
	  !gc2_legacy_has_base(g, lj_arena_cellptr(a, cell)))
	bad++;
    }
  }
  return bad;
}

uint32_t lj_gc2_paranoia_legacy_diff(global_State *g)
{
  TGState *tg;
  GCArena *a;
  uint32_t bad = 0;
  if (!g)
    return 0;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    uint8_t flags = lj_tg_flags_acq(tg);
    if ((flags & (TGF_DEAD|TGF_ARENA_INTERNAL)) != TGF_ARENA_INTERNAL)
      continue;
    for (a = tg->alloc.owned[LJ_ARENAK_TRAVERSABLE];
	 a != NULL; a = lj_arena_next_acq(a))
      bad += gc2_paranoia_scan_arena(g, a);
    for (a = tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE]; a != NULL;
	 a = lj_arena_next_acq(a))
      bad += gc2_paranoia_scan_arena(g, a);
  }
  return bad;
}

#endif
