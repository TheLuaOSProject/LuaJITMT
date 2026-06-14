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
  UNUSED(g);
  lj_assertG(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv,
	     "broken upvalue chain");
  setgcrefr(uvnext(uv)->prev, uv->prev);
  setgcrefr(uvprev(uv)->next, uv->next);
}

/* Find existing open upvalue for a stack slot or create a new one. */
static GCupval *func_finduv(lua_State *L, TValue *slot)
{
  global_State *g = G(L);
  GCRef *pp = &L->openupval;
  GCupval *p;
  GCupval *uv;
  /* Search the sorted list of open upvalues. */
  while (gcref(*pp) != NULL && uvval((p = gco2uv(gcref(*pp)))) >= slot) {
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
  lj_obj_setgcwr(obj2gco(uv), *pp);  /* Insert into sorted open-upvalue list. */
  setgcref(*pp, obj2gco(uv));
  setgcref(uv->prev, obj2gco(&g->uvhead));  /* Insert into GC list, too. */
  setgcrefr(uv->next, g->uvhead.next);
  setgcref(uvnext(uv)->prev, obj2gco(uv));
  setgcref(g->uvhead.next, obj2gco(uv));
  lj_assertG(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv,
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
  copyTV(L, &uv->tv, slot);
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
  while (gcref(L->openupval) != NULL &&
	 uvval((uv = gco2uv(gcref(L->openupval)))) >= level) {
    GCobj *o = obj2gco(uv);
    lj_assertG(!isblack(o), "bad black upvalue");
    lj_assertG(!uv->closed && uvval(uv) != &uv->tv, "closed upvalue in chain");
    setgcrefr(L->openupval, *lj_obj_gcwref(o));  /* No longer open. */
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
  if (!uv->closed)
    unlinkuv(g, uv);
  lj_mem_freet(g, uv);
}

/* -- Functions (closures) ------------------------------------------------ */

GCfunc *lj_func_newC(lua_State *L, MSize nelems, GCtab *env)
{
  GCfunc *fn = (GCfunc *)lj_mem_newgco(L, sizeCfunc(nelems));
  fn->c.gct = ~LJ_TFUNC;
  fn->c.ffid = FF_C;
  fn->c.nupvalues = (uint8_t)nelems;
  /* NOBARRIER: The GCfunc is new (marked white). */
  setmref(fn->c.pc, &G(L)->bc_cfunc_ext);
  setgcref(fn->c.env, obj2gco(env));
  return fn;
}

static GCfunc *func_newL(lua_State *L, GCproto *pt, GCtab *env)
{
  uint32_t count;
  GCfunc *fn = (GCfunc *)lj_mem_newgco(L, sizeLfunc((MSize)pt->sizeuv));
  fn->l.gct = ~LJ_TFUNC;
  fn->l.ffid = FF_LUA;
  fn->l.nupvalues = 0;  /* Set to zero until upvalues are initialized. */
  /* NOBARRIER: Really a setgcref. But the GCfunc is new (marked white). */
  setmref(fn->l.pc, proto_bc(pt));
  setgcref(fn->l.env, obj2gco(env));
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
  /* NOBARRIER: The GCfunc is new (marked white). */
  for (i = 0; i < nuv; i++) {
    GCupval *uv = func_newuvclosed(L);
    int32_t v = proto_uv(pt)[i];
    uv->immutable = ((v / PROTO_UV_IMMUTABLE) & 1);
    uv->dhash = (uint32_t)(uintptr_t)pt ^ (v << 24);
    setgcref(fn->l.uvptr[i], obj2gco(uv));
  }
  fn->l.nupvalues = (uint8_t)nuv;
  return fn;
}

/* Do a GC check and create a new Lua function with inherited upvalues. */
static GCfunc *func_newL_gc_base(lua_State *L, TValue *base, GCproto *pt,
				 GCfuncL *parent)
{
  GCfunc *fn;
  GCRef *puv;
  MSize i, nuv;
  fn = func_newL(L, pt, tabref_acq(parent->env));
  /* NOBARRIER: The GCfunc is new (marked white). */
  puv = parent->uvptr;
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
	uv = proto_legacyuv(pt) && la_load32_acq(&G(L)->mt_active) ?
	     func_snapshotuv(L, slot) : func_finduv(L, slot);
	func_uvmeta(uv, parent, v);
      }
    } else {
      uv = &gcref(puv[v])->uv;
    }
    setgcref(fn->l.uvptr[i], obj2gco(uv));
  }
  fn->l.nupvalues = (uint8_t)nuv;
  return fn;
}

GCfunc *lj_func_newL_gc(lua_State *L, GCproto *pt, GCfuncL *parent)
{
  lj_gc_check_fixtop(L);
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
