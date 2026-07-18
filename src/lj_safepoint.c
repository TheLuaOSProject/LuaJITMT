/*
** Safepoint and soft-handshake scaffold.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_safepoint_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_ccall.h"
#include "lj_err.h"
#include "lj_gc2.h"
#include "lj_mcode.h"
#include "lj_oserr.h"
#include "lj_profile.h"
#include "lj_safepoint.h"
#include "lj_state.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_trace.h"

static uint64_t safepoint_now_ns(void)
{
  return lj_thr_now_ns();
}

static uint32_t safepoint_latency_bucket(uint64_t ns)
{
  uint32_t bucket = 0;
  while ((ns >>= 1) != 0 && bucket + 1u < LJ_GC2_HS_LATENCY_BUCKETS)
    bucket++;
  return bucket;
}

static void safepoint_note_ack_latency(global_State *g)
{
  uint64_t start, now, delta, old;
  if (!g)
    return;
  start = gc2_hs_signal_ns_acq(g);
  if (start == 0)
    return;
  now = safepoint_now_ns();
  if (now < start)
    return;
  delta = now - start;
  gc2_hs_ack_latency_samples_add(g, 1);
  gc2_hs_ack_latency_sum_add(g, delta);
  gc2_hs_ack_latency_bucket_add(g, safepoint_latency_bucket(delta), 1);
  old = gc2_hs_ack_latency_max_acq(g);
  while (delta > old) {
    uint64_t expect = old;
    if (gc2_hs_ack_latency_max_cas(g, &expect, delta))
      break;
    old = expect;
  }
}

static int safepoint_wait_consumed_ack(TGState *tg, uint32_t actions)
{
  uint32_t poll;
  /* A native-ack leader clears poll only after it finishes applying actions.
  ** If this thread races with that consumed request, it must not resume the VM
  ** while the leader may still be scanning its Lua stack. New request
  ** publication wakes poll waiters after storing reqmask, so this can wait for
  ** either poll clear or a late request without a timeout. */
  while ((poll = lj_tg_poll_acq(tg)) != 0) {
    global_State *g = tg->gl;
    uint32_t tid = lj_tg_tid_acq(tg);
    /* A trace-flush leader can re-enter native boundaries while it runs
    ** leader-owned actions such as JIT dump events. It must not wait for its
    ** own consumed poll: only the leader can clear that poll after the action
    ** finishes.
    */
    if (g && tid != 0 && gc2_hs_leader_acq(g) == tid)
      return 0;
    /*
    ** A trace-exit C frame keeps jit_base published through snapshot restore.
    ** It is cleared either by the GC-defer path once restore is complete or by
    ** vm_exit_interp before its poll check. If the earlier part of that frame
    ** waits here, the leader waits for jit_base to clear and neither side can
    ** progress. Defer the consumed-poll wait to the later GC/VM-exit boundary:
    ** the trace slot is still protected by jit_base while this frame unwinds.
    */
    if ((actions & (LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ)) &&
	lj_tg_load_jit_base(tg) != NULL
#if LJ_HASFFI && LJ_HASJIT
	&& !lj_ffi_native_trace_consumed_poll_wait_required(tg)
#endif
	)
      return 0;
    if (lj_tg_reqmask_acq(tg) != 0)
      return 1;
    lj_tg_poll_futex_wait(tg, poll, -1);
  }
  return 0;
}

static void safepoint_clear_poll(TGState *tg)
{
  lj_tg_poll_rel(tg, 0);
  lj_tg_poll_futex_wake(tg, 1);
}

static void safepoint_restore_counted_poll(TGState *tg)
{
  la_fence_seq();
  /* reqmask is the per-TG counted publication and precedes poll. A bare global
  ** leader may already have completed this TG's only clear pass, so using it
  ** here would manufacture an orphan poll after FRESH was consumed. */
  if (tg && lj_tg_reqmask_acq(tg) != 0) {
    lj_tg_poll_rel(tg, 1);
    lj_tg_poll_futex_wake(tg, 1);
  }
}

static void safepoint_rearm_fresh_stopreq_poll(TGState *tg)
{
  if (!tg || !lj_tg_flags_test_acq(tg, TGF_STOPREQ_FRESH) ||
      (lj_tg_load_cur_L(tg) == NULL && lj_tg_load_thread_L(tg) == NULL))
    return;
  lj_tg_poll_rel(tg, 1);
  la_fence_seq();
  if (!lj_tg_flags_test_acq(tg, TGF_STOPREQ_FRESH)) {
    /* checkstop won between the FRESH snapshot and poll publication. Remove
    ** the orphan synthetic edge, then repair any newer counted request. */
    safepoint_clear_poll(tg);
    safepoint_restore_counted_poll(tg);
    return;
  }
  lj_tg_poll_futex_wake(tg, 1);
}

static void safepoint_consume_fresh_stopreq_poll(TGState *tg)
{
  if (!tg)
    return;
  /* FRESH is the consume LP. A concurrent rearm which observed the old value
  ** must detect its disappearance after publishing poll and remove that edge. */
  (void)lj_tg_flags_and_rlx(tg, (uint8_t)~TGF_STOPREQ_FRESH);
  if (lj_tg_poll_acq(tg) == 0)
    return;
  safepoint_clear_poll(tg);
  /* A new leader can publish between the poll snapshot and clear. Do not let
  ** consuming the uncounted STOPREQ edge erase its counted signal. */
  safepoint_restore_counted_poll(tg);
}

static int safepoint_hold_poll_until_leader(global_State *g, uint32_t actions)
{
  /* These boundaries inspect or rotate owner-private state and may keep using
  ** process-global state after the individual TG acknowledgement. Keep a
  ** native-to-Lua reentry parked until the leader completes the matching root,
  ** SSB, allocator, or trace boundary. Epochs without a live leader cannot
  ** complete that consumed-poll phase, so they clear at acknowledgement.
  */
  return (actions & (LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_SCAN_OWNER_ROOTS|
		     LJ_GC2_HS_FLUSH_SSB|
		     LJ_GC2_HS_RESET_ALLOC|LJ_GC2_HS_RESTORE_ALLOC|
		     LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ)) &&
	 gc2_hs_leader_acq(g) != 0;
}

static void safepoint_clear_consumed_polls(global_State *g, uint64_t epoch)
{
  TGState *tg;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    if (lj_tg_flags_test_acq(tg, TGF_DEAD))
      continue;
    if (lj_tg_hs_epoch_ack_acq(tg) == epoch &&
	lj_tg_reqmask_acq(tg) == 0 && lj_tg_poll_acq(tg) != 0)
      safepoint_clear_poll(tg);
  }
}

static void safepoint_rearm_fresh_stopreq_polls(global_State *g)
{
  TGState *tg;
  /* Run only after leader_leave. First clear every consumed hold above so a
  ** combined STOPREQ|root/SSB/allocator request cannot deadlock its waiter;
  ** this second pass creates a distinct, uncounted VM-dispatch edge. */
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (lj_tg_flags_test_acq(tg, TGF_DEAD) ||
	!lj_tg_flags_test_acq(tg, TGF_STOPREQ_FRESH) ||
	lj_tg_reqmask_acq(tg) != 0 ||
	(lj_tg_load_cur_L(tg) == NULL && lj_tg_load_thread_L(tg) == NULL))
      continue;
    safepoint_rearm_fresh_stopreq_poll(tg);
  }
}

static int safepoint_claim_epoch(TGState *tg, uint64_t epoch)
{
  uint64_t oldepoch = lj_tg_hs_epoch_ack_acq(tg);
  while (oldepoch != epoch) {
    uint64_t expect = oldepoch;
    if (lj_tg_hs_epoch_ack_cas(tg, &expect, epoch))
      break;
    oldepoch = expect;
  }
  return oldepoch != epoch;
}

static void safepoint_apply_tg_mode(global_State *g, TGState *tg,
				    uint32_t actions, int native_parked,
				    int roots_already_scanned)
{
  if (actions & LJ_GC2_HS_ENABLE_BARRIER) {
    /* Mark activation runs with native entry closed. Invalidate comparison
    ** authority before enabling the TG barrier mirror, so a wrapped or stale
    ** certificate can never authorize a new cycle. */
    lj_tg_fnew_cert_reset_rel(tg);
    lj_tg_mark_active_rel(tg, 1);
  }
  if (actions & LJ_GC2_HS_DISABLE_BARRIER)
    lj_tg_mark_active_rel(tg, 0);
  if (actions & LJ_GC2_HS_ALLOC_BLACK)
    lj_tg_alloc_black_rel(tg, 1);
  if (actions & LJ_GC2_HS_ALLOC_WHITE)
    lj_tg_alloc_black_rel(tg, 0);
  if (!roots_already_scanned &&
      (actions & (LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_SCAN_OWNER_ROOTS))) {
    /* Each stopped TG publishes its complete private root set. Passing the TG
    ** directly is essential: cur_L may be NULL, may differ from thread_L, and
    ** must not be used to rediscover the owner tid for NEEDSCAN handoffs. A
    ** full SCAN_ROOTS epoch adds the once-per-snapshot global pass below;
    ** SCAN_OWNER_ROOTS intentionally services only an existing owner handoff. */
    if (native_parked)
      lj_gc2_scan_cycle_owner_tg_roots_native_parked(g, tg);
    else
      lj_gc2_scan_cycle_owner_tg_roots(g, tg);
  }
  if (actions & LJ_GC2_HS_FLUSH_SSB) {
    /*
    ** Remote-native ACK may observe no-Lua-stack GC worker TGs while they are
    ** actively draining GC2 work under their own TG. Let those workers publish
    ** their own SSB at GC-owner boundaries; remotely swapping their active SSB
    ** can race traversal-local rescan publication. Native mutators still carry
    ** cur_L and are flushed here.
    */
    if (lj_tg_load_cur_L(tg) != NULL || tg == lj_thr_get_tg_fallback(g))
      lj_gc2_flush_ssb(g, tg);  /* 05 section 5.6.2. */
  }
  (void)lj_gc2_flush_alloc(g, tg);  /* 04 section 4.8 safepoint flush. */
  if ((actions & LJ_GC2_HS_RESET_ALLOC) &&
      lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL)) {
    int prepared = lj_arena_alloc_prepare_sweep_kind(
	&tg->alloc, LJ_ARENAK_TRAVERSABLE);
    int plain_restored = lj_arena_alloc_restore_sweep_kind(
	&tg->alloc, LJ_ARENAK_PLAIN);
    if (lj_tg_flags_test_acq(tg, TGF_HUGETAB)) {
      lj_arena_hugetab_prepare_sweep(&tg->huge);
      tg->alloc.huge_retire_cursor = 0;
      tg->alloc.huge_reclaim_cursor = 0;
      tg->alloc.huge_retire_done = 0;
    }
    if (prepared && plain_restored)
      tg->alloc.prepare_epoch = gc2_cycle_acq(g);
    else
      lj_gc2_sweep_publish_wake(g);
  }
  if ((actions & LJ_GC2_HS_RESTORE_ALLOC) &&
      lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL)) {
    int restored;
    /* Abort is legal only before bounded root detachment/quarantine. Restore
    ** owner-local bins on the owning TG while it is stopped at this ACK. */
    restored = lj_arena_alloc_restore_sweep_kind(
	&tg->alloc, LJ_ARENAK_TRAVERSABLE);
    if (lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      lj_arena_hugetab_abort_sweep(&tg->huge);
    if (restored)
      tg->alloc.prepare_epoch = 0;
    else
      lj_gc2_sweep_publish_wake(g);
    tg->alloc.huge_retire_cursor = 0;
    tg->alloc.huge_reclaim_cursor = 0;
    tg->alloc.huge_retire_done = 0;
  }
  if (actions & LJ_GC2_HS_REDISPATCH)
    lj_tg_sync_dispatch_tg(g, tg);  /* 03 section 3.6, 07 section 7.3. */
  if (actions & LJ_GC2_HS_EXIT_TRACES)
    lj_trace_abort(g);  /* 08 section 8.7: no active recorder past ack. */
  if (actions & LJ_GC2_HS_STOPREQ)
    lj_tg_flags_or_rlx(tg, TGF_STOPREQ|TGF_STOPREQ_FRESH);
}

void lj_safepoint_apply_tg(global_State *g, TGState *tg, uint32_t actions)
{
  /* Attach/TG-only catch-up is not proof that the caller owns a stable Lua
  ** stack. Keep the public entry on the conservative owner-root path. */
  safepoint_apply_tg_mode(g, tg, actions, 0, 0);
}

static void safepoint_requeue_consumed(TGState *tg, uint32_t actions)
{
  /* Handshake leadership serializes counted request publication. Preserve the
  ** original pending slot and return the exact mask to the owner; it will
  ** consume this after closing native state if remote certification failed. */
  lj_tg_reqmask_rel(tg, actions);
  lj_tg_poll_rel(tg, 1);
  la_fence_seq();
  lj_tg_poll_futex_wake(tg, 1);
}

static uint32_t safepoint_ack_tg(global_State *g, TGState *tg,
				 int note_latency, int wait_consumed,
				 int native_parked,
				 int native_trace_cert_required)
{
  uint64_t epoch;
  uint32_t actions, oldpending;
  int roots_scanned;
  if (!g || !tg)
    return 0;
#if !(LJ_HASFFI && LJ_HASJIT)
  UNUSED(native_trace_cert_required);
#endif
retry:
  roots_scanned = 0;
  actions = lj_tg_reqmask_xchg_acqrel(tg, 0);  /* 05 section 5.4.2. */
  if (actions == 0) {
    if (lj_tg_poll_acq(tg) != 0) {
      if (lj_tg_reqmask_acq(tg) != 0)
	goto retry;
      /* Uncounted poll re-armed by a remote/TG-only STOPREQ ACK. Once the
      ** producing leader has left, consume the wake edge and return the action
      ** so an L-aware ack/check boundary throws from TGF_STOPREQ_FRESH. */
      if (wait_consumed && gc2_hs_leader_acq(g) == 0 &&
	  lj_tg_flags_test_acq(tg, TGF_STOPREQ_FRESH)) {
	/* Keep the uncounted edge armed until lj_safepoint_checkstop consumes
	** TGF_STOPREQ_FRESH. A caller which ignores the returned mask must not
	** silently lose the VM interrupt. */
	return LJ_GC2_HS_STOPREQ;
      }
      /* Remote leader acks do not resume this TG's VM, so they must not
      ** block behind another leader's already-consumed poll bit. */
      if (wait_consumed &&
	  safepoint_wait_consumed_ack(tg, gc2_hs_actions_acq(g)))
	goto retry;
    }
    return 0;
  }
  epoch = gc2_hs_epoch_acq(g);  /* 05 section 5.4.2 epoch. */
  if (native_parked && lj_tg_hs_epoch_ack_acq(tg) != epoch) {
#if LJ_HASFFI && LJ_HASJIT
    /* Admission captured this obligation before reqmask consumption. Never
    ** re-decide it from jit_base here: an ACTIVE->SUSPENDED callback
    ** transition can clear jit_base in the intervening window. */
    if (native_trace_cert_required &&
	!lj_ffi_native_trace_remote_certify(tg, NULL)) {
      safepoint_requeue_consumed(tg, actions);
      return 0;
    }
#endif
    if (actions & (LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_SCAN_OWNER_ROOTS)) {
      if (!lj_gc2_scan_cycle_owner_tg_roots_native_parked(g, tg)) {
	safepoint_requeue_consumed(tg, actions);
	return 0;
      }
      roots_scanned = 1;
    }
  }
  if (!safepoint_claim_epoch(tg, epoch)) {
    int hold;
    /* A nonzero reqmask consumed here owns a pending-count slot even when the
    ** epoch was already caught up by attach or by an earlier ack race. Do not
    ** apply actions twice, but do release the counted slot.
    */
    hold = safepoint_hold_poll_until_leader(g, actions);
    if (!hold)
      safepoint_clear_poll(tg);
    oldpending = gc2_hs_pending_sub_acqrel(g, 1);
    if (oldpending == 1)
      gc2_hs_pending_futex_wake(g, 1);
    if (hold && wait_consumed &&
	safepoint_wait_consumed_ack(tg, actions))
      goto retry;
    return 0;
  }
  safepoint_apply_tg_mode(g, tg, actions, native_parked, roots_scanned);
  if (note_latency)
    safepoint_note_ack_latency(g);  /* 13.8: mutator-observed poll latency. */
  {
    int hold = safepoint_hold_poll_until_leader(g, actions);
    if (!hold)
      safepoint_clear_poll(tg);
    oldpending = gc2_hs_pending_sub_acqrel(g, 1);  /* 05 section 5.4.2. */
    if (oldpending == 1)
      gc2_hs_pending_futex_wake(g, 1);
    /* VM-owned acks must not resume with a consumed trace-flush poll. Drop
    ** pending first so the leader can retire/unlink traces and clear poll.
    */
    /* Trace exit CP frames can acknowledge before snapshot restore is complete
    ** and jit_base can be cleared. Defer their consumed-poll wait to the later
    ** GC-defer or VM-exit boundary.
    */
    if (hold && wait_consumed &&
	safepoint_wait_consumed_ack(tg, actions))
      goto retry;
  }
  if (wait_consumed && (actions & LJ_GC2_HS_STOPREQ) &&
      (lj_tg_load_cur_L(tg) != NULL || lj_tg_load_thread_L(tg) != NULL)) {
    /* L-aware and TG-only owner ACKs retain a one-shot dispatch edge until
    ** checkstop throws. Remote ACKs use the post-leader registry rearm pass. */
    safepoint_rearm_fresh_stopreq_poll(tg);
  }
  return actions;
}

uint32_t lj_safepoint_retire_dead_tg(global_State *g, TGState *tg)
{
  uint64_t epoch;
  uint32_t oldpending, pending;
  if (!g || !tg || !lj_tg_flags_test_acq(tg, TGF_DEAD))
    return 0;
  pending = lj_tg_reqmask_xchg_acqrel(tg, 0);
  safepoint_clear_poll(tg);
  if (!pending)
    return 0;
  epoch = gc2_hs_epoch_acq(g);
  if (!safepoint_claim_epoch(tg, epoch)) {
    /* The nonzero reqmask means the handshake counted this TG. The already
    ** acknowledged epoch means attach/self-ack or a racing owner already
    ** applied the actions, but this retire path still owns the counted slot.
    */
    oldpending = gc2_hs_pending_sub_acqrel(g, 1);
    if (oldpending == 1)
      gc2_hs_pending_futex_wake(g, 1);
    return 0;
  }
  oldpending = gc2_hs_pending_sub_acqrel(g, 1);
  if (oldpending == 1)
    gc2_hs_pending_futex_wake(g, 1);
  return 1;
}

uint32_t lj_safepoint_ack(lua_State *L)
{
  ptrdiff_t oldbase, oldtop;
  uint32_t actions;
  if (!L)
    return 0;
#if LJ_HASPROFILE
  /* SIGPROF only publishes TG atomics. Consume its request on the interrupted
  ** TG's ordinary VM/C owner path before any dispatch table mutation. */
  lj_profile_owner_poll(L);
#endif
  if (L->base == NULL || tvref(L->stack) == NULL)
    return safepoint_ack_tg(G(L), L2TG(L), 1, 1, 0, 0);
  oldbase = savestack(L, L->base);
  oldtop = savestack(L, L->top);
  actions = safepoint_ack_tg(G(L), L2TG(L), 1, 1, 0, 0);
  L->base = restorestack(L, oldbase);
  L->top = restorestack(L, oldtop);
  return actions;  /* Safepoints must preserve the interrupted VM/C frame. */
}

uint32_t lj_safepoint_poll(lua_State *L)
{
  TGState *tg;
  if (!L)
    return 0;
  tg = L2TG(L);
  if (!tg)
    return 0;
  /* reqmask owns the counted pending slot. poll is the wake/dispatch signal and
  ** can be consumed independently when a thread catches up with the current
  ** epoch, so a nonzero reqmask must still drive the acknowledgement path. */
  if (lj_tg_poll_acq(tg) == 0 && lj_tg_reqmask_acq(tg) == 0 &&
      lj_tg_profile_request_acq(tg) == 0)
    return 0;
  return lj_safepoint_ack(L);
}

uint32_t lj_safepoint_poll_tg(TGState *tg)
{
  if (!tg || !tg->gl)
    return 0;
  if (lj_tg_poll_acq(tg) == 0 && lj_tg_reqmask_acq(tg) == 0)
    return 0;
  /* Worker/no-Lua-stack TGs still own private allocator and SSB state. They
  ** therefore participate at the same consumed-poll boundary instead of
  ** silently resuming after a remote native acknowledgement. */
  return safepoint_ack_tg(tg->gl, tg, 1, 1, 0, 0);
}

void lj_safepoint_checkstop(lua_State *L, uint32_t actions)
{
  TGState *tg;
  if (!L)
    return;
  tg = L2TG(L);
  /* TGF_STOPREQ is sticky bookkeeping: native regions use it to decide
  ** whether a shutdown request was already pending on entry. The fresh bit is
  ** the one-shot interrupt edge for VM dispatch checks. Otherwise a Lua pcall
  ** that catches a STOPREQ would immediately rethrow from the next dispatch
  ** boundary while the sticky bit is still intentionally set.
  */
  if ((actions & LJ_GC2_HS_STOPREQ) ||
      (tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ_FRESH))) {
    if (tg) {
      LJOSerrState oserr;
      lj_oserr_save(&oserr);
      /* Consume only the synthetic uncounted edge. A new handshake can publish
      ** between the first poll load and clear; fence and recheck its counted
      ** reqmask/leader publication, then restore poll before throwing. */
      safepoint_consume_fresh_stopreq_poll(tg);
      lj_oserr_restore(&oserr);
    }
    lj_err_callermsg(L, "thread interrupted: VM shutdown");
  }
}

uint32_t lj_safepoint_ack_check(lua_State *L)
{
  LJOSerrState oserr;
  uint32_t actions;
  /* VM external-unwind landings reach this helper immediately after their
  ** authoritative errno/LastError restore. ACK bookkeeping is observational
  ** and must not replace the error-edge pair, including immediately before a
  ** STOPREQ throws again. */
  lj_oserr_save(&oserr);
  actions = lj_safepoint_ack(L);
  lj_oserr_restore(&oserr);
  lj_safepoint_checkstop(L, actions);
  lj_oserr_restore(&oserr);
  return actions;
}

void lj_native_enter(TGState *tg)
{
  if (tg)
    lj_tg_in_native_inc_rel(tg);
}

void lj_native_enter_l(lua_State *L, LJNativeFrame *frame)
{
  TGState *tg = L ? L2TG(L) : NULL;
  if (frame) {
    frame->base = 0;
    frame->top = 0;
    frame->active = 0;
    if (L && L->base != NULL && tvref(L->stack) != NULL) {
      frame->base = savestack(L, L->base);
      frame->top = savestack(L, L->top);
      frame->active = 1;
    }
  }
  lj_native_enter(tg);
}

uint32_t lj_native_leave(lua_State *L)
{
  TGState *tg;
  uint32_t depth;
  if (!L)
    return 0;
  tg = L2TG(L);
  if (!tg)
    return 0;
  depth = lj_tg_in_native_acq(tg);
  if (depth == 1) {
    /* Close the remotely readable native snapshot before checking for work.
    ** If a remote ACK consumed poll first, the consumed-poll gate keeps this
    ** owner here until that scan completes. If this store wins, the leader
    ** cannot start a new remote-private scan and leaves the request for this
    ** owner-side poll. No Lua/TG-private mutation is permitted between them. */
    lj_tg_in_native_rel(tg, 0);
    /* Paired with the post-signal fence in safepoint_signal_late(): the owner
    ** cannot miss poll while the leader misses this native-state close. */
    la_fence_seq();
    return lj_safepoint_poll(L);
  }
  if (lj_tg_in_native_dec_rel(tg) != 0)
    return 0;
  return lj_safepoint_poll(L);
}

uint32_t lj_native_leave_tg(TGState *tg)
{
  uint32_t depth;
  if (!tg)
    return 0;
  depth = lj_tg_in_native_acq(tg);
  if (depth == 0)
    return 0;
  if (depth > 1) {
    (void)lj_tg_in_native_dec_rel(tg);
    return 0;
  }
  lj_tg_in_native_rel(tg, 0);
  la_fence_seq();  /* Pair with post-signal fence before remote native load. */
  return lj_safepoint_poll_tg(tg);
}

uint32_t lj_native_leave_l(lua_State *L, LJNativeFrame *frame)
{
  uint32_t actions = lj_native_leave(L);
  if (frame && frame->active && L && tvref(L->stack) != NULL) {
    L->base = restorestack(L, frame->base);
    L->top = restorestack(L, frame->top);
    frame->active = 0;
  }
  return actions;
}

static uint32_t safepoint_signal_late(global_State *g, uint32_t actions,
				      uint64_t epoch)
{
  TGState *tg;
  uint32_t signaled = 0;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    if (lj_tg_flags_test_acq(tg, TGF_DEAD))  /* 05 section 5.4.1. */
      continue;
    if (lj_tg_hs_epoch_ack_acq(tg) == epoch)
      continue;  /* 09 section 9.3: attach self-caught this epoch. */
    if (lj_tg_reqmask_acq(tg) != 0)
      continue;  /* Already counted by this handshake. */
    (void)gc2_hs_pending_add_rlx(g, 1);
    signaled++;
    lj_tg_reqmask_rel(tg, actions);  /* 05 section 5.4.2. */
    /* A consumed trace-flush poll can remain set after the TG has acked an
    ** earlier epoch, keeping that TG parked until the leader retires trace
    ** slots. A later handshake must still publish a fresh reqmask and count
    ** the TG for this epoch; the waiter will see reqmask and retry.
    */
    lj_tg_poll_rel(tg, 1);  /* 05 section 5.4.2 signal word. */
    /* Paired with the native owner close->poll fence. This is the Dekker edge
    ** which forbids both sides observing the old value: either the owner sees
    ** the request and acknowledges itself, or the leader observes native and
    ** completes the remote snapshot before clearing the consumed poll. */
    la_fence_seq();
    lj_tg_poll_futex_wake(tg, 1);
    if (lj_safepoint_retire_dead_tg(g, tg) && signaled != 0)
      signaled--;
  }
  return signaled;
}

static int safepoint_native_ack_allowed(TGState *tg, uint32_t actions,
					int *trace_cert_required)
{
  if (trace_cert_required)
    *trace_cert_required = 0;
  /* A sanctioned native-to-Lua reentry clears in_native with release ordering
  ** and polls before changing the Lua stack, C frame, root anchors, SSB or
  ** allocator. If a remote acknowledgement consumed the request first, its
  ** poll remains set through leader completion and the reentry waits for it.
  ** Thus a still-native TG is a stable owner-private snapshot for root/SSB and
  ** allocator boundary actions. Trace retirement has the extra JIT gate below.
  */
#if LJ_HASJIT
  /* Trace entry publishes jit_base before it loads a body/mcode pointer, and
  ** ordinary trace exit retains it through snapshot restore. Only generated
  ** FFI frames add an exact even-frame, pinned-slot certificate; all other
  ** jit_base windows continue to veto remote trace acknowledgment. A positive
  ** vmstate without jit_base is merely a conservative trace root for GC.
  */
  if ((actions & (LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ)) &&
      lj_tg_load_jit_base(tg) != NULL) {
#if LJ_HASFFI
    if (!lj_ffi_native_trace_remote_shape_allowed(tg))
      return 0;
    if (trace_cert_required)
      *trace_cert_required = 1;
#else
    return 0;
#endif
  }
#else
  UNUSED(tg); UNUSED(actions);
#endif
  return 1;
}

static void safepoint_ack_native(global_State *g, uint32_t actions)
{
  TGState *tg;
  TGState *self = lj_thr_get_tg_fallback(g);
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    if (lj_tg_flags_test_acq(tg, TGF_DEAD))  /* 05 section 5.4.1. */
      continue;
    if (tg == self)
      (void)safepoint_ack_tg(g, tg, 0, 0, 0, 0);
    else if (lj_tg_in_native_acq(tg)) {
      int trace_cert_required = 0;
      if (!safepoint_native_ack_allowed(tg, actions,
					&trace_cert_required))
	continue;
      /* Only this remote-native branch carries the consumed-poll stability
      ** certificate. If it wins reqmask, native leave must remain parked until
      ** the leader clears poll, so published frame offsets cannot race stack
      ** relocation or release during the exact scan. */
      (void)safepoint_ack_tg(g, tg, 0, 0, 1, trace_cert_required);
    }
  }
}

static int safepoint_trace_tg_active(global_State *g)
{
#if LJ_HASJIT
  TGState *tg;
  TGState *self = lj_thr_get_tg_fallback(g);
  uint32_t leader = gc2_hs_leader_acq(g);
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    if (lj_tg_flags_test_acq(tg, TGF_DEAD))
      continue;
    /*
    ** Trace quiescence protects peer TGs that may still need trace slots or
    ** mcode while leaving compiled code. The leader can legitimately start a
    ** trace-flush handshake from trace-exit C code before snapshot restore has
    ** reached the safe jit_base clear; waiting for that self-published edge
    ** would be a self-deadlock. The leader still applies its own safepoint action
    ** synchronously before this quiescence check.
    */
    if (tg == self && leader != 0 && lj_tg_tid_acq(tg) == leader)
      continue;
    if (lj_tg_load_jit_base(tg) != NULL) {
#if LJ_HASFFI
      uint64_t epoch = gc2_hs_epoch_acq(g);
      /* A current-epoch consumed poll freezes this exact generated frame until
      ** leader completion. Revalidate its TraceVec slot/pin before exempting it
      ** from quiescence; every ordinary trace-exit jit_base still vetoes. */
      if (lj_tg_poll_acq(tg) != 0 && lj_tg_reqmask_acq(tg) == 0 &&
	  lj_tg_hs_epoch_ack_acq(tg) == epoch &&
	  lj_ffi_native_trace_remote_certify(tg, NULL))
	continue;
#endif
      return 1;
    }
  }
#else
  UNUSED(g);
#endif
  return 0;
}

static void safepoint_wait_trace_quiescent(global_State *g, uint32_t actions)
{
  if (!(actions & (LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ)))
    return;
  while (safepoint_trace_tg_active(g)) {
    /* The pending count is the primary boundary. This final quiescence check
    ** closes attach/native-observation races before trace slots are unlinked.
    */
    safepoint_ack_native(g, actions);
    (void)lj_thr_retry_yield(NULL);
  }
}

static uint32_t safepoint_leader_id(global_State *g)
{
  TGState *self = lj_thr_get_tg_fallback(g);
  uint32_t id = self ? lj_tg_tid_acq(self) : 0;
  return lj_thr_id_is_owner(id) ? id : 1u;
}

#if LJ_HASJIT
static lua_State *safepoint_leader_lua_state(global_State *g)
{
  TGState *self = lj_thr_get_tg_fallback(g);
  lua_State *L = self ? lj_tg_load_cur_L(self) : NULL;
  /*
  ** FLUSHJ is executed by the safepoint leader after peer TGs have left traces,
  ** but trace retirement still allocates raw metadata through the token owner's
  ** lua_State. Use the leader TG's current state for that ownership; fall back to
  ** the main state only for bootstrap/test handshakes without a published cur_L.
  ** The eventless lj_trace_flushall_gc() path below keeps VM event stacks out of
  ** arbitrary leader action.
  */
  if (L && mref(L->glref, global_State) == g)
    return L;
  return mainthread_acq(g);
}
#endif

static uint32_t safepoint_leader_enter(global_State *g)
{
  uint32_t id = safepoint_leader_id(g);
  for (;;) {
    uint32_t expect = 0;
    if (gc2_hs_leader_cas(g, &expect, id))
      return id;
    /* A contender may help only its exact TLS TG. Remote acknowledgement is
    ** owned by the active leader after request publication and its full fence;
    ** helping arbitrary native TGs here could race that fence, or scan the
    ** leader TG while its self-wait bypass is active. */
    {
      TGState *self = lj_thr_get_tg();
      if (self && self->gl == g)
	safepoint_ack_tg(g, self, 0, 0, 0, 0);
    }
    if (expect == 0)
      continue;
    gc2_hs_leader_futex_wait(g, expect, 1000000);
  }
}

static void safepoint_leader_leave(global_State *g, uint32_t id)
{
  uint32_t expect = id;
  if (gc2_hs_leader_cas(g, &expect, 0))
    gc2_hs_leader_futex_wake(g, 1);
  else {
    gc2_hs_leader_rel(g, 0);
    gc2_hs_leader_futex_wake(g, 1);
  }
}

static void safepoint_finish_prior_epoch(global_State *g)
{
  /* A synchronous leader can be entered while a previously published async
  ** request still owns counted reqmask slots (notably STOPREQ arriving between
  ** bytecodes and an allocation-triggered GC). Never overwrite that pending
  ** count with the next epoch's sentinel. Finish the prior boundary first;
  ** applying STOPREQ here only publishes its sticky/fresh flag, so the normal
  ** VM/native exit boundary still performs the user-visible throw. */
  while (gc2_hs_pending_acq(g) != 0) {
    uint32_t pending = gc2_hs_pending_acq(g);
    safepoint_ack_native(g, gc2_hs_actions_acq(g));
    if (gc2_hs_pending_acq(g) == 0)
      break;
    gc2_hs_pending_futex_wait(g, pending, 1000000);
  }
}

uint32_t lj_safepoint_handshake(global_State *g, uint32_t actions)
{
  uint64_t epoch;
  uint32_t leader;
  uint32_t signaled, oldpending;
  if (!g || actions == 0)
    return 0;
  leader = safepoint_leader_enter(g);
  safepoint_finish_prior_epoch(g);
  gc2_hs_actions_rel(g, actions);  /* 05 section 5.4.2. */
  gc2_hs_pending_rel(g, 1);  /* 09 section 9.3 leader sentinel. */
  epoch = gc2_hs_epoch_rlx(g) + 1u;
  gc2_hs_signal_ns_rel(g, safepoint_now_ns());
  gc2_hs_epoch_rel(g, epoch);  /* 05 section 5.4.2. */

  signaled = safepoint_signal_late(g, actions, epoch);
  for (;;) {
    uint32_t late;
    safepoint_ack_native(g, actions);
    late = safepoint_signal_late(g, actions, epoch);
    if (late == 0)
      break;
    signaled += late;
  }

  oldpending = gc2_hs_pending_sub_acqrel(g, 1);  /* Drop sentinel. */
  if (oldpending == 1)
    gc2_hs_pending_futex_wake(g, 1);
  while (gc2_hs_pending_acq(g) != 0) {
    signaled += safepoint_signal_late(g, actions, epoch);
    safepoint_ack_native(g, actions);
    if (gc2_hs_pending_acq(g) == 0)
      break;
    gc2_hs_pending_futex_wait(g, gc2_hs_pending_rlx(g), 1000000);
  }
  if (actions & LJ_GC2_HS_SCAN_ROOTS) {
    TGState *self = lj_thr_get_tg_fallback(g);
    lj_gc2_scan_cycle_global_roots(g);
    /* Global traversal can enqueue rescans in the leader's active SSB after
    ** its self acknowledgement already flushed. Publish that final suffix. */
    if (self)
      (void)lj_gc2_flush_ssb(g, self);
  }
  safepoint_wait_trace_quiescent(g, actions);
#if LJ_HASJIT
  if (actions & LJ_GC2_HS_FLUSHJ) {
    /*
     ** The safepoint leader can be any participating thread. Flush trace state
     ** here, but do not emit a TRACE "flush" event through the VM event stack:
     ** that stack belongs to vmthread and is not owned by an arbitrary leader.
     */
    (void)lj_trace_flushall_gc(safepoint_leader_lua_state(g));  /* 08 section 8.7 leader action. */
  }
#endif
  (void)lj_gc2_reclaim_retired(g, epoch);  /* 05 section 5.9 grace drain. */
  if (actions & LJ_GC2_HS_FLUSH_SSB) {
    TGState *self = lj_thr_get_tg_fallback(g);
    /* Epoch reclamation can preserve a late metadata/object edge after the
    ** global-root flush above. Publish that suffix before the handshake's
    ** completion LP so fixpoint callers never manufacture another handshake
    ** solely to expose one leader-local SSB entry. */
    if (self)
      (void)lj_gc2_flush_ssb(g, self);
  }
  /* hs_pending==0 does not pin registry nodes. Hold the ordinary tactical
  ** reader lease across the final list walks and leader handoff; TG reclamation
  ** is try-only and backs out rather than waiting on this reader. */
  lj_gc2_smr_read_enter(g);
  safepoint_clear_consumed_polls(g, epoch);
  safepoint_leader_leave(g, leader);
  safepoint_rearm_fresh_stopreq_polls(g);
  lj_gc2_smr_read_leave(g);
  return signaled;
}
