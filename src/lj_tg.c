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
  tg->ssb_node[0].owner = tg;
  tg->ssb_node[0].next = NULL;
  tg->ssb_node[0].n = 0;
  tg->ssb_node[1].owner = tg;
  tg->ssb_node[1].next = NULL;
  tg->ssb_node[1].n = 0;
  tg->ssb_active = &tg->ssb_node[0];
  tg->ssb_free = &tg->ssb_node[1];
  tg->ssb_base = tg->ssb_node[0].slot;
  tg->ssb_next = tg->ssb_base;
  tg->ssb_end = tg->ssb_base + TG_GC2_SSB_SLOTS;
}

static void tg_init_common(global_State *g, TGState *tg, lua_State *L)
{
  tg->gl = g;
  tg->cur_L = L;
  tg->thread_L = L;
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
  g->main_tg = tg;
  tg->tid = lj_thr_newid();
  tg->alloc.owner_tid = tg->tid;
  L->tg_hint = tg;
  L->thr_owner = tg->tid;
  lj_thr_set_tg(tg);  /* 03 section 3.2: bootstrap main OS-thread TLS. */
  if (!alloc_ready)
    lj_arena_alloc_init(&tg->alloc);
  else
    tg->tg_flags |= TGF_ARENA_INTERNAL;
  lj_arena_allocd_init(&tg->allocd, &tg->alloc, &tg->prng, 0);
  if (alloc_ready && lj_arena_hugetab_init(&tg->huge, TG_HUGETAB_BITS)) {
    tg->tg_flags |= TGF_HUGETAB;
    lj_arena_allocd_sethugetab(&tg->allocd, &tg->huge);
  }
  tg_init_common(g, tg, L);
}

void lj_tg_fini(global_State *g)
{
  if (g->main_tg) {
    lj_buf_free(g, &g->main_tg->tmpbuf);
    if (g->main_tg->tg_flags & TGF_HUGETAB)
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
    tg->tg_flags |= TGF_ARENA_INTERNAL;
  lj_arena_allocd_init(&tg->allocd, &tg->alloc, &tg->prng, 0);
  if (arena_internal && lj_arena_hugetab_init(&tg->huge, TG_HUGETAB_BITS)) {
    tg->tg_flags |= TGF_HUGETAB;
    lj_arena_allocd_sethugetab(&tg->allocd, &tg->huge);
  }
  tg_init_common(g, tg, L);
}

void lj_tg_fini_thread(global_State *g, TGState *tg)
{
  if (!tg)
    return;
  lj_buf_free(g, &tg->tmpbuf);
  if (tg->tg_flags & TGF_HUGETAB)
    lj_arena_hugetab_fini(&tg->huge);
  lj_arena_alloc_fini(&tg->alloc);
}

static void tg_adopt_gc2_phase(global_State *g, TGState *tg)
{
  uint32_t phase = la_load32_acq(&g->gc2.phase);
  if (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK) {
    tg->mark_active = 1;
    tg->alloc.alloc_black = 1;
  } else if (phase == LJ_GC2_SWEEP) {
    tg->mark_active = 0;
    tg->alloc.alloc_black =
      la_load32_acq(&g->gc2.cycle_sweep_minor) == 0;
  } else {
    tg->mark_active = la_load32_acq(&g->gc2.generational) != 0;
    tg->alloc.alloc_black = 0;
  }
}

static void tg_attach_catchup(global_State *g, TGState *tg)
{
  uint64_t epoch = la_load64_acq(&g->gc2.hs_epoch);
  uint32_t pending = la_load32_acq(&g->gc2.hs_pending);
  uint32_t actions = pending ? la_load32_acq(&g->gc2.hs_actions) : 0;
  tg->hs_epoch_ack = epoch;
  if (actions) {
    lj_safepoint_apply_tg(g, tg, actions);
    la_store32_rel(&tg->reqmask, 0);
    la_store32_rel(&tg->poll, 0);
    la_store64_rel(&tg->hs_epoch_ack, epoch);  /* 09 section 9.3 self-ack. */
  }
}

void lj_tg_attach(global_State *g, TGState *tg)
{
  void *head;
  if (!g || !tg)
    return;
  tg->poll = 0;
  tg->reqmask = 0;
  tg_adopt_gc2_phase(g, tg);  /* 09 section 9.3 attach catch-up scaffold. */
  tg_attach_catchup(g, tg);
  tg->tg_flags &= (uint8_t)~TGF_DEAD;
  do {
    head = la_loadptr_acq((void *const *)&g->gc2.tg_list);  /* 05 section 5.4.1. */
    tg->next_tg = (TGState *)head;
  } while (!la_casptr((void **)&g->gc2.tg_list, &head, tg,
		      LA_ACQ_REL, LA_ACQ));  /* 05 section 5.4.1 CAS-prepend. */
  la_add32_rlx(&g->gc2.n_threads, 1);  /* Live TG count; list keeps dead nodes. */
}

void lj_tg_detach(global_State *g, TGState *tg)
{
  uint8_t oldflags;
  if (!g || !tg)
    return;
  if (tg->thread_L &&
      (la_load32_acq(&tg->reqmask) != 0 || la_load32_acq(&tg->poll) != 0))
    (void)lj_safepoint_ack(tg->thread_L);  /* Leaving TG owns its ack. */
  (void)lj_gc2_flush_ssb(g, tg);  /* 09 section 9.3 detach publishes SSB. */
  (void)lj_gc2_flush_alloc(g, tg);  /* 04 section 4.8 detach accounting. */
  la_fence_rel();
  oldflags = la_or8_rlx(&tg->tg_flags, TGF_DEAD);  /* 05 section 5.4.1. */
  if (!(oldflags & TGF_DEAD))
    (void)la_sub32_acqrel(&g->gc2.n_threads, 1);
  la_store32_rel(&tg->reqmask, 0);
  la_store32_rel(&tg->poll, 0);
  la_store8_rlx(&tg->in_native, 0);
#if LJ_HASFFI
  tg->ffi_call_func = NULL;
#endif
}

static int tg_transfer_dead_alloc(global_State *g, TGState *tg)
{
  TGState *main_tg = g ? g->main_tg : NULL;
  if (!(tg->tg_flags & TGF_ARENA_INTERNAL))
    return 1;
  if (!main_tg || !(main_tg->tg_flags & TGF_ARENA_INTERNAL))
    return 0;
  lj_buf_free(g, &tg->tmpbuf);  /* Route through the still-findable owner. */
  lj_buf_init(NULL, &tg->tmpbuf);
  if (tg->tg_flags & TGF_HUGETAB) {
    if (!(main_tg->tg_flags & TGF_HUGETAB) ||
	!lj_arena_hugetab_transfer(&main_tg->huge, &tg->huge,
				   main_tg->alloc.owner_tid))
      return 0;
    lj_arena_hugetab_fini(&tg->huge);
    tg->tg_flags &= (uint8_t)~TGF_HUGETAB;
    lj_arena_allocd_sethugetab(&tg->allocd, NULL);
  }
  (void)lj_arena_alloc_transfer(&main_tg->alloc, &tg->alloc);
  lj_arena_allocd_init(&tg->allocd, &tg->alloc, &tg->prng, 0);
  tg->tg_flags &= (uint8_t)~TGF_ARENA_INTERNAL;
  return 1;
}

uint32_t lj_tg_reclaim_dead(global_State *g)
{
  TGState *prev, *tg;
  uint32_t reclaimed = 0;
  if (!g || la_load32_acq(&g->gc2.n_threads) != 1 ||
      la_load32_acq(&g->gc2.hs_pending) != 0)
    return 0;
restart:
  prev = NULL;
  tg = (TGState *)la_loadptr_acq((void *const *)&g->gc2.tg_list);
  while (tg != NULL) {
    TGState *next = (TGState *)la_loadptr_acq((void *const *)&tg->next_tg);
    if (la_load8_acq(&tg->tg_flags) & TGF_DEAD) {
      if (!tg_transfer_dead_alloc(g, tg)) {
	prev = tg;  /* Keep owner lookup live until allocator transfer succeeds. */
	tg = next;
	continue;
      }
      if (prev) {
	la_storeptr_rel((void **)&prev->next_tg, next);
      } else {
	void *expect = tg;
	if (!la_casptr((void **)&g->gc2.tg_list, &expect, next,
		       LA_ACQ_REL, LA_ACQ))
	  goto restart;
      }
      la_storeptr_rel((void **)&tg->next_tg, NULL);
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
  for (tg = (TGState *)la_loadptr_acq((void *const *)&g->gc2.tg_list);
       tg != NULL;
       tg = (TGState *)la_loadptr_acq((void *const *)&tg->next_tg)) {
    if (la_load32_acq(&tg->tid) == owner_tid)
      return tg;
  }
  return g->main_tg && la_load32_acq(&g->main_tg->tid) == owner_tid ?
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
