/*
** Threading library.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define lib_threading_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_chan.h"
#include "lj_ccallback.h"
#include "lj_err.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_lib.h"
#include "lj_safepoint.h"
#include "lj_state.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_udata.h"

#define THREADING_MAIN_KEY	"__main"

static int threading_arena_internal(global_State *g)
{
  return g->allocf == lj_arena_allocf && g->main_tg &&
	 lj_tg_flags_test_acq(g->main_tg, TGF_ARENA_INTERNAL);
}

/* -- Thread methods ------------------------------------------------------ */

static LJThread *threading_tothread(lua_State *L)
{
  if (!(L->base < L->top && tvisudata(L->base) &&
	lj_udata_udtype_acq(udataV(L->base)) == UDTYPE_THREAD))
    lj_err_argtype(L, 1, "threading.thread");
  return (LJThread *)uddata(udataV(L->base));
}

static LJThreadLive *threading_live_head(global_State *g)
{
  return (LJThreadLive *)la_loadptr_acq((void *const *)&g->threading_live);
}

static GCudata *threading_live_ud(LJThreadLive *node)
{
  GCobj *o = gcref_acq(node->ud);
  if (o && o->gch.gct == ~LJ_TUDATA &&
      lj_udata_udtype_acq(gco2ud(o)) == UDTYPE_THREAD)
    return gco2ud(o);
  return NULL;
}

static LJThreadLive *threading_live_new(lua_State *L, GCudata *ud)
{
  LJThreadLive *node = lj_mem_newt(L, sizeof(LJThreadLive), LJThreadLive);
  TValue tv;
  lj_thread_live_next_rel(node, NULL);
  setgcrefrel(node->ud, obj2gco(ud));
  setudataV(L, &tv, ud);
  lj_gc_pubroot(L, &tv);  /* 09 section 9.2: native live root. */
  return node;
}

static void threading_live_publish(global_State *g, LJThread *th,
				   LJThreadLive *node)
{
  void *head;
  do {
    head = la_loadptr_acq((void *const *)&g->threading_live);
    lj_thread_live_next_rel(node, (LJThreadLive *)head);
  } while (!la_casptr((void **)&g->threading_live, &head, node,
		      LA_ACQ_REL, LA_ACQ));
  la_storeptr_rel((void **)&th->live_node, node);
}

static void threading_live_free_node(global_State *g, LJThreadLive *node)
{
  if (node) {
    setgcrefrel(node->ud, NULL);
    lj_mem_freet(g, node);
  }
}

static void threading_live_remove(LJThread *th)
{
  LJThreadLive *node;
  if (!th)
    return;
  node = (LJThreadLive *)la_loadptr_acq((void *const *)&th->live_node);
  if (node) {
    setgcrefrel(node->ud, NULL);
    la_storeptr_rel((void **)&th->live_node, NULL);
  }
}

static void threading_live_free_all(global_State *g)
{
  LJThreadLive *node = (LJThreadLive *)
    la_xchgptr_acqrel((void **)&g->threading_live, NULL);
  while (node) {
    LJThreadLive *next = lj_thread_live_next_acq(node);
    lj_mem_freet(g, node);
    node = next;
  }
}

static GCtab *threading_env_from_module(lua_State *L, GCtab *mod)
{
  cTValue *tv = lj_tab_getstr(mod, lj_str_newlit(L, "spawn"));
  if (tv && tvisfunc(tv)) {
    GCfunc *fn = funcV(tv);
    if (!isluafunc(fn)) {
      GCtab *env = tabref_acq(fn->c.env);
      if (env)
	return env;
    }
  }
  return NULL;
}

static GCtab *threading_loaded_env(lua_State *L)
{
  GCtab *reg = tabV(registry(L));
  cTValue *tv = lj_tab_getstr(reg, lj_str_newlit(L, "_LOADED"));
  if (tv && tvistab(tv)) {
    tv = lj_tab_getstr(tabV(tv), lj_str_newlit(L, LUA_THREADINGLIBNAME));
    if (tv && tvistab(tv))
      return threading_env_from_module(L, tabV(tv));
  }
  return NULL;
}

static GCtab *threading_ensure_env(lua_State *L)
{
  global_State *g = G(L);
  GCobj *o = gcref_acq(g->gcroot[GCROOT_THREADING_ENV]);
  GCtab *env = o && o->gch.gct == ~LJ_TTAB ? gco2tab(o) : NULL;
  if (!env) {
    TValue *top = L->top;
    env = threading_loaded_env(L);
    if (!env) {
      (void)luaopen_threading(L);
      env = threading_loaded_env(L);
      if (!env && L->top > top && tvistab(L->top-1))
	env = threading_env_from_module(L, tabV(L->top-1));
      L->top = top;
    }
    if (!env)
      lj_err_callermsg(L, "threading library unavailable");
    setgcrefroot(g->gcroot[GCROOT_THREADING_ENV], obj2gco(env));
  }
  return env;
}

static void threading_state_set_ud(lua_State *L, lua_State *L1, GCudata *ud)
{
  setgcrefrel(L1->mt_thread, obj2gco(ud));
  lj_gc_pubobjobj(L, L1, ud);
}

static TValue *threading_storeudata_str(lua_State *L, GCtab *env, GCstr *key,
					GCudata *ud)
{
  TValue keytv, tv, *dst;
  setudataV(L, &tv, ud);
  setstrV(L, &keytv, key);
  for (;;) {
    dst = lj_tab_setstr(L, env, key);
    if (lj_tab_trystoretv_cas_keyed(L, env, dst, &keytv, &tv) ==
	LJ_TAB_STORE_CAS_OK)
      return dst;
    lj_tab_store_wait_no_l();  /* threading env store saw stale/FORWARD slot. */
  }
}

static GCudata *threading_new_thread_ud(lua_State *L, GCtab *env)
{
  global_State *g = G(L);
  GCudata *ud = lj_udata_new(L, sizeof(LJThread), env);
  LJThread *th = (LJThread *)uddata(ud);
  memset(th, 0, sizeof(*th));
  setgcrefmt(ud->metatable, obj2gco(env));
  lj_gc_pubobjobj(L, ud, env);
  th->ud = ud;
  lj_gc2_finreg_udata_register_mt(L, g, ud, env);
  setudataV(L, L->top++, ud);
  return ud;
}

static void threading_publish_thread_state(lua_State *L, GCudata *ud,
					   LJThread *th, lua_State *L1)
{
  lj_thread_state_store_rel(th, L1);
  lj_gc_pubobjobj(L, ud, L1);
  lj_udata_udtype_rel(ud, UDTYPE_THREAD);
}

static int64_t threading_timeout_ns(lua_State *L, int narg, int has_default,
				    int64_t def)
{
  if (has_default && L->base + narg - 1 >= L->top)
    return def;
  {
    lua_Number sec = lj_lib_checknum(L, narg);
    lua_Number nsec;
    if (sec <= 0)
      return 0;
    nsec = sec * 1000000000.0;
    return nsec > (lua_Number)INT64_MAX ? INT64_MAX : (int64_t)nsec;
  }
}

static int64_t threading_capi_timeout_ns(lua_Number sec)
{
  lua_Number nsec;
  if (sec < 0)
    return -1;
  if (sec == 0)
    return 0;
  nsec = sec * 1000000000.0;
  return nsec > (lua_Number)INT64_MAX ? INT64_MAX : (int64_t)nsec;
}

static void threading_wake_thread(LJThread *th)
{
  la_add32_rlx(&th->futex, 1);
  la_futex_wake(&th->futex, INT_MAX);
}

static void threading_entering_leave(global_State *g)
{
  if (mt_entering_sub_acqrel(g, 1) == 1)
    mt_entering_futex_wake(g, INT_MAX);
}

static int threading_entering_begin(global_State *g)
{
  mt_entering_add_rlx(g, 1);
  if (mt_shutdown_acq(g) != 0) {
    threading_entering_leave(g);
    return 0;
  }
  return 1;
}

static void threading_wait_entering(global_State *g)
{
  while (mt_entering_acq(g) != 0) {
    uint32_t entering = mt_entering_acq(g);
    if (entering == 0)
      break;
    mt_entering_futex_wait(g, entering, 1000000);
  }
}

void lj_threading_shutdown(lua_State *L)
{
  global_State *g = G(L);
  TGState *cur = lj_thr_get_tg();
  LJThreadLive *node;
  LJThread *th;
  mt_shutdown_rel(g, 1);
  mt_gc_exclusive_futex_wake(g, INT_MAX);
  if (!threading_live_head(g) && mt_live_acq(g) == 0 &&
      mt_entering_acq(g) == 0)
    return;
  lj_assertG(cur == NULL || cur == g->main_tg,
	     "lua_close called from non-main OS thread");
  UNUSED(cur);
  threading_wait_entering(g);
  if (mt_live_acq(g) != 0) {
    (void)lj_safepoint_handshake(g, LJ_GC2_HS_STOPREQ);
    while (mt_live_acq(g) != 0) {
      uint32_t live = mt_live_acq(g);
      if (live == 0)
	break;
      mt_live_futex_wait(g, live, 1000000);
    }
  }
  for (node = threading_live_head(g); node != NULL;
       node = lj_thread_live_next_acq(node)) {
    GCudata *ud = threading_live_ud(node);
    if (!ud)
      continue;
    th = (LJThread *)uddata(ud);
    if (th->main_thread || lj_thread_state_load_acq(th) == NULL)
      continue;
    while (la_load32_acq(&th->state) != LJ_THREAD_DONE) {
      uint32_t futex = la_load32_acq(&th->futex);
      (void)la_futex_wait(&th->futex, futex, 1000000);
    }
    if (la_load32_acq(&th->joined) == 0) {
      uint32_t expect = 0;
      if (la_cas32(&th->joined, &expect, 1, LA_ACQ_REL, LA_ACQ))
	(void)lj_thr_join(&th->thr, NULL);
    }
    threading_live_remove(th);
    (void)lj_tg_reclaim_dead(g);
  }
  threading_live_free_all(g);
}

static void threading_gc_leave(global_State *g);

static int threading_gc_enter_counted(lua_State *L)
{
  global_State *g = G(L);
  for (;;) {
    uint32_t expect;
    uint32_t exclusive;
    while ((exclusive = mt_gc_exclusive_acq(g)) != 0) {
      if (mt_shutdown_acq(g) != 0) {
	threading_entering_leave(g);
	return 0;
      }
      mt_gc_exclusive_futex_wait(g, exclusive, 1000000);
    }
    if (mt_shutdown_acq(g) != 0) {
      threading_entering_leave(g);
      return 0;
    }
    expect = 0;
    (void)mt_active_cas(g, &expect, 1);
    if (mt_live_add_rlx(g, 1) == 0) {
      GCSize threshold = lj_gc_threshold_load(g);
      if (threshold == LJ_MAX_MEM && g->gc.state == GCSfinalize)
	threshold = lj_gc_mt_threshold_load(g);
      lj_gc_mt_threshold_store(g, threshold);
      /* M4: no automatic GC while children run. */
      lj_gc_threshold_store(g, LJ_MAX_MEM);
    }
    if (mt_gc_exclusive_acq(g) == 0) {
      threading_entering_leave(g);
      if (mt_shutdown_acq(g) != 0) {
	threading_gc_leave(g);
	return 0;
      }
      return 1;
    }
    threading_gc_leave(g);
  }
}

static int threading_gc_enter(lua_State *L)
{
  global_State *g = G(L);
  if (!threading_entering_begin(g))
    return 0;
  return threading_gc_enter_counted(L);
}

static void threading_gc_leave(global_State *g)
{
  if (mt_live_sub_acqrel(g, 1) == 1) {
    lj_gc_threshold_store(g, lj_gc_mt_threshold_load(g));
    lj_gc2_finalizer_spawn_release(g);
    mt_live_futex_wake(g, INT_MAX);
  }
}

static void threading_rehome_unstarted_stack(lua_State *L, lua_State *L1,
					     TGState *tg)
{
  global_State *g = G(L);
  TGState *dst = L2TG(L);
  TValue *oldst = tvref(L1->stack);
  TValue *st;
  ptrdiff_t delta;
  GCobj *up;
  MSize stacksize;
  size_t sz;
  L1->tg_hint = dst;
  if (!oldst || !tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ||
      g->allocf != lj_arena_allocf)
    return;
  if (lj_arena_owner_acq(lj_arena_of(oldst)) !=
      lj_arena_alloc_owner_acq(&tg->alloc))
    return;
  stacksize = L1->stacksize;
  sz = (size_t)stacksize * sizeof(TValue);
  st = (TValue *)lj_mem_realloc(L, NULL, 0, (GCSize)sz);
  memcpy(st, oldst, sz);
  setmref(L1->stack, st);
  delta = (char *)st - (char *)oldst;
  setmref(L1->maxstack, (TValue *)((char *)tvref(L1->maxstack) + delta));
  L1->base = (TValue *)((char *)L1->base + delta);
  L1->top = (TValue *)((char *)L1->top + delta);
  for (up = gcref_acq(L1->openupval); up != NULL;
       up = lj_obj_gcw_acq(up))
    setmref(gco2uv(up)->v, (TValue *)((char *)uvval(gco2uv(up)) + delta));
  lj_gc_total_sub(g, (GCSize)sz);
  (void)lj_arena_allocf(&tg->allocd, oldst, sz, 0);
}

static void *threading_worker(void *arg)
{
  LJThread *th = (LJThread *)arg;
  lua_State *L = lj_thread_state_load_acq(th);
  TGState *tg = th->tg;
  global_State *g = G(L);
  uint32_t tid = lj_thr_id(&th->thr);
  int status;

  lj_thr_set_tg(tg);
  lj_tg_tid_rel(tg, tid);
  L->tg_hint = tg;
  if (!lj_state_claim(L, tid)) {
    th->status = LUA_ERRRUN;
    th->nresults = 0;
    lj_thr_set_tg(NULL);
    threading_gc_leave(g);
    la_store32_rel(&th->state, LJ_THREAD_DONE);
    threading_wake_thread(th);
    return NULL;
  }
  lj_tg_store_cur_L(tg, L);
  lj_tg_store_thread_L(tg, L);
  tg->thread_ud = th->ud;
  lj_tg_attach(g, tg);

  if (mt_shutdown_acq(g) != 0) {
    th->status = LUA_ERRRUN;
    th->nresults = 0;
  } else {
    status = lua_pcall(L, (int)th->nargs, LUA_MULTRET, 0);
    th->status = (uint32_t)status;
    th->nresults = (uint32_t)(L->top - L->base);
  }

  lj_ccallback_disown_state(L);
  lj_state_release(L, tid);
  lj_tg_detach(g, tg);
  lj_tg_store_cur_L(tg, NULL);
  lj_tg_store_thread_L(tg, NULL);
  tg->thread_ud = NULL;
  lj_thr_set_tg(NULL);
  threading_gc_leave(g);

  la_store32_rel(&th->state, LJ_THREAD_DONE);
  threading_wake_thread(th);
  return NULL;
}

static int threading_is_current_thread(lua_State *L, LJThread *th)
{
  TGState *tg = L2TG(L);
  return tg != NULL && tg->thread_ud == th->ud;
}

static int threading_tg_is_registered(global_State *g, TGState *target)
{
  TGState *tg;
  if (!g || !target)
    return 0;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    if (tg == target)
      return 1;
  }
  return 0;
}

static LJThread *threading_thread_from_state(lua_State *L, lua_State *child)
{
  GCobj *o;
  if (!child || G(child) != G(L))
    lj_err_callermsg(L, "bad child thread");
  o = gcref_acq(child->mt_thread);
  if (o && o->gch.gct == ~LJ_TUDATA) {
    GCudata *ud = gco2ud(o);
    if (lj_udata_udtype_acq(ud) == UDTYPE_THREAD) {
      LJThread *th = (LJThread *)uddata(ud);
      if (lj_thread_state_load_acq(th) == child)
	return th;
    }
  }
  lj_err_callermsg(L, "child thread is not joinable");
  return NULL;  /* unreachable */
}

static uint32_t threading_join_claim_results(lua_State *L, lua_State *child,
					     uint32_t tid)
{
  uint32_t actions = 0;
  while (!lj_state_claim(child, tid)) {
    actions |= lj_thr_sleep_ns(L, 1000000);
    lj_safepoint_checkstop(L, actions);
  }
  return actions;
}

static int threading_join_core(lua_State *L, LJThread *th, int has_timeout,
			       int64_t ns)
{
  int remove_live = 0;
  uint32_t join_actions = 0;
  uint32_t state;
  if (threading_is_current_thread(L, th)) {
    if (has_timeout) {
      setnilV(L->top++);
      lua_pushliteral(L, "timeout");
      return 2;
    }
    lj_err_callermsg(L, "self-join would deadlock");
  }
  if (th->main_thread)
    lj_err_callermsg(L, "cannot join main thread");
  for (;;) {
    state = la_load32_acq(&th->state);
    if (state == LJ_THREAD_DONE)
      break;
    if (ns == 0) {
      setnilV(L->top++);
      lua_pushliteral(L, "timeout");
      return 2;
    }
    {
      uint32_t futex = la_load32_acq(&th->futex);
      uint32_t actions;
      lj_native_enter(L2TG(L));
      (void)la_futex_wait(&th->futex, futex, ns);
      actions = lj_native_leave(L);
      lj_safepoint_checkstop(L, actions);
    }
    if (ns > 0 && la_load32_acq(&th->state) != LJ_THREAD_DONE) {
      setnilV(L->top++);
      lua_pushliteral(L, "timeout");
      return 2;
    }
  }

  if (la_load32_acq(&th->joined) == 0) {
    uint32_t expect = 0;
    if (la_cas32(&th->joined, &expect, 1, LA_ACQ_REL, LA_ACQ)) {
      lj_native_enter(L2TG(L));
      (void)lj_thr_join(&th->thr, NULL);
      join_actions = lj_native_leave(L);
      remove_live = 1;
    }
  }

  lj_state_checkstack(L, th->nresults + 1u);
  setboolV(L->top++, th->status == LUA_OK);
  {
    uint32_t tid = lj_thr_current_id(G(L));
    uint32_t i;
    lua_State *child = lj_thread_state_load_acq(th);
    join_actions |= threading_join_claim_results(L, child, tid);
    for (i = 0; i < th->nresults; i++)
      copyTV(L, L->top++, child->base + i);
    lj_state_release(child, tid);
  }
  if (remove_live) {
    threading_live_remove(th);
    (void)lj_tg_reclaim_dead(G(L));
  }
  lj_safepoint_checkstop(L, join_actions);
  return (int)th->nresults + 1;
}

#define LJLIB_MODULE_threading_thread

LJLIB_CF(threading_thread_join)
{
  LJThread *th = threading_tothread(L);
  int64_t ns;
  int has_timeout;
  has_timeout = L->base + 1 < L->top;
  ns = threading_timeout_ns(L, 2, 1, -1);
  return threading_join_core(L, th, has_timeout, ns);
}

LJLIB_CF(threading_thread_id)
{
  LJThread *th = threading_tothread(L);
  setintV(L->top++, (int32_t)lj_thr_id(&th->thr));
  return 1;
}

LJLIB_CF(threading_thread_running)
{
  LJThread *th = threading_tothread(L);
  setboolV(L->top++, th->main_thread ||
	   la_load32_acq(&th->state) != LJ_THREAD_DONE);
  return 1;
}

LJLIB_CF(threading_thread___tostring)
{
  (void)threading_tothread(L);
  lua_pushliteral(L, "threading.thread");
  return 1;
}

LJLIB_CF(threading_thread___gc)
{
  if (L->base < L->top && tvisudata(L->base) &&
      lj_udata_udtype_acq(udataV(L->base)) == UDTYPE_THREAD) {
    global_State *g = G(L);
    LJThread *th = (LJThread *)uddata(udataV(L->base));
    if (!th->main_thread && th->tg &&
	la_load32_acq(&th->state) == LJ_THREAD_DONE &&
	la_load32_acq(&th->joined) != 0) {
      (void)lj_tg_reclaim_dead(g);
      if (!threading_tg_is_registered(g, th->tg)) {
	lj_tg_fini_thread(g, th->tg);
	lj_mem_freet(g, th->tg);
	th->tg = NULL;
	lj_thread_state_store_rel(th, NULL);
      }
    }
  }
  return 0;
}

LJLIB_PUSH("threading.thread") LJLIB_SET(__metatable)
LJLIB_PUSH(top-1) LJLIB_SET(__index)

/* -- Mutex methods ------------------------------------------------------- */

#define LJ_MUTEX_UNLOCKED	0u
#define LJ_MUTEX_LOCKED		1u

typedef struct LJMutex {
  uint32_t state;
} LJMutex;

static LJMutex *threading_tomutex(lua_State *L)
{
  if (!(L->base < L->top && tvisudata(L->base) &&
	lj_udata_udtype_acq(udataV(L->base)) == UDTYPE_MUTEX))
    lj_err_argtype(L, 1, "threading.mutex");
  return (LJMutex *)uddata(udataV(L->base));
}

#define LJLIB_MODULE_threading_mutex

LJLIB_CF(threading_mutex_lock)
{
  LJMutex *m = threading_tomutex(L);
  for (;;) {
    uint32_t expect = LJ_MUTEX_UNLOCKED;
    if (la_cas32(&m->state, &expect, LJ_MUTEX_LOCKED, LA_ACQ_REL, LA_ACQ))
      return 0;
    lj_native_enter(L2TG(L));
    (void)la_futex_wait(&m->state, LJ_MUTEX_LOCKED, 1000000);
    lj_safepoint_checkstop(L, lj_native_leave(L));
  }
}

LJLIB_CF(threading_mutex_trylock)
{
  LJMutex *m = threading_tomutex(L);
  uint32_t expect = LJ_MUTEX_UNLOCKED;
  setboolV(L->top++, la_cas32(&m->state, &expect, LJ_MUTEX_LOCKED,
			      LA_ACQ_REL, LA_ACQ));
  return 1;
}

LJLIB_CF(threading_mutex_unlock)
{
  LJMutex *m = threading_tomutex(L);
  uint32_t old = la_xchg32_acqrel(&m->state, LJ_MUTEX_UNLOCKED);
  if (old == LJ_MUTEX_UNLOCKED)
    lj_err_callermsg(L, "unlock of unlocked mutex");
  la_futex_wake(&m->state, INT_MAX);
  return 0;
}

LJLIB_CF(threading_mutex___tostring)
{
  (void)threading_tomutex(L);
  lua_pushliteral(L, "threading.mutex");
  return 1;
}

LJLIB_PUSH("threading.mutex") LJLIB_SET(__metatable)
LJLIB_PUSH(top-1) LJLIB_SET(__index)

/* -- Channel methods ----------------------------------------------------- */

static LJChan *threading_tochan(lua_State *L)
{
  if (!(L->base < L->top && tvisudata(L->base) &&
	lj_udata_udtype_acq(udataV(L->base)) == UDTYPE_CHANNEL))
    lj_err_argtype(L, 1, "threading.channel");
  return (LJChan *)uddata(udataV(L->base));
}

static void threading_push_recv(lua_State *L, int rc, TValue *out)
{
  if (rc == LJ_CHAN_OK) {
    copyTV(L, L->top++, out);
    setboolV(L->top++, 1);
  } else if (rc == LJ_CHAN_CLOSED) {
    setnilV(L->top++);
    setboolV(L->top++, 0);
  } else if (rc == LJ_CHAN_TIMEOUT) {
    setnilV(L->top++);
    lua_pushliteral(L, "timeout");
  } else {
    setnilV(L->top++);
    setboolV(L->top++, 0);
  }
}

#define LJLIB_MODULE_threading_channel

LJLIB_CF(threading_channel_send)
{
  GCudata *ud;
  LJChan *ch = threading_tochan(L);
  cTValue *tv = lj_lib_checkany(L, 2);
  int64_t ns = threading_timeout_ns(L, 3, 1, -1);
  int rc;
  ud = udataV(L->base);
  lj_gc_pubobjtv(L, ud, tv);  /* 09 section 9.5: publish Lua refs to channel. */
  rc = lj_chan_send_timeout(L, ch, tv, ns);
  if (rc == LJ_CHAN_CLOSED)
    lj_err_callermsg(L, "closed channel");
  if (rc == LJ_CHAN_TIMEOUT) {
    setnilV(L->top++);
    lua_pushliteral(L, "timeout");
    return 2;
  }
  setboolV(L->top++, 1);
  return 1;
}

LJLIB_CF(threading_channel_recv)
{
  TValue out;
  LJChan *ch = threading_tochan(L);
  int rc;
  int64_t ns = threading_timeout_ns(L, 2, 1, -1);
  setnilV(&out);
  rc = lj_chan_recv_timeout_gc(L, ch, &out, ns);
  threading_push_recv(L, rc, &out);
  return 2;
}

LJLIB_CF(threading_channel_peek)
{
  TValue out;
  LJChan *ch = threading_tochan(L);
  int rc;
  setnilV(&out);
  rc = lj_chan_peek(ch, &out);
  threading_push_recv(L, rc, &out);
  return 2;
}

LJLIB_CF(threading_channel_close)
{
  lj_chan_close(threading_tochan(L));
  return 0;
}

LJLIB_CF(threading_channel___gc)
{
  if (L->base < L->top && tvisudata(L->base) &&
      lj_udata_udtype_acq(udataV(L->base)) == UDTYPE_CHANNEL)
    lj_chan_close((LJChan *)uddata(udataV(L->base)));
  return 0;
}

LJLIB_CF(threading_channel___tostring)
{
  (void)threading_tochan(L);
  lua_pushliteral(L, "threading.channel");
  return 1;
}

LJLIB_PUSH("threading.channel") LJLIB_SET(__metatable)
LJLIB_PUSH(top-1) LJLIB_SET(__index)

#define LJLIB_MODULE_threading

LJLIB_PUSH(top-4) LJLIB_SET(!)  /* Set environment to thread methods. */

LJLIB_CF(threading_cpucount)
{
  setintV(L->top++, (int32_t)lj_thr_cpucount());
  return 1;
}

LJLIB_CF(threading_now)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    setnilV(L->top++);
  } else {
    lua_Number sec = (lua_Number)ts.tv_sec +
      (lua_Number)ts.tv_nsec / 1000000000.0;
    setnumV(L->top++, sec);
  }
  return 1;
}

LJLIB_CF(threading_fence)
{
  lj_thr_fence();
  return 0;
}

LJLIB_CF(threading_sleep)
{
  lua_Number sec = L->base < L->top ? lj_lib_checknum(L, 1) : 0;
  int64_t ns = 0;
  if (sec > 0) {
    lua_Number nsec = sec * 1000000000.0;
    ns = nsec > (lua_Number)INT64_MAX ? INT64_MAX : (int64_t)nsec;
  }
  lj_safepoint_checkstop(L, lj_thr_sleep_ns(L, ns));
  return 0;
}

static lua_State *threading_spawn_core(lua_State *L, GCtab *env, TValue *base,
				       ptrdiff_t nargs)
{
  GCudata *ud;
  LJThread *th;
  LJThreadLive *live;
  TGState *tg;
  lua_State *L1;
  uint32_t tid = lj_thr_current_id(G(L));
  ptrdiff_t baseofs = base - L->base;
  ptrdiff_t i;
  int rc;

  if (mt_shutdown_acq(G(L)) != 0)
    lj_err_callermsg(L, "VM shutdown in progress");
  lj_state_checkstack(L, 2);
  base = L->base + baseofs;
  L1 = lua_newthread(L);
  if (!lj_state_claim(L1, tid))
    lj_err_callermsg(L, "thread busy");
  lj_state_checkstack(L1, (MSize)(nargs + 1));
  for (i = 0; i <= nargs; i++)
    copyTV(L1, L1->top++, base + i);
  lj_state_release(L1, tid);

  ud = threading_new_thread_ud(L, env);
  threading_state_set_ud(L, L1, ud);
  th = (LJThread *)uddata(ud);
  tg = lj_mem_newt(L, sizeof(TGState), TGState);
  threading_publish_thread_state(L, ud, th, L1);
  th->tg = tg;
  th->state = LJ_THREAD_RUNNING;
  th->nargs = (uint32_t)nargs;
  lj_tg_init_thread(G(L), tg, L1, threading_arena_internal(G(L)));
  th->thr.tid = lj_thr_newid();
  lj_tg_tid_rel(tg, th->thr.tid);
  tg->thread_ud = ud;
  if (!lj_state_rehome_stack(L1)) {
    L1->tg_hint = L2TG(L);
    lj_tg_fini_thread(G(L), tg);
    lj_mem_freet(G(L), tg);
    th->tg = NULL;
    th->state = LJ_THREAD_DONE;
    lj_err_mem(L);
  }
  live = threading_live_new(L, ud);

  if (!threading_gc_enter(L)) {
    threading_live_free_node(G(L), live);
    threading_rehome_unstarted_stack(L, L1, tg);
    lj_tg_fini_thread(G(L), tg);
    lj_mem_freet(G(L), tg);
    th->tg = NULL;
    th->state = LJ_THREAD_DONE;
    lj_err_callermsg(L, "VM shutdown in progress");
  }
  threading_live_publish(G(L), th, live);
  rc = lj_thr_create(&th->thr, threading_worker, th);
  if (rc != 0) {
    char errbuf[LJ_ERR_ERRNO_BUFSZ];
    const char *emsg = lj_err_strerrno(rc, errbuf, sizeof(errbuf));
    threading_live_remove(th);
    threading_rehome_unstarted_stack(L, L1, tg);
    lj_tg_fini_thread(G(L), tg);
    lj_mem_freet(G(L), tg);
    th->tg = NULL;
    th->state = LJ_THREAD_DONE;
    threading_gc_leave(G(L));
    lj_err_callermsg(L, emsg);
  }

  return L1;
}

LJLIB_CF(threading_spawn)
{
  ptrdiff_t nargs;
  if (!(L->base < L->top && tvisfunc(L->base)))
    lj_err_argt(L, 1, LUA_TFUNCTION);
  nargs = L->top - L->base - 1;
  (void)threading_spawn_core(L, tabref_acq(curr_func(L)->c.env), L->base,
			     nargs);
  copyTV(L, L->base, L->top-1);
  L->top = L->base + 1;
  return 1;
}

LJLIB_CF(threading_current)
{
  GCtab *env = tabref_acq(curr_func(L)->c.env);
  TGState *tg = L2TG(L);
  GCudata *ud = tg ? tg->thread_ud : NULL;
  if (!ud) {
    GCstr *key = lj_str_newlit(L, THREADING_MAIN_KEY);
    cTValue *tv = lj_tab_getstr(env, key);
    if (L != mainthread_acq(G(L)))
      lj_err_callermsg(L, "attached thread is not joinable");
    if (tv && tvisudata(tv) &&
	lj_udata_udtype_acq(udataV(tv)) == UDTYPE_THREAD) {
      ud = udataV(tv);
    } else {
      LJThread *th;
      ud = threading_new_thread_ud(L, env);
      th = (LJThread *)uddata(ud);
      threading_publish_thread_state(L, ud, th, L);
      th->tg = tg;
      th->state = LJ_THREAD_RUNNING;
      th->main_thread = (L == mainthread_acq(G(L)));
      th->thr.tid = tg ? lj_tg_tid_acq(tg) : 0;
      threading_state_set_ud(L, L, ud);
      if (tg)
	tg->thread_ud = ud;
      threading_storeudata_str(L, env, key, ud);
      lj_gc_pubtab(L, env);
      return 1;
    }
    if (tg)
      tg->thread_ud = ud;
    threading_state_set_ud(L, L, ud);
  }
  setudataV(L, L->top++, ud);
  return 1;
}

LJLIB_PUSH(top-3) LJLIB_SET(!)  /* Set environment to mutex methods. */

LJLIB_CF(threading_mutex)
{
  GCtab *env = tabref_acq(curr_func(L)->c.env);
  GCudata *ud = lj_udata_new(L, sizeof(LJMutex), env);
  LJMutex *m = (LJMutex *)uddata(ud);
  setgcrefmt(ud->metatable, obj2gco(env));
  lj_gc_pubobjobj(L, ud, env);
  lj_gc2_finreg_udata_register_mt(L, G(L), ud, env);
  m->state = LJ_MUTEX_UNLOCKED;
  lj_udata_udtype_rel(ud, UDTYPE_MUTEX);
  setudataV(L, L->top++, ud);
  lj_gc_check(L);
  return 1;
}

LJLIB_PUSH(top-2) LJLIB_SET(!)  /* Set environment to channel methods. */

LJLIB_CF(threading_channel)
{
  int32_t n = L->base < L->top ? lj_lib_checkint(L, 1) : 0;
  uint32_t cap;
  uint32_t rcap;
  uint64_t bytes;
  GCtab *env;
  GCudata *ud;
  if (n < 0)
    lj_err_arg(L, 1, LJ_ERR_NUMRNG);
  cap = (uint32_t)n;
  rcap = lj_chan_round_capacity(cap);
  bytes = sizeof(LJChan) + ((uint64_t)rcap - 1u) * sizeof(LJChanSlot);
  if (bytes > LJ_MAX_UDATA)
    lj_err_arg(L, 1, LJ_ERR_NUMRNG);
  env = tabref_acq(curr_func(L)->c.env);
  ud = lj_udata_new(L, lj_chan_memsize(cap), env);
  setgcrefmt(ud->metatable, obj2gco(env));
  lj_gc_pubobjobj(L, ud, env);
  lj_gc2_finreg_udata_register_mt(L, G(L), ud, env);
  lj_chan_init((LJChan *)uddata(ud), cap);
  lj_udata_udtype_rel(ud, UDTYPE_CHANNEL);
  setudataV(L, L->top++, ud);
  lj_gc_check(L);
  return 1;
}

LUA_API lua_State *luaMT_spawn(lua_State *L, int nargs)
{
  ptrdiff_t baseofs;
  TValue *base;
  GCtab *env;
  lua_State *L1;
  if (nargs < 0 || L->top - L->base <= nargs)
    lj_err_callermsg(L, "function expected");
  baseofs = (L->top - L->base) - (ptrdiff_t)nargs - 1;
  env = threading_ensure_env(L);
  base = L->base + baseofs;
  if (!tvisfunc(base))
    lj_err_callermsg(L, "function expected");
  L1 = threading_spawn_core(L, env, base, (ptrdiff_t)nargs);
  base = L->base + baseofs;
  copyTV(L, base, L->top-2);  /* Leave the child Lua thread for rooting. */
  L->top = base + 1;
  return L1;
}

LUA_API int luaMT_join(lua_State *L, lua_State *child, lua_Number timeout)
{
  LJThread *th = threading_thread_from_state(L, child);
  int has_timeout = timeout >= 0;
  return threading_join_core(L, th, has_timeout,
			     threading_capi_timeout_ns(timeout));
}

LUA_API void luaMT_fence(void)
{
  lj_thr_fence();
}

int lj_threading_attach(lua_State *L)
{
  global_State *g;
  TGState *cur, *tg;
  GCobj *o;
  uint32_t tid;
  if (!L)
    return 0;
  g = G(L);
  cur = lj_thr_get_tg();
  if (cur)
    return lj_tg_load_thread_L(cur) == L;
  if (L == mainthread_acq(g))
    return 0;
  if (!threading_entering_begin(g))
    return 0;
  tid = lj_thr_newid();
  if (!lj_state_claim(L, tid)) {
    threading_entering_leave(g);
    return 0;
  }
  tg = (TGState *)malloc(sizeof(TGState));
  if (!tg) {
    lj_state_release(L, tid);
    threading_entering_leave(g);
    return 0;
  }
  lj_tg_init_thread(g, tg, L, threading_arena_internal(g));
  lj_tg_tid_rel(tg, tid);
  L->tg_hint = tg;
  lj_thr_set_tg(tg);
  lj_tg_store_cur_L(tg, L);
  lj_tg_store_thread_L(tg, L);
  o = gcref_acq(L->mt_thread);
  if (o && o->gch.gct == ~LJ_TUDATA &&
      lj_udata_udtype_acq(gco2ud(o)) == UDTYPE_THREAD)
    tg->thread_ud = gco2ud(o);
  if (!threading_gc_enter_counted(L)) {
    L->tg_hint = NULL;
    lj_thr_set_tg(NULL);
    lj_state_release(L, tid);
    lj_tg_fini_thread(g, tg);
    free(tg);
    return 0;
  }
  lj_tg_attach(g, tg);
  if (mt_shutdown_acq(g) != 0) {
    lj_threading_detach(L, 1);
    return 0;
  }
  return 1;
}

LUA_API int luaMT_attach(lua_State *L)
{
  return lj_threading_attach(L);
}

void lj_threading_detach(lua_State *L, int disown_callbacks)
{
  global_State *g;
  TGState *tg;
  uint32_t tid;
  if (!L)
    return;
  g = G(L);
  tg = lj_thr_get_tg();
  if (!tg || tg == g->main_tg || lj_tg_load_thread_L(tg) != L)
    return;
  tid = lj_tg_tid_acq(tg);
  if (disown_callbacks)
    lj_ccallback_disown_state(L);
  lj_tg_detach(g, tg);
  lj_tg_store_cur_L(tg, NULL);
  lj_tg_store_thread_L(tg, NULL);
  tg->thread_ud = NULL;
  L->tg_hint = NULL;
  lj_state_release(L, tid);
  threading_gc_leave(g);
  lj_thr_set_tg(NULL);
  (void)lj_tg_reclaim_dead(g);
}

LUA_API void luaMT_detach(lua_State *L)
{
  lj_threading_detach(L, 1);
}

#include "lj_libdef.h"

LUALIB_API int luaopen_threading(lua_State *L)
{
  GCtab *env;
  LJ_LIB_REG(L, NULL, threading_thread);
  LJ_LIB_REG(L, NULL, threading_mutex);
  LJ_LIB_REG(L, NULL, threading_channel);
  LJ_LIB_REG(L, LUA_THREADINGLIBNAME, threading);
  env = threading_env_from_module(L, tabV(L->top-1));
  if (env)
    setgcrefroot(G(L)->gcroot[GCROOT_THREADING_ENV], obj2gco(env));
  return 1;
}
