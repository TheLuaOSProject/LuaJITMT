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
      if (isdead(g, obj2gco(p))) {  /* Resurrect it, if it's dead. */
	flipwhite(obj2gco(p));
	lj_gc_arena_markobj(g, obj2gco(p));
      }
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

/* Create an empty and closed upvalue. */
static GCupval *func_newuvclosed(lua_State *L)
{
  global_State *g = G(L);
  GCupval *uv = (GCupval *)lj_mem_newgco_unlinked(L, sizeof(GCupval));
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
  }
  return uv;
}

GCupval *lj_func_newuvcell_forjit(lua_State *L, TValue *base, int32_t slot)
{
  GCupval *uv = lj_func_newuvcell(L);
  setgcV(L, base + slot, obj2gco(uv), LJ_TUPVAL);
  return uv;
}

/* Create a closed upvalue initialized from a stack slot. */
static GCupval *func_snapshotuv_unlinked(lua_State *L, const TValue *slot)
{
  global_State *g = G(L);
  GCupval *uv = (GCupval *)lj_mem_newgco_unlinked(L, sizeof(GCupval));
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  copyTVrel(L, &uv->tv, slot);
  setmref(uv->v, &uv->tv);
  uv->immutable = 0;
  uv->dhash = 0;
  newwhite(g, uv);
  lj_gc_pubobjtv(L, uv, &uv->tv);
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
  uv->immutable = ((v / PROTO_UV_IMMUTABLE) & 1);
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
    lj_assertG(!isblack(o), "bad black upvalue");
    lj_assertG(!uv->closed && uvval(uv) != &uv->tv, "closed upvalue in chain");
    lj_state_openupval_rel(L, lj_obj_gcw_acq(o));  /* No longer open. */
    if (isdead(g, o)) {
      lj_func_freeuv(g, uv);
    } else {
      unlinkuv(g, uv);
      lj_gc_closeuv(g, uv);
    }
  }
}

void LJ_FASTCALL lj_func_freeuv(global_State *g, GCupval *uv)
{
  if (!uv->closed) {
    unlinkuv(g, uv);
  } else if (lj_mem_freegco_defer(g, uv, sizeof(GCupval))) {
    return;
  }
  lj_mem_freet(g, uv);
}

/* -- Functions (closures) ------------------------------------------------ */

#ifdef LJ_FUNC_TEST_HELPERS
static uint32_t func_test_gc1num_bump_fast_calls;
static uint32_t func_test_gc1num_bump_fallback_calls;
static uint32_t func_test_gc1num_bump_interp_calls;
static uint32_t func_test_gc1uv_chain_calls;
static uint32_t func_test_uv_afterfn_calls;
static uint32_t func_test_gc0_bump_interp_calls;

static LJ_AINLINE void func_test_gc1num_bump_fast_call(void)
{
  (void)la_add32_acqrel(&func_test_gc1num_bump_fast_calls, 1);
}

static LJ_AINLINE void func_test_gc1num_bump_fallback_call(void)
{
  (void)la_add32_acqrel(&func_test_gc1num_bump_fallback_calls, 1);
}

static LJ_AINLINE void func_test_gc1num_bump_interp_call(void)
{
  (void)la_add32_acqrel(&func_test_gc1num_bump_interp_calls, 1);
}

static LJ_AINLINE void func_test_gc1uv_chain_call(void)
{
  (void)la_add32_acqrel(&func_test_gc1uv_chain_calls, 1);
}

static LJ_AINLINE void func_test_uv_afterfn_call(void)
{
  (void)la_add32_acqrel(&func_test_uv_afterfn_calls, 1);
}

static LJ_AINLINE void func_test_gc0_bump_interp_call(void)
{
  (void)la_add32_acqrel(&func_test_gc0_bump_interp_calls, 1);
}

uint32_t lj_func_test_gc1num_bump_fast_calls(void)
{
  return la_load32_acq(&func_test_gc1num_bump_fast_calls);
}

void lj_func_test_reset_gc1num_bump_fast_calls(void)
{
  la_store32_rel(&func_test_gc1num_bump_fast_calls, 0);
}

uint32_t lj_func_test_gc1num_bump_fallback_calls(void)
{
  return la_load32_acq(&func_test_gc1num_bump_fallback_calls);
}

void lj_func_test_reset_gc1num_bump_fallback_calls(void)
{
  la_store32_rel(&func_test_gc1num_bump_fallback_calls, 0);
}

uint32_t lj_func_test_gc1num_bump_interp_calls(void)
{
  return la_load32_acq(&func_test_gc1num_bump_interp_calls);
}

void lj_func_test_reset_gc1num_bump_interp_calls(void)
{
  la_store32_rel(&func_test_gc1num_bump_interp_calls, 0);
}

uint32_t lj_func_test_gc1uv_chain_calls(void)
{
  return la_load32_acq(&func_test_gc1uv_chain_calls);
}

void lj_func_test_reset_gc1uv_chain_calls(void)
{
  la_store32_rel(&func_test_gc1uv_chain_calls, 0);
}

uint32_t lj_func_test_uv_afterfn_calls(void)
{
  return la_load32_acq(&func_test_uv_afterfn_calls);
}

void lj_func_test_reset_uv_afterfn_calls(void)
{
  la_store32_rel(&func_test_uv_afterfn_calls, 0);
}

uint32_t lj_func_test_gc0_bump_interp_calls(void)
{
  return la_load32_acq(&func_test_gc0_bump_interp_calls);
}

void lj_func_test_reset_gc0_bump_interp_calls(void)
{
  la_store32_rel(&func_test_gc0_bump_interp_calls, 0);
}
#else
#define func_test_gc1num_bump_fast_call()	((void)0)
#define func_test_gc1num_bump_fallback_call()	((void)0)
#define func_test_gc1num_bump_interp_call()	((void)0)
#define func_test_gc1uv_chain_call()		((void)0)
#define func_test_uv_afterfn_call()		((void)0)
#define func_test_gc0_bump_interp_call()	((void)0)
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
static void func_arena_set_alloc(GCArena *a, uint32_t cell, int black)
{
  lj_arena_bm_set(a->block, cell);
  if (black)
    lj_arena_bm_set(a->mark, cell);
  else
    lj_arena_bm_clear(a->mark, cell);
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
  LJArenaBump *b;
  GCArena *a;
  GCfunc *fn;
  GCtab *env;
  GCupval *uv;
  TValue *slot;
  GCobj *oldhead;
  uint32_t cell, uvcell, end, next, black;

  if (g == NULL || tg == NULL ||
      mt_active_or_entering_acq(g) || gc2_n_workers_acq(g) != 0 ||
      g->allocf_arena == 0 || tg != g->main_tg ||
      lj_tg_flags_test_acq(tg, TGF_DEAD) ||
      !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ||
      g->allocd != &tg->allocd ||
      lj_tg_local_total_acq(tg) >= LJ_GC2_ACCT_FLUSH - nbytes)
    return NULL;

  /*
  ** Match the empty-table fast path: only consume the current bump run when
  ** the owner-local free-run mask says no bin could satisfy either object.
  ** That preserves allocator reuse order and keeps this helper a pure
  ** no-contention bump specialization for the fresh function/upvalue pair.
  */
  if (lj_arena_alloc_has_run_ge(&tg->alloc, LJ_ARENAK_TRAVERSABLE,
				fncells < uvcells ? fncells : uvcells))
    return NULL;

  b = &tg->alloc.bump[LJ_ARENAK_TRAVERSABLE];
  a = b->a;
  if (a == NULL)
    return NULL;
  cell = b->cell;
  end = b->end;
  next = cell + ncells;
  if (next < cell || next > end)
    return NULL;

  b->cell = next;
  uvcell = cell + fncells;
  black = lj_arena_alloc_black_acq(&tg->alloc);
  func_arena_set_alloc(a, cell, black);
  func_arena_set_alloc(a, uvcell, black);

  fn = (GCfunc *)lj_arena_cellptr(a, cell);
  uv = (GCupval *)lj_arena_cellptr(a, uvcell);

  fn->l.gct = ~LJ_TFUNC;
  fn->l.ffid = FF_LUA;
  fn->l.nupvalues = 0;
  setmref(fn->l.pc, proto_bc(pt));
  env = lj_funcL_env_acq(parent);
  lj_func_env_rel(fn, env);
  newwhite(g, obj2gco(fn));
  lj_gc_pubobjobj(L, fn, pt);
  lj_gc_pubobjobj(L, fn, env);
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
  lj_gc_pubobjtv(L, uv, &uv->tv);

  slot = (base ? base : L->base) + slotno;
  if (!(uvspec & PROTO_UV_IMMUTABLE))
    setgcV(L, slot, obj2gco(uv), LJ_TUPVAL);
  func_uvmeta(uv, parent, uvspec);
  setgcrefrel(fn->l.uvptr[0], obj2gco(uv));
  lj_gc_pubobjobj(L, fn, uv);
  fn->l.nupvalues = 1;
  lj_obj_setgcwrel(obj2gco(fn), obj2gco(uv));

  lj_gc_total_add(g, nbytes);
  (void)lj_tg_local_total_add_rlx(tg, nbytes);
  oldhead = lj_tg_gcroot_pending_acq(tg);
  if (oldhead)
    lj_obj_setgcwrel(obj2gco(uv), oldhead);
  else
    lj_obj_setgcwnullrel(obj2gco(uv));
  lj_tg_gcroot_pending_store_rel(tg, obj2gco(fn));
  if (count_kind == 1)
    func_test_gc1num_bump_fast_call();
  else if (count_kind == 2)
    func_test_gc1num_bump_interp_call();
  return fn;
}

static GCfunc *func_newL_gc0_bump(lua_State *L, global_State *g, TGState *tg,
				  GCproto *pt, GCfuncL *parent)
{
  const GCSize nbytes = (GCSize)sizeLfunc(0);
  const uint32_t ncells = lj_arena_ncells(nbytes);
  LJArenaBump *b;
  GCArena *a;
  GCfunc *fn;
  GCtab *env;
  GCobj *oldhead;
  uint32_t count, cell, end, next, black;

  if (g == NULL || tg == NULL ||
      mt_active_or_entering_acq(g) || gc2_n_workers_acq(g) != 0 ||
      g->allocf_arena == 0 || tg != g->main_tg ||
      lj_tg_flags_test_acq(tg, TGF_DEAD) ||
      !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ||
      g->allocd != &tg->allocd ||
      lj_tg_local_total_acq(tg) >= LJ_GC2_ACCT_FLUSH - nbytes)
    return NULL;

  /*
  ** Interpreter BC_FNEW already ran lj_gc_check_fixtop(). This helper only
  ** replaces the allocation/root-publication body for no-upvalue closures in
  ** the same single-producer window used by empty TNEW and one-upvalue FNEW.
  */
  if (lj_arena_alloc_has_run_ge(&tg->alloc, LJ_ARENAK_TRAVERSABLE, ncells))
    return NULL;

  b = &tg->alloc.bump[LJ_ARENAK_TRAVERSABLE];
  a = b->a;
  if (a == NULL)
    return NULL;
  cell = b->cell;
  end = b->end;
  next = cell + ncells;
  if (next < cell || next > end)
    return NULL;

  b->cell = next;
  black = lj_arena_alloc_black_acq(&tg->alloc);
  func_arena_set_alloc(a, cell, black);

  fn = (GCfunc *)lj_arena_cellptr(a, cell);
  fn->l.gct = ~LJ_TFUNC;
  fn->l.ffid = FF_LUA;
  fn->l.nupvalues = 0;
  setmref(fn->l.pc, proto_bc(pt));
  env = lj_funcL_env_acq(parent);
  lj_func_env_rel(fn, env);
  newwhite(g, obj2gco(fn));
  lj_gc_pubobjobj(L, fn, pt);
  lj_gc_pubobjobj(L, fn, env);
  count = (uint32_t)pt->flags + PROTO_CLCOUNT;
  pt->flags = (uint8_t)(count - ((count >> PROTO_CLC_BITS) &
				  PROTO_CLCOUNT));

  lj_gc_total_add(g, nbytes);
  (void)lj_tg_local_total_add_rlx(tg, nbytes);
  oldhead = lj_tg_gcroot_pending_acq(tg);
  if (oldhead)
    lj_obj_setgcwrel(obj2gco(fn), oldhead);
  else
    lj_obj_setgcwnullrel(obj2gco(fn));
  lj_tg_gcroot_pending_store_rel(tg, obj2gco(fn));
  func_test_gc0_bump_interp_call();
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
  lj_gc_linkobj_after(obj2gco(fn), obj2gco(uv));
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
    uv->immutable = ((v / PROTO_UV_IMMUTABLE) & 1);
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
  lj_gc_check_fixtop(L);
#if LJ_HASJIT
  if (pt->sizeuv == 0) {
    GCfunc *fn = func_newL_gc0_bump(L, G(L), L2TG(L), pt, parent);
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
  MSize size = isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			       sizeCfunc((MSize)fn->c.nupvalues);
  if (!lj_mem_freegco_defer(g, fn, size))
    lj_mem_free(g, fn, size);
}
