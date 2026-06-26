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
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include <time.h>

static uint64_t safepoint_now_ns(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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

void lj_safepoint_apply_tg(global_State *g, TGState *tg, uint32_t actions)
{
  if (actions & LJ_GC2_HS_ENABLE_BARRIER)
    la_store32_rel(&tg->mark_active, 1);
  if (actions & LJ_GC2_HS_DISABLE_BARRIER)
    la_store32_rel(&tg->mark_active, 0);
  if (actions & LJ_GC2_HS_ALLOC_BLACK)
    la_store8_rel(&tg->alloc.alloc_black, 1);
  if (actions & LJ_GC2_HS_ALLOC_WHITE)
    la_store8_rel(&tg->alloc.alloc_black, 0);
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
    lj_tg_flags_or_rlx(tg, TGF_STOPREQ);  /* 09 section 9.6 shutdown. */
}

static uint32_t safepoint_ack_tg(global_State *g, TGState *tg)
{
  uint64_t epoch, oldepoch;
  uint32_t actions, oldpending;
  if (!g || !tg)
    return 0;
  actions = lj_tg_reqmask_xchg_acqrel(tg, 0);  /* 05 section 5.4.2. */
  if (actions == 0)
    return 0;
  epoch = gc2_hs_epoch_acq(g);  /* 05 section 5.4.2 epoch. */
  oldepoch = lj_tg_hs_epoch_ack_acq(tg);
  while (oldepoch != epoch) {
    uint64_t expect = oldepoch;
    if (lj_tg_hs_epoch_ack_cas(tg, &expect, epoch))
      break;  /* This thread owns the ack for the epoch. */
    oldepoch = expect;
  }
  if (oldepoch == epoch)
    return 0;
  lj_safepoint_apply_tg(g, tg, actions);
  safepoint_note_ack_latency(g);  /* 13.8: mutator-observed poll latency. */
  lj_tg_poll_rel(tg, 0);
  oldpending = gc2_hs_pending_sub_acqrel(g, 1);  /* 05 section 5.4.2. */
  if (oldpending == 1)
    gc2_hs_pending_futex_wake(g, 1);
  return actions;
}

uint32_t lj_safepoint_ack(lua_State *L)
{
  return L ? safepoint_ack_tg(G(L), L2TG(L)) : 0;
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
      (tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ)))
    lj_err_callermsg(L, "thread interrupted: VM shutdown");
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
    la_store8_rel(&tg->in_native, 1);  /* 05 section 5.4.3. */
}

uint32_t lj_native_leave(lua_State *L)
{
  TGState *tg;
  if (!L)
    return 0;
  tg = L2TG(L);
  if (!tg)
    return 0;
  la_store8_rlx(&tg->in_native, 0);  /* 05 section 5.4.3 boundary. */
  return lj_safepoint_poll(L);
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
    if (lj_tg_reqmask_acq(tg) != 0 || lj_tg_poll_acq(tg) != 0)
      continue;  /* Already counted by this handshake. */
    (void)gc2_hs_pending_add_rlx(g, 1);
    signaled++;
    lj_tg_reqmask_rel(tg, actions);  /* 05 section 5.4.2. */
    lj_tg_poll_rel(tg, 1);  /* 05 section 5.4.2 signal word. */
  }
  return signaled;
}

static void safepoint_ack_native(global_State *g)
{
  TGState *tg;
  TGState *self = lj_thr_get_tg_fallback(g);
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    if (lj_tg_flags_test_acq(tg, TGF_DEAD))  /* 05 section 5.4.1. */
      continue;
    if (tg == self)
      safepoint_ack_tg(g, tg);  /* Leader self-ack is a real poll. */
    else if (la_load8_acq(&tg->in_native))
      safepoint_ack_tg(g, tg);  /* 05 section 5.4.3 remote native ack. */
  }
}

static uint32_t safepoint_leader_id(global_State *g)
{
  TGState *self = lj_thr_get_tg_fallback(g);
  uint32_t id = self ? la_load32_acq(&self->tid) : 0;
  return id && id != LJ_THREAD_GCSCAN ? id : 1u;
}

static uint32_t safepoint_leader_enter(global_State *g)
{
  uint32_t id = safepoint_leader_id(g);
  for (;;) {
    uint32_t expect = 0;
    if (gc2_hs_leader_cas(g, &expect, id))
      return id;
    safepoint_ack_native(g);  /* Avoid leader-wait vs ack-wait deadlock. */
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
    safepoint_ack_native(g);
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
    safepoint_ack_native(g);
    if (gc2_hs_pending_acq(g) == 0)
      break;
    gc2_hs_pending_futex_wait(g, gc2_hs_pending_rlx(g), 1000000);
  }
  if (actions & LJ_GC2_HS_FLUSHJ)
    (void)lj_trace_flushall(mainthread_acq(g));  /* 08 section 8.7 leader action. */
  (void)lj_gc2_reclaim_retired(g, epoch);  /* 05 section 5.9 grace drain. */
  safepoint_leader_leave(g, leader);
  return signaled;
}
