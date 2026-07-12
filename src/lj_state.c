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

int lj_vm_cpcall(lua_State *L, lua_CFunction func, void *ud, lua_CPFunction cp)
{
  TGState *oldtg;
  uint32_t owner;
  int status;
  if (LJ_UNLIKELY(L == NULL))
    return LUA_ERRRUN;
  if (LJ_UNLIKELY(mref(L->glref, global_State) == NULL && L->tg_hint != NULL &&
		  L->tg_hint->gl != NULL))
    setmref(L->glref, L->tg_hint->gl);
  oldtg = L->tg_hint;
  owner = lj_state_owner_acq(L);
  if (LJ_UNLIKELY(oldtg == NULL))
    L->tg_hint = lj_thr_get_tg_fallback(G(L));
  status = lj_vm_cpcall_asm(L, func, ud, cp);
  if (LJ_UNLIKELY(oldtg == NULL && owner == 0 && lj_state_owner_acq(L) == 0))
    L->tg_hint = NULL;  /* Ownerless coroutine states stay TG-neutral. */
  return status;
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
  setmrefrel(L->stack, st);
  delta = (char *)st - (char *)oldst;
  setmrefrel(L->maxstack, (TValue *)((char *)tvref(L->maxstack) + delta));
  L->base = (TValue *)((char *)L->base + delta);
  L->top = (TValue *)((char *)L->top + delta);
  for (up = lj_state_openupval_acq(L); up != NULL;
       up = lj_obj_gcw_acq(up))
    setmref(gco2uv(up)->v, (TValue *)((char *)uvval(gco2uv(up)) + delta));
  lj_state_stack_pubrange(L, L);
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
  /*
  ** The running thread may enter GC through a C/JIT helper with saved VM return
  ** state that still names its current stack range. Relocate only stacks that
  ** are not the active TG stack; the active one can shrink after it yields or
  ** becomes a non-current coroutine.
  */
  if (L == lj_tg_cur_L(G(L)))
    return;
  if (4*used < L->stacksize &&
      2*(LJ_STACK_START+LJ_STACK_EXTRA) < L->stacksize)
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

void lj_state_stack_pubtv(lua_State *L, lua_State *target, cTValue *tv)
{
  TGState *tg = target ? L2TG(target) : (L ? L2TG(L) : NULL);
  if (tg)
    lj_tg_stack_dirty_epoch_add_rlx(tg, 1);
  tv_rawstore_rel((TValue *)tv, tv_rawload(tv));
  lj_gc_pubroot(L, tv);
}

void lj_state_stack_pubrange(lua_State *L, lua_State *target)
{
  TValue *o = tvref(target->stack) + 1 + LJ_FR2;
  TValue *top = target->top;
  while (o < top)
    lj_state_stack_pubtv(L, target, o++);
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
  fixstring(g, lj_err_str(L, LJ_ERR_ERRMEM));  /* Preallocate memory error msg. */
  fixstring(g, lj_err_str(L, LJ_ERR_ERRERR));  /* Preallocate err in err msg. */
  lj_gc_threshold_store(g, 4*lj_gc_total_load(g));
  lj_mcode_init(g);
  lj_trace_initstate(g);
  lj_err_verify();
  vmthread_rel(g, lj_state_new(L));
  lj_gc2_update_pacing(g);
  lj_gc2_publish_idle_threshold(g);
  return NULL;
}

static int close_state_root_link_valid(global_State *g, GCobj *o)
{
  GCobj *th;
  if (o == NULL)
    return 0;
  th = gcref_acq(*mainthread_ref(g));
  if (th && th->gch.gct == ~LJ_TTHREAD && o == th)
    return 1;
  th = gcref_acq(*vmthread_ref(g));
  if (th && th->gch.gct == ~LJ_TTHREAD && o == th)
    return 1;
  return lj_gc2_obj_valid(g, o);
}

static void close_state_reanchor_root(global_State *g, GCobj *target)
{
  GCobj *head, *o;
  uint32_t n = 0;
  if (!g || !target || !close_state_root_link_valid(g, target))
    return;
  (void)lj_gc_flush_root_pending(g);
  head = lj_gc_root_acq(g);
  for (o = head; o != NULL;) {
    GCobj *next;
    if (!close_state_root_link_valid(g, o))
      break;
    next = lj_obj_gcw_acq(o);
    if (o == target)
      return;
    if (next == o || ++n >= 1000000u)
      return;
    o = next;
  }
  /* A damaged/stale head is not a valid successor for the reanchored object.
  ** Shutdown is single-threaded here, so fail closed to a one-node spine. */
  if (head && !close_state_root_link_valid(g, head))
    head = NULL;
  lj_obj_setgcwrel(target, head);
  lj_gc_root_rel(g, target);
  lj_gcroot_repair_epoch_add(g);
}

int lj_state_thread_registry_valid(global_State *g, lua_State *th)
{
  return g && th && lj_gc2_obj_valid_queued(g, obj2gco(th)) &&
	 th->gct == ~LJ_TTHREAD;
}

void lj_state_thread_registry_publish(global_State *g, lua_State *th)
{
  lua_State *head;
  if (!g || !th)
    return;
  do {
    head = lj_state_thread_registry_head_acq(g);
    lj_state_thread_registry_next_rel(th, head);
  } while (!lj_state_thread_registry_head_cas(g, &head, th));
}

static void state_registry_remove(global_State *g, lua_State *th)
{
  lua_State *prev, *cur;
  if (!g || !th)
    return;
restart:
  prev = NULL;
  cur = lj_state_thread_registry_head_acq(g);
  while (cur && lj_state_thread_registry_valid(g, cur)) {
    lua_State *next = lj_state_thread_registry_next_acq(cur);
    if (cur == th) {
      if (prev) {
	if (!lj_state_thread_registry_next_cas(prev, &cur, next))
	  goto restart;
      } else {
	if (!lj_state_thread_registry_head_cas(g, &cur, next))
	  goto restart;
      }
      lj_state_thread_registry_next_rel(th, NULL);
      return;
    }
    prev = cur;
    cur = next;
  }
}

static void close_state_reanchor_registered_states(global_State *g,
					     lua_State *L)
{
  lua_State *th = lj_state_thread_registry_head_xchg(g, NULL);
  uint32_t n = 0;
  while (th && lj_state_thread_registry_valid(g, th)) {
    lua_State *next = lj_state_thread_registry_next_acq(th);
    lj_state_thread_registry_next_rel(th, NULL);
    /* A threading.thread userdata may still carry this state pointer after
    ** lj_threading_shutdown(). Keep every exact state on the terminal GC2
    ** ownership spine and let the single destructor drain free it once. */
    if (th != L)
      close_state_reanchor_root(g, obj2gco(th));
    th = next;
    if (++n >= 1000000u)
      break;
  }
}

static void close_state_arena_free_noinsert(global_State *g)
{
  TGState *tg;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg))
    lj_arena_alloc_free_noinsert_rel(&tg->alloc, 1);
}

static void close_state(lua_State *L)
{
  global_State *g = G(L);
  int arena_alloc = g->main_tg &&
		    lj_tg_flags_test_acq(g->main_tg, TGF_ARENA_INTERNAL);
  GCobj *o;
  uint32_t n = 0;
  /* Parked GC2 workers are collector threads, not threading.* children.
  ** Join them before touching TG registries, ownership roots, strings or
  ** destructors; lj_gc2_fini() repeats this idempotently for partial states. */
  if (LJ_UNLIKELY(!lj_gc2_worker_stop(g)))
    abort();  /* A failed join is not permission to reclaim worker storage. */
  /* threading_shutdown tombstones native live-root nodes before entering this
  ** terminal path. Only now are all collector readers joined, so their raw
  ** arena storage may be physically released. */
  lj_threading_live_free_all(g);
#if LJ_HASJIT
  /* Disconnect every live trace while its start prototype and bytecode are
  ** still alive. Retired bodies stay list-owned until trace_freestate(). */
  (void)lj_trace_flushall_gc(L);
#endif
  lj_func_closeuv(L, tvref(L->stack));
  /*
  ** Thread states are strong runtime roots (mainthread_ref, TG roots or
  ** threading.thread userdata). Reanchor them before the shutdown sweep if
  ** lockless root-list maintenance left any thread state off the GC chain.
  */
  close_state_reanchor_root(g, obj2gco(L));
  close_state_reanchor_registered_states(g, L);
  lj_thr_fence();
  if (mt_shutdown_acq(g) != 0)
    (void)lj_tg_reclaim_dead_terminal(g);
  else
    (void)lj_tg_reclaim_dead(g);  /* Partial-state initialization failure. */
  (void)lj_gc_flush_root_pending(g);
  for (o = lj_gc_root_acq(g); o != NULL;) {
    GCobj *next;
    if (!close_state_root_link_valid(g, o))
      break;
    next = lj_obj_gcw_acq(o);
    if (o->gch.gct == ~LJ_TUDATA &&
	lj_udata_udtype_acq(gco2ud(o)) == UDTYPE_THREAD) {
      LJThread *th = (LJThread *)uddata(gco2ud(o));
      lua_State *child = lj_thread_state_load_acq(th);
      if (child)
	close_state_reanchor_root(g, obj2gco(child));
    }
    if (next == o || ++n >= 1000000u)
      break;
    o = next;
  }
  n = 0;
  /* The VM callback state is now owned only by the terminal root drain. Clear
  ** this side root before a custom allocator can release the state body. */
  setgcrefnullrel(*vmthread_ref(g));
  (void)lj_gc2_shutdown_discard_ssb(g);
  /* Terminal discard can drop the final embedded-node pin on a DEAD TG which
  ** the first shutdown scan deliberately retained. Unlink/transfer it before
  ** freeall invalidates userdata and allocator ownership metadata. */
  if (mt_shutdown_acq(g) != 0)
    (void)lj_tg_reclaim_dead_terminal(g);
  if (arena_alloc)
    close_state_arena_free_noinsert(g);
  lj_gc2_freeall(g);
  /* Root/object destructors can close suspended-thread upvalues and run GC2
  ** publication barriers while freeall walks the ownership spine. Workers and
  ** secondary publishers are already joined, so discard that final main-TG
  ** suffix before GC2/TG finalization checks published-node pins. */
  (void)lj_gc2_shutdown_discard_ssb(g);
  (void)lj_gc_flush_root_pending(g);
  for (o = lj_gc_root_acq(g); o != NULL;) {
    GCobj *next;
    if (!close_state_root_link_valid(g, o))
      break;
    next = lj_obj_gcw_acq(o);
    if (o == obj2gco(L))
      break;
    if (next == o || ++n >= 1000000u) {
      lj_assertG(0, "root list cycle after freeall");
      break;
    }
    o = next;
  }
  lj_assertG(o == obj2gco(L), "main thread missing after freeall");
  lj_str_flush_num_credit(g, g->main_tg);
  {
    MSize strnum = lj_str_num_acq(g);
    lj_assertG(strnum == 0, "leaked %d strings", strnum);
    UNUSED(strnum);
  }
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
  /* Close stable body admission and prove zero borrows before destroying any
  ** main-TG subordinate storage. The tagged body remains RECLAIMING until the
  ** final raw-owner/orphan drain below completes. */
  if (!lj_tg_registry_main_close_begin(g))
    abort();
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
  /* This is the final owner-lookup boundary. GC objects, subsystem/GC2 raw
  ** metadata, per-main/global buffers, the Lua stack and lightud segments are
  ** all gone, while GG, the main allocator, TG registry and embedded worker-
  ** retire list remain live. Dead allocators which could not fit a runtime
  ** hugetab transfer can now be destroyed in place without invalidating a
  ** subsequent lj_mem_free(). */
  if (mt_shutdown_acq(g) != 0)
    (void)lj_gc2_terminal_reclaim_tgs(g);
  /* Stable TG slots outlive GC2 teardown and every secondary body. The late
  ** terminal owner drain above is the final legacy authority which may still
  ** need their keys, so only now close the main incarnation and free the
  ** immutable slot spine. Partial initialization reaches this point with just
  ** the main slot and uses the same terminal cleanup. */
  lj_tg_registry_fini(g);
  if (arena_alloc) {
    /*
    ** Internal arena slabs are released when the arena allocator is destroyed
    ** below. GC2 arena sweep can settle object bodies before close_state frees
    ** the final runtime structures, so the byte counter is no longer a precise
    ** leak oracle at this boundary. Underflow is still caught at each subtract;
    ** normalize to the GG floor before releasing the allocator itself.
    */
    lj_gc_total_store(g, sizeof(GG_State));
  }
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
    if (g->main_tg && lj_tg_flags_test_acq(g->main_tg, TGF_HUGETAB)) {
      LJHugeInfo gghi;
      /* All subsystem/TG finalizers and owner lookups are complete. Destroy
      ** any residual main-owner huge mapping before the side table itself;
      ** dead source tables were drained first at the final owner-lookup
      ** boundary above, so
      ** an old transferred slot can never name an already-unmapped header. */
      if (lj_arena_hugetab_lookup(&g->main_tg->huge, GG, &gghi) == 1) {
	/* Boot/adoption configurations may register GG after its direct huge
	** allocation. Forget (but do not unmap) that exact entry so fini_all
	** cannot release the state executing this code; the manual unmap below
	** remains GG's sole physical owner. */
	if (!gghuge || gghi.size != sizeof(GG_State) ||
	    !lj_arena_hugetab_forget_terminal(&g->main_tg->huge, GG, NULL))
	  abort();
      }
      (void)lj_arena_hugetab_fini_all(&g->main_tg->huge);
    }
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

LUA_API lua_State *lua_newstate(lua_Alloc allocf, void *allocd)
{
  PRNGState prng;
  TGAlloc boot_alloc;
  LJArenaAllocD boot_ad;
  GG_State *GG;
  lua_State *L;
  global_State *g;
  uint32_t tid;
  int arena_internal = 0;
#if LJ_GC2_INTERNAL_ALLOCATOR_ONLY
  /* The callback remains in the ABI, but is deliberately not invoked yet. */
  allocf = LJ_ALLOCF_INTERNAL;
  allocd = NULL;
#endif
  /* Windows publishes its process-wide tagged TG TLS index as a retryable unit.
  ** Fail before allocating or publishing a Lua universe; a later newstate may
  ** retry InitOnce after transient TlsAlloc exhaustion. POSIX is a no-op. */
  if (!lj_thr_tg_tls_init())
    return NULL;
  /* We need the PRNG for the memory allocator, so initialize this first. */
  if (!lj_prng_seed_secure_l(NULL, &prng)) {
    lj_assertX(0, "secure PRNG seeding failed");
    /* Can only return NULL here, so this errors with "not enough memory". */
    return NULL;
  }
  /* Owner ids are process-wide, monotonic identities. Exhaustion is reported
  ** through lua_newstate's existing NULL failure result; it must never wrap
  ** into an older live identity or the reserved GC scanner sentinel. */
  tid = lj_thr_newid();
  if (tid == 0)
    return NULL;
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
  lj_state_grayagain_cycle_store_rlx(L, 0);
  lj_state_scan_epoch_rel(L, 0);
  lj_state_scan_dirty_epoch_rel(L, 0);
  lj_state_scan_handoff_epoch_rel(L, 0);
  g->gc.currentwhite = LJ_GC_WHITE0 | LJ_GC_FIXED;
  g->strempty.marked = LJ_GC_WHITE0;
  g->strempty.gct = ~LJ_TSTR;
  lj_str_canon_store_rlx(&g->strempty, LJ_STR_CANON_LIVE);
  g->allocf = allocf;
  g->allocd = allocd;
  g->allocf_arena = (allocf == lj_arena_allocf);
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
  lj_str_qtabh_store_rlx(g, NULL);
  lj_str_qretired_head_store_rlx(g, NULL);
  g->str.retired_body = NULL;
  g->str.sweep_pending = NULL;
  g->str.sweep_hdr = NULL;
  g->str.sweep_link = NULL;
  g->str.sweep_grace_epoch = 0;
  g->str.sweep_tagged = 0;
  g->str.sweep_rescued = 0;
  g->str.sweep_unlinked = 0;
  g->str.sweep_reclaimed = 0;
  g->str.sweep_bucket = 0;
  g->str.sweep_phase = 0;
  g->str.sweep_cycle = 0;
  lj_str_mask_store_rlx(g, ~(MSize)0);
  lj_str_qmask_store_rlx(g, ~(MSize)0);
  lj_str_qcount_store_rlx(g, 0);
  g->tab.retired_nodes = NULL;
  g->tab.retired_arrays = NULL;
  g->threading_live = NULL;
  g->threading_live_retired = NULL;
  g->threading_live_count = 0;
  lj_state_thread_registry_head_clear(g);
  lj_registry_setnil_rel(L);
  g->nilnodehdr.hmask = 0;
  g->nilnodehdr.flags = 0;
  setmref(g->nilnodehdr.next_gen, NULL);
  setnilV(&g->nilnode.val);
  setnilV(&g->nilnode.key);
  lj_buf_init(NULL, &g->tmpbuf);
  g->gc.state = GCSpause;
  lj_gc_root_rel(g, obj2gco(L));
  setmref(g->gc.sweep, lj_gc_root_ref(g));
  lj_gc_total_store(g, sizeof(GG_State));
  lj_gc_pause_store(g, LUAI_GCPAUSE);
  lj_gc_stepmul_store(g, LUAI_GCMUL);
  lj_dispatch_init((GG_State *)L);
  lj_tg_init((GG_State *)L, arena_internal, tid);
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

lua_State *lj_state_new_withenv(lua_State *L, GCtab *env)
{
  global_State *g = G(L);
  lua_State *L1 = (lua_State *)lj_mem_newgco_unlinked(L, sizeof(lua_State));
  L1->gct = ~LJ_TTHREAD;
  L1->dummy_ffid = FF_C;
  L1->status = LUA_OK;
  L1->stacksize = 0;
  setmref(L1->stack, NULL);
  L1->cframe = NULL;
  L1->tg_hint = NULL;
  lj_state_thread_registry_next_rel(L1, NULL);
  lj_state_owner_rel(L1, 0);
  lj_state_grayagain_cycle_store_rlx(L1, 0);
  lj_state_scan_epoch_rel(L1, 0);
  lj_state_scan_dirty_epoch_rel(L1, 0);
  lj_state_scan_handoff_epoch_rel(L1, 0);
  lj_state_openupval_clear_rel(L1);
  lj_state_mt_thread_clear_rel(L1);
  setmrefr(L1->glref, L->glref);
  lj_state_env_rel(L1, env);
  newwhite(g, obj2gco(L1));
  stack_init(L1, L);  /* init stack */
  lj_gc_linkobj_new_after_main(g, obj2gco(L1));
  if (env)
    lj_gc_pubobjobj(L, L1, env);
  lj_assertL(iswhite(obj2gco(L1)), "new thread object is not white");
  return L1;
}

lua_State *lj_state_new(lua_State *L)
{
  return lj_state_new_withenv(L, lj_state_env_acq(L));
}

void LJ_FASTCALL lj_state_free(global_State *g, lua_State *L)
{
  lj_assertG(L != mainthread_acq(g), "free of main thread");
#if LJ_HASFFI
  lj_ccallback_disown_state(L);
#endif
  if (L == lj_tg_cur_L(g))
    lj_tg_clearcur_L(g);
  state_registry_remove(g, L);
  if (lj_state_openupval_acq(L) != NULL) {
    lj_func_closeuv(L, tvref(L->stack));
    lj_trace_abort(g);  /* For aa_uref soundness. */
    lj_assertG(lj_state_openupval_acq(L) == NULL, "stale open upvalues");
  }
  lj_mem_freevec(g, tvref(L->stack), L->stacksize, TValue);
  if (!lj_mem_freegco_defer(g, L, sizeof(lua_State)))
    lj_mem_freet(g, L);
}
