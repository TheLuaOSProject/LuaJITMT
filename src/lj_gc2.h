/*
** Concurrent GC scaffold.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC2_H
#define _LJ_GC2_H

#include "lj_obj.h"

#ifndef LJ_GC2_PARANOIA
#define LJ_GC2_PARANOIA		0
#endif

typedef void (*GC2FinalizerMarkFunc)(global_State *g, GCobj *o);
typedef void (*GC2SweepBridgePreserveFunc)(global_State *g);
typedef void (*GC2FinRegMarkFunc)(global_State *g, cTValue *tv);
typedef void (*GC2FinRegMarkObjFunc)(global_State *g, GCobj *o);
typedef void (*GC2FinRegMarkMemFunc)(global_State *g, void *p);
typedef void (*GC2FinRegMarkTVFunc)(global_State *g, cTValue *tv);
#if defined(lj_gc2_c) || defined(LJ_GC2_TEST_HELPERS) || defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
typedef int (*GC2FinalizerDispatchFunc)(lua_State *L, global_State *g,
					GCobj *o);
#define LJ_GC2_HAS_FINALIZER_DISPATCH_TYPE 1
#endif

typedef struct GC2StatsSnapshot {
  GCSize total_bytes;
  uint32_t phase;
  uint32_t generational;
  uint32_t cycle_minor_requested;
  uint32_t cycle_sweep_minor;
  uint32_t minor_sweep_enabled;
  uint32_t cycle_roots_minor;
  uint32_t minor_roots_enabled;
  uint64_t cycle_requests;
  uint64_t cycle_starts;
  uint64_t major_cycle_starts;
  uint64_t minor_cycle_requests;
  uint64_t minor_cycle_starts;
  uint64_t minor_sweep_deferred;
  uint64_t minor_sweep_arenas;
  uint64_t minor_roots_deferred;
  uint64_t major_root_scans;
  uint64_t minor_root_scans;
  uint64_t minor_survival_base_live;
  uint64_t minor_survival_bytes;
  uint32_t minor_survival_pct;
  uint32_t minor_survival_threshold_pct;
  uint64_t minor_survival_major_requests;
  uint64_t remembered_barriers;
  uint64_t remembered_pushed;
  uint64_t remembered_overflows;
  uint64_t remembered_filtered;
  uint64_t remembered_drained;
  uint64_t poll_ack_samples;
  uint64_t poll_ack_latency_sum_ns;
  uint64_t poll_ack_latency_max_ns;
  uint64_t poll_ack_latency_buckets[LJ_GC2_HS_LATENCY_BUCKETS];
  uint64_t alloc_since_trigger;
  uint64_t cycle_alloc_bytes;
  uint64_t trigger_bytes;
  uint64_t hard_bytes;
  uint64_t assist_runs;
  uint64_t assist_grey_drained;
  uint64_t assist_ssb_converted;
  uint64_t assist_weak_drained;
  uint64_t worker_runs;
  uint64_t worker_grey_drained;
  uint64_t worker_ssb_converted;
  uint64_t worker_weak_drained;
  uint64_t worker_idle_declares;
  uint64_t worker_busy_retries;
  uint64_t worker_wakes;
  uint64_t worker_parks;
  uint64_t worker_async_progress;
  uint64_t sweep_owner_runs;
  uint64_t sweep_owner_arenas;
  uint64_t sweep_owner_live_cells;
  uint64_t sweep_live_updates;
  uint64_t sweep_live_huge_bytes;
  uint64_t live_estimate;
  uint64_t weak_clear_tables;
  uint64_t weak_clear_cleared;
  uint64_t weak_bridge_skipped;
  uint64_t weak_bridge_fallbacks;
  uint64_t weak_bridge_backfills;
  uint64_t weak_bridge_backfill_tables;
  uint64_t weak_bridge_backfill_slots;
  uint64_t weak_bridge_backfill_cleared;
  uint64_t weak_keys_marked;
  uint64_t weak_values_marked;
  uint64_t finreg_cdata_sets;
  uint64_t finreg_cdata_clears;
  uint64_t finreg_cdata_queued;
  uint64_t finreg_cdata_sweep_queued;
  uint64_t finreg_cdata_pweak_queued;
  uint64_t finreg_cdata_pweak_claimed;
  uint64_t finreg_cdata_preclaim_overflow;
  uint64_t finreg_cdata_preclaim_dispatched;
  uint64_t finreg_cdata_order_seen;
  uint64_t finreg_cdata_order_claimed;
  uint64_t finreg_cdata_order_unlinked;
  uint64_t finreg_cdata_order_queued;
  uint64_t finreg_cdata_order_retired;
  uint64_t finreg_cdata_order_tombstones;
  uint64_t finreg_cdata_order_fallbacks;
  uint64_t finreg_cdata_pending_order_hits;
  uint64_t finreg_udata_sets;
  uint64_t finreg_udata_clears;
  uint64_t finreg_udata_queued;
  uint64_t finreg_udata_registered;
  uint64_t finreg_udata_retired_nodes;
  uint64_t finreg_udata_discovered;
  uint64_t finreg_udata_forgets;
  uint64_t finalizer_queued;
  uint64_t finalizer_dequeued;
  uint64_t finalizer_mpsc_drained;
  uint64_t finalizer_enters;
  uint64_t finalizer_leaves;
  uint64_t finalizer_sweep_blocks;
  uint64_t finalizer_spawn_deferrals;
  uint64_t finalizer_spawn_release_wakes;
} GC2StatsSnapshot;

enum {
  LJ_GC2_IDLE,
  LJ_GC2_MARK,
  LJ_GC2_WEAK,
  LJ_GC2_SWEEP
};

#define LJ_GC2_HS_ENABLE_BARRIER	0x00000001u
#define LJ_GC2_HS_DISABLE_BARRIER	0x00000002u
#define LJ_GC2_HS_ALLOC_BLACK		0x00000004u
#define LJ_GC2_HS_ALLOC_WHITE		0x00000008u
#define LJ_GC2_HS_SCAN_ROOTS		0x00000010u
#define LJ_GC2_HS_FLUSH_SSB		0x00000020u
#define LJ_GC2_HS_RESET_ALLOC		0x00000040u
#define LJ_GC2_HS_EXIT_TRACES		0x00000080u
#define LJ_GC2_HS_REDISPATCH		0x00000100u
#define LJ_GC2_HS_FLUSHJ		0x00000200u
#define LJ_GC2_HS_STOPREQ		0x00000400u

#define LJ_GC2_ACCT_FLUSH		32768u
#define LJ_GC2_TRIGGER_MIN		(LJ_GC2_ACCT_FLUSH * 8192u)
#define LJ_GC2_HELPER_IDLE_STEP		(LJ_GC2_ACCT_FLUSH * 4u)
#define LJ_GC2_WORKER_DRAIN_BATCH	64u
#define LJ_GC2_WEAK_DRAIN_BATCH		64u
#define LJ_GC2_SWEEP_BATCH		64u
#define LJ_GC2_MINOR_SURVIVAL_MAJOR_PCT	80u

LJ_FUNC void lj_gc2_init(global_State *g);
LJ_FUNC void lj_gc2_fini(global_State *g);
LJ_FUNC void lj_gc2_account_alloc(global_State *g, TGState *tg, GCSize bytes);
LJ_FUNC uint64_t lj_gc2_flush_alloc(global_State *g, TGState *tg);
LJ_FUNC int lj_gc2_request_cycle_explicit(global_State *g, TGState *tg);
LJ_FUNC int lj_gc2_request_major(global_State *g, TGState *tg);
LJ_FUNC int lj_gc2_request_stopped_major(global_State *g, TGState *tg);
LJ_FUNC void lj_gc2_check_trigger(global_State *g, TGState *tg);
LJ_FUNC void lj_gc2_update_pacing(global_State *g);
LJ_FUNC void lj_gc2_publish_idle_threshold(global_State *g);
LJ_FUNC uint32_t lj_gc2_assist_shift_from_stepmul(uint32_t stepmul);
LJ_FUNC uint32_t lj_gc2_assist(global_State *g, TGState *tg);
LJ_FUNC void lj_gc2_set_generational(global_State *g, int enabled);
LJ_FUNC void lj_gc2_legacy_mark_begin(global_State *g);
LJ_FUNC int lj_gc2_mark_phase_active(global_State *g);
LJ_FUNC int lj_gc2_minor_roots_active(global_State *g);
LJ_FUNC int lj_gc2_minor_roots_skip_bridge_mark(global_State *g);
LJ_FUNC void lj_gc2_force_major(global_State *g);
LJ_FUNC void lj_gc2_legacy_preserve_abort(global_State *g);
LJ_FUNC void lj_gc2_legacy_cycle_end(global_State *g);
LJ_FUNC int lj_gc2_sweep_bridge_close(global_State *g);
LJ_FUNC void lj_gc2_sweep_bridge_ready(global_State *g);
LJ_FUNC void lj_gc2_sweep_bridge_boundary_reached(global_State *g);
LJ_FUNC void lj_gc2_sweep_prepare_bridge_boundary(global_State *g,
						  GC2SweepBridgePreserveFunc preserve);
LJ_FUNC int lj_gc2_sweep_to_idle(global_State *g);
LJ_FUNC int lj_gc2_sweep_tg_ready(TGState *tg);
LJ_FUNC int lj_gc2_sweep_bridge_can_progress(global_State *g);
LJ_FUNC int lj_gc2_sweep_minor_active(global_State *g);
LJ_FUNC int lj_gc2_sweep_needs_prepare(global_State *g);
LJ_FUNC int lj_gc2_sweep_pending(global_State *g);
LJ_FUNC uint32_t lj_gc2_handshake(global_State *g, uint32_t actions);
LJ_FUNC uint64_t lj_gc2_retire_epoch(global_State *g);
LJ_FUNC uint32_t lj_gc2_reclaim_retired(global_State *g, uint64_t epoch);
LJ_FUNC void lj_gc2_stats_snapshot(global_State *g, GC2StatsSnapshot *s);
LJ_FUNC void lj_gc2_scan_cycle_roots(global_State *g, lua_State *L);
LJ_FUNC uint32_t lj_gc2_flush_ssb(global_State *g, TGState *tg);
LJ_FUNC uint32_t lj_gc2_workers_count(global_State *g);
LJ_FUNC int lj_gc2_workers_set(global_State *g, uint32_t n);
LJ_FUNC int lj_gc2_workers_set_l(lua_State *L, uint32_t n,
				 uint32_t *actionsp);
LJ_FUNC void lj_gc2_worker_stop(global_State *g);
LJ_FUNC uint32_t lj_gc2_worker_drain(global_State *g, uint32_t limit);
LJ_FUNC uint32_t lj_gc2_fixpoint_round(global_State *g, lua_State *L,
				       uint32_t limit);
LJ_FUNC uint32_t lj_gc2_mark_complete(global_State *g, lua_State *L,
				      uint32_t max_rounds, uint32_t limit);
LJ_FUNC void lj_gc2_mark_to_weak(global_State *g);
LJ_FUNC int lj_gc2_weak_complete(global_State *g, GCobj *bridge_head,
				 uint32_t drain_limit);
LJ_FUNC void lj_gc2_weak_to_sweep(global_State *g);
LJ_FUNC void lj_gc2_finreg_cdata_set(global_State *g, GCobj *o, int enabled);
LJ_FUNC void lj_gc2_finreg_cdata_note_sweep_queued(global_State *g);
LJ_FUNC void lj_gc2_finreg_cdata_note_order_retired(global_State *g);
LJ_FUNC size_t lj_gc2_finreg_cdata_finalize_pweak(lua_State *L,
						  global_State *g,
						  GC2FinRegMarkFunc mark);
LJ_FUNC void lj_gc2_finreg_cdata_mark_roots(global_State *g,
					    GC2FinRegMarkObjFunc markobj,
					    GC2FinRegMarkMemFunc markmem,
					    GC2FinRegMarkTVFunc marktv);
LJ_FUNC size_t lj_gc2_finreg_cdata_finalize_close(global_State *g);
LJ_FUNC void lj_gc2_finreg_cdata_disable(global_State *g);
LJ_FUNC int lj_gc2_finreg_cdata_pending(global_State *g);
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
LJ_FUNC void lj_gc2_test_finreg_cdata_preclaim_fail(global_State *g,
								    uint32_t n);
LJ_FUNC void lj_gc2_test_finreg_cdata_preclaim_publish_pause(global_State *g);
LJ_FUNC void lj_gc2_test_finalizer_drain_pause(global_State *g);
#endif
LJ_FUNC int lj_gc2_finreg_udata_set(global_State *g, GCobj *o, int enabled);
LJ_FUNC void lj_gc2_finreg_udata_register(lua_State *L, global_State *g,
					  GCobj *o);
LJ_FUNC void lj_gc2_finreg_udata_register_mt(lua_State *L, global_State *g,
					     GCudata *ud, GCtab *mt);
LJ_FUNC void lj_gc2_finreg_udata_forget(global_State *g, GCobj *o);
LJ_FUNC size_t lj_gc2_finreg_udata_finalize(global_State *g, int all);
LJ_FUNC void lj_gc2_finalizer_dispatch_all(lua_State *L);
LJ_FUNC int lj_gc2_finalizer_step(lua_State *L,
				  GCSize finalize_cost, GCSize *cost);
LJ_FUNC void lj_gc2_finalizer_mark_all(global_State *g,
				       GC2FinalizerMarkFunc mark);
LJ_FUNC int lj_gc2_finalizer_fullgc_deferred(global_State *g);
LJ_FUNC int lj_gc2_finalizer_phase_pending(global_State *g);
LJ_FUNC int lj_gc2_finalizer_close_pending(global_State *g);
#if defined(lj_gc2_c) || defined(LJ_GC2_TEST_HELPERS) || defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
LJ_FUNC int lj_gc2_test_ssb_push(global_State *g, GCobj *o);
LJ_FUNC uint32_t lj_gc2_test_ssb_drain(global_State *g);
LJ_FUNC int lj_gc2_test_ssb_empty(global_State *g);
LJ_FUNC uint32_t lj_gc2_test_weak_snapshot_count(global_State *g);
LJ_FUNC GCtab *lj_gc2_test_weak_snapshot_tab(global_State *g, uint32_t idx);
LJ_FUNC uint32_t lj_gc2_test_weak_snapshot_scan(global_State *g,
					       uint32_t limit);
LJ_FUNC uint32_t lj_gc2_test_weak_snapshot_clear(global_State *g,
						uint32_t limit);
LJ_FUNC uint32_t lj_gc2_test_weak_drain(global_State *g, uint32_t limit);
LJ_FUNC int lj_gc2_test_weak_snapshot_covers_bridge(global_State *g,
						    GCobj *bridge_head);
LJ_FUNC void lj_gc2_test_update_minor_survival_policy(global_State *g,
						      uint64_t live);
LJ_FUNC uint32_t lj_gc2_test_sweep_owner_progress(global_State *g,
						  TGState *tg,
						  uint32_t limit);
#if LJ_GC2_PARANOIA
LJ_FUNC uint32_t lj_gc2_test_paranoia_root_diff(global_State *g);
#endif
LJ_FUNC void lj_gc2_test_finreg_cdata_finalizer_enqueue(global_State *g,
							GCobj *o);
LJ_FUNC int lj_gc2_test_finreg_cdata_preclaim(lua_State *L, global_State *g,
					      GCobj *o, cTValue *fin);
LJ_FUNC int lj_gc2_test_finreg_cdata_preclaim_take(lua_State *L,
						   global_State *g,
						   GCobj *o, TValue *fin);
LJ_FUNC void lj_gc2_test_finreg_udata_queue(global_State *g, GCobj *o);
LJ_FUNC void lj_gc2_test_scan_roots(global_State *g, lua_State *L);
LJ_FUNC void lj_gc2_test_scan_minor_roots(global_State *g, lua_State *L);
LJ_FUNC GCobj *lj_gc2_test_grey_steal(global_State *g);
LJ_FUNC void lj_gc2_test_worker_wake(global_State *g);
LJ_FUNC int lj_gc2_test_finalizer_try_enter(global_State *g);
LJ_FUNC void lj_gc2_test_finalizer_enter(global_State *g);
LJ_FUNC void lj_gc2_test_finalizer_leave(global_State *g);
LJ_FUNC void lj_gc2_test_finalizer_drain_owned(global_State *g);
LJ_FUNC void lj_gc2_test_finalizer_drain(global_State *g);
LJ_FUNC GCobj *lj_gc2_test_finalizer_dequeue(global_State *g);
LJ_FUNC void lj_gc2_test_finalizer_enqueue(global_State *g, GCobj *o);
LJ_FUNC int lj_gc2_test_finalizer_queue_pending(global_State *g);
LJ_FUNC int lj_gc2_test_finalizer_pending(global_State *g);
LJ_FUNC int lj_gc2_test_finalizer_sweep_pending(global_State *g);
LJ_FUNC int lj_gc2_test_finalizer_step_dispatch(lua_State *L,
						GC2FinalizerDispatchFunc dispatch,
						GCSize finalize_cost,
						GCSize *cost);
#endif
LJ_FUNC void lj_gc2_finalizer_spawn_release(global_State *g);
LJ_FUNCA void lj_gc2_barrier_tv_g(global_State *g, cTValue *tv);
LJ_FUNCA void lj_gc2_barrier_tvn_pair_g(global_State *g, GCobj *parent,
					cTValue *tv, uint32_t n);
LJ_FUNCA void lj_gc2_barrier_obj_pair(lua_State *L, GCobj *parent, GCobj *child);
LJ_FUNCA void lj_gc2_barrier_tv_pair_g(global_State *g, GCobj *parent,
				       cTValue *tv);
LJ_FUNC void lj_gc2_barrier_tv_pair(lua_State *L, GCobj *parent, cTValue *tv);
LJ_FUNCA void lj_gc2_barrier_tab_g(global_State *g, GCtab *t);
LJ_FUNC void lj_gc2_barrier_tab(lua_State *L, GCtab *t);
LJ_FUNC int lj_gc2_markobj(global_State *g, GCobj *o);
LJ_FUNC int lj_gc2_markmem(global_State *g, void *p);
LJ_FUNC int lj_gc2_ismarkedmem(global_State *g, void *p);
LJ_FUNC int lj_gc2_ismarked(global_State *g, GCobj *o);

#endif
