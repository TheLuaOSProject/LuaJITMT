/*
** Concurrent GC scaffold.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_gc2_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_gc.h"
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

void lj_gc2_legacy_preserve_abort(global_State *g)
{
  TGState *tg = G2TG(g);
  g->gc2.phase = LJ_GC2_IDLE;
  if (tg && (tg->tg_flags & TGF_ARENA_INTERNAL)) {
    tg->alloc.alloc_black = 0;
    tg->mark_active = 0;
  }
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
  return o ? lj_gc2_markmem(g, gc2_mark_base(o)) : 0;
}

int lj_gc2_ismarkedmem(global_State *g, void *p)
{
  TGState *tg = G2TG(g);
  GCArena *a;
  uint32_t cell;
  if (!p || !tg || !(tg->tg_flags & TGF_ARENA_INTERNAL))
    return -1;
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
  for (o = gcref(g->gc.root); o != NULL; o = gcnext(o)) {
    if (gc2_legacy_liveobj(o) && gc2_mark_base(o) == p)
      return 1;
    if (o->gch.gct == ~LJ_TTHREAD) {
      GCobj *uv;
      for (uv = gcref(gco2th(o)->openupval); uv != NULL; uv = gcnext(uv))
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
  TGState *tg = G2TG(g);
  GCArena *a;
  uint32_t bad = 0;
  if (!tg || !(tg->tg_flags & TGF_ARENA_INTERNAL))
    return 0;
  for (a = tg->alloc.owned[LJ_ARENAK_TRAVERSABLE]; a != NULL; a = a->hdr.next)
    bad += gc2_paranoia_scan_arena(g, a);
  for (a = tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE]; a != NULL;
       a = a->hdr.next)
    bad += gc2_paranoia_scan_arena(g, a);
  return bad;
}

#endif
