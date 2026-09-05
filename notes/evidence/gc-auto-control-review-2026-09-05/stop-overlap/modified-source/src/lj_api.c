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
#include "lj_arena.h"
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

static LJ_AINLINE int api_tv_on_stack(lua_State *L, cTValue *tv)
{
  uintptr_t p, lo, hi;
  if (!L || !tv)
    return 0;
  p = (uintptr_t)(const void *)tv;
  lo = (uintptr_t)(const void *)tvref(L->stack);
  hi = (uintptr_t)(const void *)tvref(L->maxstack);
  return p >= lo && p < hi;
}

/* Materialize a thread/function environment edge in an enumerated root.
** Source SMR begins before the GCRef load and the exact child lease is retained
** until after release publication, closing replacement-to-reuse ABA in
** LUA_GLOBALSINDEX/LUA_ENVIRONINDEX lookups. `fn == NULL` selects L->env; an
** executing C closure is itself retained by L's claimed frame chain. A stack
** destination must already be below L->top before this function is called. */
static TValue *index2adr_envcapture(lua_State *L, GCfunc *fn, TValue *root,
				    int stackroot)
{
  ptrdiff_t rootofs = stackroot ? savestack(L, root) : 0;
  for (;;) {
    LJGC2Lease lease;
    TValue snap;
    TValue *curroot = stackroot ? restorestack(L, rootofs) : root;
    GCtab *env;
    int status;
    if (LJ_UNLIKELY(!lj_gc2_smr_read_try(G(L)))) {
      lj_tab_wait_l(L);
      continue;
    }
    env = fn ? lj_func_env_acq(fn) : lj_state_env_acq(L);
    if (!env) {
      setnilV(&snap);
      copyTVrel(L, curroot, &snap);
      lj_gc2_smr_read_leave(G(L));
      if (stackroot) {
	lj_state_stack_pubtv(L, L, curroot);
	return restorestack(L, rootofs);
      }
      return root;
    }
    setgcVraw(&snap, obj2gco(env), LJ_TTAB);
    status = lj_gc2_tv_lease_acquire(G(L), &snap, &lease);
    if (status == LJ_GC2_TV_EDGE_VALID) {
      copyTVrel(L, curroot, &snap);
      lj_gc2_smr_read_leave(G(L));
      if (stackroot)
	lj_state_stack_pubtv(L, L, curroot);
      else
	lj_gc_pubroot(L, curroot);
      lj_gc2_lease_release(&lease);
      return stackroot ? restorestack(L, rootofs) : root;
    }
    lj_gc2_smr_read_leave(G(L));
    lj_tab_wait_l(L);
  }
}

static TValue *index2adr_envroot(lua_State *L, GCfunc *fn)
{
  return index2adr_envcapture(L, fn, &L2TG(L)->tmptv, 0);
}

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
    return index2adr_envroot(L, NULL);
  } else if (idx == LUA_REGISTRYINDEX) {
    return registry(L);
  } else {
    GCfunc *fn = curr_func(L);
    lj_checkapi(fn->c.gct == ~LJ_TFUNC && !isluafunc(fn),
		"calling frame is not a C function");
    if (idx == LUA_ENVIRONINDEX) {
      return index2adr_envroot(L, fn);
    } else {
      idx = LUA_GLOBALSINDEX - idx;
      return (uint32_t)idx <= lj_funcC_nupvalues(&fn->c) ?
	     &fn->c.upvalue[idx-1] : niltv(L);
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

static LJ_AINLINE void api_trace_flush_mutation_claimed(lua_State *L,
							lua_State *errL,
							LJStateClaim *claim)
{
  if (lj_trace_hasany(G(L)) && lj_trace_flushall_hs(L)) {
    lj_state_dropresumeclaim(claim);
    lj_err_caller(errL, LJ_ERR_NOGCMM);
  }
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

typedef struct ApiGCRoot {
  TGState *tg;
  uint32_t idx;
} ApiGCRoot;

static void api_gcroot_release(ApiGCRoot *root)
{
  if (!root || !root->tg)
    return;
  lj_tg_root_anchor_pop(root->tg, root->idx);
  root->tg = NULL;
  root->idx = 0;
}

static TValue *api_gcroot_push_reserved(lua_State *L, cTValue *tv,
					ApiGCRoot *root)
{
  TValue *slot;
  TGState *tg = L2TG(L);
  uint32_t idx;
  lj_assertL(root != NULL && tg != NULL,
	     "API GC root without an owner TG");
  slot = lj_tg_root_anchor_push(L, tg, tv, &idx);
  lj_assertL(slot != NULL, "reserved API GC root allocation failed");
  root->tg = tg;
  root->idx = idx;
  lj_gc_pubroot(L, slot);
  return slot;
}

/* Capture an edge whose storage address is stable for this operation (for
** example a claimed stack/upvalue slot or the global registry root). Push a
** nil anchor first, so the allocation/debug hook cannot observe an unretained
** candidate. Source SMR then starts before the TValue load and the exact
** allocation lease remains live while the already-enumerated fixed anchor
** slot is release-filled and barrier-published. This closes both
** replacement-to-reuse ABA and the non-JIT TG.tmptv root gap. The caller must
** reserve the next anchor slot first. */
static TValue *api_gcroot_capture_edge_reserved(lua_State *L,
						cTValue *edge,
						ApiGCRoot *root)
{
  TValue nilv;
  TValue *slot;
  int edgestack = api_tv_on_stack(L, edge);
  ptrdiff_t edgeofs = edgestack ?
			   savestack(L, (TValue *)(void *)edge) : 0;
  setnilV(&nilv);
  slot = api_gcroot_push_reserved(L, &nilv, root);
  for (;;) {
    LJGC2Lease lease;
    TValue snap;
    cTValue *curedge = edgestack ? restorestack(L, edgeofs) : edge;
    int status;
    if (LJ_UNLIKELY(!lj_gc2_smr_read_try(G(L)))) {
      lj_tab_wait_l(L);
      continue;
    }
    lj_tv_load_acq(&snap, curedge);
    status = lj_gc2_tv_lease_acquire(G(L), &snap, &lease);
    if (status == LJ_GC2_TV_EDGE_VALID) {
      slot = lj_tg_root_anchor_slot_acq(root->tg, root->idx);
      lj_assertL(slot != NULL, "lost reserved API capture root");
      copyTVrel(L, slot, &snap);
      lj_gc2_smr_read_leave(G(L));
      lj_gc_pubroot(L, slot);
      lj_gc2_lease_release(&lease);
      return slot;
    }
    lj_gc2_smr_read_leave(G(L));
    /* STALE can be the old side of a raced root replacement; reacquire the
    ** root rather than publishing that unretained incarnation as nil/data. */
    lj_tab_wait_l(L);
  }
}

/* Transfer a stable authoritative edge into an already-enumerated stack slot.
** Source SMR precedes the edge load and the exact child lease remains live
** until release-publication, so mutable registry/upvalue roots
** cannot cross replacement-to-reuse ABA. No durable anchor or cleanup action
** survives a catchable error. */
static TValue *api_stackroot_capture_edge(lua_State *L, cTValue *edge,
					   TValue *out)
{
  int edgestack = api_tv_on_stack(L, edge);
  ptrdiff_t edgeofs = edgestack ?
			   savestack(L, (TValue *)(void *)edge) : 0;
  ptrdiff_t outofs;
  lj_assertL(api_tv_on_stack(L, out),
	     "API stack-root destination is not on the stack");
  outofs = savestack(L, out);
  for (;;) {
    LJGC2Lease lease;
    TValue snap;
    cTValue *curedge = edgestack ? restorestack(L, edgeofs) : edge;
    TValue *curout = restorestack(L, outofs);
    int status;
    if (LJ_UNLIKELY(!lj_gc2_smr_read_try(G(L)))) {
      lj_tab_wait_l(L);
      continue;
    }
    lj_tv_load_acq(&snap, curedge);
    status = lj_gc2_tv_lease_acquire(G(L), &snap, &lease);
    if (status == LJ_GC2_TV_EDGE_VALID) {
      copyTVrel(L, curout, &snap);
      lj_gc2_smr_read_leave(G(L));
      lj_state_stack_pubtv(L, L, curout);
      lj_gc2_lease_release(&lease);
      return restorestack(L, outofs);
    }
    lj_gc2_smr_read_leave(G(L));
    lj_tab_wait_l(L);
  }
}

static LJ_AINLINE int api_index_is_envroot(int idx)
{
  return idx == LUA_GLOBALSINDEX || idx == LUA_ENVIRONINDEX;
}

/* Capture a pseudo-index from its authoritative mutable GCRef, never from the
** shared TG scratch returned by index2adr(). The current C closure is retained
** by the claimed frame while its environment edge is admitted. */
static TValue *api_stackroot_capture_envindex(lua_State *L, int idx,
					       TValue *out)
{
  GCfunc *fn = NULL;
  lj_assertL(api_index_is_envroot(idx), "bad API environment pseudo-index");
  if (idx == LUA_ENVIRONINDEX) {
    fn = curr_func(L);
    lj_checkapi(fn->c.gct == ~LJ_TFUNC && !isluafunc(fn),
		"calling frame is not a C function");
  }
  return index2adr_envcapture(L, fn, out, 1);
}

/* Push an exact indexed value as a natural stack root. Ordinary negative
** indices are resolved before top moves, preserving receiver/key/value
** self-aliasing. Environment pseudo-indices bypass TG scratch and transfer
** directly from their authoritative mutable source. The caller has already
** reserved one stack slot. */
static TValue *api_stackroot_push_index(lua_State *L, int idx)
{
  TValue *edge = NULL;
  TValue *out;
  ptrdiff_t edgeofs = 0, outofs;
  int edgestack = 0;
  if (!api_index_is_envroot(idx)) {
    edge = index2adr(L, idx);
    edgestack = api_tv_on_stack(L, edge);
    if (edgestack)
      edgeofs = savestack(L, edge);
  }
  out = L->top;
  outofs = savestack(L, out);
  setnilV(out);
  lj_state_stack_pubtv(L, L, out);
  L->top++;
  out = restorestack(L, outofs);
  if (api_index_is_envroot(idx))
    return api_stackroot_capture_envindex(L, idx, out);
  if (edgestack)
    edge = restorestack(L, edgeofs);
  return api_stackroot_capture_edge(L, edge, out);
}

static uint32_t api_envroot_claimed(lua_State *errL, GCtab *env,
				    LJStateClaim *claim, ApiGCRoot *root)
{
  TValue envv;
  if (LJ_UNLIKELY(!lj_tg_root_anchor_reserve_nothrow(errL, L2TG(errL)))) {
    lj_state_dropclaim(claim);
    lj_err_mem(errL);
  }
  if (env)
    settabV(errL, &envv, env);
  else
    setnilV(&envv);
  (void)api_gcroot_push_reserved(errL, &envv, root);
  return root->idx;
}

static GCstr *api_str_new_rooted(lua_State *L, const char *str, size_t len,
				 ApiGCRoot *root)
{
  TValue tv;
  GCstr *s;
  if (LJ_UNLIKELY(!lj_tg_root_anchor_reserve_nothrow(L, L2TG(L))))
    lj_err_mem(L);
  s = lj_str_new(L, str, len);
  setstrV(L, &tv, s);
  (void)api_gcroot_push_reserved(L, &tv, root);
  return s;
}

static GCstr *api_str_newz_rooted(lua_State *L, const char *str,
				  ApiGCRoot *root)
{
  return api_str_new_rooted(L, str, strlen(str), root);
}

static void api_gcroot_replace(lua_State *L, ApiGCRoot *root, cTValue *tv)
{
  TValue *slot = lj_tg_root_anchor_slot_acq(root->tg, root->idx);
  lj_assertL(slot != NULL, "missing API GC root slot");
  copyTVrel(L, slot, tv);
  lj_gc_pubroot(L, slot);
}

static void index2adr_storestr_noflush(lua_State *L, int idx, TValue *o,
				      GCstr *s)
{
  if (index_iscupvalue(idx)) {
    TValue tv;
    setstrV(L, &tv, s);
    o = index2adr(L, idx);
    copyTVrel(L, o, &tv);
    lj_gc_pubobjtv(L, curr_func(L), &tv);
  } else {
    setstrV(L, o, s);
    lj_state_stack_pubtv(L, L, o);
  }
}

static GCstr *api_tolstring_claimed(lua_State *L, int idx, int *status)
{
  for (;;) {
    LJStateClaim claim;
    ApiGCRoot root = { NULL, 0 };
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
      TValue strtv;
      lj_gc_check(errL);
      if (LJ_UNLIKELY(!lj_tg_root_anchor_reserve_nothrow(errL,
							 L2TG(errL))))
	lj_err_mem(errL);
      s = lj_strfmt_number(errL, &numtv);
      setstrV(errL, &strtv, s);
      (void)api_gcroot_push_reserved(errL, &strtv, &root);
    }

    api_checkclaim(L, &claim);
    o = index2adr_read(L, idx, &snap);
    if (tvisstr(o)) {
      s = strV(o);
      lj_state_dropclaim(&claim);
      api_gcroot_release(&root);
      *status = API_TOSTR_OK;
      return s;
    }
    if (tvisnumber(o)) {
      if (tv_rawload_acq(o) == tv_rawload(&numtv)) {
	if (index_iscupvalue(idx) && lj_trace_hasany(G(L))) {
	  lj_state_dropclaim(&claim);
	  api_gcroot_release(&root);
	  api_trace_flush_mutation(api_errstate(L));
	  continue;
	}
	index2adr_storestr_noflush(L, idx,
				 index_iscupvalue(idx) ? NULL : (TValue *)o, s);
	lj_state_dropclaim(&claim);
	api_gcroot_release(&root);
	*status = API_TOSTR_OK;
	return s;
      }
      lj_state_dropclaim(&claim);
      api_gcroot_release(&root);
      continue;
    }
    *status = tvisnil(o) ? API_TOSTR_NIL : API_TOSTR_BAD;
    lj_state_dropclaim(&claim);
    api_gcroot_release(&root);
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

/* api_checkclaim() uses tryclaim rather than resumeclaim and therefore does
** not save/replace an ownerless coroutine's tg_hint. Its failure cleanup must
** only release ownership; dropresumeclaim() would incorrectly clear an
** existing hint. */
static void api_checkstack1_preclaimed(lua_State *L, lua_State *errL,
				       LJStateClaim *claim)
{
  if ((mref(L->maxstack, char) - (char *)L->top) <=
      (ptrdiff_t)sizeof(TValue)) {
    int status = lj_state_cpgrowstack(L, 1);
    if (status != LUA_OK) {
      if (L->top > L->base) L->top--;
      lj_state_dropclaim(claim);
      lj_err_callermsg(errL, status == LUA_ERRMEM ?
		       "not enough memory" : "stack overflow");
    }
  }
}

static void api_checkstack1_gcroot_claimed(lua_State *L, lua_State *errL,
					   LJStateClaim *claim,
					   ApiGCRoot *root)
{
  if ((mref(L->maxstack, char) - (char *)L->top) <=
      (ptrdiff_t)sizeof(TValue)) {
    int status = lj_state_cpgrowstack(L, 1);
    if (status != LUA_OK) {
      if (L->top > L->base) L->top--;
      lj_state_dropresumeclaim(claim);
      api_gcroot_release(root);
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
    int status = lj_vm_pcall_unwind(L, base, nres1, 0);
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
  (void)lj_tg_stack_dirty_epoch_add_rlx(L2TG(L), 1);
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
  lua_State *errL = api_errstate(L);
  TValue *o;
  ptrdiff_t rootoffs;
  size_t len;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  o = api_stackroot_push_index(L, idx);
  rootoffs = savestack(L, o);
  if (tvisstr(o)) {
    len = strV(o)->len;
  } else if (tvistab(o)) {
    len = (size_t)lj_tab_len_rooted(L, o);
  } else if (tvisudata(o)) {
    len = udataV(o)->len;
  } else if (tvisnumber(o)) {
    int status;
    GCstr *s;
    L->top = restorestack(L, rootoffs);
    lj_state_dropresumeclaim(&claim);
    s = api_tolstring_claimed(L, idx, &status);
    return status == API_TOSTR_OK ? s->len : 0;
  } else {
    len = 0;
  }
  L->top = restorestack(L, rootoffs);
  lj_state_dropresumeclaim(&claim);
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
  ApiGCRoot root = { NULL, 0 };
  lua_State *errL;
  GCstr *s;
  api_checkclaim(L, &preclaim);
  lj_state_dropclaim(&preclaim);
  errL = api_errstate(L);
  s = api_str_new_rooted(errL, str, len, &root);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim)) {
    api_gcroot_release(&root);
    lj_err_callermsg(errL, "thread busy");
  }
  api_checkstack1_gcroot_claimed(L, errL, &claim, &root);
  setstrV(L, L->top, s);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  api_gcroot_release(&root);
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_pushstring(lua_State *L, const char *str)
{
  LJStateClaim preclaim, claim;
  ApiGCRoot root = { NULL, 0 };
  lua_State *errL;
  GCstr *s = NULL;
  if (str == NULL) {
    errL = api_errstate(L);
  } else {
    api_checkclaim(L, &preclaim);
    lj_state_dropclaim(&preclaim);
    errL = api_errstate(L);
    s = api_str_newz_rooted(errL, str, &root);
  }
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim)) {
    api_gcroot_release(&root);
    lj_err_callermsg(errL, "thread busy");
  }
  if (str == NULL)
    api_checkstack1_claimed(L, errL, &claim);
  else
    api_checkstack1_gcroot_claimed(L, errL, &claim, &root);
  if (str == NULL)
    setnilV(L->top);
  else
    setstrV(L, L->top, s);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  api_gcroot_release(&root);
  lj_state_dropresumeclaim(&claim);
}

LUA_API const char *lua_pushvfstring(lua_State *L, const char *fmt,
				     va_list argp)
{
  LJStateClaim preclaim, claim;
  ApiGCRoot root = { NULL, 0 };
  lua_State *errL;
  GCstr *s;
  TValue strtv;
  api_checkclaim(L, &preclaim);
  lj_state_dropclaim(&preclaim);
  errL = api_errstate(L);
  if (LJ_UNLIKELY(!lj_tg_root_anchor_reserve_nothrow(errL, L2TG(errL))))
    lj_err_mem(errL);
  s = lj_strfmt_vstr(errL, fmt, argp);
  setstrV(errL, &strtv, s);
  (void)api_gcroot_push_reserved(errL, &strtv, &root);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim)) {
    api_gcroot_release(&root);
    lj_err_callermsg(errL, "thread busy");
  }
  api_checkstack1_gcroot_claimed(L, errL, &claim, &root);
  setstrV(L, L->top, s);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  api_gcroot_release(&root);
  lj_state_dropresumeclaim(&claim);
  return strdata(s);
}

LUA_API const char *lua_pushfstring(lua_State *L, const char *fmt, ...)
{
  const char *ret;
  va_list argp;
  va_start(argp, fmt);
  ret = lua_pushvfstring(L, fmt, argp);
  va_end(argp);
  return ret;
}

LUA_API void lua_pushcclosure(lua_State *L, lua_CFunction f, int n)
{
  LJStateClaim preclaim, claim;
  ApiGCRoot root = { NULL, 0 };
  lua_State *errL;
  GCtab *env;
  GCfunc *fn;
  TValue fnv;
  TValue *src;
  TGState *construct_tg;
  uint32_t anchoridx;
  int nup = n;
  api_checkclaim(L, &preclaim);
  lj_checkapi_slot(n);
  env = getcurrenv(L);
  errL = api_errstate(L);
  if (nup == 0)
    api_checkstack1_preclaimed(L, errL, &preclaim);
  anchoridx = api_envroot_claimed(errL, env, &preclaim, &root);
  lj_state_dropclaim(&preclaim);
  construct_tg = root.tg;
  fn = lj_func_newC_envrooted(errL, (MSize)n, env, anchoridx);
  fn->c.f = f;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim)) {
    lj_tg_root_anchor_pop(construct_tg, anchoridx);
    lj_err_callermsg(errL, "thread busy");
  }
  lj_checkapi_slot(nup);
  src = L->top - nup;
  while (nup--) {
    copyTVrel(L, &fn->c.upvalue[nup], src+nup);
    lj_gc_pubobjtv(L, fn, &fn->c.upvalue[nup]);
  }
  L->top = src;
  lj_assertL(iswhite(obj2gco(fn)), "new GC object is not white");
  setfuncV(L, &fnv, fn);
  copyTVrel(L, L->top, &fnv);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_tg_root_anchor_pop(construct_tg, anchoridx);
  lj_state_dropresumeclaim(&claim);
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
  LJTabRoot root;
  api_checkclaim(L, &preclaim);
  errL = api_errstate(L);
  /* Reserve the common-case destination before releasing the target claim.
  ** A racy peer may still consume it, so the reacquired path rechecks and has
  ** explicit root cleanup around its protected grow. */
  api_checkstack1_preclaimed(L, errL, &preclaim);
  lj_state_dropclaim(&preclaim);
  t = lj_tab_new_ah_rooted(errL, (uint32_t)narray, (uint32_t)nrec, &root);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim)) {
    lj_tab_root_release(&root);
    lj_err_callermsg(errL, "thread busy");
  }
  if ((mref(L->maxstack, char) - (char *)L->top) <=
      (ptrdiff_t)sizeof(TValue)) {
    int status = lj_state_cpgrowstack(L, 1);
    if (status != LUA_OK) {
      if (L->top > L->base) L->top--;
      lj_state_dropresumeclaim(&claim);
      lj_tab_root_release(&root);
      lj_err_callermsg(errL, status == LUA_ERRMEM ?
		       "not enough memory" : "stack overflow");
    }
  }
  settabV(L, L->top, t);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_tab_root_release(&root);
  lj_state_dropresumeclaim(&claim);
}

typedef struct ApiNewMetatableCtx {
  lua_State *target;
  const char *tname;
  int result;
} ApiNewMetatableCtx;

#if defined(LJ_API_ROOT_TEST_HELPERS)
typedef void (*LJApiNewMetatableHook)(lua_State *L, int stage, GCtab *regt,
				      GCstr *key, TValue *valueslot,
				      TValue *rootslot);
static LJApiNewMetatableHook api_test_newmetatable_hook;

void lj_api_test_set_newmetatable_hook(LJApiNewMetatableHook hook)
{
  api_test_newmetatable_hook = hook;
}

static void api_test_newmetatable(lua_State *L, int stage, GCtab *regt,
				  GCstr *key, TValue *valueslot,
				  TValue *rootslot)
{
  LJApiNewMetatableHook hook = api_test_newmetatable_hook;
  if (hook) {
    if (stage == 0 || stage == 2)
      api_test_newmetatable_hook = NULL;
    hook(L, stage, regt, key, valueslot, rootslot);
  }
}
#else
#define api_test_newmetatable(L, stage, regt, key, valueslot, rootslot) \
  ((void)0)
#endif

static void api_newmetatable_push(lua_State *errL, ApiNewMetatableCtx *ctx,
				  ApiGCRoot *root)
{
  LJStateClaim claim;
  TValue out;
  TValue *slot;
  lua_State *L = ctx->target;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim)) {
    api_gcroot_release(root);
    lj_err_callermsg(errL, "thread busy");
  }
  api_checkstack1_gcroot_claimed(L, errL, &claim, root);
  slot = lj_tg_root_anchor_slot_acq(root->tg, root->idx);
  lj_assertL(slot != NULL, "lost new-metatable result root");
  lj_tv_load_acq(&out, slot);
  copyTV(L, L->top, &out);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  api_gcroot_release(root);
  lj_state_dropresumeclaim(&claim);
}

static TValue *api_newmetatable_cp(lua_State *errL, lua_CFunction dummy,
				    void *ud)
{
  ApiNewMetatableCtx *ctx = (ApiNewMetatableCtx *)ud;
  ApiGCRoot regroot = { NULL, 0 };
  ApiGCRoot keyroot = { NULL, 0 };
  ApiGCRoot winnerroot = { NULL, 0 };
  GCtab *regt;
  GCstr *key;
  TValue nilv;
  TValue *regslot, *keyslot, *winner;
  UNUSED(dummy);
  cframe_errfunc(errL->cframe) = -1;
  if (LJ_UNLIKELY(!lj_tg_root_anchor_reserve_nothrow(errL, L2TG(errL))))
    lj_err_mem(errL);
  regslot = api_gcroot_capture_edge_reserved(
    errL, registry(errL), &regroot);
  lj_assertX(tvistab(regslot), "registry root is not a table");
  regt = tabV(regslot);
  key = api_str_newz_rooted(errL, ctx->tname, &keyroot);
  keyslot = lj_tg_root_anchor_slot_acq(keyroot.tg, keyroot.idx);
  lj_assertX(keyslot != NULL, "missing new-metatable key root");
  if (LJ_UNLIKELY(!lj_tg_root_anchor_reserve_nothrow(errL, L2TG(errL))))
    lj_err_mem(errL);
  setnilV(&nilv);
  winner = api_gcroot_push_reserved(errL, &nilv, &winnerroot);
  for (;;) {
    TValue *tv;
    int rc;
    regslot = lj_tg_root_anchor_slot_acq(regroot.tg, regroot.idx);
    keyslot = lj_tg_root_anchor_slot_acq(keyroot.tg, keyroot.idx);
    winner = lj_tg_root_anchor_slot_acq(winnerroot.tg, winnerroot.idx);
    lj_assertX(regslot != NULL && keyslot != NULL && winner != NULL,
	       "missing new-metatable transaction root");
    (void)lj_tab_gettv_rooted(errL, regslot, keyslot, winner);
    if (!tvisnil(winner))
      api_test_newmetatable(errL, 0, regt, key, NULL, winner);
    if (tvisnil(winner)) {
      LJTabRoot mtroot;
      GCtab *mt = lj_tab_new_ah_rooted(errL, 0, 1, &mtroot);
      TValue mtv;
      settabV(errL, &mtv, mt);
      tv = lj_tab_setstr(errL, regt, key);
      api_test_newmetatable(errL, 1, regt, key, tv, keyslot);
      rc = lj_tab_trysetnil_cas_keyed_rooted(
	errL, regt, tv, keyslot, &mtv, winner);
      if (rc == LJ_TAB_STORE_CAS_OK) {
	lj_gc_pubtab(errL, regt);
	/* Transfer the result into the dedicated enumerated output root before
	** dropping the top constructor root. */
	api_gcroot_replace(errL, &winnerroot, &mtv);
	lj_tab_root_release(&mtroot);
	ctx->result = 1;
	api_newmetatable_push(errL, ctx, &winnerroot);
	api_gcroot_release(&keyroot);
	api_gcroot_release(&regroot);
	return NULL;
      }
      if (rc == LJ_TAB_STORE_CAS_EXISTS) {
	/* The NIL-CAS published the competing winner directly into winnerroot
	** before releasing its vector lifetime. */
	api_test_newmetatable(errL, 2, regt, key, tv,
	  lj_tg_root_anchor_slot_acq(winnerroot.tg, winnerroot.idx));
	lj_tab_root_release(&mtroot);
	ctx->result = 0;
	api_newmetatable_push(errL, ctx, &winnerroot);
	api_gcroot_release(&keyroot);
	api_gcroot_release(&regroot);
	return NULL;
      }
      lj_tab_root_release(&mtroot);
      lj_tab_store_wait_l(errL);
      continue;
    }
    /* The by-value lookup copied the winner into an enumerated root and ran
    ** its publication barrier before releasing result/vector leases. */
    ctx->result = 0;
    api_newmetatable_push(errL, ctx, &winnerroot);
    api_gcroot_release(&keyroot);
    api_gcroot_release(&regroot);
    return NULL;
  }
}

LUALIB_API int luaL_newmetatable(lua_State *L, const char *tname)
{
  LJStateClaim preclaim;
  ApiNewMetatableCtx ctx;
  lua_State *errL;
  int status;
  api_checkclaim(L, &preclaim);
  lj_state_dropclaim(&preclaim);
  errL = api_errstate(L);
  ctx.target = L;
  ctx.tname = tname;
  ctx.result = 0;
  /* Nested protection is intentional: Lua fast pcall/xpcall does not pass
  ** through the C wrapper checkpoint, so every key/table anchor is unwound
  ** here before the original error is rethrown. This path is initialization-
  ** oriented and not part of ordinary field-access performance. */
  status = lj_vm_cpcall(errL, NULL, &ctx, api_newmetatable_cp);
  if (LJ_UNLIKELY(status != LUA_OK))
    lj_err_throw(errL, status);
  return ctx.result;
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
  LJStateClaim preclaim, claim;
  ApiGCRoot root = { NULL, 0 };
  lua_State *errL;
  GCtab *env;
  lua_State *L1;
  TGState *anchor_tg;
  uint32_t anchoridx;
  api_checkclaim(L, &preclaim);
  env = getcurrenv(L);
  errL = api_errstate(L);
  anchoridx = api_envroot_claimed(errL, env, &preclaim, &root);
  lj_state_dropclaim(&preclaim);
  anchor_tg = root.tg;
  L1 = lj_state_new_withenv_envrooted(errL, env, anchoridx);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim)) {
    lj_tg_root_anchor_pop(anchor_tg, anchoridx);
    lj_err_callermsg(errL, "thread busy");
  }
  if ((mref(L->maxstack, char) - (char *)L->top) <=
      (ptrdiff_t)sizeof(TValue)) {
    int status = lj_state_cpgrowstack(L, 1);
    if (status != LUA_OK) {
      if (L->top > L->base) L->top--;
      lj_state_dropresumeclaim(&claim);
      lj_tg_root_anchor_pop(anchor_tg, anchoridx);
      lj_err_callermsg(errL, status == LUA_ERRMEM ?
		       "not enough memory" : "stack overflow");
    }
  }
  setthreadV(L, L->top, L1);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_tg_root_anchor_pop(anchor_tg, anchoridx);
  lj_state_dropresumeclaim(&claim);
  return L1;
}

LUA_API void *lua_newuserdata(lua_State *L, size_t size)
{
  LJStateClaim preclaim, claim;
  ApiGCRoot envroot = { NULL, 0 };
  lua_State *errL;
  GCtab *env;
  GCudata *ud;
  LJUdataRoot root;
  api_checkclaim(L, &preclaim);
  env = getcurrenv(L);
  errL = api_errstate(L);
  if (size > LJ_MAX_UDATA - sizeof(GCudata)) {
    lj_state_dropclaim(&preclaim);
    lj_err_msg(errL, LJ_ERR_UDATAOV);
  }
  (void)api_envroot_claimed(errL, env, &preclaim, &envroot);
  root.tg = envroot.tg;
  root.idx = envroot.idx;
  lj_state_dropclaim(&preclaim);
  ud = lj_udata_newrooted_envrooted(errL, (MSize)size, env, &root);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim)) {
    lj_udata_root_release(&root);
    lj_err_callermsg(errL, "thread busy");
  }
  if ((mref(L->maxstack, char) - (char *)L->top) <=
      (ptrdiff_t)sizeof(TValue)) {
    int status = lj_state_cpgrowstack(L, 1);
    if (status != LUA_OK) {
      if (L->top > L->base) L->top--;
      lj_state_dropresumeclaim(&claim);
      lj_udata_root_release(&root);
      lj_err_callermsg(errL, status == LUA_ERRMEM ?
		       "not enough memory" : "stack overflow");
    }
  }
  setudataV(L, L->top, ud);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_udata_root_release(&root);
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
      lj_state_stack_pubtv(L, L, L->top-1);
    } while (--n > 0);
  } else if (n == 0) {  /* Push empty string. */
    setstrV(L, L->top, &G(L)->strempty);
    lj_state_stack_pubtv(L, L, L->top);
    incr_top(L);
  }
  /* else n == 1: nothing to do. */
  lj_state_dropresumeclaim(&claim);
}

/* -- Object getters ------------------------------------------------------ */

LUA_API void lua_gettable(lua_State *L, int idx)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue *out;
  cTValue *t, *v;
  int receiver_root = api_index_is_envroot(idx);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  if (receiver_root) {
    api_checkstack1_claimed(L, errL, &claim);
    t = api_stackroot_push_index(L, idx);
  } else {
    t = index2adr_check(L, idx);
  }
  out = L->top - (receiver_root ? 2 : 1);
  v = lj_meta_tgettv_rooted(L, t, out, out);
  if (v == NULL) {
    L->top += 2;
    api_vm_call_claimed(L, L->top-2, 1+1, &claim);
    L->top -= 2+LJ_FR2;
    v = L->top+1+LJ_FR2;
  }
  out = L->top - (receiver_root ? 2 : 1);
  copyTV(L, out, v);
  lj_state_stack_pubtv(L, L, out);
  if (receiver_root)
    L->top--;  /* Pop the temporary receiver; the result replaced the key. */
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_getfield(lua_State *L, int idx, const char *k)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue *out;
  cTValue *v, *t;
  int receiver_root = api_index_is_envroot(idx);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  /* Materialize the key first, then resolve the authoritative table edge
  ** immediately before rooted meta capture. This avoids a durable API anchor
  ** across a throwing allocation. Ordinary negative indices move down by one
  ** when the key is pushed; pseudo and positive indices do not. */
  setstrV(L, L->top, lj_str_newz(L, k));
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  if (receiver_root) {
    api_checkstack1_claimed(L, errL, &claim);
    t = api_stackroot_push_index(L, idx);
  } else {
    if (idx < 0 && idx > LUA_REGISTRYINDEX)
      idx--;
    t = index2adr_check(L, idx);
  }
  out = L->top - (receiver_root ? 2 : 1);
  v = lj_meta_tgettv_rooted(L, t, out, out);
  if (v == NULL) {
    L->top += 2;
    api_vm_call_claimed(L, L->top-2, 1+1, &claim);
    L->top -= 2+LJ_FR2;
    v = L->top+1+LJ_FR2;
  }
  out = L->top - (receiver_root ? 2 : 1);
  copyTV(L, out, v);
  lj_state_stack_pubtv(L, L, out);
  if (receiver_root)
    L->top--;
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_rawget(lua_State *L, int idx)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue *troot;
  api_checkclaim(L, &claim);
  /* Resolve idx before extending top, then capture its exact incarnation into
  ** a temporary stack parent. This covers mutable pseudo/upvalue roots and the
  ** valid table/key self-alias without a durable anchor. */
  api_checkstack1_preclaimed(L, errL, &claim);
  troot = api_stackroot_push_index(L, idx);
  lj_checkapi(tvistab(troot), "stack slot %d is not a table", idx);
  (void)lj_tab_gettv_rooted(L, troot, L->top-2, L->top-2);
  L->top--;  /* Pop the temporary parent; result replaced the key. */
  lj_state_dropclaim(&claim);
}

LUA_API void lua_rawgeti(lua_State *L, int idx, int n)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue *troot;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  /* Resolve idx before extending top. Parent/output aliasing is supported, so
  ** the exact captured table can become the eventual result in place. */
  troot = api_stackroot_push_index(L, idx);
  lj_checkapi(tvistab(troot), "stack slot %d is not a table", idx);
  (void)lj_tab_getinttv_rooted(L, troot, n, troot);
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
retry_after_grow:
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
  if ((mref(L->maxstack, char) - (char *)L->top) <=
      (ptrdiff_t)sizeof(TValue)) {
    /* The old mt snapshot must not cross protected growth. Grow, then reload
    ** the indexed object and its current metatable from the relocated stack. */
    api_checkstack1_claimed(L, errL, &claim);
    mt = NULL;
    goto retry_after_grow;
  }
  settabV(L, L->top, mt);
  lj_state_stack_pubtv(L, L, L->top);
  incr_top(L);
  lj_state_dropresumeclaim(&claim);
  return 1;
}

LUALIB_API int luaL_getmetafield(lua_State *L, int idx, const char *field)
{
  LJStateClaim preclaim, claim;
  ApiGCRoot root = { NULL, 0 };
  lua_State *errL;
  GCstr *key;
  int ok;
  api_checkclaim(L, &preclaim);
  lj_state_dropclaim(&preclaim);
  errL = api_errstate(L);
  key = api_str_newz_rooted(errL, field, &root);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim)) {
    api_gcroot_release(&root);
    lj_err_callermsg(errL, "thread busy");
  }
  api_checkstack1_gcroot_claimed(L, errL, &claim, &root);
  ok = api_getmetafield_key_claimed(L, idx, key, &claim);
  api_gcroot_release(&root);
  lj_state_dropresumeclaim(&claim);
  return ok;
}

static int api_getmetafield_key_claimed(lua_State *L, int idx, GCstr *field,
					LJStateClaim *claim)
{
  TValue snap;
  cTValue *o, *tv;
  GCtab *mt = NULL;
  UNUSED(claim);
  o = index2adr_read(L, idx, &snap);
  if (tvistab(o))
    mt = lj_tab_metatable_acq(tabV(o));
  else if (tvisudata(o))
    mt = lj_udata_metatable_acq(udataV(o));
  else
    mt = lj_basemt_obj_acq(G(L), o);
  if (mt != NULL) {
    TValue mtv;
    /* The caller pre-reserved this slot. Root the metatable itself before the
    ** generation-following lookup can yield, then replace that natural root
    ** with the result snapshot. */
    settabV(L, L->top, mt);
    lj_state_stack_pubtv(L, L, L->top);
    incr_top(L);
    tv = lj_tab_getstr(mt, field);
    if (tv != NULL) {
      lj_tv_load_acq(&mtv, tv);
      if (!tvisnil(&mtv)) {
	copyTV(L, L->top-1, &mtv);
	lj_state_stack_pubtv(L, L, L->top-1);
	return 1;
      }
    }
    setnilV(L->top-1);
    L->top--;
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
    settabV(L, L->top, env);
    lj_state_stack_pubtv(L, L, L->top);
    lj_state_dropclaim(&thclaim);
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
  TValue *troot, *key, *out;
  ptrdiff_t trootofs, keyofs, outofs;
  int more;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  lj_checkapi_slot(1);
  api_checkstack1_claimed(L, errL, &claim);
  /* Resolve and publish the receiver before moving top, preserving the valid
  ** self-key form lua_next(L, -1). The receiver slot is later recycled as the
  ** returned value so the public two-result stack layout remains unchanged. */
  troot = api_stackroot_push_index(L, idx);
  lj_checkapi(tvistab(troot), "stack slot %d is not a table", idx);
  trootofs = savestack(L, troot);
  key = L->top-2;
  keyofs = savestack(L, key);
  api_checkstack1_claimed(L, errL, &claim);
  troot = restorestack(L, trootofs);
  key = restorestack(L, keyofs);
  out = L->top;
  outofs = savestack(L, out);
  setnilV(out);
  lj_state_stack_pubtv(L, L, out);
  L->top++;
  more = lj_tab_next_rooted(L, troot, key, key, out, NULL);
  if (more > 0) {
    troot = restorestack(L, trootofs);
    out = restorestack(L, outofs);
    copyTVrel(L, troot, out);
    lj_state_stack_pubtv(L, L, troot);
    L->top = restorestack(L, trootofs) + 1;
  } else if (!more) {  /* End of traversal. */
    L->top = restorestack(L, keyofs);  /* Remove key and temporary roots. */
  } else {
    L->top = restorestack(L, keyofs);
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
    lj_checkapi((uint32_t)n < lj_funcL_nupvalues(&fn->l),
		"bad upvalue %d", n+1);
    id = (void *)func_uvptr_acq(&fn->l, (uint32_t)n);
  } else {
    lj_checkapi((uint32_t)n < lj_funcC_nupvalues(&fn->c),
		"bad upvalue %d", n+1);
    id = (void *)&fn->c.upvalue[n];
  }
  lj_state_dropclaim(&claim);
  return id;
}

LUA_API void lua_upvaluejoin(lua_State *L, int idx1, int n1, int idx2, int n2)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue snap1, snap2;
  GCfunc *fn1, *fn2;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  fn1 = funcV(index2adr_read(L, idx1, &snap1));
  fn2 = funcV(index2adr_read(L, idx2, &snap2));
  n1--; n2--;
  lj_checkapi(isluafunc(fn1), "stack slot %d is not a Lua function", idx1);
  lj_checkapi(isluafunc(fn2), "stack slot %d is not a Lua function", idx2);
  lj_checkapi((uint32_t)n1 < lj_funcL_nupvalues(&fn1->l),
	      "bad upvalue %d", n1+1);
  lj_checkapi((uint32_t)n2 < lj_funcL_nupvalues(&fn2->l),
	      "bad upvalue %d", n2+1);
  {
    GCobj *uv = func_uvptr_acq(&fn2->l, (uint32_t)n2);
    GCobj *old = func_uvptr_acq(&fn1->l, (uint32_t)n1);
    if (old != uv) {
      api_trace_flush_mutation_claimed(L, errL, &claim);
      setgcrefrel(fn1->l.uvptr[n1], uv);
      lj_gc_pubobjobj(L, fn1, uv);
    }
  }
  lj_state_dropresumeclaim(&claim);
}

LUALIB_API void *luaL_testudata(lua_State *L, int idx, const char *tname)
{
  LJStateClaim claim;
  ApiGCRoot root = { NULL, 0 };
  ApiGCRoot regroot = { NULL, 0 };
  ApiGCRoot mtroot = { NULL, 0 };
  lua_State *errL;
  TValue snap, nilv;
  TValue *keyslot, *regslot, *mtslot;
  cTValue *o;
  void *p = NULL;
  api_checkclaim(L, &claim);
  o = index2adr_read(L, idx, &snap);
  if (!tvisudata(o)) {
    lj_state_dropclaim(&claim);
    return NULL;
  }
  lj_state_dropclaim(&claim);
  errL = api_errstate(L);
  (void)api_str_newz_rooted(errL, tname, &root);
  if (LJ_UNLIKELY(!lj_tg_root_anchor_reserve_nothrow(errL, L2TG(errL)))) {
    api_gcroot_release(&root);
    lj_err_mem(errL);
  }
  regslot = api_gcroot_capture_edge_reserved(
    errL, registry(errL), &regroot);
  lj_assertX(tvistab(regslot), "registry root is not a table");
  if (LJ_UNLIKELY(!lj_tg_root_anchor_reserve_nothrow(errL, L2TG(errL)))) {
    api_gcroot_release(&regroot);
    api_gcroot_release(&root);
    lj_err_mem(errL);
  }
  setnilV(&nilv);
  (void)api_gcroot_push_reserved(errL, &nilv, &mtroot);
  if (!lj_state_tryclaim(L, lj_thr_current_id(G(L)), &claim)) {
    api_gcroot_release(&mtroot);
    api_gcroot_release(&regroot);
    api_gcroot_release(&root);
    lj_err_callermsg(errL, "thread busy");
  }
  o = index2adr_read(L, idx, &snap);
  if (tvisudata(o)) {
    keyslot = lj_tg_root_anchor_slot_acq(root.tg, root.idx);
    regslot = lj_tg_root_anchor_slot_acq(regroot.tg, regroot.idx);
    mtslot = lj_tg_root_anchor_slot_acq(mtroot.tg, mtroot.idx);
    lj_assertX(keyslot != NULL && regslot != NULL && mtslot != NULL,
	       "missing testudata lookup root");
    (void)lj_tab_gettv_rooted(errL, regslot, keyslot, mtslot);
    /* The rooted registry lookup may wait. Reload the selected API edge only
    ** afterwards, then retain that exact userdata incarnation through its
    ** final metatable/body access. A shared C-upvalue may race replacement;
    ** either valid incarnation is a lawful racy result, never a stale body. */
    for (;;) {
      LJGC2Lease udlease;
      TValue udsnap;
      TValue *udedge;
      GCudata *ud;
      int status;
      if (LJ_UNLIKELY(!lj_gc2_smr_read_try(G(L)))) {
	lj_tab_wait_l(L);
	continue;
      }
      udedge = index2adr(L, idx);
      lj_tv_load_acq(&udsnap, udedge);
      if (!tvisudata(&udsnap))
	status = LJ_GC2_TV_EDGE_STALE;
      else
	status = lj_gc2_tv_lease_acquire(G(L), &udsnap, &udlease);
      lj_gc2_smr_read_leave(G(L));
      if (!tvisudata(&udsnap))
	break;
      if (status == LJ_GC2_TV_EDGE_RETRY) {
	lj_tab_wait_l(L);
	continue;
      }
      if (status != LJ_GC2_TV_EDGE_VALID)
	break;
      ud = udataV(&udsnap);
      mtslot = lj_tg_root_anchor_slot_acq(mtroot.tg, mtroot.idx);
      if (tvistab(mtslot) && tabV(mtslot) == lj_udata_metatable_acq(ud))
	p = uddata(ud);
      lj_gc2_lease_release(&udlease);
      break;
    }
  }
  lj_state_dropclaim(&claim);
  api_gcroot_release(&mtroot);
  api_gcroot_release(&regroot);
  api_gcroot_release(&root);
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
  lua_State *errL = api_errstate(L);
  TValue *key, *val;
  cTValue *t;
  ptrdiff_t keyofs, valofs;
  int receiver_root = api_index_is_envroot(idx);
  lj_checkapi_slot(2);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  if (receiver_root) {
    api_checkstack1_claimed(L, errL, &claim);
    t = api_stackroot_push_index(L, idx);
  } else {
    t = index2adr_check(L, idx);
  }
  key = L->top - (receiver_root ? 3 : 2);
  val = key + 1;
  keyofs = savestack(L, key);
  valofs = savestack(L, val);
  if (lj_meta_tsettv_pair(L, t, key, val)) {
    L->top = restorestack(L, keyofs);
    lj_state_dropresumeclaim(&claim);
    return;
  }
  {
    TValue *base = L->top;
    copyTV(L, base+2, restorestack(L, valofs));
    L->top = base+3;
    api_vm_call_claimed(L, base, 0+1, &claim);
    L->top = restorestack(L, keyofs);
  }
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_setfield(lua_State *L, int idx, const char *k)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue valtmp;
  TValue *key, *val;
  cTValue *t;
  ptrdiff_t keyofs, valofs;
  int receiver_root = api_index_is_envroot(idx);
  lj_checkapi_slot(1);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  api_checkstack1_claimed(L, errL, &claim);
  /* Materialize |key|value| on the real stack. This roots both operands across
  ** stale-generation waits and gives __newindex the same frame contract as
  ** lua_settable(). Resolve the table only after this shuffle, so no durable
  ** API anchor spans string allocation or a catchable metamethod error. */
  setstrV(L, L->top, lj_str_newz(L, k));
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  copyTV(L, &valtmp, L->top-2);
  copyTVrel(L, L->top-2, L->top-1);
  copyTVrel(L, L->top-1, &valtmp);
  lj_state_stack_pubtv(L, L, L->top-2);
  lj_state_stack_pubtv(L, L, L->top-1);
  /* Pushing the key moves an older ordinary negative index down one slot. The
  ** valid -1 extreme is special: its selected table/value moved with the
  ** shuffle and is still the new top-1 value slot. */
  if (receiver_root) {
    api_checkstack1_claimed(L, errL, &claim);
    t = api_stackroot_push_index(L, idx);
  } else {
    if (idx < 0 && idx > LUA_REGISTRYINDEX && idx != -1)
      idx--;
    else if (idx > 0 && idx == (int)(L->top - L->base) - 1)
      idx++;  /* Positive index named the old top value, now at new top-1. */
    t = index2adr_check(L, idx);
  }
  key = L->top - (receiver_root ? 3 : 2);
  val = key + 1;
  keyofs = savestack(L, key);
  valofs = savestack(L, val);
  if (lj_meta_tsettv_pair(L, t, key, val)) {
    L->top = restorestack(L, keyofs);
    lj_state_dropresumeclaim(&claim);
    return;
  }
  {
    TValue *base = L->top;
    copyTV(L, base+2, restorestack(L, valofs));
    L->top = base+3;
    api_vm_call_claimed(L, base, 0+1, &claim);
    L->top = restorestack(L, keyofs);
  }
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_rawset(lua_State *L, int idx)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue *troot;
  GCtab *t;
  TValue *dst, *key, *val;
  ptrdiff_t trootofs, keyofs, valofs;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  lj_checkapi_slot(2);
  api_checkstack1_claimed(L, errL, &claim);
  troot = api_stackroot_push_index(L, idx);
  lj_checkapi(tvistab(troot), "stack slot %d is not a table", idx);
  key = L->top-3;
  val = key+1;
  trootofs = savestack(L, troot);
  keyofs = savestack(L, key);
  valofs = savestack(L, val);
  for (;;) {
    int rc;
    troot = restorestack(L, trootofs);
    key = restorestack(L, keyofs);
    val = restorestack(L, valofs);
    t = tabV(troot);
    dst = lj_tab_set(L, t, key);
    /* lj_tab_set() may resize, wait or relocate L's stack. Rebind every
    ** stack-backed operand before the keyed CAS. */
    troot = restorestack(L, trootofs);
    key = restorestack(L, keyofs);
    val = restorestack(L, valofs);
    t = tabV(troot);
    rc = lj_tab_trystoretv_cas_keyed(L, t, dst, key, val);
    if (rc == LJ_TAB_STORE_CAS_OK)
      break;
    lj_tab_store_wait_l(L);  /* C API rawset saw stale/FORWARD slot. */
  }
  troot = restorestack(L, trootofs);
  key = restorestack(L, keyofs);
  val = restorestack(L, valofs);
  t = tabV(troot);
  lj_gc2_barrier_weak_write(L, t, key, val);
  troot = restorestack(L, trootofs);
  key = restorestack(L, keyofs);
  t = tabV(troot);
  lj_gc_pubtabkey(L, t, key);
  troot = restorestack(L, trootofs);
  val = restorestack(L, valofs);
  t = tabV(troot);
  lj_gc_pubtabtv(L, t, val);
  L->top = restorestack(L, keyofs);
  lj_state_dropresumeclaim(&claim);
}

LUA_API void lua_rawseti(lua_State *L, int idx, int n)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue *troot;
  GCtab *t;
  TValue *dst, *src;
  TValue key;
  ptrdiff_t trootofs, srcofs;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  lj_checkapi_slot(1);
  api_checkstack1_claimed(L, errL, &claim);
  troot = api_stackroot_push_index(L, idx);
  lj_checkapi(tvistab(troot), "stack slot %d is not a table", idx);
  src = L->top-2;
  trootofs = savestack(L, troot);
  srcofs = savestack(L, src);
  setintV(&key, n);
  for (;;) {
    int rc;
    troot = restorestack(L, trootofs);
    src = restorestack(L, srcofs);
    t = tabV(troot);
    dst = lj_tab_setint(L, t, n);
    /* Rebind the receiver and source after a resize/wait-capable lookup. */
    troot = restorestack(L, trootofs);
    src = restorestack(L, srcofs);
    t = tabV(troot);
    rc = lj_tab_trystoretv_cas_keyed(L, t, dst, &key, src);
    if (rc == LJ_TAB_STORE_CAS_OK)
      break;
    lj_tab_store_wait_l(L);  /* C API rawseti saw stale/FORWARD slot. */
  }
  troot = restorestack(L, trootofs);
  src = restorestack(L, srcofs);
  t = tabV(troot);
  lj_gc2_barrier_weak_write(L, t, &key, src);
  troot = restorestack(L, trootofs);
  src = restorestack(L, srcofs);
  t = tabV(troot);
  lj_gc_pubtabtv(L, t, src);
  L->top = restorestack(L, srcofs);
  lj_state_dropresumeclaim(&claim);
}

LUA_API int lua_setmetatable(lua_State *L, int idx)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  global_State *g;
  GCtab *mt;
  TValue snap;
  cTValue *o;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  o = index2adr_check_read(L, idx, &snap);
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
    api_trace_flush_mutation_claimed(L, errL, &claim);
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
  lj_state_dropresumeclaim(&claim);
  return 1;
}

LUALIB_API void luaL_setmetatable(lua_State *L, const char *tname)
{
  lua_getfield(L, LUA_REGISTRYINDEX, tname);
  lua_setmetatable(L, -2);
}

LUA_API int lua_setfenv(lua_State *L, int idx)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue snap;
  cTValue *o;
  GCtab *t;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  o = index2adr_check_read(L, idx, &snap);
  lj_checkapi_slot(1);
  lj_checkapi(tvistab(L->top-1), "top stack slot is not a table");
  t = tabV(L->top-1);
  if (tvisfunc(o)) {
    GCfunc *fn = funcV(o);
    api_trace_flush_mutation_claimed(L, errL, &claim);
    lj_func_env_rel(fn, t);
    lj_gc_pubobjobj(L, fn, t);
  } else if (tvisudata(o)) {
    GCudata *ud = udataV(o);
    lj_udata_env_rel(ud, t);
    lj_gc_pubobjobj(L, ud, t);
  } else if (tvisthread(o)) {
    lua_State *L1 = threadV(o);
    LJStateClaim thclaim;
    api_trace_flush_mutation_claimed(L, errL, &claim);
    if (!lj_state_tryclaim(L1, lj_thr_current_id(G(L)), &thclaim)) {
      lj_state_dropresumeclaim(&claim);
      lj_err_callermsg(errL, "thread busy");
    }
    lj_state_env_rel(L1, t);
    lj_state_dropclaim(&thclaim);
    lj_gc_pubobjobj(L, obj2gco(L1), t);
  } else {
    L->top--;
    lj_state_dropresumeclaim(&claim);
    return 0;
  }
  L->top--;
  lj_state_dropresumeclaim(&claim);
  return 1;
}

LUA_API const char *lua_setupvalue(lua_State *L, int idx, int n)
{
  LJStateClaim claim;
  lua_State *errL = api_errstate(L);
  TValue snap;
  cTValue *f;
  TValue *val;
  GCobj *o;
  const char *name;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(errL, "thread busy");
  f = index2adr_read(L, idx, &snap);
  lj_checkapi_slot(1);
  name = lj_debug_uvnamev(f, (uint32_t)(n-1), &val, &o);
  if (name) {
    if (o->gch.gct == ~LJ_TFUNC && !isluafunc(gco2func(o)))
      api_trace_flush_mutation_claimed(L, errL, &claim);
    else if (o->gch.gct == ~LJ_TUPVAL &&
	     tv_rawload_acq(val) != tv_rawload(L->top-1))
      api_trace_flush_mutation_claimed(L, errL, &claim);
    L->top--;
    copyTVrel(L, val, L->top);
    lj_gc_pubobjtv(L, o, L->top);
  }
  lj_state_dropresumeclaim(&claim);
  return name;
}

/* -- Calls --------------------------------------------------------------- */

static TValue *api_call_base(lua_State *L, int nargs)
{
  TValue *o = L->top, *base = o - nargs;
  TValue *p;
  L->top = o+1;
  for (; o > base; o--) copyTV(L, o, o-1);
  setnilV(o);
  for (p = o; p < L->top; p++)
    lj_state_stack_pubtv(L, L, p);
  return o+1;
}

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
    int status = lj_vm_pcall_unwind(L, api_call_base(L, nargs), nresults+1, 0);
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
  status = lj_vm_pcall_unwind(L, api_call_base(L, nargs), nresults+1, ef);
  if (status) hook_restore(g, oldh);
  lj_state_dropresumeclaim(&claim);
  return status;
}

static TValue *cpcall(lua_State *L, lua_CFunction func, void *ud)
{
  uint32_t anchoridx;
  GCfunc *fn;
  TValue fnv, *top;
#if LJ_64
  ud = lj_lightud_intern(L, ud);
#endif
  fn = lj_func_newC(L, 0, getcurrenv(L), &anchoridx);
  top = L->top;
  fn->c.f = func;
  setfuncV(L, &fnv, fn);
  copyTVrel(L, top++, &fnv);
  if (LJ_FR2) setnilV(top++);
  setrawlightudV(top++, ud);
  cframe_nres(L->cframe) = 1+0;  /* Zero results. */
  L->top = top;
  lj_state_stack_pubrange(L, L);
  lj_tg_root_anchor_pop(L2TG(L), anchoridx);
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
  ApiGCRoot root = { NULL, 0 };
  lua_State *errL;
  GCstr *key;
  api_checkclaim(L, &preclaim);
  lj_state_dropclaim(&preclaim);
  errL = api_errstate(L);
  key = api_str_newz_rooted(errL, field, &root);
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim)) {
    api_gcroot_release(&root);
    lj_err_callermsg(errL, "thread busy");
  }
  api_checkstack1_gcroot_claimed(L, errL, &claim, &root);
  if (api_getmetafield_key_claimed(L, idx, key, &claim)) {
    TValue snap;
    TValue *top = L->top--;
    api_gcroot_release(&root);
    if (LJ_FR2) setnilV(top++);
    copyTV(L, top++, index2adr_read(L, idx, &snap));
    L->top = top;
    lj_state_stack_pubrange(L, L);
    api_vm_call_claimed(L, top-1, 1+1, &claim);
    lj_state_dropresumeclaim(&claim);
    return 1;
  }
  api_gcroot_release(&root);
  lj_state_dropresumeclaim(&claim);
  return 0;
}

/* -- Coroutine yield and resume ------------------------------------------ */

LUA_API int lua_isyieldable(lua_State *L)
{
  LJStateClaim claim;
  int ok;
  if (!lj_state_tryclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(api_errstate(L), "thread busy");
  ok = cframe_canyield(L->cframe);
  lj_state_dropclaim(&claim);
  return ok;
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
    status = lj_vm_resume_unwind(L,
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
  /* First attachment publishes mt_live before initializing its saved threshold.
  ** Publish explicit STOP independently so that stale MT data cannot restart
  ** automatic collection. Attach/detach and temporary finalizer thresholds
  ** never clear this veto; only a public restart/full collection does. */
  if (threshold == LJ_MAX_MEM)
    lj_gc_auto_stopped_store(g, 1);
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
  if (threshold != LJ_MAX_MEM)
    lj_gc_auto_stopped_store(g, 0);
}

static GCSize api_gc_restart_threshold(global_State *g)
{
  GCSize total = lj_gc_total_load(g);
  return (total/100) * lj_gc_pause_load(g);
}

static TValue *api_gc_collect_cp(lua_State *L, lua_CFunction dummy, void *ud)
{
  int *result = (int *)ud;
  UNUSED(dummy);
  *result = lj_gc2_collect_active(L);
  return NULL;
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
    /* b1.2 permits physical string-body reclamation only for this explicit
    ** full-collection boundary. Admission remains opportunistic and refuses
    ** any live/entering secondary mutator or configured GC worker pool. */
    {
      int collected = 0;
      int errcode;
      lj_str_gc2_reclaim_request(g);
      /* Keep the one-shot request cleanup outside the protected collector.
      ** All post-admission operations are non-throwing; protection also clears
      ** a request if an earlier mark/bridge allocation ever gains an error
      ** path in a later revision. */
      errcode = lj_vm_cpcall(L, NULL, &collected, api_gc_collect_cp);
      lj_str_gc2_reclaim_cancel(g);
      if (LJ_UNLIKELY(errcode != LUA_OK))
	lj_err_throw(L, errcode);
      UNUSED(collected);
    }
    api_gc_setlogical(g, api_gc_restart_threshold(g));
    break;
  case LUA_GCCOUNT:
    res = (int)(lj_gc_total_load(g) >> 10);
    break;
  case LUA_GCCOUNTB:
    res = (int)(lj_gc_total_load(g) & 0x3ff);
    break;
  case LUA_GCSTEP:
    res = lj_gc2_step_explicit(L, data > 0 ? (uint32_t)data : 1u);
    break;
  case LUA_GCSETPAUSE:
    res = (int)lj_gc_pause_xchg(g, (MSize)data);
    gc2_gcpause_pct_rel(g, data > 0 ? (uint32_t)data : 1u);
    lj_gc2_update_pacing(g);
    lj_gc2_publish_idle_threshold(g);
    break;
  case LUA_GCSETSTEPMUL:
    res = (int)lj_gc_stepmul_xchg(g, (MSize)data);
    gc2_assist_shift_rel(g, lj_gc2_assist_shift_from_stepmul((uint32_t)data));
    break;
  case LUA_GCISRUNNING:
    res = (!lj_gc_auto_stopped_load(g) &&
	   (mt_live_acq(g) != 0 ? lj_gc_mt_threshold_load(g) :
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
#if LJ_GC2_INTERNAL_ALLOCATOR_ONLY
  /* Temporary GC2 policy: preserve the arena ownership domain. */
  UNUSED(L); UNUSED(f); UNUSED(ud);
#else
  global_State *g = G(L);
  uint32_t arena = (f == lj_arena_allocf);
  if (!arena)
    la_store32_rel(&g->allocf_arena, 0);
  g->allocd = ud;
  g->allocf = f;
  if (arena)
    la_store32_rel(&g->allocf_arena, 1);
#endif
}
