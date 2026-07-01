/*
** State and stack handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_state_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ccallback.h"
#include "lj_ctype.h"
#include "lj_clib.h"
#endif
#include "lj_trace.h"
#include "lj_dispatch.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_vm.h"
#include "lj_prng.h"
#include "lj_lex.h"
#include "lj_alloc.h"
#include "lj_mcode.h"
#include "luajit.h"

/* -- Stack handling ------------------------------------------------------ */

/* Stack sizes. */
#define LJ_STACK_MIN	LUA_MINSTACK	/* Min. stack size. */
#define LJ_STACK_MAX	LUAI_MAXSTACK	/* Max. stack size. */
#define LJ_STACK_START	(2*LJ_STACK_MIN)	/* Starting stack size. */
#define LJ_STACK_MAXEX	(LJ_STACK_MAX + 1 + LJ_STACK_EXTRA)

/* Explanation of LJ_STACK_EXTRA:
**
** Calls to metamethods store their arguments beyond the current top
** without checking for the stack limit. This avoids stack resizes which
** would invalidate passed TValue pointers. The stack check is performed
** later by the function header. This can safely resize the stack or raise
** an error. Thus we need some extra slots beyond the current stack limit.
**
** Most metamethods need 4 slots above top (cont, mobj, arg1, arg2) plus
** one extra slot if mobj is not a function. Only lj_meta_tset needs 5
** slots above top, but then mobj is always a function. So we can get by
** with 5 extra slots.
** LJ_FR2: We need 2 more slots for the frame PC and the continuation PC.
*/

/* Resize stack slots and adjust pointers in state. */
static void resizestack(lua_State *L, MSize n)
{
  TValue *st, *oldst = tvref(L->stack);
  ptrdiff_t delta;
  MSize oldsize = L->stacksize;
  MSize realsize = n + 1 + LJ_STACK_EXTRA;
  TValue *jbase;
  uintptr_t jaddr, oldaddr;
  GCobj *up;
  lj_assertL((MSize)(tvref(L->maxstack)-oldst) == L->stacksize-LJ_STACK_EXTRA-1,
	     "inconsistent stack size");
  st = (TValue *)lj_mem_realloc(L, tvref(L->stack),
				(MSize)(oldsize*sizeof(TValue)),
				(MSize)(realsize*sizeof(TValue)));
  setmref(L->stack, st);
  delta = (char *)st - (char *)oldst;
  setmref(L->maxstack, st + n);
  while (oldsize < realsize)  /* Clear new slots. */
    setnilV(st + oldsize++);
  L->stacksize = realsize;
  jbase = lj_tg_jit_base(G(L));
  jaddr = (uintptr_t)jbase;
  oldaddr = (uintptr_t)oldst;
  if (jaddr - oldaddr < (uintptr_t)oldsize * sizeof(TValue))
    lj_tg_setjit_base(G(L), (TValue *)((char *)jbase + delta));
  L->base = (TValue *)((char *)L->base + delta);
  L->top = (TValue *)((char *)L->top + delta);
  for (up = lj_state_openupval_acq(L); up != NULL;
       up = lj_obj_gcw_acq(up))
    setmref(gco2uv(up)->v, (TValue *)((char *)uvval(gco2uv(up)) + delta));
}

int lj_state_rehome_stack(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  TValue *oldst = tvref(L->stack);
  TValue *st;
  ptrdiff_t delta;
  GCobj *up;
  MSize stacksize = L->stacksize;
  size_t sz = (size_t)stacksize * sizeof(TValue);
  if (!oldst || !tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ||
      g->allocf != lj_arena_allocf)
    return 1;
  if (lj_arena_owner_acq(lj_arena_of(oldst)) ==
      lj_arena_alloc_owner_acq(&tg->alloc))
    return 1;
  st = (TValue *)lj_arena_allocf(&tg->allocd, NULL, 0, sz);
  if (st == NULL)
    return 0;
  memcpy(st, oldst, sz);
  lj_gc_total_add(g, (GCSize)sz);
  lj_gc2_account_alloc(g, tg, (GCSize)sz);  /* 04 section 4.8 worker stack. */
  setmref(L->stack, st);
  delta = (char *)st - (char *)oldst;
  setmref(L->maxstack, (TValue *)((char *)tvref(L->maxstack) + delta));
  L->base = (TValue *)((char *)L->base + delta);
  L->top = (TValue *)((char *)L->top + delta);
  for (up = lj_state_openupval_acq(L); up != NULL;
       up = lj_obj_gcw_acq(up))
    setmref(gco2uv(up)->v, (TValue *)((char *)uvval(gco2uv(up)) + delta));
  lj_mem_freevec(g, oldst, stacksize, TValue);
  return 1;
}

/* Relimit stack after error, in case the limit was overdrawn. */
void lj_state_relimitstack(lua_State *L)
{
  if (L->stacksize > LJ_STACK_MAXEX && L->top-tvref(L->stack) < LJ_STACK_MAX-1)
    resizestack(L, LJ_STACK_MAX);
}

/* Try to shrink the stack (called from GC). */
void lj_state_shrinkstack(lua_State *L, MSize used)
{
  if (L->stacksize > LJ_STACK_MAXEX)
    return;  /* Avoid stack shrinking while handling stack overflow. */
  if (4*used < L->stacksize &&
      2*(LJ_STACK_START+LJ_STACK_EXTRA) < L->stacksize &&
      /* Don't shrink stack of live trace. */
      (lj_tg_jit_base(G(L)) == NULL || L != lj_tg_cur_L(G(L))))
    resizestack(L, L->stacksize >> 1);
}

/* Try to grow stack. */
void LJ_FASTCALL lj_state_growstack(lua_State *L, MSize need)
{
  MSize n = L->stacksize + need;
  if (LJ_LIKELY(n < LJ_STACK_MAX)) {  /* The stack can grow as requested. */
    if (n < 2 * L->stacksize) {  /* Try to double the size. */
      n = 2 * L->stacksize;
      if (n > LJ_STACK_MAX)
	n = LJ_STACK_MAX;
    }
    resizestack(L, n);
  } else {  /* Request would overflow. Raise a stack overflow error. */
    if (LJ_HASJIT) {
      TValue *base = lj_tg_jit_base(G(L));
      if (base) L->base = base;
    }
    if (curr_funcisL(L)) {
      L->top = curr_topL(L);
      if (L->top > tvref(L->maxstack)) {
	/* The current Lua frame violates the stack, so replace it with a
	** dummy. This can happen when BC_IFUNCF is trying to grow the stack.
	*/
	L->top = L->base;
	setframe_gc(L->base - 1 - LJ_FR2, obj2gco(L), LJ_TTHREAD);
      }
    }
    if (L->stacksize <= LJ_STACK_MAXEX) {
      /* An error handler might want to inspect the stack overflow error, but
      ** will need some stack space to run in. We give it a stack size beyond
      ** the normal limit in order to do so, then rely on lj_state_relimitstack
      ** calls during unwinding to bring us back to a convential stack size.
      ** The + 1 is space for the error message, and 2 * LUA_MINSTACK is for
      ** the lj_state_checkstack() call in lj_err_run().
      */
      resizestack(L, LJ_STACK_MAX + 1 + 2 * LUA_MINSTACK);
      lj_err_stkov(L);  /* May invoke an error handler. */
    } else {
      /* If we're here, then the stack overflow error handler is requesting
      ** to grow the stack even further. We have no choice but to abort the
      ** error handler.
      */
      GCstr *em = lj_err_str(L, LJ_ERR_STKOV);  /* Might OOM. */
      setstrV(L, L->top++, em);  /* There is always space to push an error. */
      lj_err_throw(L, LUA_ERRERR);  /* Does not invoke an error handler. */
    }
  }
}

void LJ_FASTCALL lj_state_growstack1(lua_State *L)
{
  lj_state_growstack(L, 1);
}

static TValue *cpgrowstack(lua_State *co, lua_CFunction dummy, void *ud)
{
  UNUSED(dummy);
  lj_state_growstack(co, *(MSize *)ud);
  return NULL;
}

int LJ_FASTCALL lj_state_cpgrowstack(lua_State *L, MSize need)
{
  return lj_vm_cpcall(L, NULL, &need, cpgrowstack);
}

/* Allocate basic stack for new state. */
static void stack_init(lua_State *L1, lua_State *L)
{
  TValue *stend, *st = lj_mem_newvec(L, LJ_STACK_START+LJ_STACK_EXTRA, TValue);
  setmref(L1->stack, st);
  L1->stacksize = LJ_STACK_START + LJ_STACK_EXTRA;
  stend = st + L1->stacksize;
  setmref(L1->maxstack, stend - LJ_STACK_EXTRA - 1);
  setthreadV(L1, st++, L1);  /* Needed for curr_funcisL() on empty stack. */
  if (LJ_FR2) setnilV(st++);
  L1->base = L1->top = st;
  while (st < stend)  /* Clear new slots. */
    setnilV(st++);
}

/* -- State handling ------------------------------------------------------ */

/* Open parts that may cause memory-allocation errors. */
static TValue *cpluaopen(lua_State *L, lua_CFunction dummy, void *ud)
{
  global_State *g = G(L);
  UNUSED(dummy);
  UNUSED(ud);
  stack_init(L, L);
  /* NOBARRIER: State initialization, all objects are white. */
  lj_state_env_rel(L, lj_tab_new(L, 0, LJ_MIN_GLOBAL));
  lj_registry_settab_rel(L, lj_tab_new(L, 0, LJ_MIN_REGISTRY));
  lj_str_init(L);
  lj_meta_init(L);
  lj_lex_init(L);
  fixstring(lj_err_str(L, LJ_ERR_ERRMEM));  /* Preallocate memory error msg. */
  fixstring(lj_err_str(L, LJ_ERR_ERRERR));  /* Preallocate err in err msg. */
  lj_gc_threshold_store(g, 4*lj_gc_total_load(g));
  lj_mcode_init(g);
  lj_trace_initstate(g);
  lj_err_verify();
  vmthread_rel(g, lj_state_new(L));
  lj_gc2_update_pacing(g);
  lj_gc2_publish_idle_threshold(g);
  return NULL;
}

static void close_state(lua_State *L)
{
  global_State *g = G(L);
  int arena_alloc = g->main_tg &&
		    lj_tg_flags_test_acq(g->main_tg, TGF_ARENA_INTERNAL);
  lj_func_closeuv(L, tvref(L->stack));
  lj_gc_freeall(g);
  lj_assertG(lj_gc_root_acq(g) == obj2gco(L),
	     "main thread is not first GC object");
  lj_assertG(g->str.num == 0, "leaked %d strings", g->str.num);
  lj_trace_freestate(g);
#if LJ_HASFFI
  lj_ctype_freestate(g);
  lj_clib_cache_freeretired(g);
#endif
  lj_str_freetab(g);
  lj_tab_freeretired(g);
  lj_gc2_fini(g);
  if (lj_thr_get_tg() == g->main_tg)
    lj_thr_set_tg(NULL);
  if (arena_alloc && g->main_tg) {
    lj_tg_fini_ssb(g->main_tg);
    lj_buf_free(g, &g->main_tg->tmpbuf);
  } else {
    lj_tg_fini(g);
  }
  lj_buf_free(g, &g->tmpbuf);
  lj_mem_freevec(g, tvref(L->stack), L->stacksize, TValue);
#if LJ_64
  if (mref(g->gc.lightudseg, uint32_t)) {
    MSize segnum = g->gc.lightudnum ? (2 << lj_fls(g->gc.lightudnum)) : 2;
    lj_mem_freevec(g, mref(g->gc.lightudseg, uint32_t), segnum, uint32_t);
  }
#endif
  lj_assertG(lj_gc_total_load(g) == sizeof(GG_State),
	     "memory leak of %lld bytes",
	     (long long)(lj_gc_total_load(g) - sizeof(GG_State)));
  if (arena_alloc) {
    GG_State *GG = G2GG(g);
    int gghuge = lj_arena_ishuge(lj_arena_of(GG));
    TGAlloc alloc;
    if (g->main_tg)
      alloc = g->main_tg->alloc;
    else
      lj_arena_alloc_init(&alloc);
    if (g->main_tg && lj_tg_flags_test_acq(g->main_tg, TGF_HUGETAB))
      lj_arena_hugetab_fini(&g->main_tg->huge);
    lj_arena_alloc_fini(&alloc);
    if (gghuge)
      lj_arena_huge_unmap(GG, sizeof(GG_State));
    return;
  }
#ifndef LUAJIT_USE_SYSMALLOC
  if (g->allocf == lj_alloc_f)
    lj_alloc_destroy(g->allocd);
  else
#endif
    g->allocf(g->allocd, G2GG(g), sizeof(GG_State), 0);
}

#if LJ_64 && !LJ_GC64 && !(defined(LUAJIT_USE_VALGRIND) && defined(LUAJIT_USE_SYSMALLOC))
lua_State *lj_state_newstate(lua_Alloc allocf, void *allocd)
#else
LUA_API lua_State *lua_newstate(lua_Alloc allocf, void *allocd)
#endif
{
  PRNGState prng;
  TGAlloc boot_alloc;
  LJArenaAllocD boot_ad;
  GG_State *GG;
  lua_State *L;
  global_State *g;
  int arena_internal = 0;
  /* We need the PRNG for the memory allocator, so initialize this first. */
  if (!lj_prng_seed_secure_l(NULL, &prng)) {
    lj_assertX(0, "secure PRNG seeding failed");
    /* Can only return NULL here, so this errors with "not enough memory". */
    return NULL;
  }
  if (allocf == LJ_ALLOCF_INTERNAL) {
    lj_arena_alloc_init(&boot_alloc);
    lj_arena_allocd_init(&boot_ad, &boot_alloc, &prng, 0);
    allocf = lj_arena_allocf;
    allocd = &boot_ad;
    arena_internal = 1;
  }
  GG = (GG_State *)allocf(allocd, NULL, 0, sizeof(GG_State));
  if (GG == NULL) {
    if (arena_internal)
      lj_arena_alloc_fini(&boot_alloc);
    return NULL;
  }
  if (!checkptrGC(GG)) {
    allocf(allocd, GG, sizeof(GG_State), 0);
    if (arena_internal)
      lj_arena_alloc_fini(&boot_alloc);
    return NULL;
  }
  memset(GG, 0, sizeof(GG_State));
  if (arena_internal) {
    GG->main_tg.alloc = boot_alloc;
    GG->main_tg.prng = prng;
    lj_arena_allocd_init(&GG->main_tg.allocd, &GG->main_tg.alloc,
			 &GG->main_tg.prng, 0);
    allocd = &GG->main_tg.allocd;
    lj_arena_alloc_init(&boot_alloc);
  }
  L = &GG->L;
  g = &GG->g;
  L->gct = ~LJ_TTHREAD;
  lj_obj_setgcflags(obj2gco(L), LJ_GC_WHITE0 | LJ_GC_FIXED | LJ_GC_SFIXED);
  L->dummy_ffid = FF_C;
  setmref(L->glref, g);
  lj_state_owner_rel(L, 0);
  lj_state_scan_epoch_rel(L, 0);
  lj_state_scan_dirty_epoch_rel(L, 0);
  lj_state_scan_handoff_epoch_rel(L, 0);
  g->gc.currentwhite = LJ_GC_WHITE0 | LJ_GC_FIXED;
  g->strempty.marked = LJ_GC_WHITE0;
  g->strempty.gct = ~LJ_TSTR;
  g->allocf = allocf;
  g->allocd = allocd;
  g->prng = prng;
#if LJ_HASJIT
  g->jitp = &GG->J;
#endif
#ifndef LUAJIT_USE_SYSMALLOC
  if (allocf == lj_alloc_f) {
    lj_alloc_setprng(allocd, &g->prng);
  }
#endif
  mainthread_rel(g, L);
  lj_uv_setprev_rel(&g->uvhead, &g->uvhead);
  lj_uv_setnext_rel(&g->uvhead, &g->uvhead);
  lj_str_tabh_store_rlx(g, NULL);
  lj_str_retired_head_store_rlx(g, NULL);
  g->str.mask = ~(MSize)0;
  g->tab.retired_nodes = NULL;
  g->tab.retired_arrays = NULL;
  lj_registry_setnil_rel(L);
  g->nilnodehdr.hmask = 0;
  g->nilnodehdr.flags = 0;
  setmref(g->nilnodehdr.next_gen, NULL);
#if !LJ_GC64
  g->nilnodehdr.reserved = 0;
#endif
  setnilV(&g->nilnode.val);
  setnilV(&g->nilnode.key);
#if !LJ_GC64
  setmref(g->nilnode.freetop, &g->nilnode);
#endif
  lj_buf_init(NULL, &g->tmpbuf);
  g->gc.state = GCSpause;
  lj_gc_root_rel(g, obj2gco(L));
  setmref(g->gc.sweep, lj_gc_root_ref(g));
  lj_gc_total_store(g, sizeof(GG_State));
  g->gc.pause = LUAI_GCPAUSE;
  g->gc.stepmul = LUAI_GCMUL;
  lj_dispatch_init((GG_State *)L);
  lj_tg_init((GG_State *)L, arena_internal);
  lj_gc2_init(g);
  L->status = LUA_ERRERR+1;  /* Avoid touching the stack upon memory error. */
  if (lj_vm_cpcall(L, NULL, NULL, cpluaopen) != 0) {
    /* Memory allocation error: free partial state. */
    close_state(L);
    return NULL;
  }
  L->status = LUA_OK;
  return L;
}

static TValue *cpfinalize(lua_State *L, lua_CFunction dummy, void *ud)
{
  UNUSED(dummy);
  UNUSED(ud);
#if LJ_HASFFI
  (void)lj_gc2_finreg_cdata_finalize_close(G(L));
#endif
  lj_gc2_finalizer_dispatch_all(L);
  /* Frame pop omitted. */
  return NULL;
}

LUA_API void lua_close(lua_State *L)
{
  global_State *g = G(L);
  L = mainthread_acq(g);  /* Only the main thread can be closed. */
#if LJ_HASPROFILE
  luaJIT_profile_stop(L);
#endif
  lj_threading_shutdown(L);
  lj_tg_clearcur_L(g);
  lj_func_closeuv(L, tvref(L->stack));
  /* Separate udata which have GC metamethods. */
  lj_gc2_finreg_udata_finalize(g, 1);
#if LJ_HASJIT
  jit_flags_setmask(G2J(g), JIT_F_ON, 0);
  lj_trace_state_store(G2J(g), LJ_TRACE_IDLE);
  lj_dispatch_update(g, 0);
#endif
  for (;;) {
    hook_enter(g);
    L->status = LUA_OK;
    L->base = L->top = tvref(L->stack) + 1 + LJ_FR2;
    L->cframe = NULL;
    if (lj_vm_cpcall(L, NULL, NULL, cpfinalize) == LUA_OK) {
      /* Separate udata again. */
      lj_gc2_finreg_udata_finalize(g, 1);
      /* Until nothing is left to do. */
      if (!lj_gc2_finalizer_close_pending(g))
	break;
    }
  }
#if LJ_HASFFI
  lj_gc2_finreg_cdata_disable(g);
#endif
  close_state(L);
}

lua_State *lj_state_new(lua_State *L)
{
  lua_State *L1 = lj_mem_newobj(L, lua_State);
  L1->gct = ~LJ_TTHREAD;
  L1->dummy_ffid = FF_C;
  L1->status = LUA_OK;
  L1->stacksize = 0;
  setmref(L1->stack, NULL);
  L1->cframe = NULL;
  L1->tg_hint = NULL;
  lj_state_owner_rel(L1, 0);
  lj_state_scan_epoch_rel(L1, 0);
  lj_state_scan_dirty_epoch_rel(L1, 0);
  lj_state_scan_handoff_epoch_rel(L1, 0);
  lj_state_openupval_clear_rel(L1);
  lj_state_mt_thread_clear_rel(L1);
  setmrefr(L1->glref, L->glref);
  lj_state_env_copy_rel(L1, L);
  {
    GCtab *env = lj_state_env_acq(L1);
    if (env)
      lj_gc_pubobjobj(L, L1, env);
  }
  stack_init(L1, L);  /* init stack */
  lj_assertL(iswhite(obj2gco(L1)), "new thread object is not white");
  return L1;
}

void LJ_FASTCALL lj_state_free(global_State *g, lua_State *L)
{
  lj_assertG(L != mainthread_acq(g), "free of main thread");
#if LJ_HASFFI
  lj_ccallback_disown_state(L);
#endif
  if (L == lj_tg_cur_L(g))
    lj_tg_clearcur_L(g);
  if (lj_state_openupval_acq(L) != NULL) {
    lj_func_closeuv(L, tvref(L->stack));
    lj_trace_abort(g);  /* For aa_uref soundness. */
    lj_assertG(lj_state_openupval_acq(L) == NULL, "stale open upvalues");
  }
  lj_mem_freevec(g, tvref(L->stack), L->stacksize, TValue);
  lj_mem_freet(g, L);
}
