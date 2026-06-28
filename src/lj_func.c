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

/* Create an empty and closed upvalue. */
static GCupval *func_newuvclosed(lua_State *L)
{
  GCupval *uv = (GCupval *)lj_mem_newgco(L, sizeof(GCupval));
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  setnilV(&uv->tv);
  setmref(uv->v, &uv->tv);
  uv->immutable = 0;
  uv->dhash = 0;
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
static GCupval *func_snapshotuv(lua_State *L, const TValue *slot)
{
  GCupval *uv = (GCupval *)lj_mem_newgco(L, sizeof(GCupval));
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  copyTVrel(L, &uv->tv, slot);
  lj_gc_pubobjtv(L, uv, &uv->tv);
  setmref(uv->v, &uv->tv);
  uv->immutable = 0;
  uv->dhash = 0;
  return uv;
}

static void func_uvmeta(GCupval *uv, GCfuncL *parent, uint32_t v)
{
  uv->immutable = ((v / PROTO_UV_IMMUTABLE) & 1);
  uv->dhash = (uint32_t)(uintptr_t)mref(parent->pc, char) ^ (v << 24);
}

/* Promote a source local slot to a closed upvalue cell, or inherit one. */
static GCupval *func_celluv(lua_State *L, TValue *slot, uint32_t v,
			    GCfuncL *parent)
{
  GCupval *uv;
  if (itype(slot) == LJ_TUPVAL) {
    uv = gco2uv(gcV(slot));
    lj_assertL(uv->closed && uvval(uv) == &uv->tv,
	       "bad local cell upvalue");
  } else {
    uv = func_snapshotuv(L, slot);
    if (!(v & PROTO_UV_IMMUTABLE))
      setgcV(L, slot, obj2gco(uv), LJ_TUPVAL);
  }
  func_uvmeta(uv, parent, v);
  return uv;
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

GCfunc *lj_func_newC(lua_State *L, MSize nelems, GCtab *env)
{
  GCfunc *fn = (GCfunc *)lj_mem_newgco(L, sizeCfunc(nelems));
  fn->c.gct = ~LJ_TFUNC;
  fn->c.ffid = FF_C;
  fn->c.nupvalues = (uint8_t)nelems;
  setmref(fn->c.pc, &G(L)->bc_cfunc_ext);
  lj_func_env_rel(fn, env);
  lj_gc_pubobjobj(L, fn, env);
  return fn;
}

static GCfunc *func_newL(lua_State *L, GCproto *pt, GCtab *env)
{
  uint32_t count;
  GCfunc *fn = (GCfunc *)lj_mem_newgco(L, sizeLfunc((MSize)pt->sizeuv));
  fn->l.gct = ~LJ_TFUNC;
  fn->l.ffid = FF_LUA;
  fn->l.nupvalues = 0;  /* Set to zero until upvalues are initialized. */
  setmref(fn->l.pc, proto_bc(pt));
  lj_gc_pubobjobj(L, fn, pt);
  lj_func_env_rel(fn, env);
  lj_gc_pubobjobj(L, fn, env);
  /* Saturating 3 bit counter (0..7) for created closures. */
  count = (uint32_t)pt->flags + PROTO_CLCOUNT;
  pt->flags = (uint8_t)(count - ((count >> PROTO_CLC_BITS) & PROTO_CLCOUNT));
  return fn;
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
  fn = func_newL(L, pt, lj_funcL_env_acq(parent));
  nuv = pt->sizeuv;
  if (base == NULL)
    base = L->base;
  for (i = 0; i < nuv; i++) {
    uint32_t v = proto_uv(pt)[i];
    GCupval *uv;
    if ((v & PROTO_UV_LOCAL)) {
      TValue *slot = base + (v & 0xff);
      if (proto_celluv(pt)) {
	uv = func_celluv(L, slot, v, parent);
      } else {
	uv = func_finduv(L, slot);
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

static void func_newL_interp_softgc(lua_State *L)
{
  global_State *g = G(L);
  GCSize total;
  uint64_t soft, max;
  if (lj_gc_threshold_load(g) == LJ_MAX_MEM ||
      g->gc.state != GCSpause || gc2_phase_acq(g) != LJ_GC2_IDLE)
    return;
  total = lj_gc_total_load(g);
  soft = lj_gc2_helper_soft_limit_load(g);
  if ((uint64_t)total < soft)
    return;
  max = ~(uint64_t)0;
  lj_gc2_helper_soft_limit_store(g, max);
  if (curr_funcisL(L))
    L->top = curr_topL(L);
  (void)lj_gc_step(L);
}

GCfunc *lj_func_newL_gc(lua_State *L, GCproto *pt, GCfuncL *parent)
{
  lj_gc_check_fixtop(L);
  if (pt->sizeuv == 0)
    func_newL_interp_softgc(L);
  return func_newL_gc_base(L, NULL, pt, parent);
}

GCfunc *lj_func_newL_gc_forjit(lua_State *L, TValue *base, GCproto *pt,
			       GCfuncL *parent)
{
  lj_gc_check_fixtop(L);
  return func_newL_gc_base(L, base, pt, parent);
}

void LJ_FASTCALL lj_func_free(global_State *g, GCfunc *fn)
{
  MSize size = isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			       sizeCfunc((MSize)fn->c.nupvalues);
  if (!lj_mem_freegco_defer(g, fn, size))
    lj_mem_free(g, fn, size);
}
