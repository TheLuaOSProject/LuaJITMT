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
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_func.h"
#include "lj_state.h"
#include "lj_trace.h"
#include "lj_tg.h"
#include "lj_vm.h"

/* -- Prototypes ---------------------------------------------------------- */

void LJ_FASTCALL lj_func_freeproto(global_State *g, GCproto *pt)
{
  if (!lj_mem_freegco_defer(g, pt, pt->sizept))
    lj_mem_free(g, pt, pt->sizept);
}

/* -- Upvalues ------------------------------------------------------------ */

static void unlinkuv(global_State *g, GCupval *uv)
{
  GCupval *next = lj_uv_next_acq(uv);
  GCupval *prev = lj_uv_prev_acq(uv);
  UNUSED(g);
  lj_assertG(lj_uv_prev_acq(next) == uv && lj_uv_next_acq(prev) == uv,
	     "broken upvalue chain");
  lj_uv_setprev_rel(next, prev);
  lj_uv_setnext_rel(prev, next);
}

static int upval_ring_linked(GCupval *uv)
{
  GCupval *next = lj_uv_next_acq(uv);
  GCupval *prev = lj_uv_prev_acq(uv);
  return next != NULL && prev != NULL &&
	 lj_uv_prev_acq(next) == uv && lj_uv_next_acq(prev) == uv;
}

/* Find existing open upvalue for a stack slot or create a new one. */
static GCupval *func_finduv(lua_State *L, TValue *slot)
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
      /* The bitmap mark is deliberately unconditional: if another thread won
      ** the resurrection CAS but has not marked yet, this lookup still pins
      ** the upvalue before returning it. */
      (void)lj_gc_resurrect_if_dead(g, obj2gco(p));
      lj_gc_arena_markobj(g, obj2gco(p));
      return p;
    }
    pp = lj_obj_gcwref(obj2gco(p));
  }
  /* No matching upvalue found. Create a new one. */
  uv = (GCupval *)lj_mem_newgco_raw(L, sizeof(GCupval),
				    LJ_AF_TRAVERSABLE);
  newwhite(g, uv);
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 0;  /* Still open. */
  setmref(uv->v, slot);  /* Pointing to the stack slot. */
  /* NOBARRIER: The GCupval is new (marked white) and open. */
  lj_obj_setgcwrel(obj2gco(uv), next);  /* Insert into open-upvalue list. */
  setgcrefrel(*pp, obj2gco(uv));
  lj_uv_setprev_rel(uv, &g->uvhead);  /* Insert into GC list, too. */
  lj_uv_setnext_rel(uv, lj_uv_next_acq(&g->uvhead));
  lj_uv_setprev_rel(lj_uv_next_acq(uv), uv);
  lj_uv_setnext_rel(&g->uvhead, uv);
  lj_assertG(lj_uv_prev_acq(lj_uv_next_acq(uv)) == uv &&
	     lj_uv_next_acq(lj_uv_prev_acq(uv)) == uv,
	     "broken upvalue chain");
  return uv;
}

static void func_publishuv(global_State *g, GCupval *uv)
{
  newwhite(g, uv);
  lj_gc_linkobj_new(g, obj2gco(uv));
}

#if LJ_HASJIT
static GCupval *func_newuvclosed_bump(lua_State *L, global_State *g,
				      TGState *tg);
#endif

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
  uv = (GCupval *)lj_mem_newgco_unlinked(L, sizeof(GCupval));
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  setnilV(&uv->tv);
  setmref(uv->v, &uv->tv);
  uv->immutable = 0;
  uv->dhash = 0;
  func_publishuv(g, uv);
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
  copyTV(L, base + slot, tv);
  lj_state_stack_pubtv(L, L, base + slot);
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
    setgcV(L, dst, obj2gco(uv), LJ_TUPVAL);
    lj_state_stack_pubtv(L, L, dst);
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
  setgcV(L, base + slot, obj2gco(uv), LJ_TUPVAL);
  lj_state_stack_pubtv(L, L, base + slot);
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
    int marked = lj_gc2_ismarked(g, child);
    if (!lj_tg_alloc_black_acq(tg))
      lj_gc2_barrier_obj_pair(L, parent, child);
    else if (child->gch.gct == ~LJ_TPROTO)
      lj_gc2_barrier_marked_proto(L, gco2pt(child));
    else if (marked <= 0)
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
  func_fnew_preserve_operand(L, obj2gco(parent));
  func_fnew_preserve_operand(L, obj2gco(funcproto((GCfunc *)parent)));
  func_fnew_preserve_operand(L, obj2gco(pt));
}

/* Create a closed upvalue initialized from a stack slot. */
static GCupval *func_snapshotuv_unlinked(lua_State *L, const TValue *slot)
{
  global_State *g = G(L);
  GCupval *uv = (GCupval *)lj_mem_newgco_unlinked(L, sizeof(GCupval));
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  /*
  ** FNEW may capture a C-call result slot before the next ordinary root scan.
  ** Publish the source stack value first, so GC2 repairs the result edge before
  ** the assertion-checked copy below.
  */
  lj_gc_pubroot(L, slot);
  copyTVrel(L, &uv->tv, slot);
  setmref(uv->v, &uv->tv);
  uv->immutable = 0;
  uv->dhash = 0;
  newwhite(g, uv);
  func_pubuv_payload(L, uv);
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
    /*
    ** Header colors are no longer a liveness authority. Transfer every closed
    ** cell from the open-upvalue ring to GC2's ownership/publication path;
    ** GC2's mark domain decides whether the closed cell remains live.
    */
    unlinkuv(g, uv);
    lj_gc_closeuv(g, uv);
  }
}

void LJ_FASTCALL lj_func_freeuv(global_State *g, GCupval *uv)
{
  if (!uv->closed) {
    if (LJ_LIKELY(upval_ring_linked(uv))) {
      unlinkuv(g, uv);
    } else if (lj_mem_freegco_defer(g, uv, sizeof(GCupval))) {
      /*
      ** A normal open upvalue is always linked in the global open-upvalue ring.
      ** Lock-free root-spine sweep can still observe an arena-retained body
      ** after a close/free race has cleared the ring links but before bitmap
      ** reuse is allowed. There is no ring ownership left to unlink; preserve
      ** the body through the deferred arena path just like closed upvalues.
      */
      return;
    }
  } else if (lj_mem_freegco_defer(g, uv, sizeof(GCupval))) {
    return;
  }
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
#endif

GCfunc *lj_func_newC(lua_State *L, MSize nelems, GCtab *env)
{
  global_State *g = G(L);
  GCfunc *fn = (GCfunc *)lj_mem_newgco_unlinked(L, sizeCfunc(nelems));
  MSize i;
  fn->c.gct = ~LJ_TFUNC;
  fn->c.ffid = FF_C;
  fn->c.nupvalues = (uint8_t)nelems;
  setmref(fn->c.pc, &G(L)->bc_cfunc_ext);
  lj_func_env_rel(fn, env);
  fn->c.f = NULL;
  for (i = 0; i < nelems; i++)
    setnilV(&fn->c.upvalue[i]);
  newwhite(g, obj2gco(fn));
  lj_gc_linkobj_new(g, obj2gco(fn));
  lj_gc_pubobjobj(L, fn, env);
  return fn;
}

#if LJ_HASJIT
static void func_arena_set_alloc(GCArena *a, uint32_t cell, uint32_t ncells,
				 int black)
{
  uint32_t i;
  for (i = 1; i < ncells; i++) {
    lj_arena_block_clear(a, cell + i);
    lj_arena_bm_clear(a->mark, cell + i);
  }
  if (black)
    lj_arena_bm_set(a->mark, cell);
  else
    lj_arena_bm_clear(a->mark, cell);
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
	 g->allocd == &tg->allocd;
}

static LJ_AINLINE void func_bump_publish_obj(global_State *g, GCobj *o)
{
  /*
  ** Arena mark bits carry liveness, but they do not identify the object header
  ** or destructor occupying a traversable run. Publish every bump object on
  ** the per-TG pending ownership chain, including active-black allocations, so
  ** GC2 can prune and dispatch the exact header before arena quarantine.
  */
  lj_gc_linkobj_new(g, o);
}

static LJ_AINLINE void func_bump_publish_pair(global_State *g, GCobj *head,
					      GCobj *tail)
{
  /* One release publication makes both exact headers ownership-discoverable. */
  lj_obj_setgcwrel(head, tail);
  lj_gc_linkobj_new_chain(g, head, tail);
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
  int account_now;
  UNUSED(L);

  if (!func_bump_alloc_ready(g, tg))
    return NULL;
  local_total = lj_tg_local_total_acq(tg);
  account_now = local_total >= LJ_GC2_ACCT_FLUSH - nbytes;

  /*
  ** Closed nil local cells are leaf objects, so the bump path only replaces
  ** arena allocation and pending-root publication. The caller still owns GC
  ** pacing: interpreter BC_CNEW runs lj_gc_check_fixtop(), while traced BC_CNEW
  ** is recorded as an allocation helper and gets the trace CALLA check. After
  ** sweep, a reusable free run can become the next private bump window.
  */
  if (!lj_arena_reserve_bump(&tg->alloc, &tg->prng, LJ_AF_TRAVERSABLE,
			     ncells, &a, &cell))
    return NULL;
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
  /*
  ** Arena bump cells become visible to bitmap sweep as soon as their block bit
  ** is set. Set that bit only after the header is initialized, then publish the
  ** root before an accounting flush can assist GC; malloc-backed unlinked objects
  ** do not have this bitmap visibility.
  */
  func_bump_publish_obj(g, obj2gco(uv));
  if (account_now)
    lj_gc2_account_alloc(g, tg, nbytes);
  else
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
  int account_now;

  if (!func_bump_alloc_ready(g, tg))
    return NULL;
  local_total = lj_tg_local_total_acq(tg);
  account_now = local_total >= LJ_GC2_ACCT_FLUSH - nbytes;

  /*
  ** Use or refill the private bump window for the fresh pair. Closure identity,
  ** publication, and accounting are independent of arena address reuse order.
  */

  if (!lj_arena_reserve_bump(&tg->alloc, &tg->prng, LJ_AF_TRAVERSABLE,
			     ncells, &a, &cell))
    return NULL;
  uvcell = cell + fncells;
  fn = (GCfunc *)lj_arena_cellptr(a, cell);
  uv = (GCupval *)lj_arena_cellptr(a, uvcell);

  fn->l.gct = ~LJ_TFUNC;
  fn->l.ffid = FF_LUA;
  fn->l.nupvalues = 0;
  setmref(fn->l.pc, proto_bc(pt));
  env = lj_funcL_env_acq(parent);
  lj_func_env_rel(fn, env);
  newwhite(g, obj2gco(fn));
  black = lj_arena_alloc_black_acq(&tg->alloc);
  func_arena_set_alloc(a, cell, fncells, black);
  func_pubfreshobjobj(L, tg, fn, pt);
  func_pubfreshobjobj(L, tg, fn, env);
  {
    uint32_t count = (uint32_t)pt->flags + PROTO_CLCOUNT;
    pt->flags = (uint8_t)(count - ((count >> PROTO_CLC_BITS) &
				    PROTO_CLCOUNT));
  }

  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  copyTVrel(L, &uv->tv, src);
  setmref(uv->v, &uv->tv);
  uv->immutable = 0;
  uv->dhash = 0;
  newwhite(g, uv);
  func_arena_set_alloc(a, uvcell, uvcells, black);
  func_pubuv_payload(L, uv);

  slot = (base ? base : L->base) + slotno;
  if (!(uvspec & PROTO_UV_IMMUTABLE))
    setgcV(L, slot, obj2gco(uv), LJ_TUPVAL);
  func_uvmeta(uv, parent, uvspec);
  setgcrefrel(fn->l.uvptr[0], obj2gco(uv));
  func_pubfreshobjobj(L, tg, fn, uv);
  fn->l.nupvalues = 1;

  lj_gc_total_add(g, nbytes);
  /*
  ** Each arena cell is made visible only after the corresponding object body is
  ** safe for marker traversal. Publish before a possible accounting assist
  ** because bitmap sweep can see allocated cells before they enter the GC
  ** root spine.
  */
  func_bump_publish_pair(g, obj2gco(fn), obj2gco(uv));
  if (account_now)
    lj_gc2_account_alloc(g, tg, nbytes);
  else
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
  uint32_t count, cell, black;
  int account_now;

  if (!func_bump_alloc_ready(g, tg))
    return NULL;
  local_total = lj_tg_local_total_acq(tg);
  account_now = local_total >= LJ_GC2_ACCT_FLUSH - nbytes;

  /*
  ** The caller already owns the allocation pacing check: interpreter BC_FNEW
  ** runs lj_gc_check_fixtop(), while trace assembly emits the CALLA check.
  ** Use or refill the private bump window when available; function identity is
  ** ordinary Lua identity, but address reuse order is not observable.
  */

  if (!lj_arena_reserve_bump(&tg->alloc, &tg->prng, LJ_AF_TRAVERSABLE,
			     ncells, &a, &cell))
    return NULL;
  fn = (GCfunc *)lj_arena_cellptr(a, cell);
  fn->l.gct = ~LJ_TFUNC;
  fn->l.ffid = FF_LUA;
  fn->l.nupvalues = 0;
  setmref(fn->l.pc, proto_bc(pt));
  env = lj_funcL_env_acq(parent);
  lj_func_env_rel(fn, env);
  newwhite(g, obj2gco(fn));
  black = lj_arena_alloc_black_acq(&tg->alloc);
  func_arena_set_alloc(a, cell, ncells, black);
  func_pubfreshobjobj(L, tg, fn, pt);
  func_pubfreshobjobj(L, tg, fn, env);
  count = (uint32_t)pt->flags + PROTO_CLCOUNT;
  pt->flags = (uint8_t)(count - ((count >> PROTO_CLC_BITS) &
				  PROTO_CLCOUNT));

  lj_gc_total_add(g, nbytes);
  /*
  ** Publish the initialized arena object before a possible accounting assist.
  ** This keeps bitmap sweep from treating a still-C-owned bump cell as garbage.
  */
  func_bump_publish_obj(g, obj2gco(fn));
  if (account_now)
    lj_gc2_account_alloc(g, tg, nbytes);
  else
    (void)lj_tg_local_total_add_rlx(tg, nbytes);
  if (count_kind == 1)
    func_test_gc0_bump_interp_call();
  else if (count_kind == 2)
    func_test_gc0_bump_trace_call();
  return fn;
}
#endif

static GCfunc *func_newL_unlinked(lua_State *L, GCproto *pt, GCtab *env)
{
  global_State *g = G(L);
  uint32_t count;
  GCfunc *fn = (GCfunc *)lj_mem_newgco_unlinked(L,
						sizeLfunc((MSize)pt->sizeuv));
  fn->l.gct = ~LJ_TFUNC;
  fn->l.ffid = FF_LUA;
  fn->l.nupvalues = 0;  /* Set to zero until upvalues are initialized. */
  setmref(fn->l.pc, proto_bc(pt));
  lj_func_env_rel(fn, env);
  newwhite(g, obj2gco(fn));
  lj_gc_pubobjobj(L, fn, pt);
  lj_gc_pubobjobj(L, fn, env);
  /* Saturating 3 bit counter (0..7) for created closures. */
  count = (uint32_t)pt->flags + PROTO_CLCOUNT;
  pt->flags = (uint8_t)(count - ((count >> PROTO_CLC_BITS) & PROTO_CLCOUNT));
  return fn;
}

static GCfunc *func_newL(lua_State *L, GCproto *pt, GCtab *env)
{
  GCfunc *fn = func_newL_unlinked(L, pt, env);
  lj_gc_linkobj_new(G(L), obj2gco(fn));
  return fn;
}

static GCfunc *func_newL_gc1uv_chain(lua_State *L, TValue *base, GCproto *pt,
				     GCfuncL *parent, int32_t slotno,
				     const TValue *slot, uint32_t v)
{
  GCfunc *fn;
  GCupval *uv;

  fn = func_newL_unlinked(L, pt, lj_funcL_env_acq(parent));
  uv = func_snapshotuv_unlinked(L, slot);
  if (!(v & PROTO_UV_IMMUTABLE))
    setgcV(L, base + slotno, obj2gco(uv), LJ_TUPVAL);
  func_uvmeta(uv, parent, v);
  setgcrefrel(fn->l.uvptr[0], obj2gco(uv));
  lj_gc_pubobjobj(L, fn, uv);
  fn->l.nupvalues = 1;
  lj_obj_setgcwrel(obj2gco(fn), obj2gco(uv));
  lj_gc_linkobj_new_chain(G(L), obj2gco(fn), obj2gco(uv));
  func_test_gc1uv_chain_call();
  return fn;
}

static GCupval *func_snapshotuv_afterfn(lua_State *L, const TValue *slot,
					GCfunc *fn)
{
  GCupval *uv = func_snapshotuv_unlinked(L, slot);
  lj_gc_linkobj_after(G(L), obj2gco(fn), obj2gco(uv));
  func_test_uv_afterfn_call();
  return uv;
}

static GCupval *func_celluv_afterfn(lua_State *L, TValue *slot, uint32_t v,
				    GCfuncL *parent, GCfunc *fn)
{
  GCupval *uv;
  if (itype(slot) == LJ_TUPVAL) {
    uv = gco2uv(gcV(slot));
    lj_assertL(uv->closed && uvval(uv) == &uv->tv,
	       "bad local cell upvalue");
  } else {
    uv = func_snapshotuv_afterfn(L, slot, fn);
    if (!(v & PROTO_UV_IMMUTABLE))
      setgcV(L, slot, obj2gco(uv), LJ_TUPVAL);
  }
  func_uvmeta(uv, parent, v);
  return uv;
}

/* Create a new Lua function with empty upvalues. */
GCfunc *lj_func_newL_empty(lua_State *L, GCproto *pt, GCtab *env)
{
  GCfunc *fn = func_newL(L, pt, env);
  MSize i, nuv = pt->sizeuv;
  for (i = 0; i < nuv; i++) {
    GCupval *uv = func_newuvclosed(L);
    int32_t v = proto_uv(pt)[i];
    uv->immutable = ((v / PROTO_UV_IMMUTABLE) & 1) ? LJ_UV_IMMUTABLE : 0;
    uv->dhash = (uint32_t)(uintptr_t)pt ^ (v << 24);
    setgcrefrel(fn->l.uvptr[i], obj2gco(uv));
    lj_gc_pubobjobj(L, fn, uv);
  }
  fn->l.nupvalues = (uint8_t)nuv;
  return fn;
}

/* Do a GC check and create a new Lua function with inherited upvalues. */
static GCfunc *func_newL_gc_base(lua_State *L, TValue *base, GCproto *pt,
				 GCfuncL *parent)
{
  GCfunc *fn;
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
  fn = func_newL(L, pt, lj_funcL_env_acq(parent));
  for (i = 0; i < nuv; i++) {
    uint32_t v = proto_uv(pt)[i];
    GCupval *uv;
    if ((v & PROTO_UV_LOCAL)) {
      TValue *slot = base + (v & 0xff);
      if (proto_celluv(pt)) {
	uv = func_celluv_afterfn(L, slot, v, parent, fn);
      } else {
	uv = func_legacyuv_snapshot(G(L), pt) ?
	     func_snapshotuv_afterfn(L, slot, fn) : func_finduv(L, slot);
	func_uvmeta(uv, parent, v);
      }
    } else {
      uv = func_uv_acq(parent, v);
    }
    setgcrefrel(fn->l.uvptr[i], obj2gco(uv));
    lj_gc_pubobjobj(L, fn, uv);
  }
  fn->l.nupvalues = (uint8_t)nuv;
  return fn;
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
  fn = func_newL_unlinked(L, pt, lj_funcL_env_acq(parent));
  slot = base + slotno;
  /*
  ** The recorder selects this helper for a raw numeric slot, but the generated
  ** trace may leave the previous iteration's promoted cell in BASE[slot].
  ** Each FNEW still has ordinary Lua closure identity, so allocate a fresh
  ** closed upvalue from the numeric SSA value and republish that cell to the
  ** parent slot for mutable captures.
  */
  uv = func_snapshotuv_unlinked(L, &tv);
  if (!(v & PROTO_UV_IMMUTABLE))
    setgcV(L, slot, obj2gco(uv), LJ_TUPVAL);
  func_uvmeta(uv, parent, v);
  setgcrefrel(fn->l.uvptr[0], obj2gco(uv));
  lj_gc_pubobjobj(L, fn, uv);
  fn->l.nupvalues = 1;
  lj_obj_setgcwrel(obj2gco(fn), obj2gco(uv));
  lj_gc_linkobj_new_chain(G(L), obj2gco(fn), obj2gco(uv));
  return fn;
}

void LJ_FASTCALL lj_func_free(global_State *g, GCfunc *fn)
{
  MSize size = isluafunc(fn) ? sizeLfunc((MSize)lj_funcL_nupvalues(&fn->l)) :
				       sizeCfunc((MSize)lj_funcC_nupvalues(&fn->c));
  if (!lj_mem_freegco_defer(g, fn, size))
    lj_mem_free(g, fn, size);
}
