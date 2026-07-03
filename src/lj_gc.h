/*
** Garbage collector.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC_H
#define _LJ_GC_H

#include "lj_obj.h"

typedef struct GCArena GCArena;

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
#define LJ_GC_UDATA_FINREG	0x10
#define LJ_GC_FIXED	0x20
#define LJ_GC_SFIXED	0x40
#define LJ_GC_NEEDSCAN	0x80

#define LJ_GC_WHITES	(LJ_GC_WHITE0 | LJ_GC_WHITE1)
#define LJ_GC_COLORS	(LJ_GC_WHITES | LJ_GC_BLACK)
#define LJ_GC_WEAK	(LJ_GC_WEAKKEY | LJ_GC_WEAKVAL)

LJ_STATIC_ASSERT(LJ_GC_WHITE0 == 0x01);
LJ_STATIC_ASSERT(LJ_GC_WHITE1 == 0x02);
LJ_STATIC_ASSERT(LJ_GC_BLACK == 0x04);
LJ_STATIC_ASSERT(LJ_GC_WHITES == 0x03);
LJ_STATIC_ASSERT(LJ_GC_FINALIZED == LJ_GC_WEAKKEY);
LJ_STATIC_ASSERT(LJ_GC_CDATA_FIN == LJ_GC_WEAKVAL);
LJ_STATIC_ASSERT(LJ_GC_UDATA_FINREG == LJ_GC_WEAKVAL);
LJ_STATIC_ASSERT(LJ_GC_NEEDSCAN == 0x80);

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
LJ_FUNC uint32_t lj_gc_sweep_gc2_unmarked(global_State *g);
LJ_FUNC uint32_t lj_gc_sweep_gc2_arena_unmarked(global_State *g, GCArena *a);
LJ_FUNC uint32_t lj_gc_sweep_gc2_all_arena_bodies(global_State *g);
LJ_FUNC void lj_gc_unlink_root_obj(global_State *g, GCobj *dead);
LJ_FUNC void lj_gc_freeall(global_State *g);
LJ_FUNC void lj_gc_clearweak_bridge(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_preserveobj_legacy(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_arena_markobj(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_arena_markmem(global_State *g, void *p);
#if LJ_HASJIT
LJ_FUNC void lj_gc_mark_trace_slot(global_State *g, uint32_t traceno);
#endif
LJ_FUNC void lj_gc_linkobj(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_linkobj_new(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_linkobj_new_after_main(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_linkobj_after(GCobj *anchor, GCobj *o);
LJ_FUNC uint32_t lj_gc_flush_root_pending(global_State *g);
LJ_FUNC void *lj_mem_newgco_unlinked(lua_State *L, GCSize size);
LJ_FUNCA int LJ_FASTCALL lj_gc_step(lua_State *L);
LJ_FUNC int lj_gc_step_explicit(lua_State *L);
LJ_FUNCA void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L);
LJ_FUNCA void LJ_FASTCALL lj_gc_step_top(lua_State *L);
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

static LJ_AINLINE GCSize lj_gc_total_load(global_State *g)
{
  return lj_gcsize_load_acq(&g->gc.total);  /* 04 section 4.8 accounting. */
}

static LJ_AINLINE void lj_gc_total_store(global_State *g, GCSize total)
{
  lj_gcsize_store_rel(&g->gc.total, total);  /* 04 section 4.8 accounting. */
}

static LJ_AINLINE void lj_gc_total_add(global_State *g, GCSize bytes)
{
#if LJ_GC64
  (void)la_add64_rlx(&g->gc.total, bytes);
#else
  (void)la_add32_rlx(&g->gc.total, (uint32_t)bytes);
#endif
  /* 04 section 4.8 accounting: atomicity matters, ordering is counter-only. */
}

static LJ_AINLINE void lj_gc_total_sub(global_State *g, GCSize bytes)
{
#if LJ_GC64
  (void)la_sub64_rlx(&g->gc.total, bytes);
#else
  (void)la_sub32_rlx(&g->gc.total, (uint32_t)bytes);
#endif
  /* 04 section 4.8 accounting: atomicity matters, ordering is counter-only. */
}

static LJ_AINLINE void lj_gc_total_adjust(global_State *g, GCSize osz,
					  GCSize nsz)
{
  if (nsz >= osz)
    lj_gc_total_add(g, nsz - osz);
  else
    lj_gc_total_sub(g, osz - nsz);
}

static LJ_AINLINE GCSize lj_gc_threshold_load(global_State *g)
{
  return lj_gcsize_load_acq(&g->gc.threshold);
}

static LJ_AINLINE void lj_gc_threshold_store(global_State *g, GCSize threshold)
{
  lj_gcsize_store_rel(&g->gc.threshold, threshold);
}

static LJ_AINLINE MSize lj_gc_pause_load(global_State *g)
{
  return (MSize)la_load32_acq(&g->gc.pause);
}

static LJ_AINLINE void lj_gc_pause_store(global_State *g, MSize pause)
{
  la_store32_rel(&g->gc.pause, (uint32_t)pause);
}

static LJ_AINLINE MSize lj_gc_pause_xchg(global_State *g, MSize pause)
{
  return (MSize)la_xchg32_acqrel(&g->gc.pause, (uint32_t)pause);
}

static LJ_AINLINE MSize lj_gc_stepmul_load(global_State *g)
{
  return (MSize)la_load32_acq(&g->gc.stepmul);
}

static LJ_AINLINE void lj_gc_stepmul_store(global_State *g, MSize stepmul)
{
  la_store32_rel(&g->gc.stepmul, (uint32_t)stepmul);
}

static LJ_AINLINE MSize lj_gc_stepmul_xchg(global_State *g, MSize stepmul)
{
  return (MSize)la_xchg32_acqrel(&g->gc.stepmul, (uint32_t)stepmul);
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

static LJ_AINLINE uint64_t lj_gc2_alloc_since_load(global_State *g)
{
  return la_load64_acq(&g->gc2.alloc_since_trigger);
}

static LJ_AINLINE void lj_gc2_alloc_since_store(global_State *g,
						uint64_t bytes)
{
  la_store64_rel(&g->gc2.alloc_since_trigger, bytes);
}

static LJ_AINLINE uint64_t lj_gc2_alloc_since_add(global_State *g,
						  uint64_t bytes)
{
  return la_add64_rlx(&g->gc2.alloc_since_trigger, bytes);
}

static LJ_AINLINE uint64_t lj_gc2_alloc_since_xchg(global_State *g,
						   uint64_t bytes)
{
  return la_xchg64_acqrel(&g->gc2.alloc_since_trigger, bytes);
}

static LJ_AINLINE uint64_t lj_gc2_cycle_alloc_load(global_State *g)
{
  return la_load64_acq(&g->gc2.cycle_alloc_bytes);
}

static LJ_AINLINE void lj_gc2_cycle_alloc_store(global_State *g,
						uint64_t bytes)
{
  la_store64_rel(&g->gc2.cycle_alloc_bytes, bytes);
}

static LJ_AINLINE uint64_t lj_gc2_trigger_load(global_State *g)
{
  return la_load64_acq(&g->gc2.trigger_bytes);
}

static LJ_AINLINE void lj_gc2_trigger_store(global_State *g, uint64_t bytes)
{
  la_store64_rel(&g->gc2.trigger_bytes, bytes);
}

static LJ_AINLINE uint64_t lj_gc2_hard_load(global_State *g)
{
  return la_load64_acq(&g->gc2.hard_bytes);
}

static LJ_AINLINE void lj_gc2_hard_store(global_State *g, uint64_t bytes)
{
  la_store64_rel(&g->gc2.hard_bytes, bytes);
  la_store64_rel(&g->gc2.hard_check_bytes, bytes);
}

static LJ_AINLINE uint64_t lj_gc2_hard_check_load(global_State *g)
{
  return la_load64_acq(&g->gc2.hard_check_bytes);
}

static LJ_AINLINE void lj_gc2_hard_check_store(global_State *g,
					       uint64_t bytes)
{
  la_store64_rel(&g->gc2.hard_check_bytes, bytes);
}

static LJ_AINLINE uint64_t lj_gc2_helper_soft_limit_load(global_State *g)
{
  return la_load64_acq(&g->gc2.helper_soft_limit);
}

static LJ_AINLINE void lj_gc2_helper_soft_limit_store(global_State *g,
						      uint64_t bytes)
{
  la_store64_rel(&g->gc2.helper_soft_limit, bytes);
}

static LJ_AINLINE int lj_gc2_hard_limit_reached(global_State *g)
{
  return lj_gc2_alloc_since_load(g) > lj_gc2_hard_load(g);
}

static LJ_AINLINE int lj_gc_should_step(global_State *g)
{
  return lj_gc_total_load(g) >= lj_gc_threshold_load(g) ||
	 lj_gc2_hard_limit_reached(g);
}

static LJ_AINLINE GCobj *lj_gc_list_head_acq(const GCRef *head)
{
  return gcref_acq(*head);
}

static LJ_AINLINE void lj_gc_list_clear_rel(GCRef *head)
{
  setgcrefnullrel(*head);
}

static LJ_AINLINE void lj_gc_list_push_rel(GCRef *head, GCobj *o)
{
  GCobj *next = lj_gc_list_head_acq(head);
  if (next)
    setgcrefrel(o->gch.gclist, next);
  else
    setgcrefnullrel(o->gch.gclist);
  setgcrefrel(*head, o);
}

static LJ_AINLINE void lj_gc_list_pop_head_rel(GCRef *head, GCobj *o)
{
  GCobj *next = gcref_acq(o->gch.gclist);
  if (next)
    setgcrefrel(*head, next);
  else
    setgcrefnullrel(*head);
}

static LJ_AINLINE void lj_gc_list_move_rel(GCRef *dst, GCRef *src)
{
  GCobj *head = lj_gc_list_head_acq(src);
  if (head)
    setgcrefrel(*dst, head);
  else
    setgcrefnullrel(*dst);
  lj_gc_list_clear_rel(src);
}

/* GC check: drive collector forward if classic GC or GC2 pacing asks for work. */
#define lj_gc_check(L) \
  { if (LJ_UNLIKELY(lj_gc_should_step(G(L)))) \
      lj_gc_step_top(L); }
#define lj_gc_check_fixtop(L) \
  { if (LJ_UNLIKELY(lj_gc_should_step(G(L)))) \
      lj_gc_step_fixtop(L); }

/* Write barriers. */
LJ_FUNC void lj_gc_pubroot(lua_State *L, cTValue *tv);
LJ_FUNC void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v);
LJ_FUNC void lj_gc_closeuv(global_State *g, GCupval *uv);
#if LJ_HASJIT
LJ_FUNC void lj_gc_pubtrace(global_State *g, uint32_t traceno);
#endif
LJ_FUNCA void lj_gc2_barrier_tv_g(global_State *g, cTValue *tv);
LJ_FUNCA void lj_gc2_barrier_tvn_pair_g(global_State *g, GCobj *parent,
					cTValue *tv, uint32_t n);
LJ_FUNCA void lj_gc2_barrier_obj_pair(lua_State *L, GCobj *parent,
				      GCobj *child);
LJ_FUNCA void lj_gc2_barrier_tv_pair_g(global_State *g, GCobj *parent,
				       cTValue *tv);
LJ_FUNC void lj_gc2_barrier_tv_pair(lua_State *L, GCobj *parent, cTValue *tv);
LJ_FUNCA void lj_gc2_barrier_tab_g(global_State *g, GCtab *t);
LJ_FUNCA void lj_gc2_barrier_key_g(global_State *g, GCtab *t, cTValue *key);
LJ_FUNC void lj_gc2_barrier_tab(lua_State *L, GCtab *t);
LJ_FUNC void lj_gc2_barrier_weak_key(lua_State *L, GCtab *t, cTValue *key);
LJ_FUNC void lj_gc2_barrier_weak_value(lua_State *L, GCtab *t, cTValue *val);
LJ_FUNC void lj_gc2_barrier_weak_write(lua_State *L, GCtab *t, cTValue *key,
				       cTValue *val);
LJ_FUNC int lj_gc2_weak_write_begin(lua_State *L, GCtab *t);
LJ_FUNC void lj_gc2_weak_write_end(lua_State *L, int active);
LJ_FUNCA void lj_gc_barrierback_tab_g(global_State *g, GCtab *t);

/* Move the GC propagation frontier back for tables (make it gray again). */
static LJ_AINLINE void lj_gc_barrierback(global_State *g, GCtab *t)
{
  GCobj *o = obj2gco(t);
  lj_assertG(isblack(o) && !isdead(g, o),
	     "bad object states for backward barrier");
  lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
	     "bad GC state");
  black2gray(o);
  lj_gc_list_push_rel(&g->gc.grayagain, o);
}

static LJ_AINLINE void lj_gc_barriertv_(lua_State *L, GCtab *t, cTValue *tv)
{
  TValue snap;
  if (!tv)
    return;
  lj_tv_load_acq(&snap, tv);
  lj_gc2_barrier_tv_pair(L, obj2gco(t), &snap);
  lj_gc2_barrier_weak_value(L, t, &snap);
  if (tviswhite(&snap) && isblack(obj2gco(t)))
    lj_gc_barrierback(G(L), t);
}

static LJ_AINLINE void lj_gc_barrierobjtv_(lua_State *L, GCobj *p,
					   cTValue *tv)
{
  TValue snap;
  if (!tv)
    return;
  lj_tv_load_acq(&snap, tv);
  lj_gc2_barrier_tv_pair(L, p, &snap);
  if (tviswhite(&snap) && isblack(p))
    lj_gc_barrierf(G(L), p, gcV(&snap));
}

/* Barrier for stores to table objects. TValue and GCobj variant. */
#define lj_gc_anybarriert(L, t)  \
  { lj_gc2_barrier_tab((L), (t)); \
    if (LJ_UNLIKELY(isblack(obj2gco(t)))) lj_gc_barrierback(G(L), (t)); }
#define lj_gc_barriert(L, t, tv) \
  lj_gc_barriertv_((L), (t), (tv))
#define lj_gc_objbarriert(L, t, o)  \
  { lj_gc2_barrier_obj_pair((L), obj2gco(t), obj2gco(o)); \
    if (iswhite(obj2gco(o)) && isblack(obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }

/* Barrier for stores to any other object. TValue and GCobj variant. */
#define lj_gc_barrier(L, p, tv) \
  lj_gc_barrierobjtv_((L), obj2gco(p), (tv))
#define lj_gc_objbarrier(L, p, o) \
  { lj_gc2_barrier_obj_pair((L), obj2gco(p), obj2gco(o)); \
    if (iswhite(obj2gco(o)) && isblack(obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), obj2gco(o)); }

/*
** M5 publication wrappers. These preserve current incremental-GC behavior
** while moving runtime call sites away from raw barrier macro names counted by
** the milestone guard.
*/
#define lj_gc_pubtab(L, t) \
  { lj_gc2_barrier_tab((L), (t)); \
    if (LJ_UNLIKELY(isblack(obj2gco(t)))) lj_gc_barrierback(G(L), (t)); }
#define lj_gc_pubtabtv(L, t, tv) \
  lj_gc_barriertv_((L), (t), (tv))
#define lj_gc_pubtabobj(L, t, o) \
  { lj_gc2_barrier_obj_pair((L), obj2gco(t), obj2gco(o)); \
    if (iswhite(obj2gco(o)) && isblack(obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }
#define lj_gc_pubobjtv(L, p, tv) \
  lj_gc_barrierobjtv_((L), obj2gco(p), (tv))
#define lj_gc_pubobjobj(L, p, o) \
  { lj_gc2_barrier_obj_pair((L), obj2gco(p), obj2gco(o)); \
    if (iswhite(obj2gco(o)) && isblack(obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), obj2gco(o)); }
LJ_FUNCA void lj_gc_pubtabobj_vm(lua_State *L, GCtab *t, GCobj *o);
LJ_FUNCA void lj_gc_pubtabtv_vm(lua_State *L, GCtab *t, cTValue *tv);
LJ_FUNCA void lj_gc_pubtabtvn_vm(lua_State *L, GCtab *t, cTValue *tv,
				 uint32_t n);
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
