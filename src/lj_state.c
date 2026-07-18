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

static void state_root_anchor_unwind(TGState *tg, uint32_t saved_top)
{
  uint32_t top;
  if (!tg)
    abort();
  top = lj_tg_root_anchor_top_acq(tg);
  if (top < saved_top)
    abort();
  while (top > saved_top) {
    lj_tg_root_anchor_pop(tg, top - 1u);
    top = lj_tg_root_anchor_top_acq(tg);
  }
}

int lj_vm_cpcall(lua_State *L, lua_CFunction func, void *ud, lua_CPFunction cp)
{
  LJTabReadCheckpoint tabread;
  TGState *oldtg;
  uint32_t owner, root_anchor_top;
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
  lj_tab_read_checkpoint(L2TG(L), &tabread);
  root_anchor_top = lj_tg_root_anchor_top_acq(tabread.tg);
  status = lj_vm_cpcall_asm(L, func, ud, cp);
  lj_tab_read_unwind(&tabread);
  if (status != LUA_OK)
    state_root_anchor_unwind(tabread.tg, root_anchor_top);
  if (LJ_UNLIKELY(oldtg == NULL && owner == 0 && lj_state_owner_acq(L) == 0))
    L->tg_hint = NULL;  /* Ownerless coroutine states stay TG-neutral. */
  return status;
}

int lj_vm_pcall_unwind(lua_State *L, TValue *base, int nres1, ptrdiff_t ef)
{
  LJTabReadCheckpoint tabread;
  uint32_t root_anchor_top;
  int status;
  lj_tab_read_checkpoint(L2TG(L), &tabread);
  root_anchor_top = lj_tg_root_anchor_top_acq(tabread.tg);
  status = lj_vm_pcall(L, base, nres1, ef);
  lj_tab_read_unwind(&tabread);
  if (status != LUA_OK)
    state_root_anchor_unwind(tabread.tg, root_anchor_top);
  return status;
}

int lj_vm_resume_unwind(lua_State *L, TValue *base, int nres1, ptrdiff_t ef)
{
  LJTabReadCheckpoint tabread;
  uint32_t root_anchor_top;
  int status;
  lj_tab_read_checkpoint(L2TG(L), &tabread);
  root_anchor_top = lj_tg_root_anchor_top_acq(tabread.tg);
  status = lj_vm_resume(L, base, nres1, ef);
  lj_tab_read_unwind(&tabread);
  if (status != LUA_OK)
    state_root_anchor_unwind(tabread.tg, root_anchor_top);
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

/* Allocate basic stack for new state without unwinding past a pending header. */
static int stack_init_nothrow(lua_State *L1, lua_State *L)
{
  GCSize size = (GCSize)(LJ_STACK_START + LJ_STACK_EXTRA) * sizeof(TValue);
  TValue *stend, *st = (TValue *)lj_mem_new_nothrow(L, size);
  if (!st)
    return 0;
  setmref(L1->stack, st);
  L1->stacksize = LJ_STACK_START + LJ_STACK_EXTRA;
  stend = st + L1->stacksize;
  setmref(L1->maxstack, stend - LJ_STACK_EXTRA - 1);
  setthreadV(L1, st++, L1);  /* Needed for curr_funcisL() on empty stack. */
  if (LJ_FR2) setnilV(st++);
  L1->base = L1->top = st;
  while (st < stend)  /* Clear new slots. */
    setnilV(st++);
  return 1;
}

static void stack_init(lua_State *L1, lua_State *L)
{
  if (LJ_UNLIKELY(!stack_init_nothrow(L1, L)))
    lj_err_mem(L);
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
  {
    uint32_t anchoridx;
    lua_State *vmL = lj_state_new(L, &anchoridx);
    vmthread_rel(g, vmL);
    lj_gc_pubobjroot(L, obj2gco(vmL));
    lj_tg_root_anchor_pop(L2TG(L), anchoridx);
  }
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
  /* close_state calls this only after mutators, GC workers, finalizers and the
  ** threading registry have joined. That terminal world-stop is the body and
  ** reuse certificate for the subsequent gct/gcw reads; acquiring semantic
  ** retention here would revive objects while the sole destructor drains. */
  if (!lj_gc2_obj_valid(g, o))
    return 0;
  /* Strings are owned solely by the intern table and use nextgc as their hash
  ** successor. Never reanchor a shutdown ownership chain through a stale root
  ** address which has been recycled as a string. */
  return la_load8_acq(&o->gch.gct) != (uint8_t)~LJ_TSTR;
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
  if (head && !close_state_root_link_valid(g, head)) {
    lj_gc_root_rel(g, NULL);
    lj_gcroot_repair_epoch_add(g);
  }
  /* The terminal helper is the only exceptional stale-state reset. Every
  ** publisher/remover has joined, and the scan above proved that target has no
  ** incoming ownership edge, so repairing membership cannot create a duplicate
  ** intrusive link. */
  (void)lj_gc_linkobj_terminal(g, target);
}

int lj_state_thread_registry_lease(global_State *g, lua_State *th,
				    LJGC2Lease *lease)
{
  if (!lease)
    return 0;
  memset(lease, 0, sizeof(*lease));
  return g && th && lj_gc2_obj_lease_acquire(
    g, obj2gco(th), (uint32_t)~LJ_TTHREAD, NULL, lease) >= 0;
}

int lj_state_thread_registry_valid(global_State *g, lua_State *th)
{
  LJGC2Lease lease;
  int valid = lj_state_thread_registry_lease(g, th, &lease);
  /* Identity-only convenience probe. Registry walkers which dereference th
  ** must retain their own lease through the final body access. */
  lj_gc2_lease_release(&lease);
  return valid;
}

void lj_state_thread_registry_publish(global_State *g, lua_State *th)
{
  LJGC2Lease headlease;
  lua_State *head, *expect;
  if (!g || !th)
    return;

  for (;;) {
    /* Container publication itself needs a registry generation read even when
    ** the old head is NULL. Otherwise an exact reclaimer could prove `th`
    ** absent, cross its terminal FREE LP, and then lose to this head CAS. */
    if (!lj_gc2_smr_read_try(g)) {
      (void)lj_thr_retry_yield(NULL);
      continue;
    }
    if (lj_state_gcprep_state_acq(th) != LJ_STATE_GCPREP_NONE ||
	lj_state_owner_acq(th) == LJ_THREAD_GCPREP) {
      lj_gc2_smr_read_leave(g);
      return;
    }
    head = lj_state_thread_registry_head_acq(g);
    memset(&headlease, 0, sizeof(headlease));
    /* Keep the exact old-head incarnation alive until the head CAS either
    ** publishes that pointer as th->next or proves it is no longer current. */
    if (head && !lj_state_thread_registry_lease(g, head, &headlease)) {
      lj_gc2_smr_read_leave(g);
      continue;
    }
    lj_state_thread_registry_next_rel(th, head);
    expect = head;
    if (lj_state_thread_registry_head_cas(g, &expect, th)) {
      lj_gc2_lease_release(&headlease);
      lj_gc2_smr_read_leave(g);
      return;
    }
    lj_gc2_lease_release(&headlease);
    lj_gc2_smr_read_leave(g);
  }
}

static void state_registry_remove(global_State *g, lua_State *th)
{
  lua_State *prev, *cur;
  uint32_t retries = 0;
  if (!g || !th)
    return;
restart:
  {
    LJGC2Lease prevlease, curlease, nextlease;
    uint32_t scanned = 0;
    memset(&prevlease, 0, sizeof(prevlease));
    memset(&curlease, 0, sizeof(curlease));
    memset(&nextlease, 0, sizeof(nextlease));
    prev = NULL;
    cur = lj_state_thread_registry_head_acq(g);
    /* The type destructor already owns `th` past its terminal allocation LP,
    ** so ordinary semantic admission of that one node is expected to fail.
    ** Every other node needs a counted lease before registry_next is read. */
    if (cur && cur != th &&
	!lj_state_thread_registry_lease(g, cur, &curlease)) {
      if (++retries >= LJ_GC2_ROOT_SCAN_LIMIT) {
	lj_assertG(0, "thread registry head did not become admissible");
	abort();
      }
      goto restart;
    }
    while (cur) {
      lua_State *next;
      if (++scanned >= LJ_GC2_ROOT_SCAN_LIMIT) {
	lj_assertG(0, "thread registry scan exceeded structural bound");
	abort();
      }
      next = lj_state_thread_registry_next_acq(cur);
      if (next == cur) {
	if (cur == th)
	  next = NULL;  /* Unlink the destructor-owned self-cycle itself. */
	else {
	  lj_assertG(0, "thread registry contains a foreign self-cycle");
	  abort();
	}
      }

      /* Retain predecessor, current and successor incarnations together.
      ** Reacquiring `prev` by address immediately before the CAS is not
      ** sufficient: a removed THREAD can be freed and another THREAD can reuse
      ** the same address with an unrelated registry_next field. */
      if (next && next != th &&
	  !lj_state_thread_registry_lease(g, next, &nextlease)) {
	lj_gc2_lease_release(&prevlease);
	lj_gc2_lease_release(&curlease);
	if (++retries >= LJ_GC2_ROOT_SCAN_LIMIT) {
	  lj_assertG(0, "thread registry successor did not become admissible");
	  abort();
	}
	goto restart;
      }
      if (cur == th) {
	int removed;
	if (prev)
	  removed = lj_state_thread_registry_next_cas(prev, &cur, next);
	else
	  removed = lj_state_thread_registry_head_cas(g, &cur, next);
	if (!removed) {
	  lj_gc2_lease_release(&prevlease);
	  lj_gc2_lease_release(&curlease);
	  lj_gc2_lease_release(&nextlease);
	  goto restart;
	}
	lj_state_thread_registry_next_rel(th, NULL);
	lj_gc2_lease_release(&prevlease);
	lj_gc2_lease_release(&curlease);
	lj_gc2_lease_release(&nextlease);
	return;
      }

      lj_gc2_lease_release(&prevlease);
      prev = cur;
      prevlease = curlease;
      memset(&curlease, 0, sizeof(curlease));
      cur = next;
      curlease = nextlease;
      memset(&nextlease, 0, sizeof(nextlease));
    }
    lj_gc2_lease_release(&prevlease);
    lj_gc2_lease_release(&curlease);
  }
}

#if defined(LJ_GC2_TEST_HELPERS) || defined(LJ_STATE_TEST_HELPERS)
static uint32_t state_gcprep_test_pause_armed;
static uint32_t state_gcprep_test_pause_waiting;
static uint32_t state_gcprep_test_pre_lp_armed;
static uint32_t state_gcprep_test_pre_lp_waiting;
static uint32_t state_gcprep_test_terminal_drains;

void lj_state_test_gcprep_pause(int enabled)
{
  la_store32_rel(&state_gcprep_test_pause_armed, enabled != 0);
  if (!enabled)
    la_store32_rel(&state_gcprep_test_pause_waiting, 0);
}

uint32_t lj_state_test_gcprep_paused(void)
{
  return la_load32_acq(&state_gcprep_test_pause_waiting);
}

void lj_state_test_gcprep_pre_lp_pause(int enabled)
{
  la_store32_rel(&state_gcprep_test_pre_lp_armed, enabled != 0);
  if (!enabled)
    la_store32_rel(&state_gcprep_test_pre_lp_waiting, 0);
}

uint32_t lj_state_test_gcprep_pre_lp_paused(void)
{
  return la_load32_acq(&state_gcprep_test_pre_lp_waiting);
}

void lj_state_test_gcprep_terminal_drain_reset(void)
{
  la_store32_rel(&state_gcprep_test_terminal_drains, 0);
}

uint32_t lj_state_test_gcprep_terminal_drain_count(void)
{
  return la_load32_acq(&state_gcprep_test_terminal_drains);
}

static void state_gcprep_test_terminal_drain_add(uint32_t n)
{
  if (n != 0)
    (void)la_add32_acqrel(&state_gcprep_test_terminal_drains, n);
}

static void state_gcprep_test_pause_before_lp(void)
{
  if (la_load32_acq(&state_gcprep_test_pre_lp_armed) != 0) {
    la_store32_rel(&state_gcprep_test_pre_lp_waiting, 1);
    while (la_load32_acq(&state_gcprep_test_pre_lp_armed) != 0)
      (void)lj_thr_retry_yield(NULL);
    la_store32_rel(&state_gcprep_test_pre_lp_waiting, 0);
  }
}

static void state_gcprep_test_pause_after_publish(void)
{
  if (la_load32_acq(&state_gcprep_test_pause_armed) != 0) {
    la_store32_rel(&state_gcprep_test_pause_waiting, 1);
    while (la_load32_acq(&state_gcprep_test_pause_armed) != 0)
      (void)lj_thr_retry_yield(NULL);
    la_store32_rel(&state_gcprep_test_pause_waiting, 0);
  }
}
#else
#define state_gcprep_test_pause_after_publish() ((void)0)
#define state_gcprep_test_pause_before_lp() ((void)0)
#define state_gcprep_test_terminal_drain_add(n) ((void)(n))
#endif

static int state_gcprep_tg_names(global_State *g, TGState *tg,
				  lua_State *L)
{
  UNUSED(g);
  if (!tg)
    return 0;
  if (lj_tg_load_cur_L(tg) == L || lj_tg_load_thread_L(tg) == L)
    return 1;
#if LJ_HASFFI
  {
    CCallbackRuntime *cb = &tg->cb;
    MSize i, depth = ccallback_depth_acq(cb);
    if (ccallback_L_acq(cb) == L)
      return 1;
    if (depth > CCALLBACK_MAX_NEST)
      return 1;  /* Invalid active callback metadata vetoes destruction. */
    for (i = 0; i < depth; i++)
      if ((lua_State *)la_loadptr_acq(
	    (void *const *)&cb->frame[i].L) == L)
	return 1;
  }
#endif
  return 0;
}

/* Exact-writer preflight for process-global raw state roots. Registry
** publication now retains an ordinary SMR generation even for an empty old
** head, so absence is stable until this writer leaves. */
static int state_gcprep_registry_absent(global_State *g, lua_State *L)
{
  lua_State *th = lj_state_thread_registry_head_acq(g);
  uint32_t n = 0;
  while (th) {
    lua_State *next;
    if (th == L)
      return 0;
    if (LJ_UNLIKELY(!lj_gc2_mem_registered_known_reclaim_held(g, th))) {
      lj_assertG(0, "invalid THREAD registry during terminal preflight");
      abort();
    }
    next = lj_state_thread_registry_next_acq(th);
    if (LJ_UNLIKELY(next == th || ++n >= LJ_GC2_ROOT_SCAN_LIMIT)) {
      lj_assertG(0, "cyclic THREAD registry during terminal preflight");
      abort();
    }
    th = next;
  }
  return 1;
}

static int state_gcprep_callback_absent(global_State *g, lua_State *L)
{
#if LJ_HASFFI
  CTState *cts = ctype_ctsG(g);
  lua_State **owner;
  MSize i, n;
  if (!cts)
    return 1;
  owner = ctype_cb_owner_acq(cts);
  n = ctype_cb_sizeid_acq(cts);
  if (!owner)
    return 1;
  if (LJ_UNLIKELY(n > lj_ccallback_maxslot())) {
    lj_assertG(0, "invalid callback owner extent during THREAD preflight");
    abort();
  }
  for (i = 0; i < n; i++)
    if (ctype_cb_owner_slot_acq(owner, i) == L)
      return 0;
#else
  UNUSED(g); UNUSED(L);
#endif
  return 1;
}

static int state_gcprep_roots_absent(global_State *g, lua_State *L)
{
  TGState *tg;
  uint32_t n = 0;
  /* On TG-local VMs g->cur_L is only a bootstrap mirror and can retain the last
  ** resumed coroutine. Use the current TG accessor here; the registry walk below
  ** checks every other TG's authoritative cur_L/thread_L publication. */
  if (!g || !L || L == mainthread_acq(g) || L == vmthread_acq(g) ||
      lj_tg_cur_L(g) == L || L->cframe != NULL)
    return 0;
  for (tg = gc2_tg_list_acq(g); tg; tg = lj_tg_next_acq(tg)) {
    if (state_gcprep_tg_names(g, tg, L))
      return 0;
    if (LJ_UNLIKELY(++n >= LJ_GC2_ROOT_SCAN_LIMIT)) {
      lj_assertG(0, "cyclic TG registry during THREAD preflight");
      abort();
    }
  }
  if (g->main_tg && state_gcprep_tg_names(g, g->main_tg, L))
    return 0;
  return state_gcprep_registry_absent(g, L) &&
	 state_gcprep_callback_absent(g, L);
}

static void state_gcprep_queue_push(global_State *g, lua_State *L)
{
  lua_State *head = lj_state_gcprep_head_acq(g);
  do {
    lj_state_gcprep_next_rel(L, head);
  } while (!lj_state_gcprep_head_cas(g, &head, L));
}

int lj_state_gcprep_claim_and_pin(global_State *g, lua_State *L)
{
  GCArena *a;
  uint32_t cell, expect = 0, old;
  if (!g || !L || !lj_gc2_reclaim_context_held(g) ||
      L->gct != ~LJ_TTHREAD || mref(L->glref, global_State) != g ||
      lj_state_gcprep_state_acq(L) != LJ_STATE_GCPREP_NONE ||
      !lj_state_owner_cas(L, &expect, LJ_THREAD_GCPREP))
    return 0;
  if (!state_gcprep_roots_absent(g, L)) {
    lj_state_release(L, LJ_THREAD_GCPREP);
    return 0;
  }
  a = lj_arena_of(L);
  cell = lj_arena_cellof(L);
  if (LJ_UNLIKELY(lj_arena_ishuge(a) ||
	  cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
	  !lj_gc2_mem_registered_known_reclaim_held(g, L))) {
    lj_state_release(L, LJ_THREAD_GCPREP);
    return 0;
  }
  /* Reserve both scopes before the destructor LP. FREE+FREEING otherwise
  ** looks indistinguishable from a synchronously completed type destructor. */
  old = lj_arena_gcprep_pending_add(a, 1);
  if (LJ_UNLIKELY(old == ~(uint32_t)0)) {
    lj_assertG(0, "THREAD arena preparation pin overflow");
    abort();
  }
  old = la_add32_acqrel(&g->thread_gcprep_pending, 1);
  if (LJ_UNLIKELY(old == ~(uint32_t)0)) {
    lj_assertG(0, "THREAD preparation count overflow");
    abort();
  }
  state_gcprep_test_pause_before_lp();
  return 1;
}

void lj_state_gcprep_cancel(global_State *g, lua_State *L)
{
  GCArena *a;
  uint32_t old;
  if (!g || !L)
    return;
  a = lj_arena_of(L);
  old = la_sub32_acqrel(&g->thread_gcprep_pending, 1);
  lj_assertG(old != 0, "THREAD preparation cancel underflow");
  old = lj_arena_gcprep_pending_sub(a, 1);  /* Physical permission last. */
  lj_assertG(old != 0, "THREAD arena preparation cancel underflow");
  lj_assertG(lj_state_gcprep_state_acq(L) == LJ_STATE_GCPREP_NONE,
	     "cancel of published THREAD preparation");
  lj_state_release(L, LJ_THREAD_GCPREP);
  UNUSED(old);
}

void lj_state_gcprep_publish(global_State *g, lua_State *L)
{
  GCArena *a;
  uint32_t cell;
  if (!g || !L)
    abort();
  a = lj_arena_of(L);
  cell = lj_arena_cellof(L);
  if (LJ_UNLIKELY(!lj_gc2_reclaim_context_held(g) ||
	  lj_state_owner_acq(L) != LJ_THREAD_GCPREP ||
	  lj_state_gcprep_state_acq(L) != LJ_STATE_GCPREP_NONE ||
	  lj_arena_lifetime_state_acq(a, cell) != LJ_ARENA_LIFETIME_FREE ||
	  lj_arena_sweep_state_acq(a, cell) != LJ_ARENA_SWEEP_FREEING ||
	  lj_arena_gcprep_pending_acq(a) == 0)) {
    lj_assertG(0, "invalid terminal THREAD preparation publication");
    abort();
  }
  lj_state_gcprep_state_rel(L, LJ_STATE_GCPREP_PENDING);
  state_gcprep_queue_push(g, L);
  state_gcprep_test_pause_after_publish();
  lj_gc2_sweep_publish_wake(g);
}

static lua_State *state_gcprep_queue_pop(global_State *g)
{
  lua_State *head = lj_state_gcprep_head_acq(g);
  while (head) {
    lua_State *next = lj_state_gcprep_next_acq(head);
    if (lj_state_gcprep_head_cas(g, &head, next)) {
      lj_state_gcprep_next_rel(head, NULL);
      return head;
    }
  }
  return NULL;
}

uint32_t lj_state_gcprep_drain(global_State *g, uint32_t limit)
{
  uint32_t n = 0;
  while (g && n < limit) {
    lua_State *L = state_gcprep_queue_pop(g);
    GCArena *a;
    TValue *stack;
    MSize stacksize;
    uint32_t old;
    if (!L)
      break;
    a = lj_arena_of(L);
    if (LJ_UNLIKELY(lj_state_gcprep_state_acq(L) !=
		    LJ_STATE_GCPREP_PENDING ||
		    lj_state_owner_acq(L) != LJ_THREAD_GCPREP ||
		    lj_arena_gcprep_pending_acq(a) == 0)) {
      lj_assertG(0, "invalid queued terminal THREAD body");
      abort();
    }
#if LJ_HASFFI
    /* Pre-FREE callback ownership vetoes the LP. This is therefore an
    ** idempotent no-op in the valid path and closes any defensive stale slot
    ** before allocator permission is released. */
    lj_ccallback_disown_state(L);
    if (LJ_UNLIKELY(!state_gcprep_callback_absent(g, L))) {
      lj_assertG(0, "terminal THREAD retained callback ownership");
      abort();
    }
#endif
    if (lj_state_openupval_acq(L) != NULL) {
      lj_func_closeuv(L, tvref(L->stack));
      lj_trace_abort(g);
      if (LJ_UNLIKELY(lj_state_openupval_acq(L) != NULL)) {
	lj_assertG(0, "terminal THREAD retained open upvalues");
	abort();
      }
    }
    stack = tvref(L->stack);
    stacksize = L->stacksize;
    if (stacksize != 0) {
      lj_mem_freevec(g, stack, stacksize, TValue);
      setmref(L->stack, NULL);
      setmref(L->maxstack, NULL);
      L->base = L->top = NULL;
      L->stacksize = 0;
    }
    if (LJ_UNLIKELY(!lj_mem_freegco_defer(g, L, sizeof(lua_State)))) {
      lj_assertG(0, "terminal THREAD body escaped quarantine ownership");
      abort();
    }
    lj_state_gcprep_state_rel(L, LJ_STATE_GCPREP_DONE);
    old = la_sub32_acqrel(&g->thread_gcprep_pending, 1);
    lj_assertG(old != 0, "THREAD preparation drain underflow");
    /* This is the final access to L/a. Releasing the arena pin permits the
    ** next exact pass to clear block[] and reuse the body immediately. */
    old = lj_arena_gcprep_pending_sub(a, 1);
    lj_assertG(old != 0, "THREAD arena preparation drain underflow");
    UNUSED(old);
    n++;
  }
  return n;
}

void lj_state_gcprep_drain_terminal(global_State *g)
{
  uint32_t drained = 0;
  while (g && lj_state_gcprep_pending_acq(g) != 0) {
    uint32_t n = lj_state_gcprep_drain(g, LJ_GC2_ROOT_SCAN_LIMIT);
    if (LJ_UNLIKELY(n == 0)) {
      lj_assertG(0, "terminal THREAD preparation queue lost");
      abort();
    }
    drained += n;
  }
  if (g && LJ_UNLIKELY(lj_state_gcprep_head_acq(g) != NULL)) {
    lj_assertG(0, "terminal THREAD preparation queue/count mismatch");
    abort();
  }
  state_gcprep_test_terminal_drain_add(drained);
}

static void close_state_reanchor_registered_states(global_State *g,
					     lua_State *L)
{
  lua_State *th = lj_state_thread_registry_head_xchg(g, NULL);
  LJGC2Lease leases[2];
  uint32_t leaseidx = 0;
  uint32_t n = 0;
  memset(leases, 0, sizeof(leases));
  if (th && !lj_state_thread_registry_lease(g, th, &leases[leaseidx])) {
    lj_assertG(0, "terminal thread registry head is not admissible");
    abort();
  }
  while (th) {
    lua_State *next;
    next = lj_state_thread_registry_next_acq(th);
    if (next == th)
      next = NULL;  /* Terminally sever one exact self-cycle. */
    if (next && !lj_state_thread_registry_lease(
		  g, next, &leases[leaseidx ^ 1u])) {
      lj_gc2_lease_release(&leases[leaseidx]);
      lj_assertG(0, "terminal thread registry successor is not admissible");
      abort();
    }
    lj_state_thread_registry_next_rel(th, NULL);
    /* A threading.thread userdata may still carry this state pointer after
    ** lj_threading_shutdown(). Keep every exact state on the terminal GC2
    ** ownership spine and let the single destructor drain free it once. */
    if (th != L)
      close_state_reanchor_root(g, obj2gco(th));
    lj_gc2_lease_release(&leases[leaseidx]);
    th = next;
    leaseidx ^= 1u;
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT) {
      lj_assertG(0, "terminal thread registry contains a cycle");
      abort();
    }
  }
  lj_gc2_lease_release(&leases[leaseidx]);
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
  /* A failed pthread_create has no OS-thread owner to detach its provisional
  ** TG. If runtime checked teardown met a reader/certificate veto, its live
  ** node survived shutdown and all-userdata finalization as the retry owner.
  ** Resolve it now, before either recovery identities or native roots move. */
  if (!lj_threading_live_retry_tgs_terminal(g))
    abort();
  /* A worker may have crossed a child THREAD's irreversible FREE LP just
  ** before shutdown. Finish its semantic/side-allocation handoff while CTState,
  ** callback slots, TG registries and arena routing are all still intact. */
  lj_state_gcprep_drain_terminal(g);
  /* First joined-world ownership certificate, before close_state destroys
  ** live-root metadata, traces, objects, or allocator owners. lua_close is a
  ** one-shot ABI, so a persistent debug PINNED/malformed descriptor fails
  ** closed here instead of discovering a non-resumable veto at the tail. */
  if (!lj_gc2_terminal_prefree(g))
    abort();
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
    TGAlloc *alloc = &g->main_tg->alloc;
    if (!gghuge)
      abort();  /* Internal x64 GG must retain itself outside small slabs. */
    if (g->main_tg && lj_tg_flags_test_acq(g->main_tg, TGF_HUGETAB)) {
      LJHugeInfo gghi;
      uint32_t unmapped;
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
      if (!lj_arena_hugetab_fini_all_try(&g->main_tg->huge, &unmapped))
	abort();  /* PRE proved joined-world authority; retain/fail closed. */
      lj_tg_flags_and_rlx(g->main_tg, (uint8_t)~TGF_HUGETAB);
      lj_arena_allocd_sethugetab(&g->main_tg->allocd, NULL);
    }
    /* Finalize the authoritative in-place owner. A stack copy could make
    ** partial unmap progress and leave the original heads naming freed maps. */
    if (!lj_arena_alloc_fini_try(alloc))
      abort();
    if (!lj_gc2_small_arena_registry_fini_try(g))
      abort();
    lj_arena_huge_unmap_claimed(GG, sizeof(GG_State));
    return;
  }
  if (!lj_gc2_small_arena_registry_fini_try(g))
    abort();
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
  lj_state_gcprep_next_rel(L, NULL);
  lj_state_gcprep_state_store_rlx(L, LJ_STATE_GCPREP_NONE);
  lj_state_grayagain_cycle_store_rlx(L, 0);
  lj_state_scan_epoch_rel(L, 0);
  lj_state_scan_dirty_epoch_rel(L, 0);
  lj_state_scan_handoff_epoch_rel(L, 0);
  lj_state_scan_needscan_counted_store_rlx(L, 0);
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
  g->thread_gcprep_pending = 0;
  g->thread_gcprep = NULL;
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

static lua_State *state_new_withenv_at_anchor(lua_State *L, GCtab *env,
					      TValue *anchor, uint32_t idx)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  TValue thv;
  lua_State *L1;
  L1 = (lua_State *)lj_mem_newgco_unlinked_nothrow(L, sizeof(lua_State));
  if (LJ_UNLIKELY(!L1)) {
    lj_tg_root_anchor_pop(tg, idx);
    lj_err_mem(L);
  }
  L1->gct = ~LJ_TTHREAD;
  L1->dummy_ffid = FF_C;
  L1->status = LUA_OK;
  L1->stacksize = 0;
  setmref(L1->stack, NULL);
  setmref(L1->maxstack, NULL);
  L1->base = L1->top = NULL;
  L1->cframe = NULL;
  L1->tg_hint = NULL;
  lj_state_thread_registry_next_rel(L1, NULL);
  lj_state_gcprep_next_rel(L1, NULL);
  lj_state_owner_rel(L1, 0);
  lj_state_gcprep_state_store_rlx(L1, LJ_STATE_GCPREP_NONE);
  lj_state_grayagain_cycle_store_rlx(L1, 0);
  lj_state_scan_epoch_rel(L1, 0);
  lj_state_scan_dirty_epoch_rel(L1, 0);
  lj_state_scan_handoff_epoch_rel(L1, 0);
  lj_state_scan_needscan_counted_store_rlx(L1, 0);
  lj_state_openupval_clear_rel(L1);
  lj_state_mt_thread_clear_rel(L1);
  setmrefr(L1->glref, L->glref);
  lj_state_env_rel(L1, env);
  newwhite(g, obj2gco(L1));
  /* Keep the object opaque while its geometry is mutable. The non-throwing
  ** stack allocation lets failure explicitly cancel this READY=0 body instead
  ** of abandoning an immortal pending constructor. */
  if (LJ_UNLIKELY(!stack_init_nothrow(L1, L))) {
    lj_mem_freegco_unpublished(g, L1, sizeof(lua_State));
    lj_tg_root_anchor_pop(tg, idx);
    lj_err_mem(L);
  }
  /* Publish the pending identity through a non-intrusive TG anchor. Scanners
  ** may observe the TValue while READY=0, but reject it without hiding older
  ** roots; the post-READY barrier repairs that exact observation. */
  setthreadV(L, &thv, L1);
  copyTVrel(L, anchor, &thv);
  lj_gc_publishobj_header(g, obj2gco(L1));
  lj_gc_pubroot(L, anchor);
  lj_gc_linkobj_new_after_main(g, obj2gco(L1));
  if (env)
    lj_gc_pubobjobj(L, L1, env);
  lj_assertL(iswhite(obj2gco(L1)), "new thread object is not white");
  return L1;
}

lua_State *lj_state_new_withenv(lua_State *L, GCtab *env,
				uint32_t *anchoridx)
{
  TGState *tg = L2TG(L);
  TValue envv;
  TValue *anchor;
  uint32_t idx;
  lj_assertL(anchoridx != NULL, "missing new-thread construction anchor");
  if (env)
    settabV(L, &envv, env);
  else
    setnilV(&envv);
  anchor = lj_tg_root_anchor_push(L, tg, &envv, &idx);
  if (LJ_UNLIKELY(!anchor))
    lj_err_mem(L);
  lj_gc_pubroot(L, anchor);
  *anchoridx = idx;
  return state_new_withenv_at_anchor(L, env, anchor, idx);
}

lua_State *lj_state_new_withenv_envrooted(lua_State *L, GCtab *env,
					  uint32_t anchoridx)
{
  TGState *tg = L2TG(L);
  TValue snap;
  TValue *anchor = lj_tg_root_anchor_slot_acq(tg, anchoridx);
  lj_assertL(anchor != NULL &&
	     lj_tg_root_anchor_top_acq(tg) == anchoridx + 1u,
	     "new-thread environment root is not the top anchor");
  lj_tv_load_acq(&snap, anchor);
  lj_assertL((env && tvistab(&snap) && tabV(&snap) == env) ||
	     (!env && tvisnil(&snap)), "wrong new-thread environment root");
  return state_new_withenv_at_anchor(L, env, anchor, anchoridx);
}

lua_State *lj_state_new(lua_State *L, uint32_t *anchoridx)
{
  return lj_state_new_withenv(L, lj_state_env_acq(L), anchoridx);
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
  if (L->stacksize != 0)
    lj_mem_freevec(g, tvref(L->stack), L->stacksize, TValue);
  if (!lj_mem_freegco_defer(g, L, sizeof(lua_State)))
    lj_mem_freet(g, L);
}
