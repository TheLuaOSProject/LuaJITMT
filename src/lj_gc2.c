/*
** Concurrent GC scaffold.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_gc2_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_arena.h"
#include "lj_tg.h"

static void gc2_attach_main(global_State *g)
{
  TGState *tg = G2TG(g);
  g->gc2.tg_list = tg;
  g->gc2.n_threads = tg ? 1u : 0u;
  if (tg)
    tg->next_tg = NULL;
}

void lj_gc2_init(global_State *g)
{
  g->gc2.phase = LJ_GC2_IDLE;
  g->gc2.cycle = 0;
  g->gc2.marks_this_round = 0;
  gc2_attach_main(g);
}

static void gc2_clear_marks(global_State *g, TGState *tg)
{
  if (tg && (tg->tg_flags & TGF_ARENA_INTERNAL)) {
    lj_arena_alloc_clear_marks(&tg->alloc);
    if (tg->tg_flags & TGF_HUGETAB)
      lj_arena_hugetab_clear_marks(&tg->huge);
  }
}

void lj_gc2_legacy_mark_begin(global_State *g)
{
  TGState *tg = G2TG(g);
  if (g->gc2.tg_list == NULL && tg != NULL)
    gc2_attach_main(g);
  g->gc2.phase = LJ_GC2_MARK;
  g->gc2.cycle++;
  g->gc2.marks_this_round = 0;
  gc2_clear_marks(g, tg);
  if (tg && (tg->tg_flags & TGF_ARENA_INTERNAL)) {
    tg->alloc.alloc_black = 1;
    tg->mark_active = 1;  /* 05 section 5.3: per-TG boolean mirror. */
  }
}

void lj_gc2_legacy_sweep_begin(global_State *g)
{
  TGState *tg = G2TG(g);
  g->gc2.phase = LJ_GC2_SWEEP;
  if (tg && (tg->tg_flags & TGF_ARENA_INTERNAL))
    tg->mark_active = 0;  /* 05 section 5.3: sweep uses g->gc2.phase. */
}

void lj_gc2_legacy_cycle_end(global_State *g)
{
  TGState *tg = G2TG(g);
  g->gc2.phase = LJ_GC2_IDLE;
  if (tg && (tg->tg_flags & TGF_ARENA_INTERNAL)) {
    tg->alloc.alloc_black = 0;
    tg->mark_active = 0;
  }
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
  TGState *tg = G2TG(g);
  GCArena *a;
  uint32_t cell;
  int marked;
  if (!p || !tg || !(tg->tg_flags & TGF_ARENA_INTERNAL))
    return 0;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    if (!(tg->tg_flags & TGF_HUGETAB))
      return 0;
    marked = lj_arena_hugetab_mark(&tg->huge, p, NULL);
    if (marked == 1)
      g->gc2.marks_this_round++;
    return marked == 1;
  }
  cell = lj_arena_cellof(p);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
      !lj_arena_bm_get(a->block, cell))
    return 0;
  marked = !la_bit_test_and_set64(&a->mark[cell >> 6],
				  cell & 63);  /* 05 section 5.6.1. */
  if (marked)
    g->gc2.marks_this_round++;
  return marked;
}

int lj_gc2_markobj(global_State *g, GCobj *o)
{
  return lj_gc2_markmem(g, gc2_mark_base(o));
}

int lj_gc2_ismarked(global_State *g, GCobj *o)
{
  TGState *tg = G2TG(g);
  void *p;
  GCArena *a;
  uint32_t cell;
  if (!o || !tg || !(tg->tg_flags & TGF_ARENA_INTERNAL))
    return -1;
  p = gc2_mark_base(o);
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    LJHugeInfo hi;
    if (!(tg->tg_flags & TGF_HUGETAB) ||
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
