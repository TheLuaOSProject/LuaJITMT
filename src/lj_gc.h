/*
** Garbage collector.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC_H
#define _LJ_GC_H

#include "lj_obj.h"

typedef struct GCArena GCArena;
typedef struct LJHugeInfo LJHugeInfo;

/* Garbage collector states. Order matters. */
enum {
  GCSpause, GCSstart, GCSpropagate, GCSatomic, GCSsweepstring, GCSsweep,
  GCSfinalize
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
#define fixstring(g, s)	lj_gc_fixstring((g), (s))
#define markfinalized(x)	(lj_obj_addgcflags((x), LJ_GC_FINALIZED))

static LJ_AINLINE int lj_gc_claim_black_to_gray(GCobj *o)
{
  uint8_t old = la_load8_acq(&o->gch.marked);
  for (;;) {
    uint8_t next;
    if (!(old & LJ_GC_BLACK) || (old & LJ_GC_WHITES))
      return 0;
    next = (uint8_t)(old & (uint8_t)~LJ_GC_BLACK);
    if (la_cas8(&o->gch.marked, &old, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }
}

/* Collector. */
LJ_FUNC void lj_gc_fixstring(global_State *g, GCstr *s);
LJ_FUNC int lj_gc_udata_payload_valid_as(GCudata *ud, uint8_t udtype,
					  GCSize *sizep);
LJ_FUNC int lj_gc_udata_payload_valid(GCudata *ud, GCSize *sizep);
LJ_FUNC uint32_t lj_gc_sweep_gc2_unmarked(global_State *g);
LJ_FUNC uint32_t lj_gc_sweep_gc2_arena_unmarked(global_State *g, GCArena *a);
LJ_FUNC uint32_t lj_gc_reclaim_gc2_arena(global_State *g, GCArena *a,
					 uint32_t limit, int *donep);
LJ_FUNC uint32_t lj_gc_reclaim_gc2_huge(global_State *g, TGState *tg,
					 void *p, const LJHugeInfo *hi,
					 int *pendingp);
/* Result of an ownership-spine unlink attempt. ABSENT is a complete, valid
** spine scan which proved that no unlink was needed; UNPROVEN means an
** inadmissible entry or bounded-cycle guard prevented that proof. */
enum {
  LJ_GC_ROOT_UNLINK_UNPROVEN = -1,
  LJ_GC_ROOT_UNLINK_ABSENT = 0,
  LJ_GC_ROOT_UNLINKED = 1
};
LJ_FUNC int lj_gc_unlink_root_obj(global_State *g, GCobj *dead);
LJ_FUNC void lj_gc_preserve_root_chain_for_gc2_sweep(global_State *g);
LJ_FUNC void lj_gc_clearweak_bridge(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_arena_markobj(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_arena_markmem(global_State *g, void *p);
LJ_FUNC void lj_gc_arena_markmem_registered(global_State *g, void *p);
LJ_FUNC void lj_gc_publishobj_header(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_linkobj(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_linkobj_pending(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_linkobj_new(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_linkobj_new_chain(global_State *g, GCobj *head,
				     GCobj *tail);
LJ_FUNC void lj_gc_linkobj_new_after_main(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_linkobj_after(global_State *g, GCobj *anchor, GCobj *o);
LJ_FUNC uint32_t lj_gc_flush_root_pending(global_State *g);
LJ_FUNC uint32_t lj_gc_repair_root_spine(global_State *g);
LJ_FUNC void *lj_mem_newgco_unlinked(lua_State *L, GCSize size);
LJ_FUNCA int LJ_FASTCALL lj_gc_step(lua_State *L);
LJ_FUNCA void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L);
LJ_FUNCA void LJ_FASTCALL lj_gc_step_top(lua_State *L);
#if LJ_HASJIT
LJ_FUNC int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps);
LJ_FUNC int lj_gc2_jit_needs_exit(global_State *g);
#endif
#ifdef LJ_GC2_TEST_HELPERS
enum {
  LJ_GC_ROOT_PENDING_TEST_ORDINARY = 1,
  LJ_GC_ROOT_PENDING_TEST_CHAIN = 2,
  LJ_GC_ROOT_PENDING_TEST_AFTER_MAIN = 3,
  LJ_GC_ROOT_PENDING_TEST_VM_TNEW = 4
};
typedef void (*LJGcRootPendingLoadHook)(global_State *g, TGState *tg,
					GCobj *published, GCobj *observed,
					uint32_t path);
LJ_FUNC uint32_t lj_gc_test_step_fixtop_calls(void);
LJ_FUNC void lj_gc_test_reset_step_fixtop_calls(void);
LJ_FUNC void lj_gc_test_set_root_pending_load_hook(
  LJGcRootPendingLoadHook hook);
/* Test-build bridge used by the x64 interpreter's inlined pending-root CAS.
** Returning published keeps the VM result register live across the hook call. */
LJ_FUNC GCobj *lj_gc_test_root_pending_loaded_vm(global_State *g, TGState *tg,
						  GCobj *published,
						  GCobj *observed);
#endif

static LJ_AINLINE GCSize lj_gcsize_load_acq(const GCSize *p)
{
  return (GCSize)la_load64_acq(p);
}

static LJ_AINLINE void lj_gcsize_store_rel(GCSize *p, GCSize v)
{
  la_store64_rel(p, (uint64_t)v);
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
  (void)la_add64_rlx(&g->gc.total, bytes);
  /* 04 section 4.8 accounting: atomicity matters, ordering is counter-only. */
}

static LJ_AINLINE void lj_gc_total_sub(global_State *g, GCSize bytes)
{
#if LUA_USE_ASSERT
  GCSize old = (GCSize)la_sub64_rlx(&g->gc.total, bytes);
  lj_assertG(old >= bytes, "gc total underflow old=%llu bytes=%llu caller=%p",
	     (unsigned long long)old, (unsigned long long)bytes,
	     __builtin_return_address(0));
#else
  (void)la_sub64_rlx(&g->gc.total, bytes);
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

static LJ_AINLINE int lj_gc2_hard_check_cas(global_State *g, uint64_t *oldp,
					    uint64_t bytes)
{
  return la_cas64(&g->gc2.hard_check_bytes, oldp, bytes,
		  LA_ACQ_REL, LA_ACQ);
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

/* GC check: drive collector forward if color GC or GC2 pacing asks for work. */
#define lj_gc_check(L) \
  { if (LJ_UNLIKELY(lj_gc_should_step(G(L)))) \
      lj_gc_step_top(L); }
#define lj_gc_check_fixtop(L) \
  { if (LJ_UNLIKELY(lj_gc_should_step(G(L)))) \
      lj_gc_step_fixtop(L); }

/* Write barriers. */
LJ_FUNC void lj_gc_pubroot(lua_State *L, cTValue *tv);
LJ_FUNC void lj_gc_pubobjroot(lua_State *L, GCobj *o);
LJ_FUNC void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v);
LJ_FUNC void lj_gc_closeuv(global_State *g, GCupval *uv);
#if LJ_HASJIT
LJ_FUNC void lj_gc_pubtrace(global_State *g, uint32_t traceno);
#endif
LJ_FUNCA void lj_gc2_barrier_tv_g(global_State *g, cTValue *tv);
LJ_FUNCA void lj_gc2_barrier_tvn_pair_g(global_State *g, GCobj *parent,
					cTValue *tv, uint32_t n);
LJ_FUNCA void lj_gc2_barrier_obj_pair_g(global_State *g, GCobj *parent,
					GCobj *child);
LJ_FUNCA void lj_gc2_barrier_obj_pair(lua_State *L, GCobj *parent,
				      GCobj *child);
LJ_FUNC void lj_gc2_barrier_marked_proto(lua_State *L, GCproto *pt);
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
LJ_FUNC int lj_gc2_weak_write_candidate(lua_State *L, GCtab *t);
LJ_FUNC int lj_gc2_weak_write_begin(lua_State *L, GCtab *t);
LJ_FUNC void lj_gc2_weak_write_end(lua_State *L, int active);
LJ_FUNC int lj_gc_tv_gcref_valid(global_State *g, cTValue *tv);
LJ_FUNC void lj_gc_tbar_trace_g(global_State *g, GCtab *t, cTValue *key);
LJ_FUNCA void lj_gc_barrierback_tab_g(global_State *g, GCtab *t);

/* Compatibility entry for callers that need to rescan a mutated table. GC2 is
** the sole runtime collector, so no legacy black-to-gray list is maintained. */
static LJ_AINLINE void lj_gc_barrierback(global_State *g, GCtab *t)
{
  lj_gc2_barrier_tab_g(g, t);
}

static LJ_AINLINE void lj_gc_barriertv_(lua_State *L, GCtab *t, cTValue *tv)
{
  global_State *g;
  TValue snap;
  if (!tv)
    return;
  g = G(L);
  lj_tv_load_acq(&snap, tv);
  if (LJ_UNLIKELY(!lj_gc_tv_gcref_valid(g, &snap)))
    return;
  lj_gc2_barrier_tv_pair_g(g, obj2gco(t), &snap);
  lj_gc2_barrier_weak_value(L, t, &snap);
}

static LJ_AINLINE void lj_gc_barrierobjtv_(lua_State *L, GCobj *p,
					   cTValue *tv)
{
  global_State *g;
  TValue snap;
  if (!tv)
    return;
  g = G(L);
  lj_tv_load_acq(&snap, tv);
  if (LJ_UNLIKELY(!lj_gc_tv_gcref_valid(g, &snap)))
    return;
  lj_gc2_barrier_tv_pair_g(g, p, &snap);
}

/* Barrier for stores to table objects. TValue and GCobj variant. */
#define lj_gc_anybarriert(L, t)  \
  lj_gc2_barrier_tab((L), (t))
#define lj_gc_barriert(L, t, tv) \
  lj_gc_barriertv_((L), (t), (tv))
#define lj_gc_objbarriert(L, t, o)  \
  lj_gc2_barrier_obj_pair((L), obj2gco(t), obj2gco(o))

/* Barrier for stores to any other object. TValue and GCobj variant. */
#define lj_gc_barrier(L, p, tv) \
  lj_gc_barrierobjtv_((L), obj2gco(p), (tv))
#define lj_gc_objbarrier(L, p, o) \
  lj_gc2_barrier_obj_pair((L), obj2gco(p), obj2gco(o))

static LJ_AINLINE void lj_gc_pubtabkey_(lua_State *L, GCtab *t, cTValue *key)
{
  global_State *g = G(L);
  /*
  ** Publishing a fresh hash key only exposes that key edge. A full-table GC2
  ** barrier would requeue and rescan the whole growing table for every insert.
  */
  if (LJ_UNLIKELY(!lj_gc_tv_gcref_valid(g, key)))
    return;
  lj_gc2_barrier_key_g(g, t, key);
}

/*
** M5 publication wrappers. Callers use these GC2 barriers when publishing
** references that can be observed by another thread or concurrent traversal.
*/
#define lj_gc_pubtab(L, t) \
  lj_gc2_barrier_tab((L), (t))
#define lj_gc_pubtabtv(L, t, tv) \
  lj_gc_barriertv_((L), (t), (tv))
#define lj_gc_pubtabkey(L, t, key) \
  lj_gc_pubtabkey_((L), (t), (key))
#define lj_gc_pubtabobj(L, t, o) \
  lj_gc2_barrier_obj_pair((L), obj2gco(t), obj2gco(o))
#define lj_gc_pubobjtv(L, p, tv) \
  lj_gc_barrierobjtv_((L), obj2gco(p), (tv))
#define lj_gc_pubobjobj(L, p, o) \
  lj_gc2_barrier_obj_pair((L), obj2gco(p), obj2gco(o))
LJ_FUNCA void lj_gc_pubtabobj_vm(lua_State *L, GCtab *t, GCobj *o);
LJ_FUNCA void lj_gc_pubtabtv_vm(lua_State *L, GCtab *t, cTValue *tv);
LJ_FUNCA void lj_gc_pubtabtvn_vm(lua_State *L, GCtab *t, cTValue *tv,
				 uint32_t n);
LJ_FUNCA void lj_gc_pubtvroot_vm(lua_State *L, cTValue *tv);
LJ_FUNCA void LJ_FASTCALL lj_gc_pubuv(global_State *g, TValue *tv);

/* Allocator. */
LJ_FUNC void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz);
LJ_FUNC void *lj_mem_new_nothrow(lua_State *L, GCSize size);
/* Constructor-only allocation which updates global bytes but defers GC2 local
** accounting/assistance until every mutually dependent body is published. */
LJ_FUNC void *lj_mem_new_deferred_nothrow(lua_State *L, GCSize size);
LJ_FUNC void *lj_mem_newgco_raw_nothrow(lua_State *L, GCSize size,
					 uint32_t flags);
LJ_FUNC void *lj_mem_newgco_unlinked_nothrow(lua_State *L, GCSize size);
LJ_FUNC void *lj_mem_newgco_unlinked_deferred_nothrow(lua_State *L,
						       GCSize size);
LJ_FUNC void lj_mem_account_deferred(lua_State *L, GCSize size);
LJ_FUNC void *lj_mem_newgco_raw(lua_State *L, GCSize size, uint32_t flags);
LJ_FUNC int lj_mem_publish_cdata(lua_State *L, void *base, GCSize size,
				  int interior);
LJ_FUNC int lj_mem_publish_interior_cdata(lua_State *L, void *base,
					  GCSize size);
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
