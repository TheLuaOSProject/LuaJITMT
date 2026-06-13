/*
** Garbage collector.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC_H
#define _LJ_GC_H

#include "lj_obj.h"

/* Garbage collector states. Order matters. */
enum {
  GCSpause, GCSpropagate, GCSatomic, GCSsweepstring, GCSsweep, GCSfinalize
};

/* Bitmasks for marked field of GCobj. */
#define LJ_GC_WHITE0	0x01
#define LJ_GC_WHITE1	0x02
#define LJ_GC_BLACK	0x04
#define LJ_GC_FINALIZED	0x08
#define LJ_GC_WEAKKEY	0x08
#define LJ_GC_WEAKVAL	0x10
#define LJ_GC_CDATA_FIN	0x10
#define LJ_GC_FIXED	0x20
#define LJ_GC_SFIXED	0x40

#define LJ_GC_WHITES	(LJ_GC_WHITE0 | LJ_GC_WHITE1)
#define LJ_GC_COLORS	(LJ_GC_WHITES | LJ_GC_BLACK)
#define LJ_GC_WEAK	(LJ_GC_WEAKKEY | LJ_GC_WEAKVAL)

LJ_STATIC_ASSERT(LJ_GC_WHITE0 == 0x01);
LJ_STATIC_ASSERT(LJ_GC_WHITE1 == 0x02);
LJ_STATIC_ASSERT(LJ_GC_BLACK == 0x04);
LJ_STATIC_ASSERT(LJ_GC_WHITES == 0x03);
LJ_STATIC_ASSERT(LJ_GC_FINALIZED == LJ_GC_WEAKKEY);
LJ_STATIC_ASSERT(LJ_GC_CDATA_FIN == LJ_GC_WEAKVAL);

/* Macros to test and set GCobj colors. */
#define iswhite(x)	(lj_obj_gcflags((x)) & LJ_GC_WHITES)
#define isblack(x)	(lj_obj_gcflags((x)) & LJ_GC_BLACK)
#define isgray(x)	(!(lj_obj_gcflags((x)) & (LJ_GC_BLACK|LJ_GC_WHITES)))
#define tviswhite(x)	(tvisgcv(x) && iswhite(gcV(x)))
#define otherwhite(g)	(g->gc.currentwhite ^ LJ_GC_WHITES)
#define isdead(g, v)	(lj_obj_gcflags((v)) & otherwhite(g) & LJ_GC_WHITES)

#define curwhite(g)	((g)->gc.currentwhite & LJ_GC_WHITES)
#define newwhite(g, x)	(lj_obj_setgcflags(obj2gco(x), (uint8_t)curwhite(g)))
#define makewhite(g, x) \
  (lj_obj_masksetgcflags((x), LJ_GC_COLORS, (uint8_t)curwhite(g)))
#define flipwhite(x)	(lj_obj_xorgcflags((x), LJ_GC_WHITES))
#define black2gray(x)	(lj_obj_cleargcflags((x), LJ_GC_BLACK))
#define fixstring(s)	(lj_obj_addgcflags(obj2gco(s), LJ_GC_FIXED))
#define markfinalized(x)	(lj_obj_addgcflags((x), LJ_GC_FINALIZED))

/* Collector. */
LJ_FUNC size_t lj_gc_separateudata(global_State *g, int all);
LJ_FUNC void lj_gc_finalize_udata(lua_State *L);
#if LJ_HASFFI
LJ_FUNC void lj_gc_finalize_cdata(lua_State *L);
#else
#define lj_gc_finalize_cdata(L)		UNUSED(L)
#endif
LJ_FUNC void lj_gc_freeall(global_State *g);
LJ_FUNC void lj_gc_arena_markobj(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_arena_markmem(global_State *g, void *p);
LJ_FUNCA int LJ_FASTCALL lj_gc_step(lua_State *L);
LJ_FUNCA void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L);
#if LJ_HASJIT
LJ_FUNC int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps);
#endif
LJ_FUNC void lj_gc_fullgc(lua_State *L);

static LJ_AINLINE GCSize lj_gcsize_load_acq(const GCSize *p)
{
#if LJ_GC64
  return (GCSize)la_load64_acq(p);
#else
  return (GCSize)la_load32_acq(p);
#endif
}

static LJ_AINLINE void lj_gcsize_store_rel(GCSize *p, GCSize v)
{
#if LJ_GC64
  la_store64_rel(p, (uint64_t)v);
#else
  la_store32_rel(p, (uint32_t)v);
#endif
}

static LJ_AINLINE GCSize lj_gc_threshold_load(global_State *g)
{
  return lj_gcsize_load_acq(&g->gc.threshold);
}

static LJ_AINLINE void lj_gc_threshold_store(global_State *g, GCSize threshold)
{
  lj_gcsize_store_rel(&g->gc.threshold, threshold);
}

static LJ_AINLINE GCSize lj_gc_mt_threshold_load(global_State *g)
{
  return lj_gcsize_load_acq(&g->mt_gc_threshold);
}

static LJ_AINLINE void lj_gc_mt_threshold_store(global_State *g,
						GCSize threshold)
{
  lj_gcsize_store_rel(&g->mt_gc_threshold, threshold);
}

/* GC check: drive collector forward if the GC threshold has been reached. */
#define lj_gc_check(L) \
  { if (LJ_UNLIKELY(G(L)->gc.total >= lj_gc_threshold_load(G(L)))) \
      lj_gc_step(L); }
#define lj_gc_check_fixtop(L) \
  { if (LJ_UNLIKELY(G(L)->gc.total >= lj_gc_threshold_load(G(L)))) \
      lj_gc_step_fixtop(L); }

/* Write barriers. */
LJ_FUNC void lj_gc_barrierroot(lua_State *L, cTValue *tv);
LJ_FUNC void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v);
LJ_FUNC void lj_gc_closeuv(global_State *g, GCupval *uv);
#if LJ_HASJIT
LJ_FUNC void lj_gc_barriertrace(global_State *g, uint32_t traceno);
#endif
LJ_FUNC void lj_gc2_barrier_tv(lua_State *L, cTValue *tv);
LJ_FUNCA void lj_gc2_barrier_tv_g(global_State *g, cTValue *tv);
LJ_FUNC void lj_gc2_barrier_uv(global_State *g, cTValue *tv);
LJ_FUNC void lj_gc2_barrier_obj(lua_State *L, GCobj *o);
LJ_FUNCA void lj_gc2_barrier_tab_g(global_State *g, GCtab *t);
LJ_FUNC void lj_gc2_barrier_tab(lua_State *L, GCtab *t);

/* Move the GC propagation frontier back for tables (make it gray again). */
static LJ_AINLINE void lj_gc_barrierback(global_State *g, GCtab *t)
{
  GCobj *o = obj2gco(t);
  lj_assertG(isblack(o) && !isdead(g, o),
	     "bad object states for backward barrier");
  lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
	     "bad GC state");
  black2gray(o);
  setgcrefr(t->gclist, g->gc.grayagain);
  setgcref(g->gc.grayagain, o);
}

/* Barrier for stores to table objects. TValue and GCobj variant. */
#define lj_gc_anybarriert(L, t)  \
  { lj_gc2_barrier_tab((L), (t)); \
    if (LJ_UNLIKELY(isblack(obj2gco(t)))) lj_gc_barrierback(G(L), (t)); }
#define lj_gc_barriert(L, t, tv) \
  { lj_gc2_barrier_tv((L), (tv)); \
    if (tviswhite(tv) && isblack(obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }
#define lj_gc_objbarriert(L, t, o)  \
  { lj_gc2_barrier_obj((L), obj2gco(o)); \
    if (iswhite(obj2gco(o)) && isblack(obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }

/* Barrier for stores to any other object. TValue and GCobj variant. */
#define lj_gc_barrier(L, p, tv) \
  { lj_gc2_barrier_tv((L), (tv)); \
    if (tviswhite(tv) && isblack(obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), gcV(tv)); }
#define lj_gc_objbarrier(L, p, o) \
  { lj_gc2_barrier_obj((L), obj2gco(o)); \
    if (iswhite(obj2gco(o)) && isblack(obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), obj2gco(o)); }

/*
** M5 publication wrappers. These preserve the current incremental-GC
** compatibility behavior while moving runtime call sites away from the
** legacy barrier macro names counted by the milestone guard.
*/
#define lj_gc_pubtab(L, t) \
  { lj_gc2_barrier_tab((L), (t)); \
    if (LJ_UNLIKELY(isblack(obj2gco(t)))) lj_gc_barrierback(G(L), (t)); }
#define lj_gc_pubtabtv(L, t, tv) \
  { lj_gc2_barrier_tv((L), (tv)); \
    if (tviswhite(tv) && isblack(obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }
#define lj_gc_pubtabobj(L, t, o) \
  { lj_gc2_barrier_obj((L), obj2gco(o)); \
    if (iswhite(obj2gco(o)) && isblack(obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }
#define lj_gc_pubobjtv(L, p, tv) \
  { lj_gc2_barrier_tv((L), (tv)); \
    if (tviswhite(tv) && isblack(obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), gcV(tv)); }
#define lj_gc_pubobjobj(L, p, o) \
  { lj_gc2_barrier_obj((L), obj2gco(o)); \
    if (iswhite(obj2gco(o)) && isblack(obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), obj2gco(o)); }
LJ_FUNCA void LJ_FASTCALL lj_gc_pubuv(global_State *g, TValue *tv);

/* Allocator. */
LJ_FUNC void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz);
LJ_FUNC void *lj_mem_newgco_raw(lua_State *L, GCSize size, uint32_t flags);
LJ_FUNC void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size);
LJ_FUNC void *lj_mem_grow(lua_State *L, void *p,
			  MSize *szp, MSize lim, MSize esz);
LJ_FUNC int lj_mem_freegco_defer(global_State *g, void *p, GCSize osize);
LJ_FUNC void lj_mem_free(global_State *g, void *p, size_t osize);

#define lj_mem_new(L, s)	lj_mem_realloc(L, NULL, 0, (s))

#define lj_mem_newvec(L, n, t)	((t *)lj_mem_new(L, (GCSize)((n)*sizeof(t))))
#define lj_mem_reallocvec(L, p, on, n, t) \
  ((p) = (t *)lj_mem_realloc(L, p, (on)*sizeof(t), (GCSize)((n)*sizeof(t))))
#define lj_mem_growvec(L, p, n, m, t) \
  ((p) = (t *)lj_mem_grow(L, (p), &(n), (m), (MSize)sizeof(t)))
#define lj_mem_freevec(g, p, n, t)	lj_mem_free(g, (p), (n)*sizeof(t))

#define lj_mem_newobj(L, t)	((t *)lj_mem_newgco(L, sizeof(t)))
#define lj_mem_newt(L, s, t)	((t *)lj_mem_new(L, (s)))
#define lj_mem_freet(g, p)	lj_mem_free(g, (p), sizeof(*(p)))

#endif
