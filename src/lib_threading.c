/*
** Threading library.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define lib_threading_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_chan.h"
#include "lj_err.h"
#include "lj_func.h"
#include "lj_gc.h"
#include "lj_lib.h"
#include "lj_safepoint.h"
#include "lj_state.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_udata.h"

#define THREADING_THREADS_KEY	"__threads"
#define THREADING_MAIN_KEY	"__main"

/* -- Thread methods ------------------------------------------------------ */

static LJThread *threading_tothread(lua_State *L)
{
  if (!(L->base < L->top && tvisudata(L->base) &&
	udataV(L->base)->udtype == UDTYPE_THREAD))
    lj_err_argtype(L, 1, "threading.thread");
  return (LJThread *)uddata(udataV(L->base));
}

static GCtab *threading_live_root(global_State *g)
{
  GCobj *o = gcref(g->gcroot[GCROOT_THREADING]);
  return o && o->gch.gct == ~LJ_TTAB ? gco2tab(o) : NULL;
}

static GCtab *threading_live_table(lua_State *L, GCtab *env)
{
  GCstr *key = lj_str_newlit(L, THREADING_THREADS_KEY);
  GCtab *live = threading_live_root(G(L));
  cTValue *tv = lj_tab_getstr(env, key);
  if (live) {
    if (!tv || !tvistab(tv) || tabV(tv) != live) {
      settabV(L, lj_tab_setstr(L, env, key), live);
      lj_gc_pubtab(L, env);
    }
    return live;
  }
  if (tv && tvistab(tv)) {
    setgcrefroot(G(L)->gcroot[GCROOT_THREADING], obj2gco(tabV(tv)));
    return tabV(tv);
  }
  {
    GCtab *t = lj_tab_new(L, 0, 0);
    settabV(L, lj_tab_setstr(L, env, key), t);
    setgcrefroot(G(L)->gcroot[GCROOT_THREADING], obj2gco(t));
    lj_gc_pubtab(L, env);
    return t;
  }
}

static GCtab *threading_env_from_module(lua_State *L, GCtab *mod)
{
  cTValue *tv = lj_tab_getstr(mod, lj_str_newlit(L, "spawn"));
  if (tv && tvisfunc(tv)) {
    GCfunc *fn = funcV(tv);
    if (!isluafunc(fn) && tabref(fn->c.env))
      return tabref(fn->c.env);
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
  GCobj *o = gcref(g->gcroot[GCROOT_THREADING_ENV]);
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
  (void)threading_live_table(L, env);
  return env;
}

static void threading_state_set_ud(lua_State *L, lua_State *L1, GCudata *ud)
{
  setgcrefrel(L1->mt_thread, obj2gco(ud));
  lj_gc_pubobjobj(L, L1, ud);
}

static void threading_live_set(lua_State *L, GCtab *env, GCudata *ud,
			       lua_State *L1)
{
  GCtab *live = threading_live_table(L, env);
  TValue key;
  setudataV(L, &key, ud);
  if (L1)
    setthreadV(L, lj_tab_set(L, live, &key), L1);
  else
    setnilV(lj_tab_set(L, live, &key));
  lj_gc_pubtab(L, live);
}

static void threading_live_remove(lua_State *L, GCudata *ud)
{
  GCtab *live = threading_live_root(G(L));
  TValue key;
  if (!live || !ud)
    return;
  setudataV(L, &key, ud);
  setnilV(lj_tab_set(L, live, &key));
}

static LJThread *threading_live_next(global_State *g, GCudata **pud)
{
  GCtab *live = threading_live_root(g);
  TValue key, kv[2];
  if (!live)
    return NULL;
  setnilV(&key);
  while (lj_tab_next(live, &key, kv) > 0) {
    key = kv[0];
    if (tvisudata(&kv[0]) && udataV(&kv[0])->udtype == UDTYPE_THREAD) {
      LJThread *th = (LJThread *)uddata(udataV(&kv[0]));
      if (!th->main_thread && th->L != NULL) {
	*pud = udataV(&kv[0]);
	return th;
      }
    }
  }
  return NULL;
}

static GCudata *threading_new_thread_ud(lua_State *L, GCtab *env)
{
  GCudata *ud = lj_udata_new(L, sizeof(LJThread), env);
  LJThread *th = (LJThread *)uddata(ud);
  memset(th, 0, sizeof(*th));
  ud->udtype = UDTYPE_THREAD;
  /* NOBARRIER: The GCudata is new (marked white). */
  setgcref(ud->metatable, obj2gco(env));
  th->ud = ud;
  setudataV(L, L->top++, ud);
  return ud;
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

void lj_threading_shutdown(lua_State *L)
{
  global_State *g = G(L);
  TGState *cur = lj_thr_get_tg();
  GCudata *ud;
  LJThread *th;
  if (!threading_live_root(g))
    return;
  lj_assertG(cur == NULL || cur == g->main_tg,
	     "lua_close called from non-main OS thread");
  UNUSED(cur);
  if (la_load32_acq(&g->mt_live) != 0)
    (void)lj_safepoint_handshake(g, LJ_GC2_HS_STOPREQ);
  while ((th = threading_live_next(g, &ud)) != NULL) {
    while (la_load32_acq(&th->state) != LJ_THREAD_DONE) {
      uint32_t futex = la_load32_acq(&th->futex);
      (void)la_futex_wait(&th->futex, futex, 1000000);
    }
    if (la_load32_acq(&th->joined) == 0) {
      uint32_t expect = 0;
      if (la_cas32(&th->joined, &expect, 1, LA_ACQ_REL, LA_ACQ))
	(void)lj_thr_join(&th->thr, NULL);
    }
    threading_live_remove(L, ud);
    (void)lj_tg_reclaim_dead(g);
  }
}

static void threading_gc_enter(lua_State *L)
{
  global_State *g = G(L);
  uint32_t expect = 0;
  if (la_cas32(&g->mt_active, &expect, 1, LA_ACQ_REL, LA_ACQ))
    lj_func_closeuv(L, tvref(L->stack));
  if (la_add32_rlx(&g->mt_live, 1) == 0) {
    g->mt_gc_threshold = g->gc.threshold;
    g->gc.threshold = LJ_MAX_MEM;  /* M4: no automatic GC while children run. */
  }
}

static void threading_gc_leave(global_State *g)
{
  if (la_sub32_acqrel(&g->mt_live, 1) == 1)
    g->gc.threshold = g->mt_gc_threshold;
}

static void *threading_worker(void *arg)
{
  LJThread *th = (LJThread *)arg;
  lua_State *L = th->L;
  TGState *tg = th->tg;
  global_State *g = G(L);
  uint32_t tid = lj_thr_id(&th->thr);
  int status;

  lj_thr_set_tg(tg);
  tg->tid = tid;
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
  tg->cur_L = L;
  tg->thread_L = L;
  tg->thread_ud = th->ud;
  lj_tg_attach(g, tg);

  status = lua_pcall(L, (int)th->nargs, LUA_MULTRET, 0);
  th->status = (uint32_t)status;
  th->nresults = (uint32_t)(L->top - L->base);

  lj_state_release(L, tid);
  lj_tg_detach(g, tg);
  tg->cur_L = NULL;
  tg->thread_L = NULL;
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

static LJThread *threading_thread_from_state(lua_State *L, lua_State *child)
{
  GCobj *o;
  if (!child || G(child) != G(L))
    lj_err_callermsg(L, "bad child thread");
  o = gcref(child->mt_thread);
  if (o && o->gch.gct == ~LJ_TUDATA) {
    GCudata *ud = gco2ud(o);
    if (ud->udtype == UDTYPE_THREAD) {
      LJThread *th = (LJThread *)uddata(ud);
      if (th->L == child)
	return th;
    }
  }
  lj_err_callermsg(L, "child thread is not joinable");
  return NULL;  /* unreachable */
}

static int threading_join_core(lua_State *L, LJThread *th, int has_timeout,
			       int64_t ns)
{
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
      (void)lj_native_leave(L);
      threading_live_remove(L, th->ud);
      (void)lj_tg_reclaim_dead(G(L));
    }
  }

  lj_state_checkstack(L, th->nresults + 1u);
  setboolV(L->top++, th->status == LUA_OK);
  {
    uint32_t tid = lj_thr_current_id(G(L));
    uint32_t i;
    while (!lj_state_claim(th->L, tid))
      la_cpu_pause();
    for (i = 0; i < th->nresults; i++)
      copyTV(L, L->top++, th->L->base + i);
    lj_state_release(th->L, tid);
  }
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
	udataV(L->base)->udtype == UDTYPE_MUTEX))
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
    (void)la_futex_wait(&m->state, LJ_MUTEX_LOCKED, -1);
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
	udataV(L->base)->udtype == UDTYPE_CHANNEL))
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
  rc = lj_chan_recv_timeout(L, ch, &out, ns);
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
      udataV(L->base)->udtype == UDTYPE_CHANNEL)
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
  TGState *tg;
  lua_State *L1;
  uint32_t tid = lj_thr_current_id(G(L));
  ptrdiff_t baseofs = base - L->base;
  ptrdiff_t i;
  int rc;

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
  th->L = L1;
  th->tg = tg;
  th->state = LJ_THREAD_RUNNING;
  th->nargs = (uint32_t)nargs;
  lj_tg_init_thread(G(L), tg, L1, 0);
  th->thr.tid = lj_thr_newid();
  tg->tid = th->thr.tid;
  tg->thread_ud = ud;
  threading_live_set(L, env, ud, L1);

  threading_gc_enter(L);
  rc = lj_thr_create(&th->thr, threading_worker, th);
  if (rc != 0) {
    threading_gc_leave(G(L));
    threading_live_set(L, env, ud, NULL);
    lj_tg_fini_thread(G(L), tg);
    lj_mem_freet(G(L), tg);
    th->tg = NULL;
    th->state = LJ_THREAD_DONE;
    lj_err_callermsg(L, strerror(rc));
  }

  return L1;
}

LJLIB_CF(threading_spawn)
{
  ptrdiff_t nargs;
  if (!(L->base < L->top && tvisfunc(L->base)))
    lj_err_argt(L, 1, LUA_TFUNCTION);
  nargs = L->top - L->base - 1;
  (void)threading_spawn_core(L, tabref(curr_func(L)->c.env), L->base, nargs);
  copyTV(L, L->base, L->top-1);
  L->top = L->base + 1;
  return 1;
}

LJLIB_CF(threading_current)
{
  GCtab *env = tabref(curr_func(L)->c.env);
  TGState *tg = L2TG(L);
  GCudata *ud = tg ? tg->thread_ud : NULL;
  if (!ud) {
    GCstr *key = lj_str_newlit(L, THREADING_MAIN_KEY);
    cTValue *tv = lj_tab_getstr(env, key);
    if (tv && tvisudata(tv) && udataV(tv)->udtype == UDTYPE_THREAD) {
      ud = udataV(tv);
    } else {
      LJThread *th;
      ud = threading_new_thread_ud(L, env);
      th = (LJThread *)uddata(ud);
      th->L = L;
      th->tg = tg;
      th->state = LJ_THREAD_RUNNING;
      th->main_thread = (L == mainthread(G(L)));
      th->thr.tid = tg ? tg->tid : 0;
      threading_state_set_ud(L, L, ud);
      if (tg)
	tg->thread_ud = ud;
      setudataV(L, lj_tab_setstr(L, env, key), ud);
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
  GCtab *env = tabref(curr_func(L)->c.env);
  GCudata *ud = lj_udata_new(L, sizeof(LJMutex), env);
  LJMutex *m = (LJMutex *)uddata(ud);
  ud->udtype = UDTYPE_MUTEX;
  /* NOBARRIER: The GCudata is new (marked white). */
  setgcref(ud->metatable, obj2gco(env));
  m->state = LJ_MUTEX_UNLOCKED;
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
  env = tabref(curr_func(L)->c.env);
  ud = lj_udata_new(L, lj_chan_memsize(cap), env);
  ud->udtype = UDTYPE_CHANNEL;
  /* NOBARRIER: The GCudata is new (marked white). */
  setgcref(ud->metatable, obj2gco(env));
  lj_chan_init((LJChan *)uddata(ud), cap);
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

LUA_API int luaMT_attach(lua_State *L)
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
    return cur->thread_L == L;
  if (L == mainthread(g))
    return 0;
  tid = lj_thr_newid();
  if (!lj_state_claim(L, tid))
    return 0;
  tg = (TGState *)malloc(sizeof(TGState));
  if (!tg) {
    lj_state_release(L, tid);
    return 0;
  }
  lj_tg_init_thread(g, tg, L, 0);
  tg->tid = tid;
  L->tg_hint = tg;
  lj_thr_set_tg(tg);
  tg->cur_L = L;
  tg->thread_L = L;
  o = gcref(L->mt_thread);
  if (o && o->gch.gct == ~LJ_TUDATA && gco2ud(o)->udtype == UDTYPE_THREAD)
    tg->thread_ud = gco2ud(o);
  threading_gc_enter(L);
  lj_tg_attach(g, tg);
  return 1;
}

LUA_API void luaMT_detach(lua_State *L)
{
  global_State *g;
  TGState *tg;
  uint32_t tid;
  if (!L)
    return;
  g = G(L);
  tg = lj_thr_get_tg();
  if (!tg || tg == g->main_tg || tg->thread_L != L)
    return;
  tid = tg->tid;
  lj_tg_detach(g, tg);
  tg->cur_L = NULL;
  tg->thread_L = NULL;
  tg->thread_ud = NULL;
  L->tg_hint = NULL;
  lj_state_release(L, tid);
  threading_gc_leave(g);
  lj_thr_set_tg(NULL);
  (void)lj_tg_reclaim_dead(g);
}

#include "lj_libdef.h"

LUALIB_API int luaopen_threading(lua_State *L)
{
  GCtab *env;
  LJ_LIB_REG(L, NULL, threading_thread);
  lua_createtable(L, 0, 0);
  lua_setfield(L, -2, THREADING_THREADS_KEY);
  LJ_LIB_REG(L, NULL, threading_mutex);
  LJ_LIB_REG(L, NULL, threading_channel);
  LJ_LIB_REG(L, LUA_THREADINGLIBNAME, threading);
  env = threading_env_from_module(L, tabV(L->top-1));
  if (env)
    setgcrefroot(G(L)->gcroot[GCROOT_THREADING_ENV], obj2gco(env));
  return 1;
}
