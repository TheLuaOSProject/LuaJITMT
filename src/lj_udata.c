/*
** Userdata handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_udata_c
#define LUA_CORE

#include <stdlib.h>

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_udata.h"
#if LJ_HASFFI
#include "lj_clib.h"
#endif

#if defined(LJ_UDATA_TEST_HELPERS)
static uint32_t udata_test_fail_finreg;

void lj_udata_test_fail_finreg_after(uint32_t nth)
{
  la_store32_rel(&udata_test_fail_finreg, nth);
}

static int udata_test_fail_finreg_take(void)
{
  uint32_t old = la_load32_acq(&udata_test_fail_finreg);
  while (old != 0) {
    if (la_cas32(&udata_test_fail_finreg, &old, old - 1u,
		 LA_ACQ_REL, LA_ACQ))
      return old == 1u;
  }
  return 0;
}
#else
#define udata_test_fail_finreg_take() 0
#endif

static GCudata *udata_new_at_anchor(lua_State *L, MSize sz, GCtab *env,
				    LJUdataRoot *root, TValue *anchor,
				    uint32_t idx)
{
  GCudata *ud;
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  TValue udv;
  ud = (GCudata *)lj_mem_newgco_unlinked_nothrow(L,
						 sizeof(GCudata) + sz);
  if (LJ_UNLIKELY(ud == NULL)) {
    lj_tg_root_anchor_pop(tg, idx);
    lj_err_mem(L);
  }
  newwhite(g, ud);  /* Not finalized. */
  ud->gct = ~LJ_TUDATA;
  ud->len = sz;
  ud->unused2 = 0;
  ud->align1 = 0;
  lj_udata_metatable_rel(ud, NULL);
  lj_udata_env_rel(ud, env);
  lj_udata_udtype_rel(ud, UDTYPE_USERDATA);

  /*
  ** Install the identity while READY=0.  A concurrent root scan may observe
  ** the TValue, but arena admission rejects the opaque body.  READY publication
  ** followed by the root barrier repairs precisely that observation without a
  ** window in which a fully admitted object has no semantic constructor root.
  */
  setudataV(L, &udv, ud);
  copyTVrel(L, anchor, &udv);
  lj_gc_publishobj_header(g, obj2gco(ud));
  lj_gc_pubroot(L, anchor);
  /* Chain to userdata list after the main thread on the pending-root flush. */
  lj_gc_linkobj_new_after_main(g, obj2gco(ud));
  if (env)
    lj_gc_pubobjobj(L, ud, env);
  root->tg = tg;
  root->idx = idx;
  return ud;
}

GCudata *lj_udata_newrooted(lua_State *L, MSize sz, GCtab *env,
			    LJUdataRoot *root)
{
  TGState *tg = L2TG(L);
  TValue envv;
  TValue *anchor;
  uint32_t idx;
  lj_assertL(root != NULL, "missing userdata construction root");
  if (LJ_UNLIKELY(sz > LJ_MAX_MEM32 - sizeof(GCudata)))
    lj_err_msg(L, LJ_ERR_UDATAOV);
  root->tg = NULL;
  root->idx = 0;
  if (env)
    settabV(L, &envv, env);
  else
    setnilV(&envv);
  anchor = lj_tg_root_anchor_push(L, tg, &envv, &idx);
  if (LJ_UNLIKELY(anchor == NULL))
    lj_err_mem(L);
  lj_gc_pubroot(L, anchor);
  return udata_new_at_anchor(L, sz, env, root, anchor, idx);
}

GCudata *lj_udata_newrooted_envrooted(lua_State *L, MSize sz, GCtab *env,
				      LJUdataRoot *root)
{
  TValue snap;
  TValue *anchor;
  lj_assertL(root != NULL && root->tg == L2TG(L),
	     "userdata environment root has the wrong TG");
  anchor = lj_tg_root_anchor_slot_acq(root->tg, root->idx);
  lj_assertL(anchor != NULL &&
	     lj_tg_root_anchor_top_acq(root->tg) == root->idx + 1u,
	     "userdata environment root is not the top anchor");
  if (LJ_UNLIKELY(sz > LJ_MAX_MEM32 - sizeof(GCudata))) {
    lj_udata_root_release(root);
    lj_err_msg(L, LJ_ERR_UDATAOV);
  }
  lj_tv_load_acq(&snap, anchor);
  lj_assertL((env && tvistab(&snap) && tabV(&snap) == env) ||
	     (!env && tvisnil(&snap)), "wrong userdata environment root");
  return udata_new_at_anchor(L, sz, env, root, anchor, root->idx);
}

GCudata *lj_udata_new(lua_State *L, MSize sz, GCtab *env)
{
  LJUdataRoot root;
  GCudata *ud = lj_udata_newrooted(L, sz, env, &root);
  /* Compatibility entry.  Callers which allocate or wait before installing
  ** their own semantic root must use lj_udata_newrooted() directly. */
  lj_udata_root_release(&root);
  return ud;
}

void lj_udata_root_release(LJUdataRoot *root)
{
  if (!root || !root->tg)
    return;
  lj_tg_root_anchor_pop(root->tg, root->idx);
  root->tg = NULL;
  root->idx = 0;
}

void lj_udata_pushrooted(lua_State *L, GCudata *ud, LJUdataRoot *root)
{
  lj_assertL(L != NULL && ud != NULL && root != NULL && root->tg != NULL,
	     "invalid rooted userdata stack handoff");
  /* This handoff is deliberately non-throwing.  Callers must reserve the
  ** result slot before creating the constructor root. */
  lj_assertL((mref(L->maxstack, char) - (char *)L->top) >
	     (ptrdiff_t)sizeof(TValue),
	     "rooted userdata push without a pre-reserved stack slot");
  setudataV(L, L->top, ud);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_udata_root_release(root);
}

void lj_udata_rescan(lua_State *L, GCudata *ud)
{
  if (L && ud)
    lj_gc_pubobjroot(L, obj2gco(ud));
}

void lj_udata_finreg_mt_rooted(lua_State *L, GCudata *ud, GCtab *mt,
			       LJUdataRoot *root)
{
  lj_assertL(L != NULL && ud != NULL && root != NULL && root->tg != NULL,
	     "invalid rooted userdata FINREG registration");
  if (LJ_UNLIKELY(udata_test_fail_finreg_take() ||
		  !lj_gc2_finreg_udata_register_mt_nothrow(
			    L, G(L), ud, mt))) {
    /* OOM is reported before the FINREG primitive mutates its registry.  Drop
    ** the semantic constructor root before raising so Lua-level pcall/xpcall,
    ** which may not pass through a C checkpoint wrapper, cannot strand it. */
    lj_udata_root_release(root);
    lj_err_mem(L);
  }
}

void lj_udata_specialize(lua_State *L, GCudata *ud, uint8_t udtype)
{
  uint8_t expect = UDTYPE_USERDATA;
  GCSize size;
  if (!L || !ud || udtype == UDTYPE_USERDATA || udtype >= UDTYPE__MAX) {
    lj_assertL(0, "invalid userdata specialization request");
    abort();
  }
  /* Validate the prospective geometry while the public subtype is still the
  ** opaque generic form.  No scanner can observe a specialized tag until all
  ** fields used by that validator have passed. */
  if (LJ_UNLIKELY(!lj_gc_udata_payload_valid_as(ud, udtype, &size))) {
    UNUSED(size);
    lj_assertL(0, "invalid specialized userdata payload");
    abort();
  }
  UNUSED(size);
  if (!la_cas8(&ud->udtype, &expect, udtype, LA_REL, LA_ACQ)) {
    lj_assertL(0, "userdata specialized more than once or to invalid type");
    abort();
  }
  /* The subtype release-CAS is the one-time payload publication point.  Force
  ** a fresh root-container traversal in an active cycle (or remember it while
  ** IDLE) to repair a scan which observed the generic payload just before it. */
  lj_udata_rescan(L, ud);
}

void LJ_FASTCALL lj_udata_free(global_State *g, GCudata *ud)
{
  GCSize size = sizeudata(ud);
  if (lj_udata_udtype_acq(ud) == UDTYPE_THREAD) {
    LJThread *th = (LJThread *)uddata(ud);
    TValue *roots = lj_thread_start_roots_acq(th);
    uint32_t n = lj_thread_start_root_count_acq(th);
    lj_assertG(lj_thread_live_node_acq(th) == NULL,
	       "free of published threading.thread userdata");
    if (roots)
      lj_mem_free(g, roots, (size_t)n * sizeof(TValue));
  }
#if LJ_HASFFI
  if (lj_udata_udtype_acq(ud) == UDTYPE_FFI_CLIB) {
    CLibrary *cl = (CLibrary *)uddata(ud);
    if (LJ_UNLIKELY(lj_gc2_reclaim_context_held(g) &&
		    (lj_clib_handle_acq(cl) != NULL ||
		     lj_clib_cache_head_acq(cl) != NULL))) {
      /* Runtime physical reclaim owns the exclusive GC2 writer and must never
      ** enter dlclose/FreeLibrary or walk an unbounded live symbol cache. The
      ** registered ffi_clib finalizer performs both semantic operations before
      ** FINREG releases the userdata to sweep. Joined-world close is the sole
      ** path allowed to perform that work from the type destructor. */
      lj_assertG(0, "GC2 reclaimed an unfinalized FFI CLibrary");
      abort();
    }
    lj_clib_unload(NULL, g, cl);
  }
#endif
  if (!lj_mem_freegco_defer(g, ud, size))
    lj_mem_free(g, ud, size);
}

#if LJ_64
void *lj_lightud_intern(lua_State *L, void *p)
{
  global_State *g = G(L);
  uint64_t u = (uint64_t)p;
  uint32_t up = lightudup(u);
  uint32_t *segmap = mref(g->gc.lightudseg, uint32_t);
  MSize segnum = g->gc.lightudnum;
  if (segmap) {
    MSize seg;
    for (seg = 0; seg <= segnum; seg++)
      if (segmap[seg] == up)  /* Fast path. */
	return (void *)(((uint64_t)seg << LJ_LIGHTUD_BITS_LO) | lightudlo(u));
    segnum++;
    /* Leave last segment unused to avoid clash with ITERN key. */
    if (segnum >= (1 << LJ_LIGHTUD_BITS_SEG)-1) lj_err_msg(L, LJ_ERR_BADLU);
  }
  if (!((segnum-1) & segnum) && segnum != 1) {
    lj_mem_reallocvec(L, segmap, segnum, segnum ? 2*segnum : 2u, uint32_t);
    setmref(g->gc.lightudseg, segmap);
  }
  g->gc.lightudnum = segnum;
  segmap[segnum] = up;
  return (void *)(((uint64_t)segnum << LJ_LIGHTUD_BITS_LO) | lightudlo(u));
}
#endif
