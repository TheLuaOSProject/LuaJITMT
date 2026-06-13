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
#include "lj_safepoint.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_trace.h"

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
    lua_State *L = tg->cur_L;
    lj_gc2_scan_roots(g, L);  /* 05 section 5.7.1/5.7.2. */
  }
  if (actions & LJ_GC2_HS_FLUSH_SSB)
    lj_gc2_flush_ssb(g, tg);  /* 05 section 5.6.2. */
  if ((actions & LJ_GC2_HS_RESET_ALLOC) &&
      (tg->tg_flags & TGF_ARENA_INTERNAL))
    lj_arena_alloc_prepare_sweep(&tg->alloc);  /* 04 section 4.6. */
  if (actions & LJ_GC2_HS_REDISPATCH)
    lj_tg_sync_dispatch_tg(g, tg);  /* 03 section 3.6, 07 section 7.3. */
  if (actions & LJ_GC2_HS_EXIT_TRACES)
    lj_trace_abort(g);  /* 08 section 8.7: no active recorder past ack. */
  if (actions & LJ_GC2_HS_FLUSHJ) {
    lua_State *L = tg->cur_L ? tg->cur_L : mainthread(g);
    (void)lj_trace_flushall(L);  /* Temporary single-mutator flush action. */
  }
  if (actions & LJ_GC2_HS_STOPREQ)
    la_or8_rlx(&tg->tg_flags, TGF_STOPREQ);  /* 09 section 9.6 shutdown. */
}

static uint32_t safepoint_ack_tg(global_State *g, TGState *tg)
{
  uint64_t epoch, oldepoch;
  uint32_t actions, oldpending;
  if (!g || !tg)
    return 0;
  actions = la_xchg32_acqrel(&tg->reqmask, 0);  /* 05 section 5.4.2. */
  if (actions == 0)
    return 0;
  epoch = la_load64_acq(&g->gc2.hs_epoch);  /* 05 section 5.4.2 epoch. */
  oldepoch = la_load64_acq(&tg->hs_epoch_ack);
  while (oldepoch != epoch) {
    uint64_t expect = oldepoch;
    if (la_cas64(&tg->hs_epoch_ack, &expect, epoch, LA_ACQ_REL, LA_ACQ))
      break;  /* This thread owns the ack for the epoch. */
    oldepoch = expect;
  }
  if (oldepoch == epoch)
    return 0;
  lj_safepoint_apply_tg(g, tg, actions);
  la_store32_rlx(&tg->poll, 0);
  oldpending = la_sub32_acqrel(&g->gc2.hs_pending, 1);  /* 05 section 5.4.2. */
  if (oldpending == 1)
    la_futex_wake(&g->gc2.hs_pending, 1);
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
  if (!tg || la_load32_acq(&tg->poll) == 0)  /* 05 section 5.4.2 poll. */
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
      (tg && (la_load8_acq(&tg->tg_flags) & TGF_STOPREQ)))
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
  for (tg = (TGState *)la_loadptr_acq((void *const *)&g->gc2.tg_list);
       tg != NULL;
       tg = (TGState *)la_loadptr_acq((void *const *)&tg->next_tg)) {
    if (la_load8_acq(&tg->tg_flags) & TGF_DEAD)  /* 05 section 5.4.1. */
      continue;
    if (la_load64_acq(&tg->hs_epoch_ack) == epoch)
      continue;  /* 09 section 9.3: attach self-caught this epoch. */
    if (la_load32_acq(&tg->reqmask) != 0 || la_load32_acq(&tg->poll) != 0)
      continue;  /* Already counted by this handshake. */
    (void)la_add32_rlx(&g->gc2.hs_pending, 1);
    signaled++;
    la_store32_rel(&tg->reqmask, actions);  /* 05 section 5.4.2. */
    la_store32_rel(&tg->poll, 1);  /* 05 section 5.4.2 signal word. */
  }
  return signaled;
}

static void safepoint_ack_native(global_State *g)
{
  TGState *tg;
  TGState *self = lj_thr_get_tg();
  for (tg = (TGState *)la_loadptr_acq((void *const *)&g->gc2.tg_list);
       tg != NULL;
       tg = (TGState *)la_loadptr_acq((void *const *)&tg->next_tg)) {
    lua_State *L;
    if (la_load8_acq(&tg->tg_flags) & TGF_DEAD)  /* 05 section 5.4.1. */
      continue;
    L = (lua_State *)la_loadptr_acq((void *const *)&tg->cur_L);
    if (tg == self)
      safepoint_ack_tg(g, tg);  /* Leader self-ack is a real poll. */
    else if (la_load8_acq(&tg->in_native) && L)
      safepoint_ack_tg(g, tg);  /* 05 section 5.4.3 remote native ack. */
  }
}

uint32_t lj_safepoint_handshake(global_State *g, uint32_t actions)
{
  uint64_t epoch;
  uint32_t signaled, oldpending;
  if (!g || actions == 0)
    return 0;
  la_store32_rel(&g->gc2.hs_actions, actions);  /* 05 section 5.4.2. */
  la_store32_rel(&g->gc2.hs_pending, 1);  /* 09 section 9.3 leader sentinel. */
  epoch = la_load64_rlx(&g->gc2.hs_epoch) + 1u;
  la_store64_rel(&g->gc2.hs_epoch, epoch);  /* 05 section 5.4.2. */

  signaled = safepoint_signal_late(g, actions, epoch);
  for (;;) {
    uint32_t late;
    safepoint_ack_native(g);
    late = safepoint_signal_late(g, actions, epoch);
    if (late == 0)
      break;
    signaled += late;
  }

  oldpending = la_sub32_acqrel(&g->gc2.hs_pending, 1);  /* Drop sentinel. */
  if (oldpending == 1)
    la_futex_wake(&g->gc2.hs_pending, 1);
  while (la_load32_acq(&g->gc2.hs_pending) != 0) {
    signaled += safepoint_signal_late(g, actions, epoch);
    safepoint_ack_native(g);
    if (la_load32_acq(&g->gc2.hs_pending) == 0)
      break;
    la_futex_wait(&g->gc2.hs_pending, la_load32_rlx(&g->gc2.hs_pending),
		  1000000);
  }
  (void)lj_str_reclaim_retired(g, epoch);  /* 05 section 5.9 SMR drain. */
  (void)lj_tab_reclaim_retired(g, epoch);  /* 06 section 6.3.5 SMR drain. */
  return signaled;
}
