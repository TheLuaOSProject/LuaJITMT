/*
** Low-overhead profiling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_profile_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASPROFILE

#include "lj_err.h"
#include "lj_buf.h"
#include "lj_frame.h"
#include "lj_debug.h"
#include "lj_dispatch.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_safepoint.h"
#if LJ_HASJIT
#include "lj_jit.h"
#include "lj_trace.h"
#endif
#include "lj_profile.h"
#include "lj_vm.h"

#include "luajit.h"

#include <limits.h>

static lua_State *profile_errstate(lua_State *L)
{
  lua_State *cur = lj_tg_cur_L(G(L));
  return cur && G(cur) == G(L) ? cur : L;
}

#if LJ_PROFILE_SIGPROF

#include <sys/time.h>
#include <signal.h>

#elif LJ_PROFILE_PTHREAD

#include <pthread.h>
#include <time.h>
#if LJ_TARGET_PS3
#include <sys/timer.h>
#endif

#elif LJ_PROFILE_WTHREAD

#define WIN32_LEAN_AND_MEAN
#if LJ_TARGET_XBOX360
#include <xtl.h>
#include <xbox.h>
#else
#include <windows.h>
#endif
typedef unsigned int (WINAPI *WMM_TPFUNC)(unsigned int);

#endif

/* Profiler state. */
typedef struct ProfileState {
  global_State *g;		/* VM state that started the profiler. */
  luaJIT_profile_callback cb;	/* Profiler callback. */
  void *data;			/* Profiler callback data. */
  SBuf sb;			/* String buffer for stack dumps. */
  int interval;			/* Sample interval in milliseconds. */
  uint32_t state;		/* Serialized lifecycle state. */
  uint32_t callbacks;		/* Active callbacks using callback data. */
  uint32_t callback_tid;	/* OS thread currently in a callback. */
  uint32_t samples;		/* Number of samples for next callback. */
  int32_t vmstate;		/* VM state when profile timer triggered. */
#if LJ_PROFILE_SIGPROF
  struct sigaction oldsa;	/* Previous SIGPROF state. */
#elif LJ_PROFILE_PTHREAD
  pthread_t thread;		/* Timer thread. */
  uint32_t abort;		/* Abort timer thread. */
#elif LJ_PROFILE_WTHREAD
#if LJ_TARGET_WINDOWS
  HINSTANCE wmm;		/* WinMM library handle. */
  WMM_TPFUNC wmm_tbp;		/* WinMM timeBeginPeriod function. */
  WMM_TPFUNC wmm_tep;		/* WinMM timeEndPeriod function. */
#endif
  HANDLE thread;		/* Timer thread. */
  uint32_t abort;		/* Abort timer thread. */
#endif
} ProfileState;

/* Sadly, we have to use a static profiler state.
**
** The SIGPROF variant needs a static pointer to the global state, anyway.
** And it would be hard to extend for multiple threads. You can still use
** multiple VMs in multiple threads, but only profile one at a time.
*/
static ProfileState profile_state;

/* Default sample interval in milliseconds. */
#define LJ_PROFILE_INTERVAL_DEFAULT	10

#define LJ_PROFILE_IDLE		0u
#define LJ_PROFILE_STARTING	1u
#define LJ_PROFILE_ACTIVE	2u
#define LJ_PROFILE_STOPPING	3u

static LJ_AINLINE uint32_t profile_state_load_acq(ProfileState *ps)
{
  return la_load32_acq(&ps->state);
}

static LJ_AINLINE void profile_state_store_rel(ProfileState *ps, uint32_t state)
{
  la_store32_rel(&ps->state, state);
  la_futex_wake(&ps->state, INT_MAX);
}

static LJ_AINLINE int profile_state_cas(ProfileState *ps, uint32_t *oldp,
					uint32_t state)
{
  return la_cas32(&ps->state, oldp, state, LA_ACQ_REL, LA_ACQ);
}

static void profile_state_wait_l(lua_State *L, ProfileState *ps, uint32_t state)
{
  uint32_t actions = lj_thr_sleep_ns(L, 1000000);
  if (actions)
    lj_safepoint_checkstop(L, actions);
  UNUSED(ps); UNUSED(state);
}

static LJ_AINLINE global_State *profile_g_load_acq(ProfileState *ps)
{
  return (global_State *)la_loadptr_acq((void *const *)&ps->g);
}

static LJ_AINLINE void profile_g_store_rel(ProfileState *ps, global_State *g)
{
  la_storeptr_rel((void **)&ps->g, g);
}

static LJ_AINLINE int profile_state_active_g(ProfileState *ps, global_State *g)
{
  return profile_state_load_acq(ps) == LJ_PROFILE_ACTIVE &&
	 profile_g_load_acq(ps) == g;
}

static void profile_callback_leave(ProfileState *ps);

#if !LJ_PROFILE_TGLOCAL
static int profile_callback_enter(ProfileState *ps, global_State *g)
{
  uint32_t old = 0;
  if (!profile_state_active_g(ps, g))
    return 0;
  if (!la_cas32(&ps->callbacks, &old, 1, LA_ACQ_REL, LA_ACQ))
    return 0;
  la_store32_rel(&ps->callback_tid, lj_thr_current_id(g));
  if (profile_state_active_g(ps, g))
    return 1;
  profile_callback_leave(ps);
  return 0;
}
#endif

#if LJ_PROFILE_TGLOCAL
static int profile_callback_tryenter(ProfileState *ps, global_State *g)
{
  uint32_t old = 0;
  if (!profile_state_active_g(ps, g))
    return 0;
  if (!la_cas32(&ps->callbacks, &old, 1, LA_ACQ_REL, LA_ACQ))
    return 0;
  la_store32_rel(&ps->callback_tid, lj_thr_current_id(g));
  if (profile_state_active_g(ps, g))
    return 1;
  profile_callback_leave(ps);
  return 0;
}
#endif

static void profile_callback_leave(ProfileState *ps)
{
  la_store32_rel(&ps->callback_tid, 0);
  if (la_sub32_acqrel(&ps->callbacks, 1) == 1)
    la_futex_wake(&ps->callbacks, INT_MAX);
}

static uint32_t profile_callbacks_wait(lua_State *L, ProfileState *ps)
{
  uint32_t tid = lj_thr_current_id(G(L));
  uint32_t actions = 0;
  for (;;) {
    uint32_t callbacks = la_load32_acq(&ps->callbacks);
    if (callbacks == 0 || la_load32_acq(&ps->callback_tid) == tid)
      return actions;
    actions |= lj_thr_sleep_ns(L, 1000000);
  }
}

static LJ_AINLINE luaJIT_profile_callback
profile_cb_load_acq(ProfileState *ps)
{
  return la_loadfunc_acq(&ps->cb);
}

static LJ_AINLINE void profile_cb_store_rel(ProfileState *ps,
					    luaJIT_profile_callback cb)
{
  la_storefunc_rel(&ps->cb, cb);
}

static LJ_AINLINE void *profile_data_load_acq(ProfileState *ps)
{
  return la_loadptr_acq((void *const *)&ps->data);
}

static LJ_AINLINE void profile_data_store_rel(ProfileState *ps, void *data)
{
  la_storeptr_rel((void **)&ps->data, data);
}

static LJ_AINLINE uint32_t profile_samples_xchg(ProfileState *ps,
						uint32_t samples)
{
  return la_xchg32_acqrel(&ps->samples, samples);
}

static LJ_AINLINE void profile_samples_add(ProfileState *ps, uint32_t samples)
{
  (void)la_add32_rlx(&ps->samples, samples);
}

static LJ_AINLINE int32_t profile_vmstate_load_acq(ProfileState *ps)
{
  return (int32_t)la_load32_acq((uint32_t *)&ps->vmstate);
}

static LJ_AINLINE void profile_vmstate_store_rel(ProfileState *ps,
						 int32_t vmstate)
{
  la_store32_rel((uint32_t *)&ps->vmstate, (uint32_t)vmstate);
}

static int32_t profile_sample_vmstate_tg(global_State *g, TGState *tg)
{
  int32_t st;
  st = tg ? lj_tg_vmstate_load_acq(tg) : vmstate_load_acq(g);
  return st >= 0 ? 'N' :
	 st == ~LJ_VMST_INTERP ? 'I' :
	 st == ~LJ_VMST_C ? 'C' :
	 st == ~LJ_VMST_GC ? 'G' : 'J';
}

#if !LJ_PROFILE_TGLOCAL
static int32_t profile_sample_vmstate(global_State *g)
{
  return profile_sample_vmstate_tg(g, G2TG(g));
}
#endif

/* -- Profiler/hook interaction ------------------------------------------- */

#if !LJ_PROFILE_SIGPROF
void LJ_FASTCALL lj_profile_hook_enter(global_State *g)
{
  hook_enter(g);
}

void LJ_FASTCALL lj_profile_hook_leave(global_State *g)
{
  hook_leave(g);
}
#endif

/* -- Per-TG profile dispatch for POSIX signal timers --------------------- */

#if LJ_PROFILE_TGLOCAL
static void profile_tg_setins(TGState *tg, ASMFunction f)
{
  uint32_t i;
  for (i = 0; i < BC_FUNCF; i++)
    tg->dispatch[i] = f;
  for (i = BC_CNEW; i <= BC_CSET; i++)
    tg->dispatch[i] = f;
}

static void profile_tg_sethook(TGState *tg)
{
  profile_tg_setins(tg, lj_vm_profhook);
}

static void profile_tg_clearhook(lua_State *L, TGState *tg)
{
  lj_tg_hookmask_update(tg, HOOK_PROFILE, 0);
  lj_dispatch_update(G(L), 1);
}

static void profile_tg_drop_all(global_State *g)
{
  TGState *tg;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    lj_tg_hookmask_update(tg, HOOK_PROFILE, 0);
    (void)lj_tg_profile_samples_xchg(tg, 0);
    lj_tg_profile_vmstate_store_rel(tg, 'N');
  }
}

static int profile_tg_eligible(global_State *g, TGState *tg)
{
  return tg && tg->gl == g && !lj_tg_flags_test_acq(tg, TGF_DEAD);
}
#endif

/* -- Profile callbacks --------------------------------------------------- */

/* Callback from profile hook. */
void LJ_FASTCALL lj_profile_interpreter(lua_State *L)
{
  ProfileState *ps = &profile_state;
  global_State *g = G(L);
#if LJ_PROFILE_TGLOCAL
  TGState *tg = L2TG(L);
  uint32_t samples;
  int32_t vmstate;
  uint8_t saved;
  if (!profile_tg_eligible(g, tg) || !(lj_tg_hookmask_load(tg) & HOOK_PROFILE))
    return;
  if (!profile_state_active_g(ps, g)) {
    profile_tg_clearhook(L, tg);  /* Drop stale profile hooks. */
    (void)lj_tg_profile_samples_xchg(tg, 0);
    lj_tg_profile_vmstate_store_rel(tg, 'N');
    return;
  }
  if (!hookmask_profile_enter(g, &saved)) {
    profile_tg_clearhook(L, tg);
    return;
  }
  profile_tg_clearhook(L, tg);
  samples = lj_tg_profile_samples_xchg(tg, 0);
  vmstate = lj_tg_profile_vmstate_load_acq(tg);
  if (samples != 0) {
    luaJIT_profile_callback cb = profile_cb_load_acq(ps);
    void *data = profile_data_load_acq(ps);
    if (cb && profile_callback_tryenter(ps, g)) {
      cb(data, L, (int)samples, (int)vmstate);  /* Invoke user callback. */
      profile_callback_leave(ps);
    }
  }
  hookmask_profile_leave(g, saved);
#else
  uint8_t saved;
  if (!profile_state_active_g(ps, g)) {
    hookmask_update(g, HOOK_PROFILE, 0);  /* Drop stale profile hooks. */
    lj_dispatch_update(g, 1);
    return;
  }
  if (hookmask_profile_enter(g, &saved)) {
    luaJIT_profile_callback cb = profile_cb_load_acq(ps);
    void *data = profile_data_load_acq(ps);
    uint32_t samples = profile_samples_xchg(ps, 0);
    int32_t vmstate = profile_vmstate_load_acq(ps);
    lj_dispatch_update(g, 1);
    if (cb && profile_callback_enter(ps, g)) {
      cb(data, L, (int)samples, (int)vmstate);  /* Invoke user callback. */
      profile_callback_leave(ps);
    }
    hookmask_profile_leave(g, saved);
  }
  lj_dispatch_update(g, 1);
#endif
}

int lj_profile_pending(lua_State *L)
{
#if LJ_PROFILE_TGLOCAL
  TGState *tg = L2TG(L);
  return tg && (lj_tg_hookmask_load(tg) & HOOK_PROFILE);
#else
  return hookmask_load(G(L)) & HOOK_PROFILE;
#endif
}

/* Trigger profile hook. Asynchronous call from OS-specific profile timer. */
static void profile_trigger(ProfileState *ps)
{
  global_State *g = profile_g_load_acq(ps);
  int st;
  if (!g || !profile_state_active_g(ps, g)) return;
#if LJ_PROFILE_TGLOCAL
  {
    TGState *tg = lj_thr_get_tg();
    if (!profile_tg_eligible(g, tg))
      return;
    lj_tg_profile_samples_add(tg, 1);
    st = profile_sample_vmstate_tg(g, tg);
    lj_tg_profile_vmstate_store_rel(tg, st);
    if (hookmask_load(g) & (HOOK_VMEVENT|HOOK_GC))
      return;
    if (lj_tg_hookmask_set_if_clear(tg, HOOK_PROFILE, HOOK_PROFILE))
      profile_tg_sethook(tg);
  }
#else
  if (gc2_n_threads_acq(g) > 1) return;  /* POSIX timer lacks per-TG routing. */
  profile_samples_add(ps, 1);  /* Always increment number of samples. */
  st = profile_sample_vmstate(g);
  profile_vmstate_store_rel(ps, st);
  /* Set profile hook. */
  if (hookmask_set_if_clear(g, HOOK_PROFILE|HOOK_VMEVENT|HOOK_GC,
			    HOOK_PROFILE)) {
    lj_dispatch_update(g, 2);  /* Async timer/signal path must not spin. */
  }
#endif
}

/* -- OS-specific profile timer handling ---------------------------------- */

#if LJ_PROFILE_SIGPROF

/* SIGPROF handler. */
static void profile_signal(int sig)
{
  UNUSED(sig);
  profile_trigger(&profile_state);
}

/* Start profiling timer. */
static void profile_timer_start(ProfileState *ps)
{
  int interval = ps->interval;
  struct itimerval tm;
  struct sigaction sa;
  tm.it_value.tv_sec = tm.it_interval.tv_sec = interval / 1000;
  tm.it_value.tv_usec = tm.it_interval.tv_usec = (interval % 1000) * 1000;
  setitimer(ITIMER_PROF, &tm, NULL);
#if LJ_TARGET_QNX
  sa.sa_flags = 0;
#else
  sa.sa_flags = SA_RESTART;
#endif
  sa.sa_handler = profile_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, &ps->oldsa);
}

/* Stop profiling timer. */
static uint32_t profile_timer_stop(ProfileState *ps, lua_State *L)
{
  UNUSED(L);
  struct itimerval tm;
  tm.it_value.tv_sec = tm.it_interval.tv_sec = 0;
  tm.it_value.tv_usec = tm.it_interval.tv_usec = 0;
  setitimer(ITIMER_PROF, &tm, NULL);
  sigaction(SIGPROF, &ps->oldsa, NULL);
  return 0;
}

#elif LJ_PROFILE_PTHREAD

/* POSIX timer thread. */
static void *profile_thread(ProfileState *ps)
{
  int interval = ps->interval;
#if !LJ_TARGET_PS3
  struct timespec ts;
  ts.tv_sec = interval / 1000;
  ts.tv_nsec = (interval % 1000) * 1000000;
#endif
  while (1) {
#if LJ_TARGET_PS3
    sys_timer_usleep(interval * 1000);
#else
    nanosleep(&ts, NULL);
#endif
    if (la_load32_acq(&ps->abort)) break;
    profile_trigger(ps);
  }
  return NULL;
}

/* Start profiling timer thread. */
static void profile_timer_start(ProfileState *ps)
{
  la_store32_rel(&ps->abort, 0);
  pthread_create(&ps->thread, NULL, (void *(*)(void *))profile_thread, ps);
}

/* Stop profiling timer thread. */
static uint32_t profile_timer_stop(ProfileState *ps, lua_State *L)
{
  uint32_t actions;
  la_store32_rel(&ps->abort, 1);
  lj_native_enter(L2TG(L));
  pthread_join(ps->thread, NULL);
  actions = lj_native_leave(L);
  return actions;
}

#elif LJ_PROFILE_WTHREAD

/* Windows timer thread. */
static DWORD WINAPI profile_thread(void *psx)
{
  ProfileState *ps = (ProfileState *)psx;
  int interval = ps->interval;
#if LJ_TARGET_WINDOWS && !LJ_TARGET_UWP
  ps->wmm_tbp(interval);
#endif
  while (1) {
    Sleep(interval);
    if (la_load32_acq(&ps->abort)) break;
    profile_trigger(ps);
  }
#if LJ_TARGET_WINDOWS && !LJ_TARGET_UWP
  ps->wmm_tep(interval);
#endif
  return 0;
}

/* Start profiling timer thread. */
static void profile_timer_start(ProfileState *ps)
{
#if LJ_TARGET_WINDOWS && !LJ_TARGET_UWP
  if (!ps->wmm) {  /* Load WinMM library on-demand. */
    ps->wmm = LJ_WIN_LOADLIBA("winmm.dll");
    if (ps->wmm) {
      ps->wmm_tbp = (WMM_TPFUNC)GetProcAddress(ps->wmm, "timeBeginPeriod");
      ps->wmm_tep = (WMM_TPFUNC)GetProcAddress(ps->wmm, "timeEndPeriod");
      if (!ps->wmm_tbp || !ps->wmm_tep) {
	ps->wmm = NULL;
	return;
      }
    }
  }
#endif
  la_store32_rel(&ps->abort, 0);
  ps->thread = CreateThread(NULL, 0, profile_thread, ps, 0, NULL);
}

/* Stop profiling timer thread. */
static uint32_t profile_timer_stop(ProfileState *ps, lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&ps->abort, 1);
  WaitForSingleObject(ps->thread, INFINITE);
  return 0;
}

#endif

/* -- Public profiling API ------------------------------------------------ */

/* Start profiling. */
LUA_API void luaJIT_profile_start(lua_State *L, const char *mode,
				  luaJIT_profile_callback cb, void *data)
{
  ProfileState *ps = &profile_state;
  global_State *g = G(L);
  int interval = LJ_PROFILE_INTERVAL_DEFAULT;
#if LJ_HASJIT
  int prof_mode = 0;
#endif
  while (*mode) {
    int m = *mode++;
    switch (m) {
    case 'i':
      interval = 0;
      while (*mode >= '0' && *mode <= '9')
	interval = interval * 10 + (*mode++ - '0');
      if (interval <= 0) interval = 1;
      break;
#if LJ_HASJIT
    case 'l': case 'f':
      prof_mode = m;
      break;
#endif
    default:  /* Ignore unknown mode chars. */
      break;
    }
  }
  for (;;) {
    uint32_t state = profile_state_load_acq(ps);
    global_State *owner = profile_g_load_acq(ps);
    if (state == LJ_PROFILE_IDLE) {
      uint32_t expect = LJ_PROFILE_IDLE;
      if (profile_state_cas(ps, &expect, LJ_PROFILE_STARTING))
	break;
      continue;
    }
    if (state == LJ_PROFILE_ACTIVE) {
      if (owner != g)
	return;  /* Profiler in use by another VM. */
      luaJIT_profile_stop(L);
      continue;
    }
    profile_state_wait_l(L, ps, state);
  }
#if LJ_HASJIT
  if (prof_mode) {
    L2J(L)->prof_mode = prof_mode;
    (void)lj_trace_flushall_hs(L);
  }
#endif
  ps->interval = interval;
  profile_cb_store_rel(ps, cb);
  profile_data_store_rel(ps, data);
  (void)profile_samples_xchg(ps, 0);
  profile_vmstate_store_rel(ps, 'N');
#if LJ_PROFILE_TGLOCAL
  profile_tg_drop_all(g);
#endif
  lj_buf_init(L, &ps->sb);
  profile_g_store_rel(ps, g);
  profile_timer_start(ps);
  profile_state_store_rel(ps, LJ_PROFILE_ACTIVE);
}

int lj_profile_active(lua_State *L)
{
  return profile_state_active_g(&profile_state, G(L));
}

/* Stop profiling and return pending safepoint actions. */
uint32_t lj_profile_stop_hs(lua_State *L)
{
  ProfileState *ps = &profile_state;
  global_State *g = G(L);
  uint32_t actions = 0;
  for (;;) {
    uint32_t state = profile_state_load_acq(ps);
    global_State *owner = profile_g_load_acq(ps);
    if (state == LJ_PROFILE_IDLE)
      return 0;
    if (state == LJ_PROFILE_ACTIVE) {
      uint32_t expect = LJ_PROFILE_ACTIVE;
      if (owner != g)
	return 0;  /* Only stop profiler if started by this VM. */
      if (profile_state_cas(ps, &expect, LJ_PROFILE_STOPPING))
	break;
      continue;
    }
    profile_state_wait_l(L, ps, state);
  }
  actions = profile_timer_stop(ps, L);
#if LJ_PROFILE_TGLOCAL
  profile_tg_drop_all(g);
#endif
  hookmask_update(g, HOOK_PROFILE, 0);
  lj_dispatch_update(g, 0);
  actions |= profile_callbacks_wait(L, ps);
#if LJ_HASJIT
  if (G2J(g)->prof_mode != 0) {
    G2J(g)->prof_mode = 0;
    (void)lj_trace_flushall_hs(L);
  }
#endif
  lj_buf_free(g, &ps->sb);
  ps->sb.b = ps->sb.w = ps->sb.e = NULL;
  profile_cb_store_rel(ps, NULL);
  profile_data_store_rel(ps, NULL);
  (void)profile_samples_xchg(ps, 0);
  profile_vmstate_store_rel(ps, 'N');
  profile_g_store_rel(ps, NULL);
  profile_state_store_rel(ps, LJ_PROFILE_IDLE);
  return actions;
}

static int profile_had_stopreq(lua_State *L)
{
  TGState *tg = L2TG(L);
  return tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ);
}

static int profile_fresh_stopreq(lua_State *L, uint32_t actions,
				 int had_stopreq)
{
  return lj_safepoint_fresh_stopreq(L, actions, had_stopreq);
}

static void profile_checkstop_fresh(lua_State *L, uint32_t actions,
				    int had_stopreq)
{
  if (profile_fresh_stopreq(L, actions, had_stopreq))
    lj_safepoint_checkstop(L, actions);
}

/* Stop profiling. */
LUA_API void luaJIT_profile_stop(lua_State *L)
{
  int had_stopreq = profile_had_stopreq(L);
  uint32_t actions = lj_profile_stop_hs(L);
  profile_checkstop_fresh(L, actions, had_stopreq);
}

/* Return a compact stack dump. */
LUA_API const char *luaJIT_profile_dumpstack(lua_State *L, const char *fmt,
					     int depth, size_t *len)
{
  LJStateClaim claim;
  ProfileState *ps = &profile_state;
  SBuf *sb = &ps->sb;
  if (!lj_state_tryclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(profile_errstate(L), "thread busy");
  setsbufL(sb, L);
  lj_buf_reset(sb);
  lj_debug_dumpstack(L, sb, fmt, depth);
  *len = (size_t)sbuflen(sb);
  lj_state_dropclaim(&claim);
  return sb->b;
}

#endif
