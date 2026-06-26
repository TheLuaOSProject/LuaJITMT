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
#if LJ_HASJIT
#include "lj_jit.h"
#include "lj_trace.h"
#endif
#include "lj_profile.h"

#include "luajit.h"

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

static LJ_AINLINE global_State *profile_g_load_acq(ProfileState *ps)
{
  return (global_State *)la_loadptr_acq((void *const *)&ps->g);
}

static LJ_AINLINE void profile_g_store_rel(ProfileState *ps, global_State *g)
{
  la_storeptr_rel((void **)&ps->g, g);
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

/* -- Profile callbacks --------------------------------------------------- */

/* Callback from profile hook (HOOK_PROFILE already cleared). */
void LJ_FASTCALL lj_profile_interpreter(lua_State *L)
{
  ProfileState *ps = &profile_state;
  global_State *g = G(L);
  uint8_t saved;
  if (hookmask_profile_enter(g, &saved)) {
    luaJIT_profile_callback cb = profile_cb_load_acq(ps);
    void *data = profile_data_load_acq(ps);
    uint32_t samples = profile_samples_xchg(ps, 0);
    int32_t vmstate = profile_vmstate_load_acq(ps);
    lj_dispatch_update(g, 1);
    if (cb)
      cb(data, L, (int)samples, (int)vmstate);  /* Invoke user callback. */
    hookmask_profile_leave(g, saved);
  }
  lj_dispatch_update(g, 1);
}

/* Trigger profile hook. Asynchronous call from OS-specific profile timer. */
static void profile_trigger(ProfileState *ps)
{
  global_State *g = profile_g_load_acq(ps);
  int st;
  if (!g) return;
  profile_samples_add(ps, 1);  /* Always increment number of samples. */
  st = vmstate_load_acq(g);
  profile_vmstate_store_rel(ps, st >= 0 ? 'N' :
			    st == ~LJ_VMST_INTERP ? 'I' :
			    st == ~LJ_VMST_C ? 'C' :
			    st == ~LJ_VMST_GC ? 'G' : 'J');
  /* Set profile hook. */
  if (hookmask_set_if_clear(g, HOOK_PROFILE|HOOK_VMEVENT|HOOK_GC,
			    HOOK_PROFILE)) {
    lj_dispatch_update(g, 2);  /* Async timer/signal path must not spin. */
  }
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
static void profile_timer_stop(ProfileState *ps)
{
  struct itimerval tm;
  tm.it_value.tv_sec = tm.it_interval.tv_sec = 0;
  tm.it_value.tv_usec = tm.it_interval.tv_usec = 0;
  setitimer(ITIMER_PROF, &tm, NULL);
  sigaction(SIGPROF, &ps->oldsa, NULL);
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
static void profile_timer_stop(ProfileState *ps)
{
  la_store32_rel(&ps->abort, 1);
  pthread_join(ps->thread, NULL);
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
static void profile_timer_stop(ProfileState *ps)
{
  la_store32_rel(&ps->abort, 1);
  WaitForSingleObject(ps->thread, INFINITE);
}

#endif

/* -- Public profiling API ------------------------------------------------ */

/* Start profiling. */
LUA_API void luaJIT_profile_start(lua_State *L, const char *mode,
				  luaJIT_profile_callback cb, void *data)
{
  ProfileState *ps = &profile_state;
  int interval = LJ_PROFILE_INTERVAL_DEFAULT;
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
      L2J(L)->prof_mode = m;
      (void)lj_trace_flushall_hs(L);
      break;
#endif
    default:  /* Ignore unknown mode chars. */
      break;
    }
  }
  if (profile_g_load_acq(ps)) {
    luaJIT_profile_stop(L);
    if (profile_g_load_acq(ps)) return;  /* Profiler in use by another VM. */
  }
  ps->interval = interval;
  profile_cb_store_rel(ps, cb);
  profile_data_store_rel(ps, data);
  (void)profile_samples_xchg(ps, 0);
  profile_vmstate_store_rel(ps, 'N');
  lj_buf_init(L, &ps->sb);
  profile_g_store_rel(ps, G(L));
  profile_timer_start(ps);
}

/* Stop profiling. */
LUA_API void luaJIT_profile_stop(lua_State *L)
{
  ProfileState *ps = &profile_state;
  global_State *g = profile_g_load_acq(ps);
  if (G(L) == g) {  /* Only stop profiler if started by this VM. */
    profile_timer_stop(ps);
    hookmask_update(g, HOOK_PROFILE, 0);
    lj_dispatch_update(g, 0);
#if LJ_HASJIT
    G2J(g)->prof_mode = 0;
    (void)lj_trace_flushall_hs(L);
#endif
    lj_buf_free(g, &ps->sb);
    ps->sb.w = ps->sb.e = NULL;
    profile_cb_store_rel(ps, NULL);
    profile_data_store_rel(ps, NULL);
    (void)profile_samples_xchg(ps, 0);
    profile_vmstate_store_rel(ps, 'N');
    profile_g_store_rel(ps, NULL);
  }
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
