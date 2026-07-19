/*
** Concurrent GC scaffold.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC2_H
#define _LJ_GC2_H

#include "lj_obj.h"
#include "lj_arena.h"

#ifndef LJ_GC2_PARANOIA
#define LJ_GC2_PARANOIA		0
#endif

typedef void (*GC2FinalizerMarkFunc)(global_State *g, GCobj *o);
typedef void (*GC2SweepBridgePreserveFunc)(global_State *g);
typedef void (*GC2FinRegMarkFunc)(global_State *g, cTValue *tv);
typedef void (*GC2FinRegMarkObjFunc)(global_State *g, GCobj *o);
typedef void (*GC2FinRegMarkMemFunc)(global_State *g, void *p);
typedef void (*GC2FinRegMarkTVFunc)(global_State *g, cTValue *tv);
/* Opaque counted body admission. A successful acquire protects every payload
** byte through the final read before lj_gc2_lease_release(). Do not copy an
** active lease. Huge allocations carry a counted HugeTab reader; only the
** temporary custom-lua_Alloc path may return a valid no-op lease. */
typedef struct LJGC2Lease {
  void *arena;
  intptr_t admission;
  LJHugeReader huge;
} LJGC2Lease;
/* Non-semantic snapshot for an object whose queue/root/detached ticket already
** pins its incarnation. The caller must hold this universe's SMR reader (or
** the exact sweep-reclaimer certificate) through the final use. */
typedef struct LJGC2QueuedInfo {
  void *arena;
  void *base;
  size_t alloc_size;
  uint32_t start;
  uint32_t end;
  uint32_t gct;
  uint32_t marked;
} LJGC2QueuedInfo;
#if defined(lj_gc2_c) || defined(LJ_GC2_TEST_HELPERS) || \
    defined(LJ_TRACE_TEST_HELPERS) || defined(LUA_USE_ASSERT) || \
    LJ_GC2_PARANOIA
typedef int (*GC2FinalizerDispatchFunc)(lua_State *L, global_State *g,
					GCobj *o);
#define LJ_GC2_HAS_FINALIZER_DISPATCH_TYPE 1
#endif

typedef struct GC2StatsSnapshot {
  GCSize total_bytes;
  uint32_t phase;
  uint32_t gc_state;
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
  uint64_t pending_root_flushes;
  uint64_t pending_root_flushed;
  uint64_t pending_root_flush_max;
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
  uint64_t recovery_items;
  uint64_t recovery_huge_items;
  uint64_t recovery_published;
  uint64_t recovery_redirtied;
  uint64_t recovery_drained;
  uint32_t recovery_failed;
  uint64_t poll_ack_samples;
  uint64_t poll_ack_latency_sum_ns;
  uint64_t poll_ack_latency_max_ns;
  uint64_t poll_ack_latency_buckets[LJ_GC2_HS_LATENCY_BUCKETS];
  uint64_t alloc_total_bytes;
  uint64_t alloc_since_trigger;
  uint64_t cycle_alloc_bytes;
  uint64_t trigger_bytes;
  uint64_t hard_bytes;
  uint64_t assist_runs;
  uint64_t assist_grey_drained;
  uint64_t assist_ssb_converted;
  uint64_t assist_weak_drained;
  uint64_t jit_hard_checks;
  uint64_t interp_hard_checks;
  uint64_t worker_runs;
  uint64_t worker_grey_drained;
  uint64_t worker_ssb_converted;
  uint64_t worker_weak_drained;
  uint64_t worker_idle_declares;
  uint64_t worker_busy_retries;
  uint64_t worker_wakes;
  uint64_t worker_parks;
  uint64_t worker_async_progress;
  uint64_t thread_scan_frame_fallbacks;
  uint64_t ffi_native_scan_attempts;
  uint64_t ffi_native_scan_stable_frames;
  uint64_t ffi_native_scan_retries;
  uint64_t ffi_native_scan_invalid;
  uint64_t sweep_owner_runs;
  uint64_t sweep_owner_arenas;
  uint64_t sweep_owner_live_cells;
  uint64_t sweep_live_updates;
  uint64_t sweep_live_huge_bytes;
  uint64_t live_estimate;
  uint64_t smr_reclaim_runs;
  uint64_t smr_reclaimed;
  uint64_t root_spine_objects;
  uint64_t root_spine_tombstones;
  uint64_t root_spine_count_cap;
  uint32_t root_spine_count_capped;
  uint64_t arena_traversable_owned;
  uint64_t arena_traversable_needsweep;
  uint32_t arena_traversable_binmask;
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
#define LJ_GC2_HS_RESTORE_ALLOC		0x00000800u
#define LJ_GC2_HS_SCAN_OWNER_ROOTS	0x00001000u

#define LJ_GC2_ACCT_FLUSH		32768u
/*
** Keep active-cycle pacing tight enough for explicit collection and short-lived
** trace/prototype churn to reach stock LuaJIT lifetime points without waiting
** for a large allocation burst. These are semantic pacing thresholds, not test
** stress knobs.
*/
#define LJ_GC2_TRIGGER_MIN		LJ_GC2_ACCT_FLUSH
#define LJ_GC2_HELPER_IDLE_STEP		(LJ_GC2_ACCT_FLUSH * 4u)
#define LJ_GC2_ACTIVE_AUTO_STEP		4096u
#define LJ_GC2_TRACE_HARD_CHECK_BATCH	(LJ_GC2_ACCT_FLUSH * 16u)
#define LJ_GC2_PENDING_ROOT_TRIGGER_MAX	(LJ_GC2_ACCT_FLUSH * 12u)
#define LJ_GC2_WORKER_DRAIN_BATCH	64u
#define LJ_GC2_WEAK_DRAIN_BATCH		64u
#define LJ_GC2_SWEEP_BATCH		64u
#define LJ_GC2_GRACE_EPOCHS		2u
#define LJ_GC2_ROOT_SCAN_LIMIT		LJ_ROOT_SCAN_LIMIT
#define LJ_GC2_ROOT_RETRY_ROUNDS	8u
#define LJ_GC2_MINOR_SURVIVAL_MAJOR_PCT	80u

LJ_FUNC void lj_gc2_init(global_State *g);
LJ_FUNC void lj_gc2_fini(global_State *g);
/* The small-arena directory outlives GC2 worker/raw teardown. It is released
** only after every registered allocator has deleted its exact entries. The
** caller owns joined-world GC2/TG teardown authority. */
LJ_FUNC int lj_gc2_small_arena_registry_fini_try(global_State *g);
LJ_FUNC uint32_t lj_gc2_shutdown_discard_ssb(global_State *g);
/* Joined-world terminal PRE/POST fence. Reconciles every durable recovery
** identity and count-zero arena admission gate, and requires descriptor/token
** authority clear. It never clears root/late or lifecycle planes, which
** freeall consumes. Safe and required to repeat between freeall rounds. */
LJ_FUNC int lj_gc2_terminal_prefree(global_State *g);
LJ_FUNC void lj_gc2_freeall(global_State *g);
LJ_FUNC void lj_gc2_account_alloc(global_State *g, TGState *tg, GCSize bytes);
LJ_FUNC uint64_t lj_gc2_flush_alloc(global_State *g, TGState *tg);
/* Flush already-accounted owner-local bytes and perform the same trigger/hard
** checkpoint as an accounting-boundary allocation, without charging a future
** object before its constructor succeeds. */
LJ_FUNC uint64_t lj_gc2_flush_alloc_checkpoint(global_State *g, TGState *tg);
/* Publish ordinary automatic-GC pressure without collecting in the caller.
** This honors collectgarbage("stop") and uses the same bounded IDLE leader
** token as allocation pacing. */
LJ_FUNC int lj_gc2_request_cycle_pressure(global_State *g, TGState *tg);
LJ_FUNC int lj_gc2_request_cycle_explicit(global_State *g, TGState *tg);
LJ_FUNC int lj_gc2_request_major(global_State *g, TGState *tg);
LJ_FUNC int lj_gc2_collect_active(lua_State *L);
LJ_FUNC int lj_gc2_step_explicit(lua_State *L, uint32_t budget);
LJ_FUNC void lj_gc2_check_trigger(global_State *g, TGState *tg);
LJ_FUNC void lj_gc2_update_pacing(global_State *g);
LJ_FUNC void lj_gc2_publish_idle_threshold(global_State *g);
LJ_FUNC void lj_gc2_hard_check_advance(global_State *g, uint64_t since);
LJ_FUNC uint32_t lj_gc2_assist_shift_from_stepmul(uint32_t stepmul);
LJ_FUNC uint32_t lj_gc2_assist(global_State *g, TGState *tg);
/* Allocation-free active-MARK certificate seed for the x64 one-numeric-
** upvalue FNEW fast path. Success appends the exact proto/environment pair as
** one indivisible owner-SSB cursor publication before publishing TG cache
** authority. Failure publishes neither a partial pair nor cache authority. */
LJ_FUNC int lj_gc2_fnew_certify_pair_nodrain(global_State *g, TGState *tg,
					      GCproto *pt, GCtab *env);
#ifdef LJ_FUNC_TEST_HELPERS
/* Addresses are embedded only in test-built x64 mcode.  Production builds do
** not emit the capture-to-store pause probe. */
LJ_FUNC uintptr_t lj_gc2_test_fnew_env_pause_armed_addr(void);
LJ_FUNC uintptr_t lj_gc2_test_fnew_env_pause_waiting_addr(void);
LJ_FUNC uintptr_t lj_gc2_test_fnew_env_pause_release_addr(void);
LJ_FUNC void lj_gc2_test_fnew_env_pause_arm(void);
LJ_FUNC uint32_t lj_gc2_test_fnew_env_pause_waiting(void);
LJ_FUNC void lj_gc2_test_fnew_env_pause_continue(void);
#endif
LJ_FUNC void lj_gc2_set_generational(global_State *g, int enabled);
LJ_FUNC void lj_gc2_mark_begin(global_State *g);
LJ_FUNC int lj_gc2_mark_phase_active(global_State *g);
LJ_FUNC int lj_gc2_minor_roots_active(global_State *g);
LJ_FUNC int lj_gc2_minor_roots_skip_bridge_mark(global_State *g);
LJ_FUNC void lj_gc2_force_major(global_State *g);
LJ_FUNC void lj_gc2_preserve_abort_to_idle(global_State *g);
LJ_FUNC void lj_gc2_preserve_root(global_State *g, GCobj *o);
LJ_FUNC void lj_gc2_cycle_to_idle(global_State *g);
LJ_FUNC int lj_gc2_sweep_bridge_close(global_State *g);
LJ_FUNC void lj_gc2_sweep_bridge_ready(global_State *g);
LJ_FUNC int lj_gc2_jit_entry_open(global_State *g);
LJ_FUNC void lj_gc2_jit_mark_request_exit(global_State *g);
LJ_FUNC void lj_gc2_jit_sweep_request_exit(global_State *g);
LJ_FUNC void lj_gc2_sweep_bridge_boundary_reached(global_State *g);
LJ_FUNC void lj_gc2_sweep_prepare_bridge_boundary(global_State *g,
						  GC2SweepBridgePreserveFunc preserve);
LJ_FUNC int lj_gc2_sweep_to_idle(global_State *g);
LJ_FUNC int lj_gc2_sweep_tg_ready(TGState *tg);
LJ_FUNC int lj_gc2_sweep_bridge_can_progress(global_State *g);
LJ_FUNC int lj_gc2_sweep_minor_active(global_State *g);
LJ_FUNC int lj_gc2_sweep_needs_prepare(global_State *g);
LJ_FUNC int lj_gc2_sweep_needs_restore(global_State *g);
LJ_FUNC int lj_gc2_sweep_pending(global_State *g);
LJ_FUNC uint32_t lj_gc2_handshake(global_State *g, uint32_t actions);
LJ_FUNC uint64_t lj_gc2_retire_epoch(global_State *g);
LJ_FUNC int lj_gc2_smr_read_try(global_State *g);
LJ_FUNC void lj_gc2_smr_read_enter(global_State *g);
LJ_FUNC void lj_gc2_smr_read_leave(global_State *g);
/* True only for the current OS thread which won the active IDLE/SWEEP
** exclusive-reclaimer CAS. This is an identity certificate, not lifetime. */
LJ_FUNC int lj_gc2_reclaim_context_held(global_State *g);
/* Migration-only veto: a zero result never authorizes reclamation. */
LJ_FUNC int lj_gc2_activation_reclaim_veto(global_State *g);
LJ_FUNC uint32_t lj_gc2_reclaim_retired(global_State *g, uint64_t epoch);

/* A JIT retire-list detach is valid either in the ordinary idle zero-worker
** grace pass, or in the sweep owner's bounded quarantine drain. Both cases
** additionally require smr_reclaiming and the recorder token at the call site.
*/
static LJ_AINLINE int lj_gc2_jit_reclaim_context_acq(global_State *g)
{
  uint32_t mode;
  uint32_t phase;
  uint32_t worker;
  if (!g || (mode = gc2_smr_reclaiming_acq(g)) == LJ_GC2_SMR_OPEN)
    return 0;
  phase = gc2_phase_acq(g);
  worker = gc2_worker_active_acq(g);
  return (phase == LJ_GC2_IDLE && worker == 0 &&
	  mode == LJ_GC2_SMR_META_EXCLUSIVE) ||
	 (phase == LJ_GC2_SWEEP && worker != 0 &&
	  mode == LJ_GC2_SMR_SWEEP_STABLE);
}
LJ_FUNC void lj_gc2_stats_snapshot(global_State *g, GC2StatsSnapshot *s);
LJ_FUNC void lj_gc2_scan_cycle_roots(global_State *g, lua_State *L);
LJ_FUNC void lj_gc2_scan_cycle_global_roots(global_State *g);
LJ_FUNC void lj_gc2_scan_cycle_owner_tg_roots(global_State *g, TGState *tg);
/* Caller must own the consumed-poll remote-native stability certificate. */
LJ_FUNC int lj_gc2_scan_cycle_owner_tg_roots_native_parked(
  global_State *g, TGState *tg);
LJ_FUNC void lj_gc2_scan_cycle_owner_roots(global_State *g, lua_State *L);
LJ_FUNC void lj_gc2_thread_owner_releasing(global_State *g, lua_State *L,
					     uint32_t tid);
LJ_FUNC void lj_gc2_trace_sweep_roots(global_State *g);
LJ_FUNC uint32_t lj_gc2_flush_ssb(global_State *g, TGState *tg);
LJ_FUNC uint32_t lj_gc2_flush_ssb_detach(global_State *g, TGState *tg);
LJ_FUNC uint32_t lj_gc2_workers_count(global_State *g);
LJ_FUNC uint32_t lj_gc2_terminal_reclaim_tgs(global_State *g);
LJ_FUNC int lj_gc2_workers_set(global_State *g, uint32_t n);
LJ_FUNC void lj_gc2_sweep_publish_wake(global_State *g);
LJ_FUNC int lj_gc2_workers_set_l(lua_State *L, uint32_t n,
				 uint32_t *actionsp);
LJ_FUNC int lj_gc2_worker_stop(global_State *g);
LJ_FUNC uint32_t lj_gc2_worker_drain(global_State *g, uint32_t limit);
LJ_FUNC uint32_t lj_gc2_fixpoint_round(global_State *g, lua_State *L,
				       uint32_t limit);
LJ_FUNC uint32_t lj_gc2_mark_complete(global_State *g, lua_State *L,
				      uint32_t max_rounds, uint32_t limit);
LJ_FUNC void lj_gc2_mark_to_weak(global_State *g);
LJ_FUNC int lj_gc2_weak_complete(global_State *g, lua_State *L,
				 GCobj *bridge_head, uint32_t drain_limit);
LJ_FUNC void lj_gc2_weak_to_sweep(global_State *g, lua_State *L);
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
LJ_FUNC int lj_gc2_finreg_udata_register_mt_nothrow(lua_State *L,
						     global_State *g,
						     GCudata *ud,
						     GCtab *mt);
LJ_FUNC void lj_gc2_finreg_udata_register_mt(lua_State *L, global_State *g,
					      GCudata *ud, GCtab *mt);
LJ_FUNC void lj_gc2_finreg_udata_forget(global_State *g, GCobj *o);
LJ_FUNC size_t lj_gc2_finreg_udata_finalize(global_State *g, int all);
LJ_FUNC void lj_gc2_finalizer_dispatch_all(lua_State *L);
LJ_FUNC int lj_gc2_finalizer_step(lua_State *L,
				  GCSize finalize_cost, GCSize *cost);
LJ_FUNC int lj_gc2_finalizer_owned_by_current(global_State *g);
LJ_FUNC void lj_gc2_finalizer_mark_all(global_State *g,
				       GC2FinalizerMarkFunc mark);
LJ_FUNC int lj_gc2_finalizer_deferred(global_State *g);
LJ_FUNC int lj_gc2_finalizer_phase_pending(global_State *g);
LJ_FUNC int lj_gc2_finalizer_close_pending(global_State *g);
#if defined(lj_gc2_c) || defined(LJ_GC2_TEST_HELPERS) || \
    defined(LJ_TRACE_TEST_HELPERS) || defined(LUA_USE_ASSERT) || \
    LJ_GC2_PARANOIA
LJ_FUNC void lj_gc2_test_activation_mirror_edge(global_State *g,
						 uint32_t from_phase,
						 uint32_t to_phase);
LJ_FUNC int lj_gc2_test_activation_mark_recheck(global_State *g,
						 uint32_t expected_leader);
LJ_FUNC int lj_gc2_test_sweep_reclaim_enter(global_State *g);
LJ_FUNC int lj_gc2_test_ssb_push(global_State *g, GCobj *o);
LJ_FUNC uint32_t lj_gc2_test_ssb_drain(global_State *g);
LJ_FUNC int lj_gc2_test_ssb_empty(global_State *g);
#if defined(LJ_GC2_TEST_HELPERS) || defined(LJ_TRACE_TEST_HELPERS)
LJ_FUNC int lj_gc2_test_idle_reclaim_enter(global_State *g);
LJ_FUNC void lj_gc2_test_idle_reclaim_leave(global_State *g);
LJ_FUNC void lj_gc2_test_idle_reclaim_pause_after_jit_quiescence(void);
LJ_FUNC uint32_t lj_gc2_test_idle_reclaim_paused(void);
LJ_FUNC void lj_gc2_test_idle_reclaim_release(void);
LJ_FUNC int lj_gc2_test_sweep_reclaim_scope_enter(global_State *g);
LJ_FUNC void lj_gc2_test_sweep_reclaim_scope_leave(global_State *g);
#endif
#if defined(LJ_GC2_TEST_HELPERS)
#define LJ_GC2_RECOVERY_TEST_RESERVED	1u
#define LJ_GC2_RECOVERY_TEST_PRE_COMPLETE	2u
#define LJ_GC2_RECOVERY_TEST_SSB_COMMITTED	3u
#define LJ_GC2_RECOVERY_TEST_PRE_LIFETIME_RESTORE	4u
#define LJ_GC2_RECOVERY_TEST_POST_CLAIM	5u
#define LJ_GC2_RECOVERY_TEST_SMALL_IDLE_SAMPLED	6u
#define LJ_GC2_WEAK_FRONTIER_FAULT_VECTOR_TAB	1u
#define LJ_GC2_WEAK_FRONTIER_FAULT_OVERFLOW_NODE	2u
#define LJ_GC2_WEAK_FRONTIER_FAULT_OVERFLOW_TAB	3u
#define LJ_GC2_THREAD_NEEDSCAN_TEST_BEFORE_SET	1u
#define LJ_GC2_THREAD_NEEDSCAN_TEST_AFTER_SET	2u
#define LJ_GC2_THREAD_NEEDSCAN_TEST_INSTALLING	3u
#define LJ_GC2_TABLE_RESCAN_TEST_INSTALLING	1u
#define LJ_GC2_TABLE_RESCAN_TEST_HINT_CLEARED	2u
#define LJ_GC2_TABLE_TOKEN_TEST_PRE_TRANSFER	1u
#define LJ_GC2_TABLE_TOKEN_TEST_PRE_PROOF	2u
#define LJ_GC2_TABLE_TOKEN_TEST_POST_PROOF	3u
#define LJ_GC2_TABLE_TOKEN_TEST_POST_TRANSFER	4u
#define LJ_GC2_TABLE_TOKEN_TEST_PRE_ACK	5u
#define LJ_GC2_TABLE_TOKEN_PASS_PINNED	(-1)
#define LJ_GC2_TABLE_TOKEN_PASS_RETRY	0
#define LJ_GC2_TABLE_TOKEN_PASS_PROGRESS	1
#define LJ_GC2_TABLE_TOKEN_PASS_ACKED	2
LJ_FUNC void lj_gc2_test_jit_mark_checkpoint_reset(void);
LJ_FUNC uint64_t lj_gc2_test_jit_mark_checkpoint_closes(void);
LJ_FUNC void lj_gc2_test_jit_sweep_checkpoint_reset(void);
LJ_FUNC uint64_t lj_gc2_test_jit_sweep_checkpoint_closes(void);
LJ_FUNC void lj_gc2_test_recovery_fail_grey_grow(uint32_t count);
LJ_FUNC void lj_gc2_test_recovery_pause(uint32_t stage);
LJ_FUNC uint32_t lj_gc2_test_recovery_paused(void);
LJ_FUNC void lj_gc2_test_recovery_pause_disarm(void);
LJ_FUNC void lj_gc2_test_recovery_release(void);
LJ_FUNC void lj_gc2_test_recovery_fail_closed(global_State *g);
LJ_FUNC void lj_gc2_test_stack_admission_retry_once(GCobj *target);
LJ_FUNC uint32_t lj_gc2_test_stack_admission_retry_hits(void);
LJ_FUNC void lj_gc2_test_root_semantic_retry_once(GCobj *target);
LJ_FUNC uint32_t lj_gc2_test_root_semantic_retry_hits(void);
LJ_FUNC void lj_gc2_test_thread_needscan_pause(uint32_t stage);
LJ_FUNC uint32_t lj_gc2_test_thread_needscan_paused(void);
LJ_FUNC void lj_gc2_test_thread_needscan_release(void);
LJ_FUNC int lj_gc2_test_thread_needscan_clear(global_State *g, lua_State *L);
LJ_FUNC void lj_gc2_test_table_rescan_pause(uint32_t stage);
LJ_FUNC uint32_t lj_gc2_test_table_rescan_paused(void);
LJ_FUNC void lj_gc2_test_table_rescan_release(void);
LJ_FUNC void lj_gc2_test_queue_post_admit_pause(GCobj *target);
LJ_FUNC uint32_t lj_gc2_test_queue_post_admit_paused(void);
LJ_FUNC void lj_gc2_test_queue_post_admit_release(void);
LJ_FUNC void lj_gc2_test_queue_retry_witness_pause(GCobj *target);
LJ_FUNC uint32_t lj_gc2_test_queue_retry_witness_paused(void);
LJ_FUNC void lj_gc2_test_queue_retry_witness_release(void);
LJ_FUNC int lj_gc2_test_table_rescan_set(global_State *g, GCtab *t);
LJ_FUNC int lj_gc2_test_table_rescan_clear(global_State *g, GCtab *t);
LJ_FUNC int lj_gc2_test_table_expected_status(global_State *g, GCtab *t);
LJ_FUNC void lj_gc2_test_table_rescan_stale_hint_clear(global_State *g,
							GCtab *t);
LJ_FUNC int lj_gc2_test_weak_overflow_clear_bridge(global_State *g,
						     GCobj *bridge_head);
LJ_FUNC int lj_gc2_test_weak_trace_close_frontier(global_State *g,
						   GCobj *bridge_head);
LJ_FUNC void lj_gc2_test_weak_frontier_fault_once(uint32_t kind,
						   uint32_t skip);
LJ_FUNC uint32_t lj_gc2_test_weak_frontier_fault_hits(void);
LJ_FUNC void lj_gc2_test_weak_overflow_fail_alloc(uint32_t count);
LJ_FUNC int lj_gc2_test_weak_record(global_State *g, GCtab *t);
LJ_FUNC int lj_gc2_test_weak_overflow_singleton(global_State *g, GCtab *t);
LJ_FUNC void lj_gc2_test_table_token_pause(uint32_t stage);
LJ_FUNC uint32_t lj_gc2_test_table_token_paused(void);
LJ_FUNC void lj_gc2_test_table_token_release(void);
LJ_FUNC uint64_t lj_gc2_test_table_token_request(global_State *g, GCtab *t);
LJ_FUNC uint32_t lj_gc2_test_table_token_scan_small(global_State *g,
						     uint32_t budget);
LJ_FUNC uint32_t lj_gc2_test_table_token_scan_huge(global_State *g,
						    uint32_t budget);
LJ_FUNC int lj_gc2_test_table_token_scan_one(global_State *g, GCtab *t);
LJ_FUNC void lj_gc2_test_table_token_cursor_reset(global_State *g);
LJ_FUNC int lj_gc2_test_table_token_pass_step(global_State *g,
					       uint32_t budget,
					       uint32_t *consumedp);
LJ_FUNC void lj_gc2_test_table_token_pass_reset(global_State *g);
LJ_FUNC int lj_gc2_test_table_token_pass_ack_current(global_State *g);
LJ_FUNC void lj_gc2_test_table_dirty_bump(global_State *g, GCtab *t);
LJ_FUNC void lj_gc2_test_scan_tg_thread_root(global_State *g, TGState *tg,
					      lua_State *L);
#if LJ_HASFFI && LJ_HASJIT
LJ_FUNC int lj_gc2_test_scan_ffi_native_frames(global_State *g, TGState *tg);
#endif
LJ_FUNC void lj_gc2_test_thread_root_rescan_marked_obj(global_State *g,
						       GCobj *o);
LJ_FUNC int lj_gc2_test_recovery_publish(global_State *g, GCobj *o);
LJ_FUNC int lj_gc2_test_publish_mutator_reader(global_State *g, GCobj *o,
					       const LJHugeReader *reader);
LJ_FUNC int lj_gc2_test_recovery_mutating_recheck(GCArena *a,
							 uint32_t start);
LJ_FUNC uint32_t lj_gc2_test_recovery_drain(global_State *g, uint32_t limit);
LJ_FUNC int lj_gc2_test_recovery_state(global_State *g, GCobj *o);
LJ_FUNC void lj_gc2_test_recovery_huge_scans_reset(void);
LJ_FUNC uint32_t lj_gc2_test_recovery_huge_scans(void);
LJ_FUNC uint32_t lj_gc2_test_recovery_discard_terminal(global_State *g);
LJ_FUNC int lj_gc2_test_recovery_terminal_preflight(global_State *g);
LJ_FUNC void lj_gc2_test_recovery_huge_count_complete(global_State *g);
LJ_FUNC void lj_gc2_test_recovery_count_rollback(global_State *g);
LJ_FUNC void lj_gc2_test_recovery_huge_count_rollback(global_State *g);
LJ_FUNC int lj_gc2_test_table_scan_current(global_State *g, GCtab *t);
LJ_FUNC void lj_gc2_test_worker_table_skips_reset(void);
LJ_FUNC uint32_t lj_gc2_test_worker_table_skips(void);
#endif
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
LJ_FUNC void lj_gc2_test_finreg_udata_node_publish(
  global_State *g, GC2FinRegUDataNode *node);
LJ_FUNC void lj_gc2_test_scan_roots(global_State *g, lua_State *L);
LJ_FUNC void lj_gc2_test_scan_owned_needscan(global_State *g, lua_State *owner_L);
LJ_FUNC void lj_gc2_test_scan_minor_roots(global_State *g, lua_State *L);
#if LJ_HASJIT
LJ_FUNC int lj_gc2_test_trace_pc_proto_candidate(global_State *g, GCobj *o,
						 const BCIns *pc);
#endif
LJ_FUNC void lj_gc2_test_rescan_pending_clear_if_table(global_State *g,
						      GCobj *o);
LJ_FUNC void lj_gc2_test_rescan_pending_clear_cycle(global_State *g,
						   GCobj *o);
#if defined(LJ_GC2_TEST_HELPERS)
LJ_FUNC int lj_gc2_test_grey_push(global_State *g, GCobj *o);
LJ_FUNC GCobj *lj_gc2_test_grey_steal(global_State *g);
#endif
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
LJ_FUNCA void lj_gc2_barrier_obj_pair_g(global_State *g, GCobj *parent,
					GCobj *child);
LJ_FUNCA void lj_gc2_barrier_obj_pair(lua_State *L, GCobj *parent, GCobj *child);
LJ_FUNCA void lj_gc2_barrier_tv_pair_g(global_State *g, GCobj *parent,
				       cTValue *tv);
LJ_FUNC void lj_gc2_barrier_tv_pair(lua_State *L, GCobj *parent, cTValue *tv);
LJ_FUNCA void lj_gc2_barrier_tab_g(global_State *g, GCtab *t);
LJ_FUNCA void lj_gc2_barrier_key_g(global_State *g, GCtab *t, cTValue *key);
LJ_FUNC void lj_gc2_barrier_tab(lua_State *L, GCtab *t);
LJ_FUNC int lj_gc2_markobj(global_State *g, GCobj *o);
/* Returns -1 for dead/invalid, 0 for already live, 1 for newly retained and
** optionally snapshots the admitted header type. Active semantic graph work is
** published before return. */
LJ_FUNC int lj_gc2_markobj_status(global_State *g, GCobj *o, uint32_t *gctp);
/* As above, but reject an acquired header of a different type before changing
** its semantic mark state. This is required for tagged/stale edge admission:
** a type mismatch must not premark a graph which a later real edge must walk. */
LJ_FUNC int lj_gc2_markobj_expected_status(global_State *g, GCobj *o,
					    uint32_t expected_gct,
					    uint32_t *gctp);
/* Acquire a semantic object/body lease. expected_gct==0 accepts any GC type.
** Returns -1 for dead/invalid, 0 for already live, or 1 for newly retained;
** graph work is published before return exactly as for markobj_status(). */
LJ_FUNC int lj_gc2_obj_lease_acquire(global_State *g, GCobj *o,
				      uint32_t expected_gct, uint32_t *gctp,
				      LJGC2Lease *lease);
/* Acquire one exact registered raw allocation with the same tri-state result. */
LJ_FUNC int lj_gc2_mem_lease_acquire(global_State *g, void *p,
				      LJGC2Lease *lease);
/* Release is idempotent and zeros the public token before dropping admission. */
LJ_FUNC void lj_gc2_lease_release(LJGC2Lease *lease);
/* Resolve an interior bytecode pointer through allocator identity and retain
** its exact prototype owner, or accept permanent global C/fast-function
** pseudo-PCs.
** The caller must hold a GC2 SMR read scope while allocator ownership can
** transfer between TG registries. */
LJ_FUNC int lj_gc2_mark_proto_for_pc(global_State *g, const BCIns *pc);
/* Retain the table allocation non-semantically while comparing one current
** vector root. Returns 1 for the same generation, 0 for a valid different or
** terminal owner, and -1 when admission is transient and reclaim must retry. */
LJ_FUNC int lj_gc2_tab_generation_current(global_State *g, GCtab *t,
					   const void *generation,
					   int array_generation);
LJ_FUNC int lj_gc2_markobj_direct(global_State *g, GCobj *o);
LJ_FUNC int lj_gc2_markobj_nogrey(global_State *g, GCobj *o);
LJ_FUNC int lj_gc2_markmem(global_State *g, void *p);
LJ_FUNC int lj_gc2_obj_valid(global_State *g, GCobj *o);
LJ_FUNC int lj_gc2_obj_valid_queued(global_State *g, GCobj *o);
LJ_FUNC int lj_gc2_obj_queued_info_held(global_State *g, GCobj *o,
					 void *known_arena,
					 LJGC2QueuedInfo *info);
/* Lightweight structural snapshot for ownership-chain validation. This fills
** arena/base/start/gct/marked but deliberately skips allocation-end discovery. */
LJ_FUNC int lj_gc2_obj_queued_brief_held(global_State *g, GCobj *o,
					  void *known_arena,
					  LJGC2QueuedInfo *info);
enum {
  LJ_GC2_TV_EDGE_RETRY = -1,
  LJ_GC2_TV_EDGE_STALE = 0,
  LJ_GC2_TV_EDGE_VALID = 1
};
/* Validate a structured, authoritative TValue edge (table/root publication). */
LJ_FUNC int lj_gc2_tv_gcref_status_edge(global_State *g, cTValue *tv);
LJ_FUNC int lj_gc2_tv_gcref_valid_edge(global_State *g, cTValue *tv);
/* Retain the exact allocation named by a TValue through its final use. The
** result uses LJ_GC2_TV_EDGE_* and release is required only for VALID. */
LJ_FUNC int lj_gc2_tv_lease_acquire(global_State *g, cTValue *tv,
				      LJGC2Lease *lease);
LJ_FUNC int lj_gc2_mem_registered(global_State *g, const void *p);
LJ_FUNC int lj_gc2_mem_registered_known(global_State *g, const void *p);
/* Current-thread sweep-reclaimer certificate. This does not acquire lifetime:
** callers must already own the exact object/side-storage sweep ticket through
** their final dereference. It succeeds only on the thread which won both the
** worker and SMR-reclaimer ownership transaction, so an unrelated mutator
** cannot turn the process-global smr_reclaiming bit into body authority. */
LJ_FUNC int lj_gc2_mem_registered_known_reclaim_held(global_State *g,
						       const void *p);
LJ_FUNC int lj_gc2_markmem_registered(global_State *g, void *p);
/* Best-effort raw-allocation mark for an identity whose caller already owns
** through an independent local or published-list root. This never waits for
** an opportunistic SMR reclaimer and never treats ordinary SMR contention as
** activation corruption. A defensive active-cycle miss requests the phase's
** root-retry/wake protocol. Returns 1 only for a newly set mark; zero also
** covers an existing mark or transient admission loss. This is not a body
** lease. */
LJ_FUNC int lj_gc2_markmem_registered_publish_try(global_State *g, void *p);
/* Raw-allocation mark for an exact detached body/list owner running inside the
** current-thread exclusive reclaimer. It does not acquire ordinary SMR and
** returns -1 for no durable mark, 0 for an existing/opaque durable mark, or 1
** for a newly published mark. A nonnegative result is arena-quarantine proof,
** not a body lease: the caller's exact detached ticket remains responsible for
** every dereference. */
LJ_FUNC int lj_gc2_markmem_reclaim_held_status(global_State *g, void *p);
/* Geometry validation for a prototype whose exact public body lease remains
** held through all subsequent prototype reads. */
LJ_FUNC int lj_gc2_valid_proto_for_traverse_held(global_State *g,
						  GCproto *pt,
						  const LJGC2Lease *lease);
LJ_FUNC uint32_t lj_gc2_preserve_sweep_root(global_State *g, GCobj *o);
LJ_FUNC uint32_t lj_gc2_trace_sweep_root(global_State *g, GCobj *o);
#if LJ_HASJIT
/* Mark one currently published trace slot and report whether its exact body
** was admitted. Trace number zero is the empty edge and succeeds. */
LJ_FUNC int lj_gc2_mark_trace_slot_status(global_State *g, uint32_t traceno);
LJ_FUNC void lj_gc2_mark_trace_slot(global_State *g, uint32_t traceno);
#endif
LJ_FUNC void lj_gc2_remember_root(global_State *g, GCobj *o);
LJ_FUNC int lj_gc2_ismarkedmem(global_State *g, void *p);
LJ_FUNC int lj_gc2_ismarked(global_State *g, GCobj *o);

#endif
