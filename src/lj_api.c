/*
** Public Lua/C API.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_api_c
#define LUA_CORE

#include <limits.h>

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_bc.h"
#include "lj_frame.h"
#include "lj_trace.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_vm.h"
#include "lj_strscan.h"
#include "lj_strfmt.h"

/* -- Common helper functions --------------------------------------------- */

#define lj_checkapi_slot(idx) \
  lj_checkapi((idx) <= (L->top - L->base), "stack slot %d out of range", (idx))

static lua_State *api_errstate(lua_State *L);
static void api_checkclaim(lua_State *L, LJStateClaim *claim);

static TValue *index2adr(lua_State *L, int idx)
{
  if (idx > 0) {
    TValue *o = L->base + (idx - 1);
    return o < L->top ? o : niltv(L);
  } else if (idx > LUA_REGISTRYINDEX) {
    lj_checkapi(idx != 0 && -idx <= L->top - L->base,
		"bad stack slot %d", idx);
    return L->top + idx;
  } else if (idx == LUA_GLOBALSINDEX) {
    TValue *o = &L2TG(L)->tmptv;
    settabV(L, o, lj_state_env_acq(L));
    return o;
  } else if (idx == LUA_REGISTRYINDEX) {
    return registry(L);
  } else {
    GCfunc *fn = curr_func(L);
    lj_checkapi(fn->c.gct == ~LJ_TFUNC && !isluafunc(fn),
		"calling frame is not a C function");
    if (idx == LUA_ENVIRONINDEX) {
      TValue *o = &L2TG(L)->tmptv;
      settabV(L, o, lj_func_env_acq(fn));
      return o;
    } else {
      idx = LUA_GLOBALSINDEX - idx;
      return idx <= fn->c.nupvalues ? &fn->c.upvalue[idx-1] : niltv(L);
    }
  }
}

static LJ_AINLINE TValue *index2adr_check(lua_State *L, int idx)
{
  TValue *o = index2adr(L, idx);
  lj_checkapi(o != niltv(L), "invalid stack slot %d", idx);
  return o;
}

static LJ_AINLINE int index_iscupvalue(int idx)
{
  return idx < LUA_GLOBALSINDEX;
}

static LJ_AINLINE TValue *index2adr_read(lua_State *L, int idx, TValue *snap)
{
  TValue *o = index2adr(L, idx);
  if (idx == LUA_REGISTRYINDEX) {
    lj_registry_load_acq(G(L), snap);
    return snap;
  } else if (index_iscupvalue(idx) && o != niltv(L)) {
    lj_tv_load_acq(snap, o);
    return snap;
  }
  return o;
}

static LJ_AINLINE TValue *index2adr_check_read(lua_State *L, int idx,
					       TValue *snap)
{
  TValue *o = index2adr_read(L, idx, snap);
  lj_checkapi(o != niltv(L), "invalid stack slot %d", idx);
  return o;
}

static LJ_AINLINE void api_trace_flush_mutation(lua_State *L)
{
  if (lj_trace_hasany(G(L)) && lj_trace_flushall_hs(L))
    lj_err_caller(L, LJ_ERR_NOGCMM);
}

static LJ_AINLINE void index2adr_cupvalue_store_rel(lua_State *L, int idx,
						    const TValue *src)
{
  TValue *o = index2adr(L, idx);
  if (index_iscupvalue(idx) && o != niltv(L)) {
    GCfunc *fn = curr_func(L);
    TValue snap;
    copyTV(L, &snap, src);
    api_trace_flush_mutation(L);
    copyTVrel(L, o, &snap);
    lj_gc_pubobjtv(L, fn, &snap);
  }
}

enum {
  API_TOSTR_BAD = -1,
  API_TOSTR_NIL = 0,
  API_TOSTR_OK = 1
};

static void index2adr_storestr(lua_State *L, int idx, TValue *o, GCstr *s)
{
  if (index_iscupvalue(idx)) {
    TValue tv;
    setstrV(L, &tv, s);
    index2adr_cupvalue_store_rel(L, idx, &tv);
  } else {
    setstrV(L, o, s);
    lj_state_stack_pubtv(L, L, o);
  }
}

static GCstr *api_tolstring_claimed(lua_State *L, int idx, int *status)
{
  for (;;) {
    LJStateClaim claim;
    TValue snap, numtv;
    cTValue *o;
    GCstr *s;
    api_checkclaim(L, &claim);
    o = index2adr_read(L, idx, &snap);
    if (LJ_LIKELY(tvisstr(o))) {
      s = strV(o);
      lj_state_dropclaim(&claim);
      *status = API_TOSTR_OK;
      return s;
    }
    if (!tvisnumber(o)) {
      *status = tvisnil(o) ? API_TOSTR_NIL : API_TOSTR_BAD;
      lj_state_dropclaim(&claim);
      return NULL;
    }
    copyTV(L, &numtv, o);
    lj_state_dropclaim(&claim);

    {
      lua_State *errL = api_errstate(L);
      lj_gc_check(errL);
      s = lj_strfmt_number(errL, &numtv);
    }

    api_checkclaim(L, &claim);
    o = index2adr_read(L, idx, &snap);
    if (tvisstr(o)) {
      s = strV(o);
      lj_state_dropclaim(&claim);
      *status = API_TOSTR_OK;
      return s;
    }
    if (tvisnumber(o)) {
      if (tv_rawload_acq(o) == tv_rawload(&numtv)) {
	index2adr_storestr(L, idx, index_iscupvalue(idx) ? NULL : (TValue *)o,
			   s);
	lj_state_dropclaim(&claim);
	*status = API_TOSTR_OK;
	return s;
      }
      lj_state_dropclaim(&claim);
      continue;
    }
    *status = tvisnil(o) ? API_TOSTR_NIL : API_TOSTR_BAD;
    lj_state_dropclaim(&claim);
    return NULL;
  }
}

static TValue *index2adr_stack(lua_State *L, int idx)
{
  if (idx > 0) {
    TValue *o = L->base + (idx - 1);
    if (o < L->top) {
      return o;
    } else {
      lj_checkapi(0, "invalid stack slot %d", idx);
      return niltv(L);
    }
    return o < L->top ? o : niltv(L);
  } else {
    lj_checkapi(idx != 0 && -idx <= L->top - L->base,
		"invalid stack slot %d", idx);
    return L->top + idx;
  }
}

static GCtab *getcurrenv(lua_State *L)
{
  GCfunc *fn = curr_func(L);
  return fn->c.gct == ~LJ_TFUNC ? lj_func_env_acq(fn) :
				  lj_state_env_acq(L);
}

static lua_State *api_errstate(lua_State *L)
{
  lua_State *cur = lj_tg_cur_L(G(L));
  return cur && G(cur) == G(L) ? cur : L;
}

static void api_checkclaim(lua_State *L, LJStateClaim *claim)
{
  if (!lj_state_tryclaim(L, lj_thr_current_id(G(L)), claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
}

static void api_checkstack1_claimed(lua_State *L, lua_State *errL,
				    LJStateClaim *claim)
{
  if ((mref(L->maxstack, char) - (char *)L->top) <=
      (ptrdiff_t)sizeof(TValue)) {
    int status = lj_state_cpgrowstack(L, 1);
    if (status != LUA_OK) {
      if (L->top > L->base) L->top--;
      lj_state_dropresumeclaim(claim);
      lj_err_callermsg(errL, status == LUA_ERRMEM ?
		       "not enough memory" : "stack overflow");
    }
  }
}

static int api_getmetafield_key_claimed(lua_State *L, int idx, GCstr *field,
					LJStateClaim *claim);

static void api_vm_call_claimed(lua_State *L, TValue *base, int nres1,
				LJStateClaim *claim)
{
  if (claim && claim->release) {
    global_State *g = G(L);
    uint8_t oldh = hook_save(g);
    int status = lj_vm_pcall(L, base, nres1, 0);
    if (status)
      hook_restore(g, oldh);
    if (status) {
      lj_state_dropresumeclaim(claim);
      lj_err_throw(L, status);
    }
  } else {
    lj_vm_call(L, base, nres1);
  }
}

/* -- Miscellaneous API functions ----------------------------------------- */

LUA_API int lua_status(lua_State *L)
{
  LJStateClaim claim;
  uint32_t tid = lj_thr_current_id(G(L));
  int status;
  if (!lj_state_tryclaim(L, tid, &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  status = L->status;
  lj_state_dropclaim(&claim);
  return status;
}

LUA_API int lua_checkstack(lua_State *L, int size)
{
  LJStateClaim claim;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  if (size > LUAI_MAXCSTACK || (L->top - L->base + size) > LUAI_MAXCSTACK) {
    lj_state_dropresumeclaim(&claim);
    return 0;  /* Stack overflow. */
  } else if (size > 0) {
    int avail = (int)(mref(L->maxstack, TValue) - L->top);
    if (size > avail &&
	lj_state_cpgrowstack(L, (MSize)(size - avail)) != LUA_OK) {
      L->top--;
      lj_state_dropresumeclaim(&claim);
      return 0;  /* Out of memory. */
    }
  }
  lj_state_dropresumeclaim(&claim);
  return 1;
}

LUALIB_API void luaL_checkstack(lua_State *L, int size, const char *msg)
{
  if (!lua_checkstack(L, size))
    lj_err_callerv(L, LJ_ERR_STKOVM, msg);
}

LUA_API void lua_xmove(lua_State *L, lua_State *to, int n)
{
  LJStateClaim fromclaim, toclaim;
  TValue *f, *t;
  uint32_t tid;
  lua_State *errL;
  if (L == to) return;
  lj_checkapi(G(L) == G(to), "move across global states");
  tid = lj_thr_current_id(G(L));
  errL = api_errstate(L);
  if (!lj_state_tryclaim(to, tid, &toclaim))
    lj_err_callermsg(errL, "thread busy");
  if (!lj_state_tryclaim(L, tid, &fromclaim)) {
    lj_state_dropclaim(&toclaim);
    lj_err_callermsg(errL, "thread busy");
  }
  lj_checkapi_slot(n);
  if ((mref(to->maxstack, char) - (char *)to->top) <=
      (ptrdiff_t)n*(ptrdiff_t)sizeof(TValue)) {
    int status = lj_state_cpgrowstack(to, (MSize)n);
    if (status != LUA_OK) {
      if (to->top > to->base) to->top--;
      lj_state_dropclaim(&fromclaim);
      lj_state_dropclaim(&toclaim);
      lj_err_callermsg(errL, status == LUA_ERRMEM ?
			     "not enough memory" : "stack overflow");
    }
  }
  f = L->top;
  t = to->top = to->top + n;
  while (--n >= 0) {
    copyTV(to, --t, --f);
    lj_state_stack_pubtv(L, to, t);
  }
  L->top = f;
  lj_state_dropclaim(&fromclaim);
  lj_state_dropclaim(&toclaim);
}

LUA_API const lua_Number *lua_version(lua_State *L)
{
  static const lua_Number version = LUA_VERSION_NUM;
  UNUSED(L);
  return &version;
}

/* -- Stack manipulation -------------------------------------------------- */

LUA_API int lua_gettop(lua_State *L)
{
  LJStateClaim claim;
  int top;
  if (!lj_state_tryclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  top = (int)(L->top - L->base);
  lj_state_dropclaim(&claim);
  return top;
}

LUA_API void lua_settop(lua_State *L, int idx)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  if (idx >= 0) {
    lj_checkapi(idx <= tvref(L->maxstack) - L->base, "bad stack slot %d", idx);
    if (L->base + idx > L->top) {
      if (L->base + idx >= tvref(L->maxstack)) {
	int status = lj_state_cpgrowstack(L,
		      (MSize)idx - (MSize)(L->top - L->base));
	if (status != LUA_OK) {
	  if (L->top > L->base) L->top--;
	  lj_state_dropresumeclaim(&claim);
	  lj_err_callermsg(errL, status == LUA_ERRMEM ?
			   "not enough memory" : "stack overflow");
	}
      }
      do { setnilV(L->top++); } while (L->top < L->base + idx);
    } else {
      L->top = L->base + idx;
    }
  } else {
    lj_checkapi(-(idx+1) <= (L->top - L->base), "bad stack slot %d", idx);
    L->top += idx+1;  /* Shrinks top (idx < 0). */
  }
  lj_state_stack_pubrange(L, L);
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_remove(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue *p;
  if (!lj_state_tryclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  p = index2adr_stack(L, idx);
  while (++p < L->top) copyTV(L, p-1, p);
  L->top--;
  lj_state_stack_pubrange(L, L);
  lj_state_dropclaim(&claim);
}

LUA_API void lua_insert(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue *q, *p;
  if (!lj_state_tryclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  p = index2adr_stack(L, idx);
  for (q = L->top; q > p; q--) copyTV(L, q, q-1);
  copyTV(L, p, L->top);
  lj_state_stack_pubrange(L, L);
  lj_state_dropclaim(&claim);
}

static void copy_slot(lua_State *L, TValue *f, int idx)
{
  if (idx == LUA_GLOBALSINDEX) {
    GCtab *t;
    lj_checkapi(tvistab(f), "stack slot %d is not a table", idx);
    t = tabV(f);
    api_trace_flush_mutation(L);
    /* NOBARRIER: A thread (i.e. L) is never black. */
    lj_state_env_rel(L, t);
  } else if (idx == LUA_ENVIRONINDEX) {
    GCfunc *fn = curr_func(L);
    GCtab *t;
    if (fn->c.gct != ~LJ_TFUNC)
      lj_err_msg(L, LJ_ERR_NOENV);
    lj_checkapi(tvistab(f), "stack slot %d is not a table", idx);
    t = tabV(f);
    api_trace_flush_mutation(L);
    lj_func_env_rel(fn, t);
    lj_gc_pubobjobj(L, fn, t);
  } else {
    TValue *o = index2adr_check(L, idx);
    if (idx == LUA_REGISTRYINDEX) {
      lj_gc_pubroot(L, f);
      UNUSED(o);
      lj_registry_store_rel(L, f);
    } else if (idx < LUA_GLOBALSINDEX) {
      UNUSED(o);
      index2adr_cupvalue_store_rel(L, idx, f);
    } else {
      copyTV(L, o, f);
      lj_state_stack_pubtv(L, L, o);
    }
  }
}

LUA_API void lua_replace(lua_State *L, int idx)
{
  LJStateClaim claim;
  if (!lj_state_tryclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  lj_checkapi_slot(1);
  copy_slot(L, L->top - 1, idx);
  L->top--;
  lj_state_dropclaim(&claim);
}

LUA_API void lua_copy(lua_State *L, int fromidx, int toidx)
{
  LJStateClaim claim;
  TValue snap;
  if (!lj_state_tryclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  copy_slot(L, index2adr_read(L, fromidx, &snap), toidx);
  lj_state_dropclaim(&claim);
}

LUA_API void lua_pushvalue(lua_State *L, int idx)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue snap;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  copyTV(L, L->top, index2adr_read(L, idx, &snap));
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_state_dropresumeclaim(&claim);
}

/* -- Stack getters ------------------------------------------------------- */

LUA_API int lua_type(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  int tt;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (tvisnumber(o)) {
    tt = LUA_TNUMBER;
#if LJ_64 && !LJ_GC64
  } else if (tvislightud(o)) {
    tt = LUA_TLIGHTUSERDATA;
#endif
  } else if (o == niltv(L)) {
    tt = LUA_TNONE;
  } else {  /* Magic internal/external tag conversion. ORDER LJ_T */
    uint32_t t = ~itype(o);
#if LJ_64
    tt = (int)((U64x(75a06,98042110) >> 4*t) & 15u);
#else
    tt = (int)(((t < 8 ? 0x98042110u : 0x75a06u) >> 4*(t&7)) & 15u);
#endif
    lj_assertL(tt != LUA_TNIL || tvisnil(o), "bad tag conversion");
  }
  lj_state_dropclaim(&claim);
  return tt;
}

LUALIB_API void luaL_checktype(lua_State *L, int idx, int tt)
{
  if (lua_type(L, idx) != tt)
    lj_err_argt(L, idx, tt);
}

LUALIB_API void luaL_checkany(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  lj_state_dropclaim(&claim);
  if (o == niltv(L))
    lj_err_arg(L, idx, LJ_ERR_NOVAL);
}

LUA_API const char *lua_typename(lua_State *L, int t)
{
  UNUSED(L);
  return lj_obj_typename[t+1];
}

LUA_API int lua_iscfunction(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  int ok;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  ok = tvisfunc(o) && !isluafunc(funcV(o));
  lj_state_dropclaim(&claim);
  return ok;
}

LUA_API int lua_isnumber(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  TValue tmp;
  int ok;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  ok = (tvisnumber(o) || (tvisstr(o) && lj_strscan_number(strV(o), &tmp)));
  lj_state_dropclaim(&claim);
  return ok;
}

LUA_API int lua_isstring(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  int ok;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  ok = (tvisstr(o) || tvisnumber(o));
  lj_state_dropclaim(&claim);
  return ok;
}

LUA_API int lua_isuserdata(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  int ok;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  ok = (tvisudata(o) || tvislightud(o));
  lj_state_dropclaim(&claim);
  return ok;
}

LUA_API int lua_rawequal(lua_State *L, int idx1, int idx2)
{
  LJStateClaim claim;
  TValue snap1, snap2;
  cTValue *o1, *o2;
  int ok;
  api_checkclaim(L, &claim);
  o1 = index2adr_read(L, idx1, &snap1);
  o2 = index2adr_read(L, idx2, &snap2);
  ok = (o1 == niltv(L) || o2 == niltv(L)) ? 0 : lj_obj_equal(o1, o2);
  lj_state_dropclaim(&claim);
  return ok;
}

LUA_API int lua_equal(lua_State *L, int idx1, int idx2)
{
  LJStateClaim claim;
  TValue snap1, snap2;
  cTValue *o1, *o2;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  o1 = index2adr_read(L, idx1, &snap1);
  o2 = index2adr_read(L, idx2, &snap2);
  if (tvisint(o1) && tvisint(o2)) {
    int ok = intV(o1) == intV(o2);
    lj_state_dropresumeclaim(&claim);
    return ok;
  } else if (tvisnumber(o1) && tvisnumber(o2)) {
    int ok = numberVnum(o1) == numberVnum(o2);
    lj_state_dropresumeclaim(&claim);
    return ok;
  } else if (itype(o1) != itype(o2)) {
    lj_state_dropresumeclaim(&claim);
    return 0;
  } else if (tvispri(o1)) {
    int ok = o1 != niltv(L) && o2 != niltv(L);
    lj_state_dropresumeclaim(&claim);
    return ok;
#if LJ_64 && !LJ_GC64
  } else if (tvislightud(o1)) {
    int ok = o1->u64 == o2->u64;
    lj_state_dropresumeclaim(&claim);
    return ok;
#endif
  } else if (gcrefeq(o1->gcr, o2->gcr)) {
    lj_state_dropresumeclaim(&claim);
    return 1;
  } else if (!tvistabud(o1)) {
    lj_state_dropresumeclaim(&claim);
    return 0;
  } else {
    TValue *base = lj_meta_equal(L, gcV(o1), gcV(o2), 0);
    if ((uintptr_t)base <= 1) {
      int ok = (int)(uintptr_t)base;
      lj_state_dropresumeclaim(&claim);
      return ok;
    } else {
      int ok;
      L->top = base+2;
      api_vm_call_claimed(L, base, 1+1, &claim);
      L->top -= 2+LJ_FR2;
      ok = tvistruecond(L->top+1+LJ_FR2);
      lj_state_dropresumeclaim(&claim);
      return ok;
    }
  }
}

LUA_API int lua_lessthan(lua_State *L, int idx1, int idx2)
{
  LJStateClaim claim;
  TValue snap1, snap2;
  cTValue *o1, *o2;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  o1 = index2adr_read(L, idx1, &snap1);
  o2 = index2adr_read(L, idx2, &snap2);
  if (o1 == niltv(L) || o2 == niltv(L)) {
    lj_state_dropresumeclaim(&claim);
    return 0;
  } else if (tvisint(o1) && tvisint(o2)) {
    int ok = intV(o1) < intV(o2);
    lj_state_dropresumeclaim(&claim);
    return ok;
  } else if (tvisnumber(o1) && tvisnumber(o2)) {
    int ok = numberVnum(o1) < numberVnum(o2);
    lj_state_dropresumeclaim(&claim);
    return ok;
  } else {
    TValue *base = lj_meta_comp(L, o1, o2, 0);
    if ((uintptr_t)base <= 1) {
      int ok = (int)(uintptr_t)base;
      lj_state_dropresumeclaim(&claim);
      return ok;
    } else {
      int ok;
      L->top = base+2;
      api_vm_call_claimed(L, base, 1+1, &claim);
      L->top -= 2+LJ_FR2;
      ok = tvistruecond(L->top+1+LJ_FR2);
      lj_state_dropresumeclaim(&claim);
      return ok;
    }
  }
}

LUA_API lua_Number lua_tonumber(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  TValue tmp;
  lua_Number n;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (LJ_LIKELY(tvisnumber(o)))
    n = numberVnum(o);
  else if (tvisstr(o) && lj_strscan_num(strV(o), &tmp))
    n = numV(&tmp);
  else
    n = 0;
  lj_state_dropclaim(&claim);
  return n;
}

LUA_API lua_Number lua_tonumberx(lua_State *L, int idx, int *ok)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  TValue tmp;
  lua_Number n;
  int isnum;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (LJ_LIKELY(tvisnumber(o))) {
    isnum = 1;
    n = numberVnum(o);
  } else if (tvisstr(o) && lj_strscan_num(strV(o), &tmp)) {
    isnum = 1;
    n = numV(&tmp);
  } else {
    isnum = 0;
    n = 0;
  }
  lj_state_dropclaim(&claim);
  if (ok) *ok = isnum;
  return n;
}

LUALIB_API lua_Number luaL_checknumber(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  TValue tmp;
  lua_Number n = 0;
  int ok;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (LJ_LIKELY(tvisnumber(o))) {
    ok = 1;
    n = numberVnum(o);
  } else if (tvisstr(o) && lj_strscan_num(strV(o), &tmp)) {
    ok = 1;
    n = numV(&tmp);
  } else {
    ok = 0;
  }
  lj_state_dropclaim(&claim);
  if (!ok)
    lj_err_argt(L, idx, LUA_TNUMBER);
  return n;
}

LUALIB_API lua_Number luaL_optnumber(lua_State *L, int idx, lua_Number def)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  TValue tmp;
  lua_Number n = 0;
  int ok;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (LJ_LIKELY(tvisnumber(o))) {
    ok = 1;
    n = numberVnum(o);
  } else if (tvisnil(o)) {
    ok = 1;
    n = def;
  } else if (tvisstr(o) && lj_strscan_num(strV(o), &tmp)) {
    ok = 1;
    n = numV(&tmp);
  } else {
    ok = 0;
  }
  lj_state_dropclaim(&claim);
  if (!ok)
    lj_err_argt(L, idx, LUA_TNUMBER);
  return n;
}

LUA_API lua_Integer lua_tointeger(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  TValue tmp;
  lua_Number n;
  lua_Integer i;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (LJ_LIKELY(tvisint(o))) {
    i = intV(o);
  } else if (LJ_LIKELY(tvisnum(o))) {
    n = numV(o);
    i = lj_num2int_type(n, lua_Integer);
  } else {
    if (!(tvisstr(o) && lj_strscan_number(strV(o), &tmp))) {
      lj_state_dropclaim(&claim);
      return 0;
    }
    if (tvisint(&tmp))
      i = intV(&tmp);
    else {
      n = numV(&tmp);
      i = lj_num2int_type(n, lua_Integer);
    }
  }
  lj_state_dropclaim(&claim);
  return i;
}

LUA_API lua_Integer lua_tointegerx(lua_State *L, int idx, int *ok)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  TValue tmp;
  lua_Number n;
  lua_Integer i;
  int isnum = 1;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (LJ_LIKELY(tvisint(o))) {
    i = intV(o);
  } else if (LJ_LIKELY(tvisnum(o))) {
    n = numV(o);
    i = lj_num2int_type(n, lua_Integer);
  } else {
    if (!(tvisstr(o) && lj_strscan_number(strV(o), &tmp))) {
      isnum = 0;
      i = 0;
    } else if (tvisint(&tmp)) {
      i = intV(&tmp);
    } else {
      n = numV(&tmp);
      i = lj_num2int_type(n, lua_Integer);
    }
  }
  lj_state_dropclaim(&claim);
  if (ok) *ok = isnum;
  return i;
}

LUALIB_API lua_Integer luaL_checkinteger(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  TValue tmp;
  lua_Number n;
  lua_Integer i = 0;
  int ok = 1;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (LJ_LIKELY(tvisint(o))) {
    i = intV(o);
  } else if (LJ_LIKELY(tvisnum(o))) {
    n = numV(o);
    i = lj_num2int_type(n, lua_Integer);
  } else {
    if (!(tvisstr(o) && lj_strscan_number(strV(o), &tmp))) {
      ok = 0;
    } else if (tvisint(&tmp)) {
      i = (lua_Integer)intV(&tmp);
    } else {
      n = numV(&tmp);
      i = lj_num2int_type(n, lua_Integer);
    }
  }
  lj_state_dropclaim(&claim);
  if (!ok)
    lj_err_argt(L, idx, LUA_TNUMBER);
  return i;
}

LUALIB_API lua_Integer luaL_optinteger(lua_State *L, int idx, lua_Integer def)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  TValue tmp;
  lua_Number n;
  lua_Integer i = 0;
  int ok = 1;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (LJ_LIKELY(tvisint(o))) {
    i = intV(o);
  } else if (LJ_LIKELY(tvisnum(o))) {
    n = numV(o);
    i = lj_num2int_type(n, lua_Integer);
  } else if (tvisnil(o)) {
    i = def;
  } else {
    if (!(tvisstr(o) && lj_strscan_number(strV(o), &tmp))) {
      ok = 0;
    } else if (tvisint(&tmp)) {
      i = (lua_Integer)intV(&tmp);
    } else {
      n = numV(&tmp);
      i = lj_num2int_type(n, lua_Integer);
    }
  }
  lj_state_dropclaim(&claim);
  if (!ok)
    lj_err_argt(L, idx, LUA_TNUMBER);
  return i;
}

LUA_API int lua_toboolean(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  int ok;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  ok = tvistruecond(o);
  lj_state_dropclaim(&claim);
  return ok;
}

LUA_API const char *lua_tolstring(lua_State *L, int idx, size_t *len)
{
  int status;
  GCstr *s = api_tolstring_claimed(L, idx, &status);
  if (status != API_TOSTR_OK) {
    if (len != NULL) *len = 0;
    return NULL;
  }
  if (len != NULL) *len = s->len;
  return strdata(s);
}

LUALIB_API const char *luaL_checklstring(lua_State *L, int idx, size_t *len)
{
  int status;
  GCstr *s = api_tolstring_claimed(L, idx, &status);
  if (status != API_TOSTR_OK)
    lj_err_argt(L, idx, LUA_TSTRING);
  if (len != NULL) *len = s->len;
  return strdata(s);
}

LUALIB_API const char *luaL_optlstring(lua_State *L, int idx,
				       const char *def, size_t *len)
{
  int status;
  GCstr *s = api_tolstring_claimed(L, idx, &status);
  if (status == API_TOSTR_NIL) {
    if (len != NULL) *len = def ? strlen(def) : 0;
    return def;
  } else if (status != API_TOSTR_OK) {
    lj_err_argt(L, idx, LUA_TSTRING);
  }
  if (len != NULL) *len = s->len;
  return strdata(s);
}

LUALIB_API int luaL_checkoption(lua_State *L, int idx, const char *def,
				const char *const lst[])
{
  ptrdiff_t i;
  const char *s = lua_tolstring(L, idx, NULL);
  if (s == NULL && (s = def) == NULL)
    lj_err_argt(L, idx, LUA_TSTRING);
  for (i = 0; lst[i]; i++)
    if (strcmp(lst[i], s) == 0)
      return (int)i;
  lj_err_argv(L, idx, LJ_ERR_INVOPTM, s);
}

LUA_API size_t lua_objlen(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  size_t len;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (tvisstr(o)) {
    len = strV(o)->len;
  } else if (tvistab(o)) {
    len = (size_t)lj_tab_len(tabV(o));
  } else if (tvisudata(o)) {
    len = udataV(o)->len;
  } else if (tvisnumber(o)) {
    int status;
    GCstr *s;
    lj_state_dropclaim(&claim);
    s = api_tolstring_claimed(L, idx, &status);
    return status == API_TOSTR_OK ? s->len : 0;
  } else {
    len = 0;
  }
  lj_state_dropclaim(&claim);
  return len;
}

LUA_API lua_CFunction lua_tocfunction(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  lua_CFunction f = NULL;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (tvisfunc(o)) {
    BCOp op = bc_op(*mref(funcV(o)->c.pc, BCIns));
    if (op == BC_FUNCC || op == BC_FUNCCW)
      f = funcV(o)->c.f;
  }
  lj_state_dropclaim(&claim);
  return f;
}

LUA_API void *lua_touserdata(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  void *p;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (tvisudata(o))
    p = uddata(udataV(o));
  else if (tvislightud(o))
    p = lightudV(G(L), o);
  else
    p = NULL;
  lj_state_dropclaim(&claim);
  return p;
}

LUA_API lua_State *lua_tothread(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  lua_State *th;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  th = (!tvisthread(o)) ? NULL : threadV(o);
  lj_state_dropclaim(&claim);
  return th;
}

LUA_API const void *lua_topointer(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  const void *p;
  api_checkclaim(L, &claim);
  p = lj_obj_ptr(G(L), index2adr_read(L, idx, &snap));
  lj_state_dropclaim(&claim);
  return p;
}

/* -- Stack setters (object creation) ------------------------------------- */

LUA_API void lua_pushnil(lua_State *L)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  setnilV(L->top);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_pushnumber(lua_State *L, lua_Number n)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  setnumV(L->top, n);
  if (LJ_UNLIKELY(tvisnan(L->top)))
    setnanV(L->top);  /* Canonicalize injected NaNs. */
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_pushinteger(lua_State *L, lua_Integer n)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  setintptrV(L->top, n);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_pushlstring(lua_State *L, const char *str, size_t len)
{
  LJStateClaim preclaim, claim;
  lua_State *errL;
  GCstr *s;
  api_checkclaim(L, &preclaim);
  lj_state_dropclaim(&preclaim);
  errL = api_errstate(L);
  s = lj_str_new(errL, str, len);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  setstrV(L, L->top, s);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_pushstring(lua_State *L, const char *str)
{
  LJStateClaim preclaim, claim;
  lua_State *errL;
  GCstr *s = NULL;
  if (str == NULL) {
    errL = api_errstate(L);
  } else {
    api_checkclaim(L, &preclaim);
    lj_state_dropclaim(&preclaim);
    errL = api_errstate(L);
    s = lj_str_newz(errL, str);
  }
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  if (str == NULL)
    setnilV(L->top);
  else
    setstrV(L, L->top, s);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_state_dropresumeclaim(&claim);
}

LUA_API const char *lua_pushvfstring(lua_State *L, const char *fmt,
				     va_list argp)
{
  lj_gc_check(L);
  return lj_strfmt_pushvf(L, fmt, argp);
}

LUA_API const char *lua_pushfstring(lua_State *L, const char *fmt, ...)
{
  const char *ret;
  va_list argp;
  lj_gc_check(L);
  va_start(argp, fmt);
  ret = lj_strfmt_pushvf(L, fmt, argp);
  va_end(argp);
  return ret;
}

LUA_API void lua_pushcclosure(lua_State *L, lua_CFunction f, int n)
{
  GCfunc *fn;
  lj_gc_check(L);
  lj_checkapi_slot(n);
  fn = lj_func_newC(L, (MSize)n, getcurrenv(L));
  fn->c.f = f;
  L->top -= n;
  while (n--) {
    copyTVrel(L, &fn->c.upvalue[n], L->top+n);
    lj_gc_pubobjtv(L, fn, &fn->c.upvalue[n]);
  }
  setfuncV(L, L->top, fn);
  lj_assertL(iswhite(obj2gco(fn)), "new GC object is not white");
  incr_top(L);
}

LUA_API void lua_pushboolean(lua_State *L, int b)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  setboolV(L->top, (b != 0));
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_pushlightuserdata(lua_State *L, void *p)
{
  LJStateClaim preclaim, claim;
  lua_State *errL;
  api_checkclaim(L, &preclaim);
  lj_state_dropclaim(&preclaim);
  errL = api_errstate(L);
#if LJ_64
  p = lj_lightud_intern(errL, p);
#endif
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  setrawlightudV(L->top, p);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_createtable(lua_State *L, int narray, int nrec)
{
  LJStateClaim preclaim, claim;
  lua_State *errL;
  GCtab *t;
  api_checkclaim(L, &preclaim);
  lj_state_dropclaim(&preclaim);
  errL = api_errstate(L);
  t = lj_tab_new_ah(errL, (uint32_t)narray, (uint32_t)nrec);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  settabV(L, L->top, t);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_state_dropresumeclaim(&claim);
}

LUALIB_API int luaL_newmetatable(lua_State *L, const char *tname)
{
  GCtab *regt = lj_registry_tab_acq(G(L));
  GCstr *key = lj_str_newz(L, tname);
  TValue keytv;
  setstrV(L, &keytv, key);
  for (;;) {
    TValue *tv = lj_tab_setstr(L, regt, key);
    TValue old;
    int rc = lj_tab_read_current_keyed(regt, tv, &keytv, &old);
    if (rc != LJ_TAB_STORE_CAS_OK) {
      lj_tab_store_wait_l(L);  /* luaL_newmetatable saw stale/FORWARD slot. */
      continue;
    }
    if (tvisnil(&old)) {
      GCtab *mt = lj_tab_new(L, 0, 1);
      TValue tmp;
      settabV(L, &tmp, mt);
      rc = lj_tab_trysetnil_cas_keyed(L, regt, tv, &keytv, &tmp, &old);
      if (rc == LJ_TAB_STORE_CAS_OK) {
	settabV(L, L->top++, mt);
	lj_gc_pubtab(L, regt);
	return 1;
      }
      if (rc != LJ_TAB_STORE_CAS_EXISTS) {
	lj_tab_store_wait_l(L);
	continue;
      }
      copyTV(L, L->top++, &old);
      return 0;
    } else {
      copyTV(L, L->top++, &old);
      return 0;
    }
  }
}

LUA_API int lua_pushthread(lua_State *L)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  int ismain;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  setthreadV(L, L->top, L);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  ismain = (mainthread_acq(G(L)) == L);
  lj_state_dropresumeclaim(&claim);
  return ismain;
}

LUA_API lua_State *lua_newthread(lua_State *L)
{
  lua_State *L1;
  lj_gc_check(L);
  L1 = lj_state_new(L);
  setthreadV(L, L->top, L1);
  incr_top(L);
  return L1;
}

LUA_API void *lua_newuserdata(lua_State *L, size_t size)
{
  LJStateClaim preclaim, claim;
  lua_State *errL;
  GCtab *env;
  GCudata *ud;
  api_checkclaim(L, &preclaim);
  env = getcurrenv(L);
  lj_state_dropclaim(&preclaim);
  errL = api_errstate(L);
  if (size > LJ_MAX_UDATA)
    lj_err_msg(errL, LJ_ERR_UDATAOV);
  ud = lj_udata_new(errL, (MSize)size, env);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  setudataV(L, L->top, ud);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_state_dropresumeclaim(&claim);
  return uddata(ud);
}

LUA_API void lua_concat(lua_State *L, int n)
{
  LJStateClaim claim;
  lj_checkapi_slot(n);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  if (n >= 2) {
    n--;
    do {
      TValue *top = lj_meta_cat(L, L->top-1, -n);
      if (top == NULL) {
	L->top -= n;
	break;
      }
      n -= (int)(L->top - (top - 2*LJ_FR2));
      L->top = top+2;
      api_vm_call_claimed(L, top, 1+1, &claim);
      L->top -= 1+LJ_FR2;
      copyTV(L, L->top-1, L->top+LJ_FR2);
    } while (--n > 0);
  } else if (n == 0) {  /* Push empty string. */
    setstrV(L, L->top, &G(L)->strempty);
    incr_top(L);
  }
  /* else n == 1: nothing to do. */
  lj_state_dropresumeclaim(&claim);
}

/* -- Object getters ------------------------------------------------------ */

LUA_API void lua_gettable(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *t, *v;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  t = index2adr_check_read(L, idx, &snap);
  v = lj_meta_tget(L, t, L->top-1);
  if (v == NULL) {
    L->top += 2;
    api_vm_call_claimed(L, L->top-2, 1+1, &claim);
    L->top -= 2+LJ_FR2;
    v = L->top+1+LJ_FR2;
  }
  copyTV(L, L->top-1, v);
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_getfield(lua_State *L, int idx, const char *k)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *v, *t;
  TValue key;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  t = index2adr_check_read(L, idx, &snap);
  setstrV(L, &key, lj_str_newz(L, k));
  v = lj_meta_tget(L, t, &key);
  if (v == NULL) {
    L->top += 2;
    api_vm_call_claimed(L, L->top-2, 1+1, &claim);
    L->top -= 2+LJ_FR2;
    v = L->top+1+LJ_FR2;
  }
  copyTV(L, L->top, v);
  incr_top(L);
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_rawget(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *t;
  TValue val;
  api_checkclaim(L, &claim);
  t = index2adr_read(L, idx, &snap);
  lj_checkapi(tvistab(t), "stack slot %d is not a table", idx);
  lj_tv_load_acq(&val, lj_tab_get(L, tabV(t), L->top-1));
  copyTV(L, L->top-1, &val);
  lj_state_stack_pubtv(L, L, L->top-1);
  lj_state_dropclaim(&claim);
}

LUA_API void lua_rawgeti(lua_State *L, int idx, int n)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue snap;
  cTValue *v, *t;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  t = index2adr_read(L, idx, &snap);
  lj_checkapi(tvistab(t), "stack slot %d is not a table", idx);
  v = lj_tab_getint(tabV(t), n);
  if (v) {
    TValue val;
    lj_tv_load_acq(&val, v);
    copyTV(L, L->top, &val);
  } else {
    setnilV(L->top);
  }
  lj_state_stack_pubtv(L, L, L->top);
  incr_top(L);
  lj_state_dropresumeclaim(&claim);
}

LUA_API int lua_getmetatable(lua_State *L, int idx)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue snap;
  cTValue *o;
  GCtab *mt = NULL;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  o = index2adr_read(L, idx, &snap);
  if (tvistab(o))
    mt = lj_tab_metatable_acq(tabV(o));
  else if (tvisudata(o))
    mt = lj_udata_metatable_acq(udataV(o));
  else
    mt = lj_basemt_obj_acq(G(L), o);
  if (mt == NULL) {
    lj_state_dropresumeclaim(&claim);
    return 0;
  }
  api_checkstack1_claimed(L, errL, &claim);
  settabV(L, L->top, mt);
  lj_state_stack_pubtv(L, L, L->top);
  incr_top(L);
  lj_state_dropresumeclaim(&claim);
  return 1;
}

LUALIB_API int luaL_getmetafield(lua_State *L, int idx, const char *field)
{
  LJStateClaim preclaim, claim;
  GCstr *key;
  int ok;
  api_checkclaim(L, &preclaim);
  lj_state_dropclaim(&preclaim);
  key = lj_str_newz(api_errstate(L), field);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  ok = api_getmetafield_key_claimed(L, idx, key, &claim);
  lj_state_dropresumeclaim(&claim);
  return ok;
}

static int api_getmetafield_key_claimed(lua_State *L, int idx, GCstr *field,
					LJStateClaim *claim)
{
  TValue snap;
  cTValue *o, *tv;
  GCtab *mt = NULL;
  o = index2adr_read(L, idx, &snap);
  if (tvistab(o))
    mt = lj_tab_metatable_acq(tabV(o));
  else if (tvisudata(o))
    mt = lj_udata_metatable_acq(udataV(o));
  else
    mt = lj_basemt_obj_acq(G(L), o);
  if (mt != NULL && (tv = lj_tab_getstr(mt, field)) != NULL) {
    TValue mtv;
    lj_tv_load_acq(&mtv, tv);
    if (!tvisnil(&mtv)) {
      api_checkstack1_claimed(L, api_errstate(L), claim);
      copyTV(L, L->top, &mtv);
      lj_state_stack_pubtv(L, L, L->top);
      incr_top(L);
      return 1;
    }
  }
  return 0;
}

LUA_API void lua_getfenv(lua_State *L, int idx)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue snap;
  cTValue *o;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  o = index2adr_check_read(L, idx, &snap);
  if (tvisfunc(o)) {
    settabV(L, L->top, lj_func_env_acq(funcV(o)));
  } else if (tvisudata(o)) {
    settabV(L, L->top, lj_udata_env_acq(udataV(o)));
  } else if (tvisthread(o)) {
    LJStateClaim thclaim;
    lua_State *L1 = threadV(o);
    GCtab *env;
    if (!lj_state_tryclaim(L1, lj_thr_current_id(G(L)), &thclaim)) {
      lj_state_dropresumeclaim(&claim);
      lj_err_callermsg(errL, "thread busy");
    }
    env = lj_state_env_acq(L1);
    lj_state_dropclaim(&thclaim);
    settabV(L, L->top, env);
  } else {
    setnilV(L->top);
  }
  lj_state_stack_pubtv(L, L, L->top);
  incr_top(L);
  lj_state_dropresumeclaim(&claim);
}

LUA_API int lua_next(lua_State *L, int idx)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue snap;
  cTValue *t;
  int more;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  t = index2adr_read(L, idx, &snap);
  lj_checkapi(tvistab(t), "stack slot %d is not a table", idx);
  more = lj_tab_next(tabV(t), L->top-1, L->top-1);
  if (more > 0) {
    lj_state_stack_pubtv(L, L, L->top-1);
    lj_state_stack_pubtv(L, L, L->top);
    incr_top(L);  /* Return new key and value slot. */
  } else if (!more) {  /* End of traversal. */
    L->top--;  /* Remove key slot. */
  } else {
    lj_state_dropresumeclaim(&claim);
    lj_err_msg(L, LJ_ERR_NEXTIDX);
  }
  lj_state_dropresumeclaim(&claim);
  return more;
}

LUA_API const char *lua_getupvalue(lua_State *L, int idx, int n)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue snap;
  TValue *val;
  GCobj *o;
  const char *name;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  name = lj_debug_uvnamev(index2adr_read(L, idx, &snap),
			  (uint32_t)(n-1), &val, &o);
  if (name) {
    api_checkstack1_claimed(L, errL, &claim);
    lj_tv_load_acq(L->top, val);
    lj_state_stack_pubtv(L, L, L->top);
    incr_top(L);
  }
  lj_state_dropresumeclaim(&claim);
  return name;
}

LUA_API void *lua_upvalueid(lua_State *L, int idx, int n)
{
  LJStateClaim claim;
  TValue snap;
  GCfunc *fn;
  void *id;
  api_checkclaim(L, &claim);
  fn = funcV(index2adr_read(L, idx, &snap));
  n--;
  if (isluafunc(fn)) {
    lj_checkapi((uint32_t)n < fn->l.nupvalues, "bad upvalue %d", n+1);
    id = (void *)func_uvptr_acq(&fn->l, (uint32_t)n);
  } else {
    lj_checkapi((uint32_t)n < fn->c.nupvalues, "bad upvalue %d", n+1);
    id = (void *)&fn->c.upvalue[n];
  }
  lj_state_dropclaim(&claim);
  return id;
}

LUA_API void lua_upvaluejoin(lua_State *L, int idx1, int n1, int idx2, int n2)
{
  TValue snap1, snap2;
  GCfunc *fn1 = funcV(index2adr_read(L, idx1, &snap1));
  GCfunc *fn2 = funcV(index2adr_read(L, idx2, &snap2));
  n1--; n2--;
  lj_checkapi(isluafunc(fn1), "stack slot %d is not a Lua function", idx1);
  lj_checkapi(isluafunc(fn2), "stack slot %d is not a Lua function", idx2);
  lj_checkapi((uint32_t)n1 < fn1->l.nupvalues, "bad upvalue %d", n1+1);
  lj_checkapi((uint32_t)n2 < fn2->l.nupvalues, "bad upvalue %d", n2+1);
  {
    GCobj *uv = func_uvptr_acq(&fn2->l, (uint32_t)n2);
    GCobj *old = func_uvptr_acq(&fn1->l, (uint32_t)n1);
    if (old != uv) {
      api_trace_flush_mutation(L);
      setgcrefrel(fn1->l.uvptr[n1], uv);
      lj_gc_pubobjobj(L, fn1, uv);
    }
  }
}

LUALIB_API void *luaL_testudata(lua_State *L, int idx, const char *tname)
{
  LJStateClaim claim;
  TValue snap;
  cTValue *o;
  GCstr *key;
  void *p = NULL;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (!tvisudata(o)) {
    lj_state_dropclaim(&claim);
    return NULL;
  }
  lj_state_dropclaim(&claim);
  key = lj_str_newz(api_errstate(L), tname);
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (tvisudata(o)) {
    GCudata *ud = udataV(o);
    cTValue *tv = lj_tab_getstr(lj_registry_tab_acq(G(L)), key);
    if (tv) {
      TValue mtv;
      lj_tv_load_acq(&mtv, tv);
      if (tvistab(&mtv) && tabV(&mtv) == lj_udata_metatable_acq(ud))
	p = uddata(ud);
    }
  }
  lj_state_dropclaim(&claim);
  return p;  /* NULL if value is not a userdata with a matching metatable. */
}

LUALIB_API void *luaL_checkudata(lua_State *L, int idx, const char *tname)
{
  void *p = luaL_testudata(L, idx, tname);
  if (!p) lj_err_argtype(L, idx, tname);
  return p;
}

/* -- Object setters ------------------------------------------------------ */

LUA_API void lua_settable(lua_State *L, int idx)
{
  LJStateClaim claim;
  TValue *o;
  TValue snap;
  cTValue *t;
  GCtab *owner;
  lj_checkapi_slot(2);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  t = index2adr_check_read(L, idx, &snap);
  for (;;) {
    o = lj_meta_tset_owner(L, t, L->top-2, &owner);
    if (o) {
      TValue *key = L->top-2, *val = L->top-1;
      int weakwr = lj_gc2_weak_write_begin(L, owner);
      int rc;
      if (weakwr)
	lj_gc2_barrier_weak_write(L, owner, key, val);
      rc = lj_tab_trystoretv_cas_keyed(L, owner, o, key, val);
      if (weakwr) {
	lj_gc2_barrier_weak_write(L, owner, key, val);
	lj_gc2_barrier_tv_pair(L, obj2gco(owner), val);
	lj_gc2_weak_write_end(L, weakwr);
      }
      if (rc == LJ_TAB_STORE_CAS_OK) {
	if (!weakwr) {
	  lj_gc2_barrier_weak_write(L, owner, key, val);
	  lj_gc2_barrier_tv_pair(L, obj2gco(owner), o);
	}
	L->top = key;
	lj_state_dropresumeclaim(&claim);
	return;
      }
      lj_tab_store_wait_l(L);  /* C API settable saw stale/FORWARD slot. */
    } else {
      TValue *base = L->top;
      copyTV(L, base+2, base-3-2*LJ_FR2);
      L->top = base+3;
      api_vm_call_claimed(L, base, 0+1, &claim);
      L->top -= 3+LJ_FR2;
      lj_state_dropresumeclaim(&claim);
      return;
    }
  }
}

LUA_API void lua_setfield(lua_State *L, int idx, const char *k)
{
  LJStateClaim claim;
  TValue *o;
  TValue key;
  TValue snap;
  cTValue *t;
  GCtab *owner;
  lj_checkapi_slot(1);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  t = index2adr_check_read(L, idx, &snap);
  setstrV(L, &key, lj_str_newz(L, k));
  for (;;) {
    o = lj_meta_tset_owner(L, t, &key, &owner);
    if (o) {
      TValue *val = L->top-1;
      int weakwr = lj_gc2_weak_write_begin(L, owner);
      int rc;
      if (weakwr)
	lj_gc2_barrier_weak_write(L, owner, &key, val);
      rc = lj_tab_trystoretv_cas_keyed(L, owner, o, &key, val);
      if (weakwr) {
	lj_gc2_barrier_weak_write(L, owner, &key, val);
	lj_gc2_barrier_tv_pair(L, obj2gco(owner), val);
	lj_gc2_weak_write_end(L, weakwr);
      }
      if (rc == LJ_TAB_STORE_CAS_OK) {
	if (!weakwr) {
	  lj_gc2_barrier_weak_write(L, owner, &key, val);
	  lj_gc2_barrier_tv_pair(L, obj2gco(owner), o);
	}
	L->top = val;
	lj_state_dropresumeclaim(&claim);
	return;
      }
      lj_tab_store_wait_l(L);  /* C API setfield saw stale/FORWARD slot. */
    } else {
      TValue *base = L->top;
      copyTV(L, base+2, base-3-2*LJ_FR2);
      L->top = base+3;
      api_vm_call_claimed(L, base, 0+1, &claim);
      L->top -= 2+LJ_FR2;
      lj_state_dropresumeclaim(&claim);
      return;
    }
  }
}

LUA_API void lua_rawset(lua_State *L, int idx)
{
  TValue snap;
  GCtab *t = tabV(index2adr_read(L, idx, &snap));
  TValue *dst, *key;
  int barrier_done = 0;
  lj_checkapi_slot(2);
  key = L->top-2;
  for (;;) {
    int weakwr, rc;
    dst = lj_tab_set(L, t, key);
    weakwr = lj_gc2_weak_write_begin(L, t);
    if (weakwr)
      lj_gc2_barrier_weak_write(L, t, key, key+1);
    rc = lj_tab_trystoretv_cas_keyed(L, t, dst, key, key+1);
    if (weakwr) {
      lj_gc2_barrier_weak_write(L, t, key, key+1);
      lj_gc2_barrier_tv_pair(L, obj2gco(t), key+1);
      lj_gc_pubtab(L, t);
      lj_gc2_weak_write_end(L, weakwr);
      if (rc == LJ_TAB_STORE_CAS_OK)
	barrier_done = 1;
    }
    if (rc == LJ_TAB_STORE_CAS_OK)
      break;
    lj_tab_store_wait_l(L);  /* C API rawset saw stale/FORWARD slot. */
  }
  if (!barrier_done) {
    lj_gc2_barrier_weak_write(L, t, key, key+1);
    lj_gc_pubtab(L, t);
  }
  L->top = key;
}

LUA_API void lua_rawseti(lua_State *L, int idx, int n)
{
  TValue snap;
  GCtab *t = tabV(index2adr_read(L, idx, &snap));
  TValue *dst, *src;
  TValue key;
  int barrier_done = 0;
  lj_checkapi_slot(1);
  src = L->top-1;
  setintV(&key, n);
  for (;;) {
    int weakwr, rc;
    dst = lj_tab_setint(L, t, n);
    weakwr = lj_gc2_weak_write_begin(L, t);
    if (weakwr)
      lj_gc2_barrier_weak_write(L, t, &key, src);
    rc = lj_tab_trystoretv_cas_keyed(L, t, dst, &key, src);
    if (weakwr) {
      lj_gc2_barrier_weak_write(L, t, &key, src);
      lj_gc2_barrier_tv_pair(L, obj2gco(t), src);
      lj_gc_pubtabtv(L, t, dst);
      lj_gc2_weak_write_end(L, weakwr);
      if (rc == LJ_TAB_STORE_CAS_OK)
	barrier_done = 1;
    }
    if (rc == LJ_TAB_STORE_CAS_OK)
      break;
    lj_tab_store_wait_l(L);  /* C API rawseti saw stale/FORWARD slot. */
  }
  if (!barrier_done) {
    lj_gc2_barrier_weak_write(L, t, &key, src);
    lj_gc_pubtabtv(L, t, dst);
  }
  L->top = src;
}

LUA_API int lua_setmetatable(lua_State *L, int idx)
{
  global_State *g;
  GCtab *mt;
  TValue snap;
  cTValue *o = index2adr_check_read(L, idx, &snap);
  lj_checkapi_slot(1);
  if (tvisnil(L->top-1)) {
    mt = NULL;
  } else {
    lj_checkapi(tvistab(L->top-1), "top stack slot is not a table");
    mt = tabV(L->top-1);
  }
  g = G(L);
  if (mt)
    lj_tab_nomm_rel(mt, 0);  /* Do not trust stale metamethod miss caches. */
  if (tvistab(o)) {
    lj_tab_metatable_rel(tabV(o), mt);
    if (mt)
      lj_gc_pubtabobj(L, tabV(o), mt);
  } else if (tvisudata(o)) {
    GCudata *ud = udataV(o);
    GCtab *oldmt = lj_udata_metatable_acq(ud);
    TValue oldv, newv;
    int oldfin = lj_meta_fasttv(g, oldmt, MM_gc, &oldv) != NULL;
    int newfin = lj_meta_fasttv(g, mt, MM_gc, &newv) != NULL;
    if (mt)
      lj_gc2_finreg_udata_register(L, g, obj2gco(ud));
    lj_udata_metatable_rel(ud, mt);
    if (mt)
      lj_gc_pubobjobj(L, ud, mt);
    if (newfin) {
      (void)lj_gc2_finreg_udata_set(g, obj2gco(ud), 1);
    } else if (oldfin) {
      if (lj_gc2_finreg_udata_set(g, obj2gco(ud), 0) < 0)
	lj_gc2_finreg_udata_forget(g, obj2gco(ud));
    }
  } else {
    /* Flush cache, since traces specialize to basemt. But not during __gc. */
    if (lj_trace_flushall_hs(L))
      lj_err_caller(L, LJ_ERR_NOGCMM);
    o = index_iscupvalue(idx) ? &snap :
	index2adr(L, idx);  /* Stack may have been reallocated. */
    if (tvisbool(o)) {
      /* NOBARRIER: basemt is a GC root. */
      lj_basemt_it_rel(g, LJ_TTRUE, mt);
      lj_basemt_it_rel(g, LJ_TFALSE, mt);
    } else {
      /* NOBARRIER: basemt is a GC root. */
      lj_basemt_obj_rel(g, o, mt);
    }
  }
  L->top--;
  return 1;
}

LUALIB_API void luaL_setmetatable(lua_State *L, const char *tname)
{
  lua_getfield(L, LUA_REGISTRYINDEX, tname);
  lua_setmetatable(L, -2);
}

LUA_API int lua_setfenv(lua_State *L, int idx)
{
  TValue snap;
  cTValue *o = index2adr_check_read(L, idx, &snap);
  LJStateClaim claim;
  GCtab *t;
  lj_checkapi_slot(1);
  lj_checkapi(tvistab(L->top-1), "top stack slot is not a table");
  t = tabV(L->top-1);
  if (tvisfunc(o)) {
    GCfunc *fn = funcV(o);
    api_trace_flush_mutation(L);
    lj_func_env_rel(fn, t);
    lj_gc_pubobjobj(L, fn, t);
  } else if (tvisudata(o)) {
    GCudata *ud = udataV(o);
    lj_udata_env_rel(ud, t);
    lj_gc_pubobjobj(L, ud, t);
  } else if (tvisthread(o)) {
    lua_State *L1 = threadV(o);
    api_trace_flush_mutation(L);
    if (!lj_state_tryclaim(L1, lj_thr_current_id(G(L)), &claim))
      lj_err_callermsg(api_errstate(L), "thread busy");
    lj_state_env_rel(L1, t);
    lj_state_dropclaim(&claim);
    lj_gc_pubobjobj(L, obj2gco(L1), t);
  } else {
    L->top--;
    return 0;
  }
  L->top--;
  return 1;
}

LUA_API const char *lua_setupvalue(lua_State *L, int idx, int n)
{
  TValue snap;
  cTValue *f = index2adr_read(L, idx, &snap);
  TValue *val;
  GCobj *o;
  const char *name;
  lj_checkapi_slot(1);
  name = lj_debug_uvnamev(f, (uint32_t)(n-1), &val, &o);
  if (name) {
    if (o->gch.gct == ~LJ_TFUNC && !isluafunc(gco2func(o)))
      api_trace_flush_mutation(L);
    else if (o->gch.gct == ~LJ_TUPVAL &&
	     tv_rawload_acq(val) != tv_rawload(L->top-1))
      api_trace_flush_mutation(L);
    L->top--;
    copyTVrel(L, val, L->top);
    lj_gc_pubobjtv(L, o, L->top);
  }
  return name;
}

/* -- Calls --------------------------------------------------------------- */

#if LJ_FR2
static TValue *api_call_base(lua_State *L, int nargs)
{
  TValue *o = L->top, *base = o - nargs;
  L->top = o+1;
  for (; o > base; o--) copyTV(L, o, o-1);
  setnilV(o);
  return o+1;
}
#else
#define api_call_base(L, nargs)	(L->top - (nargs))
#endif

LUA_API void lua_call(lua_State *L, int nargs, int nresults)
{
  LJStateClaim claim;
  lj_checkapi(L->status == LUA_OK || L->status == LUA_ERRERR,
	      "thread called in wrong state %d", L->status);
  lj_checkapi_slot(nargs+1);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  if (claim.release) {
    global_State *g = G(L);
    uint8_t oldh = hook_save(g);
    int status = lj_vm_pcall(L, api_call_base(L, nargs), nresults+1, 0);
    if (status)
      hook_restore(g, oldh);
    lj_state_dropresumeclaim(&claim);
    if (status)
      lj_err_throw(L, status);
  } else {
    lj_vm_call(L, api_call_base(L, nargs), nresults+1);
    lj_state_dropresumeclaim(&claim);
  }
}

LUA_API int lua_pcall(lua_State *L, int nargs, int nresults, int errfunc)
{
  global_State *g = G(L);
  uint8_t oldh = hook_save(g);
  LJStateClaim claim;
  ptrdiff_t ef;
  int status;
  lj_checkapi(L->status == LUA_OK || L->status == LUA_ERRERR,
	      "thread called in wrong state %d", L->status);
  lj_checkapi_slot(nargs+1);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(g), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  if (errfunc == 0) {
    ef = 0;
  } else {
    cTValue *o = index2adr_stack(L, errfunc);
    ef = savestack(L, o);
  }
  status = lj_vm_pcall(L, api_call_base(L, nargs), nresults+1, ef);
  if (status) hook_restore(g, oldh);
  lj_state_dropresumeclaim(&claim);
  return status;
}

static TValue *cpcall(lua_State *L, lua_CFunction func, void *ud)
{
  GCfunc *fn = lj_func_newC(L, 0, getcurrenv(L));
  TValue *top = L->top;
  fn->c.f = func;
  setfuncV(L, top++, fn);
  if (LJ_FR2) setnilV(top++);
#if LJ_64
  ud = lj_lightud_intern(L, ud);
#endif
  setrawlightudV(top++, ud);
  cframe_nres(L->cframe) = 1+0;  /* Zero results. */
  L->top = top;
  return top-1;  /* Now call the newly allocated C function. */
}

LUA_API int lua_cpcall(lua_State *L, lua_CFunction func, void *ud)
{
  global_State *g = G(L);
  uint8_t oldh = hook_save(g);
  LJStateClaim claim;
  int status;
  lj_checkapi(L->status == LUA_OK || L->status == LUA_ERRERR,
	      "thread called in wrong state %d", L->status);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(g), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  status = lj_vm_cpcall(L, func, ud, cpcall);
  if (status) hook_restore(g, oldh);
  lj_state_dropresumeclaim(&claim);
  return status;
}

LUALIB_API int luaL_callmeta(lua_State *L, int idx, const char *field)
{
  LJStateClaim preclaim, claim;
  GCstr *key;
  api_checkclaim(L, &preclaim);
  lj_state_dropclaim(&preclaim);
  key = lj_str_newz(api_errstate(L), field);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  if (api_getmetafield_key_claimed(L, idx, key, &claim)) {
    TValue snap;
    TValue *top = L->top--;
    if (LJ_FR2) setnilV(top++);
    copyTV(L, top++, index2adr_read(L, idx, &snap));
    L->top = top;
    api_vm_call_claimed(L, top-1, 1+1, &claim);
    lj_state_dropresumeclaim(&claim);
    return 1;
  }
  lj_state_dropresumeclaim(&claim);
  return 0;
}

/* -- Coroutine yield and resume ------------------------------------------ */

LUA_API int lua_isyieldable(lua_State *L)
{
  return cframe_canyield(L->cframe);
}

LUA_API int lua_yield(lua_State *L, int nresults)
{
  void *cf = L->cframe;
  global_State *g = G(L);
  if (cframe_canyield(cf)) {
    cf = cframe_raw(cf);
    if (!hook_active(g)) {  /* Regular yield: move results down if needed. */
      cTValue *f = L->top - nresults;
      if (f > L->base) {
	TValue *t = L->base;
	while (--nresults >= 0) copyTV(L, t++, f++);
	L->top = t;
      }
      L->cframe = NULL;
      L->status = LUA_YIELD;
      return -1;
    } else {  /* Yield from hook: add a pseudo-frame. */
      TValue *top = L->top;
      hook_leave(g);
      (top++)->u64 = cframe_multres(cf);
      setcont(top, lj_cont_hook);
      if (LJ_FR2) top++;
      setframe_pc(top, cframe_pc(cf)-1);
      top++;
      setframe_gc(top, obj2gco(L), LJ_TTHREAD);
      if (LJ_FR2) top++;
      setframe_ftsz(top, ((char *)(top+1)-(char *)L->base)+FRAME_CONT);
      L->top = L->base = top+1;
#if ((defined(__GNUC__) || defined(__clang__)) && (LJ_TARGET_X64 || defined(LUAJIT_UNWIND_EXTERNAL)) && !LJ_NO_UNWIND) || LJ_TARGET_WINDOWS
      lj_err_throw(L, LUA_YIELD);
#else
      L->cframe = NULL;
      L->status = LUA_YIELD;
      lj_vm_unwind_c(cf, LUA_YIELD);
#endif
    }
  }
  lj_err_msg(L, LJ_ERR_CYIELD);
  return 0;  /* unreachable */
}

static TValue *api_resume_invalid_cp(lua_State *L, lua_CFunction dummy,
				     void *ud)
{
  UNUSED(dummy); UNUSED(ud);
  L->top = L->base;
  setstrV(L, L->top, lj_err_str(L, LJ_ERR_COSUSP));
  incr_top(L);
  return NULL;
}

LUA_API int lua_resume(lua_State *L, int nargs)
{
  LJStateClaim claim;
  int status;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  if (L->cframe == NULL && L->status <= LUA_YIELD) {
    status = lj_vm_resume(L,
      L->status == LUA_OK ? api_call_base(L, nargs) : L->top - nargs,
      0, 0);
  } else {
    ptrdiff_t oldtop = savestack(L, L->top);
    int errcode = lj_vm_cpcall(L, NULL, NULL, api_resume_invalid_cp);
    if (LJ_UNLIKELY(errcode)) {
      L->top = restorestack(L, oldtop);
      lj_state_dropresumeclaim(&claim);
      lj_err_throw(L, errcode);
    }
    status = LUA_ERRRUN;
  }
  lj_state_dropresumeclaim(&claim);
  return status;
}

/* -- GC and memory management -------------------------------------------- */

static void api_gc_setlogical(global_State *g, GCSize threshold)
{
  uint64_t soft = ~(uint64_t)0;
  if (threshold != LJ_MAX_MEM) {
    GCSize total = lj_gc_total_load(g);
    soft = (uint64_t)total > soft - LJ_GC2_HELPER_IDLE_STEP ?
	   soft : (uint64_t)total + LJ_GC2_HELPER_IDLE_STEP;
  }
  lj_gc2_helper_soft_limit_store(g, soft);
  if (mt_live_acq(g) != 0) {
    lj_gc_mt_threshold_store(g, threshold);
    if (mt_live_acq(g) == 0)
      lj_gc_threshold_store(g, threshold);
  } else {
    lj_gc_threshold_store(g, threshold);
  }
}

static GCSize api_gc_restart_threshold(global_State *g)
{
  GCSize total = lj_gc_total_load(g);
  return (total/100) * g->gc.pause;
}

static int api_gc_enterexclusive(global_State *g)
{
  uint32_t expect = 0;
  if (mt_live_acq(g) != 0)
    return 0;
  if (!mt_gc_exclusive_cas(g, &expect, 1))
    return 0;
  if (mt_live_acq(g) != 0) {
    mt_gc_exclusive_rel(g, 0);
    return 0;
  }
  return 1;
}

static void api_gc_leaveexclusive(global_State *g)
{
  mt_gc_exclusive_rel(g, 0);
  mt_gc_exclusive_futex_wake(g, INT_MAX);
}

LUA_API int lua_gc(lua_State *L, int what, int data)
{
  global_State *g = G(L);
  int res = 0;
  switch (what) {
  case LUA_GCSTOP:
    api_gc_setlogical(g, LJ_MAX_MEM);
    break;
  case LUA_GCRESTART:
    {
      api_gc_setlogical(g, data == -1 ? api_gc_restart_threshold(g) :
			lj_gc_total_load(g));
    }
    break;
  case LUA_GCCOLLECT:
    if (api_gc_enterexclusive(g)) {
      lj_gc_fullgc(L);
      api_gc_setlogical(g, api_gc_restart_threshold(g));
      api_gc_leaveexclusive(g);
    } else if (mt_live_acq(g) != 0) {
      (void)lj_gc2_collect_active(L);
      api_gc_setlogical(g, api_gc_restart_threshold(g));
    }
    break;
  case LUA_GCCOUNT:
    res = (int)(lj_gc_total_load(g) >> 10);
    break;
  case LUA_GCCOUNTB:
    res = (int)(lj_gc_total_load(g) & 0x3ff);
    break;
  case LUA_GCSTEP: {
    GCSize a = (GCSize)data << 10;
    GCSize total;
    if (!api_gc_enterexclusive(g)) {
      if (mt_live_acq(g) != 0) {
	if (lj_gc2_request_cycle_explicit(g, L2TG(L)))
	  lj_gc2_mark_begin(g);
	(void)lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH);
      }
      break;  /* Active MT steps request/assist GC2 but don't complete it. */
    }
    total = lj_gc_total_load(g);
    lj_gc_threshold_store(g, (a <= total) ? (total - a) : 0);
    while (lj_gc_total_load(g) >= lj_gc_threshold_load(g))
      if (lj_gc_step_explicit(L) > 0) {
	res = 1;
	break;
      }
    api_gc_leaveexclusive(g);
    break;
  }
  case LUA_GCSETPAUSE:
    res = (int)(g->gc.pause);
    g->gc.pause = (MSize)data;
    gc2_gcpause_pct_rel(g, data > 0 ? (uint32_t)data : 1u);
    lj_gc2_update_pacing(g);
    lj_gc2_publish_idle_threshold(g);
    break;
  case LUA_GCSETSTEPMUL:
    res = (int)(g->gc.stepmul);
    g->gc.stepmul = (MSize)data;
    gc2_assist_shift_rel(g, lj_gc2_assist_shift_from_stepmul((uint32_t)data));
    break;
  case LUA_GCISRUNNING:
    res = ((mt_live_acq(g) != 0 ? lj_gc_mt_threshold_load(g) :
	    lj_gc_threshold_load(g)) != LJ_MAX_MEM);
    break;
  default:
    res = -1;  /* Invalid option. */
  }
  return res;
}

LUA_API lua_Alloc lua_getallocf(lua_State *L, void **ud)
{
  global_State *g = G(L);
  if (ud) *ud = g->allocd;
  return g->allocf;
}

LUA_API void lua_setallocf(lua_State *L, lua_Alloc f, void *ud)
{
  global_State *g = G(L);
  g->allocd = ud;
  g->allocf = f;
}
