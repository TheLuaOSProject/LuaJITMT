/*
** Function handling (prototypes, functions and upvalues).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_func_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_err.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_func.h"
#include "lj_state.h"
#include "lj_trace.h"
#include "lj_tg.h"
#include "lj_vm.h"

#ifdef LJ_FUNC_TEST_HELPERS
static int func_test_finduv_should_fail(void);
static int func_test_finduv_should_collect(void);
#else
#define func_test_finduv_should_fail()	0
#define func_test_finduv_should_collect()	0
#endif

/* -- Prototypes ---------------------------------------------------------- */

void LJ_FASTCALL lj_func_freeproto(global_State *g, GCproto *pt)
{
  if (!lj_mem_freegco_defer(g, pt, pt->sizept))
    lj_mem_free(g, pt, pt->sizept);
}

/* -- Upvalues ------------------------------------------------------------ */

/* Find existing open upvalue for a stack slot or create a new one. */
static GCupval *func_finduv_nothrow(lua_State *L, TValue *slot)
{
  global_State *g = G(L);
  GCRef *pp = lj_state_openupval_ref(L);
  GCupval *p;
  GCupval *uv;
  GCobj *next;
  /* Search the sorted list of open upvalues. */
  while ((next = gcref_acq(*pp)) != NULL &&
	 uvval((p = gco2uv(next))) >= slot) {
    lj_assertG(!p->closed && uvval(p) != &p->tv, "closed upvalue in chain");
    if (uvval(p) == slot) {  /* Found open upvalue pointing to same slot? */
      /* Header whites are compatibility metadata, not a resurrection gate.
      ** The GC2 arena mark is the complete liveness action for this lookup. */
      lj_gc_arena_markobj(g, obj2gco(p));
      return p;
    }
    pp = lj_obj_gcwref(obj2gco(p));
  }
  /* No matching upvalue found. Create a new one. */
  if (func_test_finduv_should_fail())
    return NULL;
  uv = (GCupval *)lj_mem_newgco_raw_nothrow(L, sizeof(GCupval),
					    LJ_AF_TRAVERSABLE);
  if (LJ_UNLIKELY(uv == NULL))
    return NULL;
  newwhite(g, uv);
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 0;  /* Still open. */
  setmref(uv->v, slot);  /* Pointing to the stack slot. */
  /* The legacy global doubly-linked ring is retired. Keep its overlapping
  ** compatibility fields inert while the per-state nextgc chain owns this
  ** open upvalue. */
  setgcrefnullrel(uv->prev);
  setgcrefnullrel(uv->next);
  /* NOBARRIER: The GCupval is new (marked white) and open. */
  lj_obj_setgcwrel(obj2gco(uv), next);  /* Insert into open-upvalue list. */
  lj_gc_publishobj_header(g, obj2gco(uv));
  setgcrefrel(*pp, obj2gco(uv));
  /* The header became READY after a root snapshot may have rejected it as an
  ** opaque allocation. Publish the now-durable per-state open-list root. */
  lj_gc_pubobjroot(L, obj2gco(uv));
  if (func_test_finduv_should_collect())
    (void)lj_gc2_collect_active(L);
  return uv;
}

#if defined(LJ_GC2_TEST_HELPERS) || defined(LJ_FUNC_TEST_HELPERS)
GCupval *lj_func_test_openuv(lua_State *L, TValue *slot)
{
  return func_finduv_nothrow(L, slot);
}
#endif

#if LJ_HASJIT
static GCupval *func_newuvclosed_bump(lua_State *L, global_State *g,
				      TGState *tg);
#endif

/* Create an empty and closed upvalue. */
static GCupval *func_newuvclosed_unlinked_nothrow(lua_State *L)
{
  global_State *g = G(L);
  GCupval *uv;
  uv = (GCupval *)lj_mem_newgco_unlinked_nothrow(L, sizeof(GCupval));
  if (LJ_UNLIKELY(uv == NULL))
    return NULL;
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  setnilV(&uv->tv);
  setmref(uv->v, &uv->tv);
  uv->immutable = 0;
  uv->dhash = 0;
  newwhite(g, uv);
  lj_obj_setgcwnullrel(obj2gco(uv));
  return uv;
}

/* Create an empty and closed upvalue. */
static GCupval *func_newuvclosed(lua_State *L)
{
  global_State *g = G(L);
  GCupval *uv;
#if LJ_HASJIT
  uv = func_newuvclosed_bump(L, g, L2TG(L));
  if (uv)
    return uv;
#endif
  uv = func_newuvclosed_unlinked_nothrow(L);
  if (LJ_UNLIKELY(uv == NULL))
    lj_err_mem(L);
  lj_gc_linkobj_new(g, obj2gco(uv));
  return uv;
}

GCupval *LJ_FASTCALL lj_func_newuvcell(lua_State *L)
{
  lj_gc_check_fixtop(L);
  return func_newuvclosed(L);
}

static GCupval *func_snapshotuv(lua_State *L, const TValue *slot);

void lj_func_syncslot_forjit(lua_State *L, TValue *base, int32_t slot,
			     const TValue *tv)
{
  copyTVrel(L, base + slot, tv);
  lj_state_stack_pubtv(L, L, base + slot);
}

static LJ_AINLINE void func_storecell_pub(lua_State *L, TValue *slot,
					  GCupval *uv)
{
  TValue uvv;
  setgcV(L, &uvv, obj2gco(uv), LJ_TUPVAL);
  copyTVrel(L, slot, &uvv);
  lj_state_stack_pubtv(L, L, slot);
}

void lj_func_storeuv_pub(lua_State *L, TValue *tv, const TValue *src)
{
  copyTVrel(L, tv, src);
  lj_gc_pubuv(G(L), tv);
}

void lj_func_storeuvstr_pub(lua_State *L, TValue *tv, GCstr *str)
{
  TValue tmp;
  setstrV(L, &tmp, str);
  lj_func_storeuv_pub(L, tv, &tmp);
}

void lj_func_storeuvnum_pub(lua_State *L, TValue *tv, const lua_Number *np)
{
  TValue tmp;
  setnumV(&tmp, *np);
  lj_func_storeuv_pub(L, tv, &tmp);
}

void lj_func_storeuvpri_pub(lua_State *L, TValue *tv, uint32_t pri)
{
  TValue tmp;
  setpriV(&tmp, ~pri);
  lj_func_storeuv_pub(L, tv, &tmp);
}

void lj_func_storeuv_forjit(lua_State *L, TValue *tv, const TValue *src)
{
  copyTVrel(L, tv, src);
}

GCupval *lj_func_promoteuv_forjit(lua_State *L, TValue *base, int32_t slot,
				  const TValue *tv)
{
  TValue *dst = base + slot;
  GCupval *uv;
  if (itype(dst) == LJ_TUPVAL) {
    uv = gco2uv(gcV(dst));
  } else {
    if (tv == NULL)
      tv = dst;
    uv = func_snapshotuv(L, tv);
    func_storecell_pub(L, dst, uv);
  }
  return uv;
}

GCupval *lj_func_newuvcell_forjit(lua_State *L, TValue *base, int32_t slot)
{
  /*
  ** Trace assembly records BC_CNEW as an allocation call and owns the pacing
  ** check before the helper runs. Keep the interpreter helper as the owner of
  ** lj_gc_check_fixtop().
  */
  GCupval *uv = func_newuvclosed(L);
  func_storecell_pub(L, base + slot, uv);
  return uv;
}

static LJ_AINLINE void func_pubuv_payload(lua_State *L, GCupval *uv)
{
  /*
  ** A closed upvalue containing a primitive has no child edge to repair or
  ** remember. Keep the publication barrier on actual GC payloads, where GC2
  ** and the incremental collector must see uv->tv before the upvalue is
  ** linked into the pending root chain.
  */
  if (tvisgcv(&uv->tv))
    lj_gc_pubobjtv(L, uv, &uv->tv);
}

static LJ_AINLINE int func_bump_child_marked(global_State *g, GCobj *child)
{
  GCArena *a;
  uint32_t cell;
  /* These operands come from the active bump constructor itself: proto/env
  ** are stable strong inputs and the fresh upvalue was just READY-published in
  ** the same owner arena. No GC-capable operation occurs before the pending
  ** pair publication, so a direct allocation-start mark sample is exact. */
  if (!g || !child || g->allocf != lj_arena_allocf ||
      la_load32_acq(&g->allocf_arena) == 0 || !checkptrGC(child))
    return lj_gc2_ismarked(g, child);
  a = lj_arena_of(child);
  if (lj_arena_ishuge(a) ||
      !(lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE))
    return lj_gc2_ismarked(g, child);
  cell = lj_arena_cellof(child);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
      lj_arena_cellptr(a, cell) != (void *)child ||
      !lj_arena_bm_get(a->block, cell) || !lj_arena_ready_get(a, cell))
    return lj_gc2_ismarked(g, child);
  return (int)lj_arena_bm_get(a->mark, cell);
}

static LJ_AINLINE void func_pubfreshobjobj_(lua_State *L, TGState *tg,
					    GCobj *parent, GCobj *child)
{
  global_State *g = G(L);
  /*
  ** Bump FNEW helpers initialize a fresh white closure before linking it into
  ** the pending-root chain. With no active GC2 barrier, those edges will be
  ** discovered when the new root is first traversed. If marking is active, use
  ** the normal publication path so GC2 observes the child immediately.
  **
  ** During active black allocation the fresh parent is not yet published and
  ** its children are either established roots or freshly black-allocated in
  ** the same constructor. If a non-proto child is already marked, the edge is
  ** already owned by GC2 and repeating the mark as a locked test-and-set only
  ** adds allocation-loop contention. Protos are different: parser allocation
  ** can birth-mark a proto before any traversal is queued, so an already-marked
  ** proto still needs a one-per-proto SSB traversal handoff.
  ** Keep the normal barrier for active white allocation and unmarked/custom
  ** children.
  */
  if (tg && lj_tg_mark_active_acq(tg)) {
    if (!lj_tg_alloc_black_acq(tg))
      lj_gc2_barrier_obj_pair(L, parent, child);
    else if (child->gch.gct == ~LJ_TPROTO)
      lj_gc2_barrier_marked_proto(L, gco2pt(child));
    else if (func_bump_child_marked(g, child) <= 0)
      lj_gc2_barrier_obj_pair(L, parent, child);
  }
}

#define func_pubfreshobjobj(L, tg, p, o) \
  func_pubfreshobjobj_((L), (tg), obj2gco(p), obj2gco(o))

static LJ_AINLINE void func_fnew_preserve_operand(lua_State *L, GCobj *o)
{
  global_State *g;
  if (!L || !o || LJ_UNLIKELY(o->gch.gct == 0))
    return;
  g = G(L);
  if (!g)
    return;
  lj_gc_pubobjroot(L, o);
}

static LJ_AINLINE void func_fnew_preserve_operands(lua_State *L, GCproto *pt,
						   GCfuncL *parent)
{
  global_State *g = G(L);
  /* In incremental IDLE these three root barriers are exact no-ops: a cycle
  ** which starts after this observation snapshots operands which were already
  ** published, while an active phase is observed through the acquire load.
  ** Coalesce the identical phase/generational tests rather than entering the
  ** out-of-line barrier three times for every FNEW. */
  if (gc2_phase_acq(g) == LJ_GC2_IDLE && !gc2_generational_acq(g))
    return;
  func_fnew_preserve_operand(L, obj2gco(parent));
  func_fnew_preserve_operand(L, obj2gco(funcproto((GCfunc *)parent)));
  func_fnew_preserve_operand(L, obj2gco(pt));
}

/* Create a closed upvalue initialized from a stack slot. */
static GCupval *func_snapshotuv_unlinked_nothrow(lua_State *L,
					  const TValue *slot)
{
  global_State *g = G(L);
  GCupval *uv;
  /* Preserve a C-call result before the allocation itself can assist GC. */
  lj_gc_pubroot(L, slot);
  uv = (GCupval *)lj_mem_newgco_unlinked_nothrow(L, sizeof(GCupval));
  if (LJ_UNLIKELY(uv == NULL))
    return NULL;
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  /*
  ** FNEW may capture a C-call result slot before the next ordinary root scan.
  ** Publish the source stack value first, so GC2 repairs the result edge before
  ** the assertion-checked copy below.
  */
  copyTVrel(L, &uv->tv, slot);
  setmref(uv->v, &uv->tv);
  uv->immutable = 0;
  uv->dhash = 0;
  newwhite(g, uv);
  lj_obj_setgcwnullrel(obj2gco(uv));
  func_pubuv_payload(L, uv);
  return uv;
}

static GCupval *func_snapshotuv_unlinked(lua_State *L, const TValue *slot)
{
  GCupval *uv = func_snapshotuv_unlinked_nothrow(L, slot);
  if (LJ_UNLIKELY(uv == NULL))
    lj_err_mem(L);
  return uv;
}

static GCupval *func_snapshotuv(lua_State *L, const TValue *slot)
{
  global_State *g = G(L);
  GCupval *uv = func_snapshotuv_unlinked(L, slot);
  lj_gc_linkobj_new(g, obj2gco(uv));
  return uv;
}

static void func_uvmeta(GCupval *uv, GCfuncL *parent, uint32_t v)
{
  uv->immutable = ((v / PROTO_UV_IMMUTABLE) & 1) ? LJ_UV_IMMUTABLE : 0;
  uv->dhash = (uint32_t)(uintptr_t)mref(parent->pc, char) ^ (v << 24);
}

static LJ_AINLINE int func_legacyuv_snapshot(global_State *g, const GCproto *pt)
{
  /*
  ** A secondary VM can already be entering before the one-way mt_active latch
  ** is visible. Legacy-loaded protos do not have source cell-upvalue bytecode,
  ** so local captures must snapshot across that handoff window too.
  */
  return proto_legacyuv(pt) && mt_active_or_entering_acq(g);
}

/* Close all open upvalues pointing to some stack level or above. */
void LJ_FASTCALL lj_func_closeuv(lua_State *L, TValue *level)
{
  GCupval *uv;
  global_State *g = G(L);
  GCobj *head;
  while ((head = lj_state_openupval_acq(L)) != NULL &&
	 uvval((uv = gco2uv(head))) >= level) {
    GCobj *o = obj2gco(uv);
    lj_assertG(!uv->closed && uvval(uv) != &uv->tv, "closed upvalue in chain");
    lj_state_openupval_rel(L, lj_obj_gcw_acq(o));  /* No longer open. */
    /* Header colors are no longer a liveness authority. Transfer every closed
    ** cell from the per-state open chain to GC2's ownership/publication path;
    ** GC2's mark domain decides whether the closed cell remains live. */
    lj_gc_closeuv(g, uv);
  }
}

void LJ_FASTCALL lj_func_freeuv(global_State *g, GCupval *uv)
{
  /* Live open upvalues are roots of exactly one owner-claimed lua_State and
  ** cannot reach this destructor. Closed/stale arena observations use the same
  ** deferred-reuse path; no cross-state topology needs physical unlinking. */
  if (lj_mem_freegco_defer(g, uv, sizeof(GCupval)))
    return;
  lj_mem_freet(g, uv);
}

/* -- Functions (closures) ------------------------------------------------ */

#ifdef LJ_FUNC_TEST_HELPERS
#define FUNC_TEST_COUNTER(name, hitfn) \
static uint32_t func_test_##name; \
static LJ_AINLINE void func_test_##hitfn(void) \
{ \
  (void)la_add32_acqrel(&func_test_##name, 1); \
} \
uint32_t lj_func_test_##name(void) \
{ \
  return la_load32_acq(&func_test_##name); \
} \
void lj_func_test_reset_##name(void) \
{ \
  la_store32_rel(&func_test_##name, 0); \
}

FUNC_TEST_COUNTER(gc1num_bump_fast_calls, gc1num_bump_fast_call)
FUNC_TEST_COUNTER(gc1num_bump_fallback_calls, gc1num_bump_fallback_call)
FUNC_TEST_COUNTER(gc1num_bump_interp_calls, gc1num_bump_interp_call)
FUNC_TEST_COUNTER(gc1uv_chain_calls, gc1uv_chain_call)
FUNC_TEST_COUNTER(uv_afterfn_calls, uv_afterfn_call)
FUNC_TEST_COUNTER(gc0_bump_interp_calls, gc0_bump_interp_call)
FUNC_TEST_COUNTER(gc0_bump_trace_calls, gc0_bump_trace_call)
FUNC_TEST_COUNTER(uvcell_bump_calls, uvcell_bump_call)
static uint32_t func_test_empty_uv_fail_after;
static uint32_t func_test_finduv_fail_after;
static uint32_t func_test_finduv_collect_after;
void lj_func_test_fail_empty_uv_after(uint32_t nth)
{
  la_store32_rel(&func_test_empty_uv_fail_after, nth);
}
uint32_t lj_func_test_empty_uv_fail_remaining(void)
{
  return la_load32_acq(&func_test_empty_uv_fail_after);
}
void lj_func_test_fail_finduv_after(uint32_t nth)
{
  la_store32_rel(&func_test_finduv_fail_after, nth);
}
uint32_t lj_func_test_finduv_fail_remaining(void)
{
  return la_load32_acq(&func_test_finduv_fail_after);
}
void lj_func_test_collect_after_finduv(uint32_t nth)
{
  la_store32_rel(&func_test_finduv_collect_after, nth);
}
static int func_test_finduv_should_fail(void)
{
  uint32_t n = la_load32_acq(&func_test_finduv_fail_after);
  if (n == 0)
    return 0;
  return la_sub32_acqrel(&func_test_finduv_fail_after, 1) == 1;
}
static int func_test_finduv_should_collect(void)
{
  uint32_t n = la_load32_acq(&func_test_finduv_collect_after);
  if (n == 0)
    return 0;
  return la_sub32_acqrel(&func_test_finduv_collect_after, 1) == 1;
}
static int func_test_empty_uv_should_fail(void)
{
  uint32_t n = la_load32_acq(&func_test_empty_uv_fail_after);
  if (n == 0)
    return 0;
  return la_sub32_acqrel(&func_test_empty_uv_fail_after, 1) == 1;
}
#undef FUNC_TEST_COUNTER
#else
#define func_test_gc1num_bump_fast_call()	((void)0)
#define func_test_gc1num_bump_fallback_call()	((void)0)
#define func_test_gc1num_bump_interp_call()	((void)0)
#define func_test_gc1uv_chain_call()		((void)0)
#define func_test_uv_afterfn_call()		((void)0)
#define func_test_gc0_bump_interp_call()	((void)0)
#define func_test_gc0_bump_trace_call()		((void)0)
#define func_test_uvcell_bump_call()		((void)0)
#define func_test_empty_uv_should_fail()	0
#endif

static LJ_AINLINE void func_nupvalues_rel(GCfuncL *fn, uint32_t nup)
{
  la_store8_rel(&fn->nupvalues, (uint8_t)nup);
}

static LJ_AINLINE uint8_t func_proto_clcount_next(uint8_t flags)
{
  uint32_t count = (uint32_t)flags + PROTO_CLCOUNT;
  return (uint8_t)(count - ((count >> PROTO_CLC_BITS) & PROTO_CLCOUNT));
}

/* Generic FNEW can run concurrently for the same immutable prototype. Keep
** its saturating profiling counter data-race-free without serializing any
** closure body or allocator state. */
static void func_proto_clcount_inc_mt(GCproto *pt)
{
  uint8_t old = la_load8_rlx(&pt->flags);
  for (;;) {
    uint8_t next = func_proto_clcount_next(old);
    if (next == old || la_cas8(&pt->flags, &old, next, LA_RLX, LA_RLX))
      return;
  }
}

/* Bump constructors are admitted only while this TG is the sole allocator and
** no attached mutator/worker can update the prototype counter concurrently.
** Match the generated x64 fast path's byte load/store under that same gate. */
static LJ_AINLINE void func_proto_clcount_inc_exclusive(global_State *g,
						 GCproto *pt)
{
  lj_assertG(!mt_active_or_entering_acq(g) && gc2_n_workers_acq(g) == 0,
	     "prototype closure counter lacks exclusive bump gate");
  la_store8_rlx(&pt->flags,
		func_proto_clcount_next(la_load8_rlx(&pt->flags)));
}

static GCfunc *func_newC_at_anchor(lua_State *L, MSize nelems, GCtab *env,
				   TValue *anchor, uint32_t idx)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  TValue fnv;
  GCfunc *fn;
  MSize i;
  fn = (GCfunc *)lj_mem_newgco_unlinked_nothrow(L, sizeCfunc(nelems));
  if (LJ_UNLIKELY(fn == NULL)) {
    lj_tg_root_anchor_pop(tg, idx);
    lj_err_mem(L);
  }
  fn->c.gct = ~LJ_TFUNC;
  fn->c.ffid = FF_C;
  fn->c.nupvalues = (uint8_t)nelems;
  setmref(fn->c.pc, &G(L)->bc_cfunc_ext);
  lj_func_env_rel(fn, env);
  fn->c.f = NULL;
  for (i = 0; i < nelems; i++)
    setnilV(&fn->c.upvalue[i]);
  newwhite(g, obj2gco(fn));
  setfuncV(L, &fnv, fn);
  copyTVrel(L, anchor, &fnv);
  lj_gc_publishobj_header(g, obj2gco(fn));
  lj_gc_pubroot(L, anchor);
  lj_gc_linkobj_new(g, obj2gco(fn));
  lj_gc_pubobjobj(L, fn, env);
  return fn;
}

GCfunc *lj_func_newC(lua_State *L, MSize nelems, GCtab *env,
			 uint32_t *anchoridx)
{
  TGState *tg = L2TG(L);
  TValue envv;
  TValue *anchor;
  uint32_t idx;
  lj_assertL(anchoridx != NULL, "missing C-closure construction anchor");
  if (env)
    settabV(L, &envv, env);
  else
    setnilV(&envv);
  anchor = lj_tg_root_anchor_push(L, tg, &envv, &idx);
  if (LJ_UNLIKELY(anchor == NULL))
    lj_err_mem(L);
  lj_gc_pubroot(L, anchor);
  *anchoridx = idx;
  return func_newC_at_anchor(L, nelems, env, anchor, idx);
}

GCfunc *lj_func_newC_envrooted(lua_State *L, MSize nelems, GCtab *env,
			       uint32_t anchoridx)
{
  TGState *tg = L2TG(L);
  TValue snap;
  TValue *anchor = lj_tg_root_anchor_slot_acq(tg, anchoridx);
  lj_assertL(anchor != NULL &&
	     lj_tg_root_anchor_top_acq(tg) == anchoridx + 1u,
	     "C-closure environment root is not the top anchor");
  lj_tv_load_acq(&snap, anchor);
  lj_assertL((env && tvistab(&snap) && tabV(&snap) == env) ||
	     (!env && tvisnil(&snap)), "wrong C-closure environment root");
  return func_newC_at_anchor(L, nelems, env, anchor, anchoridx);
}

#if LJ_HASJIT
static void func_arena_set_alloc(GCArena *a, uint32_t cell, uint32_t ncells,
				 int black)
{
  /* Fresh arenas and scrubbed bump interiors are already mark-clear. A bump
  ** installed directly from sweep may retain the one marked run-head used by
  ** free-run metadata, so preserve the exact white-allocation clear only for
  ** that rare nonzero bit. func_bump_alloc_ready() excludes another marker
  ** while black is false; the acquire probe therefore only avoids a redundant
  ** locked RMW and never replaces a racing current-cycle mark decision. */
  UNUSED(ncells);
  if (black)
    lj_arena_bm_set(a->mark, cell);
  else if (LJ_UNLIKELY(lj_arena_bm_get(a->mark, cell)))
    lj_arena_bm_clear(a->mark, cell);
  lj_arena_ready_set_unpublished(a, cell);
  lj_arena_block_set(a, cell);
}

static LJ_AINLINE int func_bump_alloc_ready(global_State *g, TGState *tg)
{
  /*
  ** The closure/upvalue bump fast paths only own the main-TG arena while no
  ** attaching threads or GC2 workers can observe allocator state. Outside that
  ** single-producer window, use the regular allocator and publication path.
  */
  return g != NULL && tg != NULL &&
	 !mt_active_or_entering_acq(g) && gc2_n_workers_acq(g) == 0 &&
	 g->allocf_arena != 0 && tg == g->main_tg &&
	 !lj_tg_flags_test_acq(tg, TGF_DEAD) &&
	 lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) &&
	 g->allocd == &tg->allocd &&
	 (!lj_tg_mark_active_acq(tg) || lj_tg_alloc_black_acq(tg));
}

#ifdef LJ_FUNC_TEST_HELPERS
int lj_func_test_bump_alloc_ready(global_State *g, TGState *tg)
{
  return func_bump_alloc_ready(g, tg);
}
#endif

static LJ_AINLINE void func_bump_publish_obj(global_State *g, GCArena *a,
					      GCobj *o, uint32_t cell)
{
  int committed;
  /* The arena destructor class is the ownership identity. nextgc remains inert
  ** and no global/pending ownership edge is created on this rootless path. */
  lj_obj_setgcwnullrel(o);
  lj_assertG(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE &&
	     lj_arena_dtor_kind_acq(a, cell) != LJ_ARENA_DTOR_NONE &&
	     lj_arena_ready_get(a, cell) && lj_arena_bm_get(a->block, cell),
	     "typed bump object reached commit without discovery authority");
  committed = lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_LIVE);
  if (LJ_UNLIKELY(!committed))
    committed = lj_arena_dtor_construct_commit(a, cell);
  lj_assertG(committed, "typed bump construction commit lost");
  if (LJ_UNLIKELY(!committed))
    abort();
}

static LJ_AINLINE void func_bump_publish_pair(global_State *g, GCArena *a,
					      GCobj *head, uint32_t headcell,
					      GCobj *tail, uint32_t tailcell)
{
  int committed;
  lj_obj_setgcwnullrel(head);
  lj_obj_setgcwnullrel(tail);
  lj_assertG(lj_arena_root_state_acq(a, headcell) == LJ_ARENA_ROOT_NONE &&
	     lj_arena_root_state_acq(a, tailcell) == LJ_ARENA_ROOT_NONE &&
	     lj_arena_dtor_kind_acq(a, headcell) != LJ_ARENA_DTOR_NONE &&
	     lj_arena_dtor_kind_acq(a, tailcell) != LJ_ARENA_DTOR_NONE &&
	     lj_arena_ready_get(a, headcell) &&
	     lj_arena_ready_get(a, tailcell) &&
	     lj_arena_bm_get(a->block, headcell) &&
	     lj_arena_bm_get(a->block, tailcell),
	     "typed bump pair reached commit without discovery authority");
  committed = headcell / LJ_ARENA_LIFETIME_CELLS_PER_WORD ==
	      tailcell / LJ_ARENA_LIFETIME_CELLS_PER_WORD &&
	lj_arena_lifetime_state_cas_pair(a, headcell, tailcell,
	  LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_LIVE);
  if (LJ_UNLIKELY(!committed))
    committed = lj_arena_dtor_construct_commit_pair(
	      a, headcell, tailcell);
  lj_assertG(committed, "typed bump pair construction commit lost");
  if (LJ_UNLIKELY(!committed))
    abort();
}

static GCupval *func_newuvclosed_bump(lua_State *L, global_State *g,
				      TGState *tg)
{
  const GCSize nbytes = (GCSize)sizeof(GCupval);
  const uint32_t ncells = lj_arena_ncells(nbytes);
  GCArena *a;
  GCupval *uv;
  uint64_t local_total;
  uint32_t cell, black;
  UNUSED(L);

  if (!func_bump_alloc_ready(g, tg))
    return NULL;
  local_total = lj_tg_local_total_acq(tg);
  /* A flushing account can assist GC after READY while the result is still
  ** only in a native local. Run that checkpoint before reserving constructor
  ** state, then keep the successful post-READY handoff safepoint-free. */
  if (local_total >= LJ_GC2_ACCT_FLUSH - nbytes) {
    (void)lj_gc2_flush_alloc_checkpoint(g, tg);
    if (!func_bump_alloc_ready(g, tg) ||
	lj_tg_local_total_acq(tg) >= LJ_GC2_ACCT_FLUSH - nbytes)
      return NULL;
  }

  /*
  ** Closed nil local cells are leaf objects, so the bump path only replaces
  ** arena allocation and typed ownership publication. The caller still owns GC
  ** pacing: interpreter BC_CNEW runs lj_gc_check_fixtop(), while traced BC_CNEW
  ** is recorded as an allocation helper and gets the trace CALLA check. After
  ** sweep, a reusable free run can become the next private bump window.
  */
  if (!lj_arena_reserve_bump_dtor(&tg->alloc, &tg->prng,
				  LJ_AF_TRAVERSABLE, ncells,
				  LJ_ARENA_DTOR_CLOSED_UV, &a, &cell))
    return NULL;
  /* The typed reservation owns CONSTRUCT while root[] remains NONE. */
  uv = (GCupval *)lj_arena_cellptr(a, cell);
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  setnilV(&uv->tv);
  setmref(uv->v, &uv->tv);
  uv->immutable = 0;
  uv->dhash = 0;
  newwhite(g, uv);

  black = lj_arena_alloc_black_acq(&tg->alloc);
  func_arena_set_alloc(a, cell, ncells, black);
  lj_gc_total_add(g, nbytes);
  /* Successful bump construction has no GC-capable step after READY. */
  func_bump_publish_obj(g, a, obj2gco(uv), cell);
  (void)lj_tg_local_total_add_rlx(tg, nbytes);
  func_test_uvcell_bump_call();
  return uv;
}

static GCfunc *func_newL_gc1tv_bump(lua_State *L, global_State *g,
				    TGState *tg, GCproto *pt,
				    GCfuncL *parent, TValue *base,
				    int32_t slotno, const TValue *src,
				    uint32_t uvspec, int count_kind)
{
  const uint32_t fncells = lj_arena_ncells(sizeLfunc(1));
  const uint32_t uvcells = lj_arena_ncells(sizeof(GCupval));
  const uint32_t ncells = fncells + uvcells;
  const GCSize nbytes = (GCSize)(sizeLfunc(1) + sizeof(GCupval));
  GCArena *a;
  GCfunc *fn;
  GCtab *env;
  GCupval *uv;
  TValue *slot;
  uint64_t local_total;
  uint32_t cell, uvcell, black;

  if (!func_bump_alloc_ready(g, tg))
    return NULL;
  local_total = lj_tg_local_total_acq(tg);
  /* Keep the post-READY result handoff strictly safepoint-free. */
  if (local_total >= LJ_GC2_ACCT_FLUSH - nbytes) {
    (void)lj_gc2_flush_alloc_checkpoint(g, tg);
    if (!func_bump_alloc_ready(g, tg) ||
	lj_tg_local_total_acq(tg) >= LJ_GC2_ACCT_FLUSH - nbytes)
      return NULL;
  }

  /*
  ** Use or refill the private bump window for the fresh pair. Closure identity,
  ** publication, and accounting are independent of arena address reuse order.
  */

  if (!lj_arena_reserve_bump_dtor_pair(&tg->alloc, &tg->prng,
				       LJ_AF_TRAVERSABLE, ncells, fncells,
				       LJ_ARENA_DTOR_LFUNC1,
				       LJ_ARENA_DTOR_CLOSED_UV,
				       &a, &cell))
    return NULL;
  /* The pair reservation owns two CONSTRUCT lanes with root[] still NONE. */
  uvcell = cell + fncells;
  fn = (GCfunc *)lj_arena_cellptr(a, cell);
  uv = (GCupval *)lj_arena_cellptr(a, uvcell);

  fn->l.gct = ~LJ_TFUNC;
  fn->l.ffid = FF_LUA;
  func_nupvalues_rel(&fn->l, 0);
  setmref(fn->l.pc, proto_bc(pt));
  env = lj_funcL_env_acq(parent);
  lj_func_env_rel(fn, env);
  newwhite(g, obj2gco(fn));

  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  copyTVrel(L, &uv->tv, src);
  setmref(uv->v, &uv->tv);
  uv->immutable = 0;
  uv->dhash = 0;
  newwhite(g, uv);

  slot = (base ? base : L->base) + slotno;
  func_uvmeta(uv, parent, uvspec);
  setgcrefrel(fn->l.uvptr[0], obj2gco(uv));
  func_nupvalues_rel(&fn->l, 1);

  /* Both complete bodies and their immutable dtor kinds precede discovery.
  ** block[] is the release LP; READY only distinguishes typed from opaque. */
  black = lj_arena_alloc_black_acq(&tg->alloc);
  func_arena_set_alloc(a, cell, fncells, black);
  func_arena_set_alloc(a, uvcell, uvcells, black);
  func_pubfreshobjobj(L, tg, fn, pt);
  func_pubfreshobjobj(L, tg, fn, env);
  func_pubfreshobjobj(L, tg, fn, uv);
  func_pubuv_payload(L, uv);
  /* A traced active-black miss has now performed all normal barriers for this
  ** closure. Seed exact durable proto/environment traversal work for later
  ** inline hits. Failure is conservative: this closure remains protected by
  ** the barriers above and the next traced FNEW calls C again. */
  if (count_kind == 1)
    (void)lj_gc2_fnew_certify_pair_nodrain(g, tg, pt, env);
  func_proto_clcount_inc_exclusive(g, pt);

  lj_gc_total_add(g, nbytes);
  /* Publish the complete pair; the precheck above rules out accounting assist. */
  func_bump_publish_pair(g, a, obj2gco(fn), cell,
			 obj2gco(uv), uvcell);
  if (!(uvspec & PROTO_UV_IMMUTABLE))
    func_storecell_pub(L, slot, uv);
  (void)lj_tg_local_total_add_rlx(tg, nbytes);
  if (count_kind == 1)
    func_test_gc1num_bump_fast_call();
  else if (count_kind == 2)
    func_test_gc1num_bump_interp_call();
  return fn;
}

static GCfunc *func_newL_gc0_bump(lua_State *L, global_State *g, TGState *tg,
				  GCproto *pt, GCfuncL *parent,
				  int count_kind)
{
  const GCSize nbytes = (GCSize)sizeLfunc(0);
  const uint32_t ncells = lj_arena_ncells(nbytes);
  GCArena *a;
  GCfunc *fn;
  GCtab *env;
  uint64_t local_total;
  uint32_t cell, black;

  if (!func_bump_alloc_ready(g, tg))
    return NULL;
  local_total = lj_tg_local_total_acq(tg);
  /* Keep the post-READY result handoff strictly safepoint-free. */
  if (local_total >= LJ_GC2_ACCT_FLUSH - nbytes) {
    (void)lj_gc2_flush_alloc_checkpoint(g, tg);
    if (!func_bump_alloc_ready(g, tg) ||
	lj_tg_local_total_acq(tg) >= LJ_GC2_ACCT_FLUSH - nbytes)
      return NULL;
  }

  /*
  ** The caller already owns the allocation pacing check: interpreter BC_FNEW
  ** runs lj_gc_check_fixtop(), while trace assembly emits the CALLA check.
  ** Use or refill the private bump window when available; function identity is
  ** ordinary Lua identity, but address reuse order is not observable.
  */

  if (!lj_arena_reserve_bump_dtor(&tg->alloc, &tg->prng,
				  LJ_AF_TRAVERSABLE, ncells,
				  LJ_ARENA_DTOR_LFUNC0, &a, &cell))
    return NULL;
  /* The typed reservation owns CONSTRUCT while root[] remains NONE. */
  fn = (GCfunc *)lj_arena_cellptr(a, cell);
  fn->l.gct = ~LJ_TFUNC;
  fn->l.ffid = FF_LUA;
  func_nupvalues_rel(&fn->l, 0);
  setmref(fn->l.pc, proto_bc(pt));
  env = lj_funcL_env_acq(parent);
  lj_func_env_rel(fn, env);
  newwhite(g, obj2gco(fn));
  black = lj_arena_alloc_black_acq(&tg->alloc);
  func_arena_set_alloc(a, cell, ncells, black);
  func_pubfreshobjobj(L, tg, fn, pt);
  func_pubfreshobjobj(L, tg, fn, env);
  func_proto_clcount_inc_exclusive(g, pt);

  lj_gc_total_add(g, nbytes);
  /* Publish the completed closure; the precheck rules out accounting assist. */
  func_bump_publish_obj(g, a, obj2gco(fn), cell);
  (void)lj_tg_local_total_add_rlx(tg, nbytes);
  if (count_kind == 1)
    func_test_gc0_bump_interp_call();
  else if (count_kind == 2)
    func_test_gc0_bump_trace_call();
  return fn;
}
#endif

static GCfunc *func_newL_unlinked_nothrow(lua_State *L, GCproto *pt,
					   GCtab *env)
{
  global_State *g = G(L);
  MSize i, nuv = pt->sizeuv;
  GCfunc *fn = (GCfunc *)lj_mem_newgco_unlinked_nothrow(L,
							sizeLfunc(nuv));
  if (LJ_UNLIKELY(fn == NULL))
    return NULL;
  fn->l.gct = ~LJ_TFUNC;
  fn->l.ffid = FF_LUA;
  /* The count is both traversal metadata and the physical allocation extent.
  ** Clear every slot, then release-publish the exact count before READY. This
  ** makes constructor cancellation/free sizing exact; READY keeps scanners
  ** out until every slot has its final value. */
  for (i = 0; i < nuv; i++)
    setgcrefnullrel(fn->l.uvptr[i]);
  func_nupvalues_rel(&fn->l, (uint32_t)nuv);
  setmref(fn->l.pc, proto_bc(pt));
  lj_func_env_rel(fn, env);
  lj_obj_setgcwnullrel(obj2gco(fn));
  newwhite(g, obj2gco(fn));
  /* Saturating 3 bit counter (0..7) for concurrently created closures. */
  func_proto_clcount_inc_mt(pt);
  return fn;
}

static void func_pending_chain_add(GCobj **tailp, GCobj *o)
{
  lj_obj_setgcwnullrel(o);
  lj_obj_setgcwrel(*tailp, o);
  *tailp = o;
}

static void func_pending_chain_free(lua_State *L, GCfunc *fn)
{
  global_State *g = G(L);
  GCobj *o = lj_obj_gcw_acq(obj2gco(fn));
  while (o != NULL) {
    GCobj *next = lj_obj_gcw_acq(o);
    lj_mem_freegco_unpublished(g, o, sizeof(GCupval));
    o = next;
  }
  lj_mem_freegco_unpublished(
    g, fn, sizeLfunc((MSize)lj_funcL_nupvalues(&fn->l)));
}

static int func_pending_chain_contains(GCfunc *fn, GCupval *needle)
{
  GCobj *o;
  for (o = lj_obj_gcw_acq(obj2gco(fn)); o != NULL;
       o = lj_obj_gcw_acq(o))
    if (o == obj2gco(needle))
      return 1;
  return 0;
}

static void func_pending_cells_restore(lua_State *L, GCfunc *fn,
				       TValue *base, GCproto *pt, MSize n)
{
  MSize i;
  if (!proto_celluv(pt))
    return;
  for (i = 0; i < n; i++) {
    uint32_t v = proto_uv(pt)[i];
    if ((v & PROTO_UV_LOCAL) && !(v & PROTO_UV_IMMUTABLE)) {
      TValue *slot = base + (v & 0xffu);
      if (itype(slot) == LJ_TUPVAL) {
	GCupval *uv = gco2uv(gcV(slot));
	if (func_pending_chain_contains(fn, uv)) {
	  copyTVrel(L, slot, &uv->tv);
	  lj_state_stack_pubtv(L, L, slot);
	}
      }
    }
  }
}

static GCfunc *func_newL_gc1uv_chain(lua_State *L, TValue *base, GCproto *pt,
				     GCfuncL *parent, int32_t slotno,
				     const TValue *slot, uint32_t v)
{
  GCfunc *fn;
  GCupval *uv;
  GCobj *tail;

  fn = func_newL_unlinked_nothrow(L, pt, lj_funcL_env_acq(parent));
  if (LJ_UNLIKELY(fn == NULL))
    lj_err_mem(L);
  uv = func_snapshotuv_unlinked_nothrow(L, slot);
  if (LJ_UNLIKELY(uv == NULL)) {
    func_pending_chain_free(L, fn);
    lj_err_mem(L);
  }
  func_uvmeta(uv, parent, v);
  setgcrefrel(fn->l.uvptr[0], obj2gco(uv));
  tail = obj2gco(fn);
  func_pending_chain_add(&tail, obj2gco(uv));
  lj_gc_linkobj_new_chain(G(L), obj2gco(fn), tail);
  lj_gc_pubobjroot(L, obj2gco(fn));
  lj_gc_pubobjobj(L, fn, uv);
  if (!(v & PROTO_UV_IMMUTABLE)) {
    func_storecell_pub(L, base + slotno, uv);
  }
  func_test_gc1uv_chain_call();
  return fn;
}

/* Create a new Lua function with empty upvalues. The caller-provided anchor
** contains pt on entry and contains the completed function on return. Keeping
** the same semantic root across this handoff closes both concurrent sweep and
** error-unwind gaps without exposing a partial closure on the Lua stack. */
GCfunc *lj_func_newL_empty(lua_State *L, GCproto *pt, GCtab *env,
			   uint32_t anchoridx)
{
  TGState *tg = L2TG(L);
  TValue nilv, fnv, uvv;
  TValue *anchor = lj_tg_root_anchor_slot_acq(tg, anchoridx);
  TValue *uvanchor = NULL;
  GCfunc *fn;
  GCobj *tail, *o;
  uint32_t uvanchoridx = 0;
  MSize i, nuv = pt->sizeuv;
  lj_assertL(anchor != NULL && anchoridx < lj_tg_root_anchor_top_acq(tg) &&
	     tvisproto(anchor) && protoV(anchor) == pt,
	     "bad Lua function construction anchor");

  /* Reserve the child publication slot while the prototype is still rooted.
  ** A block allocation may throw, but no pending function exists yet. */
  if (nuv != 0) {
    setnilV(&nilv);
    uvanchor = lj_tg_root_anchor_push(L, tg, &nilv, &uvanchoridx);
    if (LJ_UNLIKELY(uvanchor == NULL))
      lj_err_mem(L);
  }

  fn = func_newL_unlinked_nothrow(L, pt, env);
  if (LJ_UNLIKELY(fn == NULL)) {
    if (uvanchor)
      lj_tg_root_anchor_pop(tg, uvanchoridx);
    lj_err_mem(L);
  }
  tail = obj2gco(fn);
  for (i = 0; i < nuv; i++) {
    GCupval *uv = func_test_empty_uv_should_fail() ? NULL :
		  func_newuvclosed_unlinked_nothrow(L);
    int32_t v = proto_uv(pt)[i];
    if (LJ_UNLIKELY(uv == NULL)) {
      func_pending_chain_free(L, fn);
      lj_tg_root_anchor_pop(tg, uvanchoridx);
      lj_err_mem(L);
    }
    uv->immutable = ((v / PROTO_UV_IMMUTABLE) & 1) ? LJ_UV_IMMUTABLE : 0;
    uv->dhash = (uint32_t)(uintptr_t)pt ^ (v << 24);
    setgcrefrel(fn->l.uvptr[i], obj2gco(uv));
    func_pending_chain_add(&tail, obj2gco(uv));
  }

  /* Replace the prototype root while the function is still opaque. READY and
  ** the post-READY root barrier then publish its complete structural extent.
  ** Each child gets the same pending-anchor/READY/barrier handshake before the
  ** private chain is made ownership-discoverable in one release publication. */
  setfuncV(L, &fnv, fn);
  copyTVrel(L, anchor, &fnv);
  lj_gc_publishobj_header(G(L), obj2gco(fn));
  lj_gc_pubroot(L, anchor);
  for (o = lj_obj_gcw_acq(obj2gco(fn)); o != NULL;
       o = lj_obj_gcw_acq(o)) {
    setgcV(L, &uvv, o, LJ_TUPVAL);
    copyTVrel(L, uvanchor, &uvv);
    lj_gc_publishobj_header(G(L), o);
    lj_gc_pubroot(L, uvanchor);
    lj_gc_pubobjobj(L, fn, o);
    copyTVrel(L, uvanchor, &nilv);
  }
  lj_gc_linkobj_new_chain(G(L), obj2gco(fn), tail);
  if (uvanchor)
    lj_tg_root_anchor_pop(tg, uvanchoridx);
  return fn;
}

/* Do a GC check and create a new Lua function with inherited upvalues. */
static GCfunc *func_newL_gc_base(lua_State *L, TValue *base, GCproto *pt,
				 GCfuncL *parent)
{
  GCfunc *fn;
  GCobj *tail;
  MSize i, nuv;
  if (base == NULL)
    base = L->base;
  nuv = pt->sizeuv;
  if (nuv == 1 && proto_celluv(pt)) {
    uint32_t v = proto_uv(pt)[0];
    if ((v & PROTO_UV_LOCAL)) {
      int32_t slotno = (int32_t)(v & 0xffu);
      TValue *slot = base + slotno;
      if (itype(slot) != LJ_TUPVAL)
	return func_newL_gc1uv_chain(L, base, pt, parent, slotno, slot, v);
    }
  }
  fn = func_newL_unlinked_nothrow(L, pt, lj_funcL_env_acq(parent));
  if (LJ_UNLIKELY(fn == NULL))
    lj_err_mem(L);
  tail = obj2gco(fn);
  for (i = 0; i < nuv; i++) {
    uint32_t v = proto_uv(pt)[i];
    GCupval *uv;
    if ((v & PROTO_UV_LOCAL)) {
      TValue *slot = base + (v & 0xff);
      if (proto_celluv(pt)) {
	if (itype(slot) == LJ_TUPVAL) {
	  uv = gco2uv(gcV(slot));
	  lj_assertL(uv->closed && uvval(uv) == &uv->tv,
		     "bad local cell upvalue");
	} else {
	  uv = func_snapshotuv_unlinked_nothrow(L, slot);
	  if (LJ_UNLIKELY(uv == NULL))
	    goto oom;
	  func_pending_chain_add(&tail, obj2gco(uv));
	  func_test_uv_afterfn_call();
	  if (!(v & PROTO_UV_IMMUTABLE))
	    func_storecell_pub(L, slot, uv);
	}
	func_uvmeta(uv, parent, v);
      } else {
	if (func_legacyuv_snapshot(G(L), pt)) {
	  uv = func_snapshotuv_unlinked_nothrow(L, slot);
	  if (LJ_UNLIKELY(uv == NULL))
	    goto oom;
	  func_pending_chain_add(&tail, obj2gco(uv));
	  func_test_uv_afterfn_call();
	} else {
	  uv = func_finduv_nothrow(L, slot);
	  if (LJ_UNLIKELY(uv == NULL))
	    goto oom;
	}
	func_uvmeta(uv, parent, v);
      }
    } else {
      uv = func_uv_acq(parent, v);
    }
    setgcrefrel(fn->l.uvptr[i], obj2gco(uv));
  }
  lj_gc_linkobj_new_chain(G(L), obj2gco(fn), tail);
  lj_gc_pubobjroot(L, obj2gco(fn));
  for (i = 0; i < nuv; i++) {
    GCobj *uv = func_uvptr_acq(&fn->l, (uint32_t)i);
    if (uv)
      lj_gc_pubobjobj(L, fn, uv);
  }
  return fn;
oom:
  func_pending_cells_restore(L, fn, base, pt, i);
  func_pending_chain_free(L, fn);
  lj_err_mem(L);
  return NULL;  /* Unreachable. */
}

GCfunc *lj_func_newL_gc(lua_State *L, GCproto *pt, GCfuncL *parent)
{
  func_fnew_preserve_operands(L, pt, parent);
  lj_gc_check_fixtop(L);
#if LJ_HASJIT
  if (pt->sizeuv == 0) {
    GCfunc *fn = func_newL_gc0_bump(L, G(L), L2TG(L), pt, parent, 1);
    if (fn)
      return fn;
  }
  if (pt->sizeuv == 1 && proto_celluv(pt)) {
    uint32_t v = proto_uv(pt)[0];
    if ((v & PROTO_UV_LOCAL)) {
      int32_t slotno = (int32_t)(v & 0xffu);
      TValue *slot = L->base + slotno;
      if (tvisnumber(slot)) {
	GCfunc *fn = func_newL_gc1tv_bump(L, G(L), L2TG(L), pt, parent,
					  L->base, slotno, slot, v, 2);
	if (fn)
	  return fn;
      }
    }
  }
#endif
  return func_newL_gc_base(L, NULL, pt, parent);
}

GCfunc *lj_func_newL_gc_forjit(lua_State *L, TValue *base, GCproto *pt,
			       GCfuncL *parent)
{
  /* Trace assembly emits the allocation GC check before CALLA helpers. */
  func_fnew_preserve_operands(L, pt, parent);
#if LJ_HASJIT
  if (pt->sizeuv == 0) {
    GCfunc *fn = func_newL_gc0_bump(L, G(L), L2TG(L), pt, parent, 2);
    if (fn)
      return fn;
  }
#endif
  return func_newL_gc_base(L, base, pt, parent);
}

GCfunc *lj_func_newL_gc1num_forjit(lua_State *L, TValue *base, GCproto *pt,
				   GCfuncL *parent, int32_t slotno,
				   lua_Number n)
{
  GCfunc *fn;
  GCupval *uv;
  GCobj *tail;
  TValue tv, *slot;
  uint32_t v;
  func_fnew_preserve_operands(L, pt, parent);
  lj_assertL(pt->sizeuv == 1 && proto_celluv(pt),
	     "bad one-upvalue FNEW helper");
  if (base == NULL)
    base = L->base;
  v = proto_uv(pt)[0];
  lj_assertL((v & PROTO_UV_LOCAL) && (int32_t)(v & 0xff) == slotno,
	     "bad one-upvalue FNEW slot");
  setnumV(&tv, n);
#if LJ_HASJIT
  fn = func_newL_gc1tv_bump(L, G(L), L2TG(L), pt, parent, base,
			    slotno, &tv, v, 1);
  if (fn)
    return fn;
  func_test_gc1num_bump_fallback_call();
#endif
  fn = func_newL_unlinked_nothrow(L, pt, lj_funcL_env_acq(parent));
  if (LJ_UNLIKELY(fn == NULL))
    lj_err_mem(L);
  slot = base + slotno;
  /*
  ** The recorder selects this helper for a raw numeric slot, but the generated
  ** trace may leave the previous iteration's promoted cell in BASE[slot].
  ** Each FNEW still has ordinary Lua closure identity, so allocate a fresh
  ** closed upvalue from the numeric SSA value and republish that cell to the
  ** parent slot for mutable captures.
  */
  uv = func_snapshotuv_unlinked_nothrow(L, &tv);
  if (LJ_UNLIKELY(uv == NULL)) {
    func_pending_chain_free(L, fn);
    lj_err_mem(L);
  }
  func_uvmeta(uv, parent, v);
  setgcrefrel(fn->l.uvptr[0], obj2gco(uv));
  tail = obj2gco(fn);
  func_pending_chain_add(&tail, obj2gco(uv));
  lj_gc_linkobj_new_chain(G(L), obj2gco(fn), tail);
  lj_gc_pubobjroot(L, obj2gco(fn));
  lj_gc_pubobjobj(L, fn, uv);
  if (!(v & PROTO_UV_IMMUTABLE)) {
    func_storecell_pub(L, slot, uv);
  }
  return fn;
}

void LJ_FASTCALL lj_func_free(global_State *g, GCfunc *fn)
{
  MSize size = isluafunc(fn) ? sizeLfunc((MSize)lj_funcL_nupvalues(&fn->l)) :
				       sizeCfunc((MSize)lj_funcC_nupvalues(&fn->c));
  if (!lj_mem_freegco_defer(g, fn, size))
    lj_mem_free(g, fn, size);
}
