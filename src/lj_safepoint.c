/*
** Safepoint and soft-handshake scaffold.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_safepoint_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_err.h"
#include "lj_gc2.h"
#include "lj_mcode.h"
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

static int safepoint_wait_consumed_ack(TGState *tg)
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

static int safepoint_hold_poll_until_leader(global_State *g, uint32_t actions)
{
  /* Trace flush boundaries mutate trace slots and patched bytecode after all
  ** TGs have acknowledged. Keep acknowledged TGs from re-entering trace
  ** dispatch while the leader is between the grace boundary and that retire
  ** action. Epochs without a live leader cannot complete that consumed-poll
  ** phase, so they clear at acknowledgement like ordinary safepoints.
  */
  return (actions & (LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ)) &&
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

void lj_safepoint_apply_tg(global_State *g, TGState *tg, uint32_t actions)
{
  if (actions & LJ_GC2_HS_ENABLE_BARRIER)
    lj_tg_mark_active_rel(tg, 1);
  if (actions & LJ_GC2_HS_DISABLE_BARRIER)
    lj_tg_mark_active_rel(tg, 0);
  if (actions & LJ_GC2_HS_ALLOC_BLACK)
    lj_tg_alloc_black_rel(tg, 1);
  if (actions & LJ_GC2_HS_ALLOC_WHITE)
    lj_tg_alloc_black_rel(tg, 0);
  if (actions & LJ_GC2_HS_SCAN_ROOTS) {
    lua_State *L = lj_tg_load_cur_L(tg);
    lj_gc2_scan_cycle_roots(g, L);  /* 05 section 5.7.1/5.7.2. */
  }
  if (actions & LJ_GC2_HS_FLUSH_SSB)
    lj_gc2_flush_ssb(g, tg);  /* 05 section 5.6.2. */
  (void)lj_gc2_flush_alloc(g, tg);  /* 04 section 4.8 safepoint flush. */
  if ((actions & LJ_GC2_HS_RESET_ALLOC) &&
      lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL)) {
    lj_arena_alloc_prepare_sweep_kind(&tg->alloc, LJ_ARENAK_TRAVERSABLE);
    lj_arena_alloc_restore_sweep_kind(&tg->alloc, LJ_ARENAK_PLAIN);
    tg->alloc.prepare_epoch = gc2_cycle_acq(g);
  }
  if (actions & LJ_GC2_HS_REDISPATCH)
    lj_tg_sync_dispatch_tg(g, tg);  /* 03 section 3.6, 07 section 7.3. */
  if (actions & LJ_GC2_HS_EXIT_TRACES)
    lj_trace_abort(g);  /* 08 section 8.7: no active recorder past ack. */
  if (actions & LJ_GC2_HS_STOPREQ)
    lj_tg_flags_or_rlx(tg, TGF_STOPREQ|TGF_STOPREQ_FRESH);
}

static uint32_t safepoint_ack_tg(global_State *g, TGState *tg,
				 int note_latency, int wait_consumed)
{
  uint64_t epoch;
  uint32_t actions, oldpending;
  if (!g || !tg)
    return 0;
retry:
  actions = lj_tg_reqmask_xchg_acqrel(tg, 0);  /* 05 section 5.4.2. */
  if (actions == 0) {
    if (lj_tg_poll_acq(tg) != 0) {
      if (lj_tg_reqmask_acq(tg) != 0)
	goto retry;
      /* Remote leader acks do not resume this TG's VM, so they must not
      ** block behind another leader's already-consumed poll bit. */
      if (wait_consumed && safepoint_wait_consumed_ack(tg))
	goto retry;
    }
    return 0;
  }
  epoch = gc2_hs_epoch_acq(g);  /* 05 section 5.4.2 epoch. */
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
    if (hold && wait_consumed && lj_tg_load_jit_base(tg) == NULL &&
	safepoint_wait_consumed_ack(tg))
      goto retry;
    return 0;
  }
  lj_safepoint_apply_tg(g, tg, actions);
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
    /* Trace exit CP frames acknowledge before vm_exit_interp can clear
    ** jit_base. Defer their consumed-poll wait to the VM exit poll.
    */
    if (hold && wait_consumed && lj_tg_load_jit_base(tg) == NULL &&
	safepoint_wait_consumed_ack(tg))
      goto retry;
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
  if (L->base == NULL || tvref(L->stack) == NULL)
    return safepoint_ack_tg(G(L), L2TG(L), 1, 1);
  oldbase = savestack(L, L->base);
  oldtop = savestack(L, L->top);
  actions = safepoint_ack_tg(G(L), L2TG(L), 1, 1);
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
  if (!tg || lj_tg_poll_acq(tg) == 0)  /* 05 section 5.4.2 poll. */
    return 0;
  return lj_safepoint_ack(L);
}

void lj_safepoint_checkstop(lua_State *L, uint32_t actions)
{
  TGState *tg;
  if (!L)
    return;
  tg = L2TG(L);
  if ((actions & LJ_GC2_HS_STOPREQ) ||
      (tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ))) {
    if (tg)
      (void)lj_tg_flags_and_rlx(tg, (uint8_t)~TGF_STOPREQ_FRESH);
    lj_err_callermsg(L, "thread interrupted: VM shutdown");
  }
}

uint32_t lj_safepoint_ack_check(lua_State *L)
{
  uint32_t actions = lj_safepoint_ack(L);
  lj_safepoint_checkstop(L, actions);
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
    uint32_t actions = lj_safepoint_poll(L);
    (void)lj_tg_in_native_dec_rel(tg);
    return actions;
  }
  if (lj_tg_in_native_dec_rel(tg) != 0)
    return 0;
  return lj_safepoint_poll(L);
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
    lj_tg_poll_futex_wake(tg, 1);
    if (lj_safepoint_retire_dead_tg(g, tg) && signaled != 0)
      signaled--;
  }
  return signaled;
}

static int safepoint_native_ack_allowed(TGState *tg, uint32_t actions)
{
  if ((actions & LJ_GC2_HS_SCAN_ROOTS)) {
    lua_State *L = lj_tg_load_cur_L(tg);
    /*
    ** Remote native acknowledgements may apply phase bits and flush buffers, but
    ** root scans for a TG with a Lua stack must be owner-applied. Lua frame
    ** functions/protos live in frame headers, not ordinary TValue slots; scanning
    ** a remote native stack as raw slots can miss the closure/proto edge and free
    ** live KGC constants.
    */
    if (L && tvref(L->stack) != NULL)
      return 0;
  }
#if LJ_HASJIT
  /* A TG leaving mcode keeps jit_base set until vm_exit_interp has restored
  ** from the trace snapshot. Trace-flush boundaries must not remotely ack that
  ** native/C window, otherwise the leader can clear the trace slot needed by
  ** lj_snap_restore_exit().
  */
  if ((actions & (LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ)) &&
      (lj_tg_load_jit_base(tg) != NULL || lj_tg_vmstate_load_acq(tg) > 0))
    return 0;
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
      safepoint_ack_tg(g, tg, 0, 0);  /* Leader owns this synthetic ack. */
    else if (lj_tg_in_native_acq(tg) &&
	     safepoint_native_ack_allowed(tg, actions))
      safepoint_ack_tg(g, tg, 0, 0);  /* 05 section 5.4.3 remote native ack. */
  }
}

static int safepoint_trace_tg_active(global_State *g)
{
#if LJ_HASJIT
  TGState *tg;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    if (lj_tg_flags_test_acq(tg, TGF_DEAD))
      continue;
    if (lj_tg_load_jit_base(tg) != NULL || lj_tg_vmstate_load_acq(tg) > 0)
      return 1;
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
  return id && id != LJ_THREAD_GCSCAN ? id : 1u;
}

static uint32_t safepoint_leader_enter(global_State *g)
{
  uint32_t id = safepoint_leader_id(g);
  for (;;) {
    uint32_t expect = 0;
    if (gc2_hs_leader_cas(g, &expect, id))
      return id;
    /* Avoid leader-wait vs ack-wait deadlock while preserving current action
    ** semantics for native acknowledgements.
    */
    safepoint_ack_native(g, gc2_hs_actions_acq(g));
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

uint32_t lj_safepoint_handshake(global_State *g, uint32_t actions)
{
  uint64_t epoch;
  uint32_t leader;
  uint32_t signaled, oldpending;
  if (!g || actions == 0)
    return 0;
  leader = safepoint_leader_enter(g);
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
  safepoint_wait_trace_quiescent(g, actions);
  if (actions & LJ_GC2_HS_EXIT_TRACES)
    (void)lj_trace_flushscope_retire_hs(g, epoch);
  if (actions & LJ_GC2_HS_FLUSHJ)
    (void)lj_trace_flushall(mainthread_acq(g));  /* 08 section 8.7 leader action. */
  (void)lj_gc2_reclaim_retired(g, epoch);  /* 05 section 5.9 grace drain. */
  safepoint_clear_consumed_polls(g, epoch);
  safepoint_leader_leave(g, leader);
  return signaled;
}
