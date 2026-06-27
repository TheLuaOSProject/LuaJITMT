/*
** Per-OS-thread state block scaffolding for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_tg_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_buf.h"
#include "lj_dispatch.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"

static void tg_init_ssb(TGState *tg)
{
  lj_gc2_ssb_owner_rel(&tg->ssb_node[0], tg);
  lj_gc2_ssb_next_rel(&tg->ssb_node[0], NULL);
  lj_gc2_ssb_count_rel(&tg->ssb_node[0], 0);
  lj_gc2_ssb_owner_rel(&tg->ssb_node[1], tg);
  lj_gc2_ssb_next_rel(&tg->ssb_node[1], NULL);
  lj_gc2_ssb_count_rel(&tg->ssb_node[1], 0);
  lj_tg_ssb_active_rel(tg, &tg->ssb_node[0]);
  lj_tg_ssb_free_store_rlx(tg, &tg->ssb_node[1]);
  lj_tg_ssb_base_rel(tg, tg->ssb_node[0].slot);
  lj_tg_ssb_next_rel(tg, tg->ssb_node[0].slot);
  lj_tg_ssb_end_rel(tg, tg->ssb_node[0].slot + TG_GC2_SSB_SLOTS);
}

static void tg_init_common(global_State *g, TGState *tg, lua_State *L)
{
  tg->gl = g;
  lj_tg_store_cur_L(tg, L);
  lj_tg_store_thread_L(tg, L);
  tg->vmstate = ~LJ_VMST_INTERP;
  tg->prng = g->prng;
  tg_init_ssb(tg);
  lj_buf_init(NULL, &tg->tmpbuf);
#if LJ_HASJIT
  memcpy(tg->hotcount, G2GG(g)->hotcount, sizeof(tg->hotcount));
#endif
  memcpy(tg->dispatch, G2GG(g)->dispatch, sizeof(tg->dispatch));
}

void lj_tg_init(GG_State *GG, int alloc_ready)
{
  TGState *tg = &GG->main_tg;
  global_State *g = &GG->g;
  lua_State *L = &GG->L;
  uint32_t tid = lj_thr_newid();
  g->main_tg = tg;
  lj_tg_tid_rel(tg, tid);
  L->tg_hint = tg;
  lj_state_owner_rel(L, tid);
  if (!lj_thr_get_tg())
    lj_thr_set_tg(tg);  /* 03 section 3.2: bootstrap main OS-thread TLS. */
  if (!alloc_ready)
    lj_arena_alloc_init(&tg->alloc);
  else
    lj_tg_flags_or_rlx(tg, TGF_ARENA_INTERNAL);
  lj_arena_allocd_init(&tg->allocd, &tg->alloc, &tg->prng, 0);
  if (alloc_ready && lj_arena_hugetab_init(&tg->huge, TG_HUGETAB_BITS)) {
    lj_tg_flags_or_rlx(tg, TGF_HUGETAB);
    lj_arena_allocd_sethugetab(&tg->allocd, &tg->huge);
  }
  tg_init_common(g, tg, L);
}

void lj_tg_fini(global_State *g)
{
  if (g->main_tg) {
    lj_buf_free(g, &g->main_tg->tmpbuf);
    if (lj_tg_flags_test_acq(g->main_tg, TGF_HUGETAB))
      lj_arena_hugetab_fini(&g->main_tg->huge);
    lj_arena_alloc_fini(&g->main_tg->alloc);
  }
}

void lj_tg_init_thread(global_State *g, TGState *tg, lua_State *L,
		       int arena_internal)
{
  memset(tg, 0, sizeof(*tg));
  if (L)
    L->tg_hint = tg;
  lj_arena_alloc_init(&tg->alloc);
  if (arena_internal)
    lj_tg_flags_or_rlx(tg, TGF_ARENA_INTERNAL);
  lj_arena_allocd_init(&tg->allocd, &tg->alloc, &tg->prng, 0);
  if (arena_internal && lj_arena_hugetab_init(&tg->huge, TG_HUGETAB_BITS)) {
    lj_tg_flags_or_rlx(tg, TGF_HUGETAB);
    lj_arena_allocd_sethugetab(&tg->allocd, &tg->huge);
  }
  tg_init_common(g, tg, L);
}

void lj_tg_fini_thread(global_State *g, TGState *tg)
{
  if (!tg)
    return;
  lj_buf_free(g, &tg->tmpbuf);
  if (lj_tg_flags_test_acq(tg, TGF_HUGETAB))
    lj_arena_hugetab_fini(&tg->huge);
  lj_arena_alloc_fini(&tg->alloc);
}

static void tg_adopt_gc2_phase(global_State *g, TGState *tg)
{
  uint32_t phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK) {
    lj_tg_mark_active_rel(tg, 1);
    lj_tg_alloc_black_rel(tg, 1);
  } else if (phase == LJ_GC2_SWEEP) {
    lj_tg_mark_active_rel(tg, 0);
    lj_tg_alloc_black_rel(tg, (uint8_t)(gc2_cycle_sweep_minor_acq(g) == 0));
  } else {
    lj_tg_mark_active_rel(tg, gc2_generational_acq(g) != 0);
    lj_tg_alloc_black_rel(tg, 0);
  }
}

static void tg_attach_catchup(global_State *g, TGState *tg)
{
  uint64_t epoch = gc2_hs_epoch_acq(g);
  uint32_t pending = gc2_hs_pending_acq(g);
  uint32_t actions = pending ? gc2_hs_actions_acq(g) : 0;
  lj_tg_hs_epoch_ack_store_rlx(tg, epoch);
  if (actions) {
    lj_safepoint_apply_tg(g, tg, actions);
    lj_tg_reqmask_rel(tg, 0);
    lj_tg_poll_rel(tg, 0);
    lj_tg_hs_epoch_ack_rel(tg, epoch);  /* 09 section 9.3 self-ack. */
  }
}

void lj_tg_attach(global_State *g, TGState *tg)
{
  TGState *head;
  if (!g || !tg)
    return;
  lj_tg_poll_store_rlx(tg, 0);
  lj_tg_reqmask_store_rlx(tg, 0);
  tg_adopt_gc2_phase(g, tg);  /* 09 section 9.3 attach catch-up scaffold. */
  tg_attach_catchup(g, tg);
  lj_tg_flags_and_rlx(tg, (uint8_t)~TGF_DEAD);
  do {
    head = gc2_tg_list_acq(g);  /* 05 section 5.4.1. */
    lj_tg_next_rel(tg, head);
  } while (!gc2_tg_list_cas(g, &head, tg));  /* 05 section 5.4.1 CAS-prepend. */
  gc2_n_threads_add_rlx(g, 1);  /* Live TG count; list keeps dead nodes. */
}

void lj_tg_detach(global_State *g, TGState *tg)
{
  uint8_t oldflags;
  lua_State *thread_L;
  if (!g || !tg)
    return;
  thread_L = lj_tg_load_thread_L(tg);
  if (thread_L &&
      (lj_tg_reqmask_acq(tg) != 0 || lj_tg_poll_acq(tg) != 0))
    (void)lj_safepoint_ack(thread_L);  /* Leaving TG owns its ack. */
  (void)lj_gc2_flush_ssb(g, tg);  /* 09 section 9.3 detach publishes SSB. */
  (void)lj_gc2_flush_alloc(g, tg);  /* 04 section 4.8 detach accounting. */
  la_fence_rel();
  oldflags = lj_tg_flags_or_rlx(tg, TGF_DEAD);  /* 05 section 5.4.1. */
  if (!(oldflags & TGF_DEAD))
    (void)gc2_n_threads_sub_acqrel(g, 1);
  lj_tg_reqmask_rel(tg, 0);
  lj_tg_poll_rel(tg, 0);
  lj_tg_in_native_store_rlx(tg, 0);
#if LJ_HASFFI
  lj_tg_ffi_call_func_rel(tg, NULL);
#endif
}

static int tg_transfer_dead_alloc(global_State *g, TGState *tg)
{
  TGState *main_tg = g ? g->main_tg : NULL;
  if (!lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return 1;
  if (!main_tg || !lj_tg_flags_test_acq(main_tg, TGF_ARENA_INTERNAL))
    return 0;
  lj_buf_free(g, &tg->tmpbuf);  /* Route through the still-findable owner. */
  lj_buf_init(NULL, &tg->tmpbuf);
  if (lj_tg_flags_test_acq(tg, TGF_HUGETAB)) {
    if (!lj_tg_flags_test_acq(main_tg, TGF_HUGETAB) ||
	!lj_arena_hugetab_transfer(&main_tg->huge, &tg->huge,
				   lj_arena_alloc_owner_acq(&main_tg->alloc)))
      return 0;
    lj_arena_hugetab_fini(&tg->huge);
    lj_tg_flags_and_rlx(tg, (uint8_t)~TGF_HUGETAB);
    lj_arena_allocd_sethugetab(&tg->allocd, NULL);
  }
  (void)lj_arena_alloc_transfer(&main_tg->alloc, &tg->alloc);
  lj_arena_allocd_init(&tg->allocd, &tg->alloc, &tg->prng, 0);
  lj_tg_flags_and_rlx(tg, (uint8_t)~TGF_ARENA_INTERNAL);
  return 1;
}

uint32_t lj_tg_reclaim_dead(global_State *g)
{
  TGState *prev, *tg;
  uint32_t reclaimed = 0;
  if (!g || gc2_n_threads_acq(g) != 1 ||
      gc2_hs_pending_acq(g) != 0)
    return 0;
restart:
  prev = NULL;
  tg = gc2_tg_list_acq(g);
  while (tg != NULL) {
    TGState *next = lj_tg_next_acq(tg);
    if (lj_tg_flags_test_acq(tg, TGF_DEAD)) {
      if (!tg_transfer_dead_alloc(g, tg)) {
	prev = tg;  /* Keep owner lookup live until allocator transfer succeeds. */
	tg = next;
	continue;
      }
      if (prev) {
	lj_tg_next_rel(prev, next);
      } else {
	TGState *expect = tg;
	if (!gc2_tg_list_cas(g, &expect, next))
	  goto restart;
      }
      lj_tg_next_rel(tg, NULL);
      reclaimed++;
      tg = next;
      continue;
    }
    prev = tg;
    tg = next;
  }
  return reclaimed;
}

TGState *lj_tg_find_owner(global_State *g, uint32_t owner_tid)
{
  TGState *tg;
  if (!g || owner_tid == 0)
    return NULL;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    if (lj_tg_tid_acq(tg) == owner_tid)
      return tg;
  }
  return g->main_tg && lj_tg_tid_acq(g->main_tg) == owner_tid ?
	 g->main_tg : NULL;
}

void lj_tg_sync_dispatch_tg(global_State *g, TGState *tg)
{
  if (g && tg)
    memcpy(tg->dispatch, G2GG(g)->dispatch, sizeof(tg->dispatch));
}

void lj_tg_sync_dispatch(global_State *g)
{
  lj_tg_sync_dispatch_tg(g, G2TG(g));
}
