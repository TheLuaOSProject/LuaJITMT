/*
** Low-overhead profiling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

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

#include <errno.h>
#include <dlfcn.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#if LJ_TARGET_OSX
#include <mach-o/dyld.h>
#elif LJ_TARGET_LINUX
#include <link.h>
#endif

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
  global_State *poll_g;		/* VM whose JIT loops require owner polls. */
  luaJIT_profile_callback cb;	/* Profiler callback. */
  void *data;			/* Profiler callback data. */
  SBuf sb;			/* String buffer for stack dumps. */
  int interval;			/* Sample interval in milliseconds. */
  uint32_t state;		/* Serialized lifecycle state. */
  uint32_t callbacks;		/* Active callbacks using callback data. */
  uint32_t callback_tid;	/* OS thread currently in a callback. */
  TGState *callback_tg;		/* Exact TG currently invoking callback. */
  uint32_t samples;		/* Number of samples for next callback. */
  int32_t vmstate;		/* VM state when profile timer triggered. */
#if LJ_PROFILE_SIGPROF
  struct sigaction oldsa;	/* Previous SIGPROF state. */
  uint32_t signal_handler_installed;  /* Our handler still owns SIGPROF. */
  uint32_t signal_handlers;  /* In-flight handlers that may have seen ACTIVE. */
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

static uint32_t profile_futex_wait_l(lua_State *L, uint32_t *addr,
				     uint32_t expect)
{
  uint32_t actions;
  lj_native_enter(L2TG(L));
  (void)la_futex_wait(addr, expect, 1000000);
  actions = lj_native_leave(L);
  return actions;
}

static void profile_state_wait_l(lua_State *L, ProfileState *ps, uint32_t state)
{
  uint32_t actions = profile_futex_wait_l(L, &ps->state, state);
  if (actions)
    lj_safepoint_checkstop(L, actions);
}

static LJ_AINLINE global_State *profile_g_load_acq(ProfileState *ps)
{
  return (global_State *)la_loadptr_acq((void *const *)&ps->g);
}

static LJ_AINLINE void profile_g_store_rel(ProfileState *ps, global_State *g)
{
  la_storeptr_rel((void **)&ps->g, g);
}

static LJ_AINLINE global_State *profile_poll_g_load_acq(ProfileState *ps)
{
  return (global_State *)la_loadptr_acq((void *const *)&ps->poll_g);
}

#if LJ_HASJIT && LJ_THR_TG_SIGNAL_CACHE
static LJ_AINLINE void profile_poll_g_store_rel(ProfileState *ps,
                                                 global_State *g)
{
  la_storeptr_rel((void **)&ps->poll_g, g);
}
#endif

int lj_profile_poll_required(global_State *g)
{
  return g != NULL && profile_poll_g_load_acq(&profile_state) == g;
}

static LJ_AINLINE int profile_state_active_g(ProfileState *ps, global_State *g)
{
  return profile_state_load_acq(ps) == LJ_PROFILE_ACTIVE &&
	 profile_g_load_acq(ps) == g;
}

static void profile_callback_leave(ProfileState *ps);

#if !LJ_PROFILE_TGLOCAL
static int profile_callback_enter(ProfileState *ps, global_State *g,
				  TGState *tg)
{
  uint32_t old = 0;
  if (!profile_state_active_g(ps, g))
    return 0;
  if (!la_cas32(&ps->callbacks, &old, 1, LA_ACQ_REL, LA_ACQ))
    return 0;
  la_store32_rel(&ps->callback_tid, lj_thr_current_id(g));
  la_storeptr_rel((void **)&ps->callback_tg, tg);
  if (profile_state_active_g(ps, g))
    return 1;
  profile_callback_leave(ps);
  return 0;
}
#endif

#if LJ_PROFILE_TGLOCAL
static int profile_callback_tryenter(ProfileState *ps, global_State *g,
				     TGState *tg)
{
  uint32_t old = 0;
  if (!profile_state_active_g(ps, g))
    return 0;
  if (!la_cas32(&ps->callbacks, &old, 1, LA_ACQ_REL, LA_ACQ))
    return 0;
  la_store32_rel(&ps->callback_tid, lj_thr_current_id(g));
  la_storeptr_rel((void **)&ps->callback_tg, tg);
  if (profile_state_active_g(ps, g))
    return 1;
  profile_callback_leave(ps);
  return 0;
}
#endif

static void profile_callback_leave(ProfileState *ps)
{
  la_storeptr_rel((void **)&ps->callback_tg, NULL);
  la_store32_rel(&ps->callback_tid, 0);
  if (la_sub32_acqrel(&ps->callbacks, 1) == 1)
    la_futex_wake(&ps->callbacks, INT_MAX);
}

int lj_profile_callback_active_tg(TGState *tg)
{
  return tg != NULL &&
    (TGState *)la_loadptr_acq((void *const *)&profile_state.callback_tg) == tg;
}

static uint32_t profile_callbacks_wait(lua_State *L, ProfileState *ps)
{
  uint32_t tid = lj_thr_current_id(G(L));
  uint32_t actions = 0;
  for (;;) {
    uint32_t callbacks = la_load32_acq(&ps->callbacks);
    if (callbacks == 0 || la_load32_acq(&ps->callback_tid) == tid)
      return actions;
    actions |= profile_futex_wait_l(L, &ps->callbacks, callbacks);
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

#if !LJ_PROFILE_TGLOCAL
static LJ_AINLINE void profile_samples_add(ProfileState *ps, uint32_t samples)
{
  (void)la_add32_rlx(&ps->samples, samples);
}

static LJ_AINLINE int32_t profile_vmstate_load_acq(ProfileState *ps)
{
  return (int32_t)la_load32_acq((uint32_t *)&ps->vmstate);
}
#endif

static LJ_AINLINE void profile_vmstate_store_rel(ProfileState *ps,
						 int32_t vmstate)
{
  la_store32_rel((uint32_t *)&ps->vmstate, (uint32_t)vmstate);
}

static LJ_AINLINE int32_t profile_sample_vmstate_tg(global_State *g,
					    TGState *tg)
{
  int32_t st;
  st = tg ? lj_tg_vmstate_load_acq(tg) : vmstate_load_acq(g);
  return st >= 0 ? 'N' :
	 st == ~LJ_VMST_INTERP ? 'I' :
	 st == ~LJ_VMST_C ? 'C' :
	 st == ~LJ_VMST_GC ? 'G' : 'J';
}

#if LJ_HASJIT && LJ_THR_TG_SIGNAL_CACHE
#if LJ_PROFILE_SIGPROF && defined(LJ_PROFILE_TIMER_TEST_HELPERS)
static uint32_t profile_test_fail_trace_flush_after;
static int profile_test_fail_now(uint32_t *count);
#endif

typedef struct ProfileTraceFlushCtx {
  int status;
} ProfileTraceFlushCtx;

static TValue *profile_trace_flush_cp(lua_State *L, lua_CFunction dummy,
                                      void *ud)
{
  ProfileTraceFlushCtx *ctx = (ProfileTraceFlushCtx *)ud;
  UNUSED(dummy);
#if LJ_PROFILE_SIGPROF && defined(LJ_PROFILE_TIMER_TEST_HELPERS)
  if (profile_test_fail_now(&profile_test_fail_trace_flush_after))
    lj_err_callermsg(L, "injected profile trace flush failure");
#endif
  ctx->status = lj_trace_flushall_hs_noevent(L);
  return NULL;
}

/* Retirement can throw through allocation/shutdown edges. Catch it so profile
** lifecycle state can be rolled back/finished before preserving the error. */
static int profile_trace_flush_catch(lua_State *L, int *status)
{
  ProfileTraceFlushCtx ctx;
  int errcode;
  ctx.status = 1;
  errcode = lj_vm_cpcall(L, NULL, &ctx, profile_trace_flush_cp);
  *status = ctx.status;
  return errcode;
}
#endif

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
  hook_call_enter(g);
}

void LJ_FASTCALL lj_profile_hook_leave(global_State *g)
{
  hook_call_leave(g);
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
    lj_tg_profile_request_rel(tg, 0);
    lj_tg_hookmask_update(tg, HOOK_PROFILE, 0);
    (void)lj_tg_profile_samples_xchg(tg, 0);
    lj_tg_profile_vmstate_store_rel(tg, 'N');
  }
}

static LJ_AINLINE int profile_tg_eligible(global_State *g, TGState *tg)
{
  return tg && tg->gl == g && !lj_tg_flags_test_acq(tg, TGF_DEAD);
}
#endif

/* Consume a TG-local signal publication in ordinary owner context. The VM's
** architecture-specific poll/request check routes here before resuming
** dispatch. Unsupported signal-cache targets compile this as a no-op;
** SIGPROF never writes hook masks or function pointers. */
void LJ_FASTCALL lj_profile_owner_poll(lua_State *L)
{
#if LJ_PROFILE_TGLOCAL
  ProfileState *ps = &profile_state;
  TGState *tg = L ? L2TG(L) : NULL;
  global_State *g = L ? G(L) : NULL;
  if (!tg || !lj_tg_profile_request_acq(tg))
    return;
  if (!lj_tg_profile_request_xchg_acqrel(tg, 0))
    return;
  if (!profile_tg_eligible(g, tg) || !profile_state_active_g(ps, g))
    return;
  if (!(hookmask_load(g) & (HOOK_VMEVENT|HOOK_GC)) &&
      lj_tg_hookmask_set_if_clear(tg,
	HOOK_PROFILE|HOOK_ACTIVE|HOOK_VMEVENT, HOOK_PROFILE))
    profile_tg_sethook(tg);
  /* A concurrent stop either clears after this publication or is observed by
  ** this second check, in which case the owner removes its own stale overlay. */
  if (!profile_state_active_g(ps, g))
    profile_tg_clearhook(L, tg);
#else
  UNUSED(L);
#endif
}

/* -- Profile callbacks --------------------------------------------------- */

/* Callback from profile hook. */
void LJ_FASTCALL lj_profile_interpreter(lua_State *L)
{
  ProfileState *ps = &profile_state;
  global_State *g = G(L);
#if LJ_PROFILE_TGLOCAL
  TGState *tg = L2TG(L);
  luaJIT_profile_callback cb = NULL;
  void *data = NULL;
  uint32_t samples;
  int32_t vmstate;
  int entered = 0;
  uint8_t saved;
  if (tg)
    (void)lj_tg_profile_request_xchg_acqrel(tg, 0);
  if (!profile_tg_eligible(g, tg) || !(lj_tg_hookmask_load(tg) & HOOK_PROFILE))
    return;
  /* A profile overlay which raced a local VM-event claim is consumed without
  ** entering another user callback. Keep accumulated samples for a later
  ** timer tick; clearhook removes only PROFILE and preserves the callback's
  ** owner-local ACTIVE|VMEVENT exclusion. */
  if (lj_tg_hookmask_load(tg) & HOOK_VMEVENT) {
    profile_tg_clearhook(L, tg);
    return;
  }
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
  samples = lj_tg_profile_samples_xchg(tg, 0);
  vmstate = lj_tg_profile_vmstate_load_acq(tg);
  if (samples != 0) {
    cb = profile_cb_load_acq(ps);
    data = profile_data_load_acq(ps);
    if (cb)
      entered = profile_callback_tryenter(ps, g, tg);
  }
  /* Publish the exact callback TG before removing PROFILE. A bounded JIT
  ** event claim therefore observes one exclusion marker or the other. */
  profile_tg_clearhook(L, tg);
  if (entered) {
    cb(data, L, (int)samples, (int)vmstate);  /* Invoke user callback. */
    profile_callback_leave(ps);
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
    if (cb && profile_callback_enter(ps, g, L2TG(L))) {
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

/* Trigger profile hook on legacy timer backends. The x86-64 SIGPROF path uses
** the atomic-only publisher below instead. */
#if !LJ_THR_TG_SIGNAL_CACHE
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
    if ((hookmask_load(g) & (HOOK_VMEVENT|HOOK_GC)) ||
	(lj_tg_hookmask_load(tg) & (HOOK_ACTIVE|HOOK_VMEVENT)))
      return;
    if (lj_tg_hookmask_set_if_clear(tg,
	  HOOK_PROFILE|HOOK_ACTIVE|HOOK_VMEVENT, HOOK_PROFILE))
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
#endif

/* -- OS-specific profile timer handling ---------------------------------- */

#if LJ_PROFILE_SIGPROF

#define PROFILE_COLD_EMPTY 0u
#define PROFILE_COLD_BUILDING 1u
#define PROFILE_COLD_READY 2u

static uint32_t profile_atfork_state;
static uintptr_t profile_process_cached;
static uint64_t profile_generation_cached;
static uint32_t profile_image_pin_state;
static void *profile_image_pin_handle;
#if LJ_TARGET_OSX && defined(LJ_PROFILE_TIMER_TEST_HELPERS)
/* Mach-O rejects ELF-style alias attributes. In private helper builds, give
** the real handler its exported artifact name so final-image disassembly still
** proves the complete kernel-reachable body rather than a wrapper thunk. */
#define profile_signal luaJIT_profile_timer_test_signal_entry
#define PROFILE_SIGNAL_SCOPE LUA_API
#else
#define PROFILE_SIGNAL_SCOPE static
#endif
PROFILE_SIGNAL_SCOPE void profile_signal(int sig);

#if defined(LJ_PROFILE_TIMER_TEST_HELPERS)
static uint32_t profile_test_fail_sigaction_after;
static uint32_t profile_test_fail_setitimer_after;
static uint32_t profile_test_fail_image_pin_after;
static uint32_t profile_test_fail_image_match_after;
static uint32_t profile_test_sigaction_count;
static uint32_t profile_test_setitimer_count;
static uint32_t profile_test_signal_pause;
static uint32_t profile_test_signal_entered_count;
static uint32_t profile_test_signal_drain_wait_count;
static uint32_t profile_test_before_arm_pause;
static uint32_t profile_test_before_arm_entered_count;

static int profile_test_fail_now(uint32_t *count)
{
  uint32_t current = la_load32_acq(count);
  while (current != 0) {
    uint32_t next = current - 1u;
    if (la_cas32(count, &current, next, LA_ACQ_REL, LA_ACQ))
      return next == 0;
  }
  return 0;
}

void lj_profile_timer_test_reset(void)
{
  la_store32_rel(&profile_test_fail_sigaction_after, 0);
  la_store32_rel(&profile_test_fail_setitimer_after, 0);
  la_store32_rel(&profile_test_fail_image_pin_after, 0);
  la_store32_rel(&profile_test_fail_image_match_after, 0);
  la_store32_rel(&profile_test_fail_trace_flush_after, 0);
  la_store32_rel(&profile_test_sigaction_count, 0);
  la_store32_rel(&profile_test_setitimer_count, 0);
  la_store32_rel(&profile_test_signal_pause, 0);
  la_store32_rel(&profile_test_signal_entered_count, 0);
  la_store32_rel(&profile_test_signal_drain_wait_count, 0);
  la_store32_rel(&profile_test_before_arm_pause, 0);
  la_store32_rel(&profile_test_before_arm_entered_count, 0);
}

void lj_profile_timer_test_fail_trace_flush(uint32_t nth)
{
  la_store32_rel(&profile_test_fail_trace_flush_after, nth);
}

void lj_profile_timer_test_fail_sigaction(uint32_t nth)
{
  la_store32_rel(&profile_test_fail_sigaction_after, nth);
}

void lj_profile_timer_test_fail_setitimer(uint32_t nth)
{
  la_store32_rel(&profile_test_fail_setitimer_after, nth);
}

void lj_profile_timer_test_fail_image_pin(uint32_t nth)
{
  la_store32_rel(&profile_test_fail_image_pin_after, nth);
}

void lj_profile_timer_test_fail_image_match(uint32_t nth)
{
  la_store32_rel(&profile_test_fail_image_match_after, nth);
}

uint32_t lj_profile_timer_test_sigaction_calls(void)
{
  return la_load32_acq(&profile_test_sigaction_count);
}

uint32_t lj_profile_timer_test_setitimer_calls(void)
{
  return la_load32_acq(&profile_test_setitimer_count);
}

uint32_t lj_profile_timer_test_handler_installed(void)
{
  return la_load32_acq(&profile_state.signal_handler_installed);
}

uint32_t lj_profile_timer_test_image_pinned(void)
{
  return la_load32_acq(&profile_image_pin_state) == PROFILE_COLD_READY;
}

uint32_t lj_profile_timer_test_signal_handlers(void)
{
  return la_load32_acq(&profile_state.signal_handlers);
}

void lj_profile_timer_test_force_signal_handlers(uint32_t handlers)
{
  la_store32_rel(&profile_state.signal_handlers, handlers);
}

void lj_profile_timer_test_force_atfork_building(void)
{
  la_store32_rel(&profile_atfork_state, PROFILE_COLD_BUILDING);
}

void lj_profile_timer_test_force_process(uintptr_t process)
{
  la_storeuptr_rel(&profile_process_cached, process);
}

void lj_profile_timer_test_pause_signal(uint32_t pause)
{
  la_store32_rel(&profile_test_signal_pause, pause != 0);
}

uint32_t lj_profile_timer_test_signal_entered(void)
{
  return la_load32_acq(&profile_test_signal_entered_count);
}

uint32_t lj_profile_timer_test_drain_waits(void)
{
  return la_load32_acq(&profile_test_signal_drain_wait_count);
}

void lj_profile_timer_test_pause_before_arm(uint32_t pause)
{
  la_store32_rel(&profile_test_before_arm_pause, pause != 0);
}

uint32_t lj_profile_timer_test_before_arm_entered(void)
{
  return la_load32_acq(&profile_test_before_arm_entered_count);
}

/* Exported only in private helper builds so a loader which is not linked to
** LuaJIT can verify the containing-DSO lifetime contract. The luaJIT prefix is
** intentionally covered by the normal shared-library export map. */
LUA_API void luaJIT_profile_timer_test_reset(void)
{
  lj_profile_timer_test_reset();
}

LUA_API void luaJIT_profile_timer_test_fail_image_pin(uint32_t nth)
{
  lj_profile_timer_test_fail_image_pin(nth);
}

LUA_API void luaJIT_profile_timer_test_fail_image_match(uint32_t nth)
{
  lj_profile_timer_test_fail_image_match(nth);
}

LUA_API void luaJIT_profile_timer_test_fail_setitimer(uint32_t nth)
{
  lj_profile_timer_test_fail_setitimer(nth);
}

LUA_API uint32_t luaJIT_profile_timer_test_image_pinned(void)
{
  return lj_profile_timer_test_image_pinned();
}

LUA_API uint32_t luaJIT_profile_timer_test_handler_installed(void)
{
  return lj_profile_timer_test_handler_installed();
}

LUA_API uint32_t luaJIT_profile_timer_test_active(lua_State *L)
{
  return profile_state_active_g(&profile_state, G(L));
}
#endif

static void profile_atfork_child(void)
{
  /* A handler which existed in a vanished parent thread cannot be in flight in
  ** the single-threaded child. Do not inherit its count or a vanished cold
  ** builder. The installed disposition and a completed permanent image pin
  ** are inherited and remain valid. */
  la_store32_rel(&profile_state.signal_handlers, 0);
  if (la_load32_acq(&profile_atfork_state) == PROFILE_COLD_BUILDING)
    la_store32_rel(&profile_atfork_state, PROFILE_COLD_EMPTY);
  if (la_load32_acq(&profile_image_pin_state) == PROFILE_COLD_BUILDING)
    la_store32_rel(&profile_image_pin_state, PROFILE_COLD_EMPTY);
#if defined(LJ_PROFILE_TIMER_TEST_HELPERS)
  la_store32_rel(&profile_test_signal_pause, 0);
#endif
  la_store64_rel(&profile_generation_cached, 0);
  la_storeuptr_rel(&profile_process_cached, (uintptr_t)getpid());
}

static int profile_process_repair(void)
{
  int saved_errno = errno;
  uint64_t generation = 0;
  uint32_t advanced = 0;
  int usable = lj_thr_tg_signal_process_snapshot(&generation, &advanced);
  uintptr_t process = (uintptr_t)getpid();
  int reset = advanced != 0;
  if (process == 0 || generation == 0) {
    errno = saved_errno;
    return 0;
  }
  for (;;) {
    uint64_t cached = la_load64_acq(&profile_generation_cached);
    uint64_t expected = cached;
    if (cached == generation)
      break;
    if (la_cas64(&profile_generation_cached, &expected, generation,
                 LA_ACQ_REL, LA_ACQ)) {
      if (cached != 0)
        reset = 1;
      break;
    }
  }
  for (;;) {
    uintptr_t cached = la_loaduptr_acq(&profile_process_cached);
    uintptr_t expected = cached;
    if (cached == process)
      break;
    if (la_casuptr(&profile_process_cached, &expected, process,
                   LA_ACQ_REL, LA_ACQ)) {
      if (cached != 0)
        reset = 1;
      break;
    }
  }
  if (reset) {
    profile_atfork_child();  /* Cold repair for raw/missed fork. */
    la_store64_rel(&profile_generation_cached, generation);
    la_storeuptr_rel(&profile_process_cached, process);
  }
  errno = saved_errno;
  return usable;
}

static int profile_atfork_ensure(void)
{
  if (!profile_process_repair())
    return 0;
  for (;;) {
    uint32_t state = la_load32_acq(&profile_atfork_state);
    if (state == PROFILE_COLD_READY)
      return 1;
    if (state == PROFILE_COLD_EMPTY) {
      uint32_t expected = PROFILE_COLD_EMPTY;
      if (la_cas32(&profile_atfork_state, &expected, PROFILE_COLD_BUILDING,
                   LA_ACQ_REL, LA_ACQ)) {
        int rc = pthread_atfork(NULL, NULL, profile_atfork_child);
        la_store32_rel(&profile_atfork_state,
                       rc == 0 ? PROFILE_COLD_READY : PROFILE_COLD_EMPTY);
        return rc == 0;
      }
      continue;
    }
    (void)sched_yield();
    if (!profile_process_repair())
      return 0;
  }
}

#if LJ_TARGET_LINUX
typedef struct ProfileMainImageProbe {
  uintptr_t address;
  int found;
} ProfileMainImageProbe;

static int profile_main_image_probe(struct dl_phdr_info *info, size_t size,
                                    void *data)
{
  ProfileMainImageProbe *probe = (ProfileMainImageProbe *)data;
  ElfW(Half) i;
  UNUSED(size);
  if (info->dlpi_name && info->dlpi_name[0] != '\0')
    return 0;
  for (i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) *phdr = &info->dlpi_phdr[i];
    uintptr_t lo;
    if (phdr->p_type != PT_LOAD)
      continue;
    lo = (uintptr_t)info->dlpi_addr + (uintptr_t)phdr->p_vaddr;
    if (probe->address >= lo &&
        probe->address - lo < (uintptr_t)phdr->p_memsz) {
      probe->found = 1;
      return 1;
    }
  }
  return 0;
}
#endif

static int profile_image_is_main(const Dl_info *info, uintptr_t address)
{
#if LJ_TARGET_OSX
  UNUSED(address);
  return info->dli_fbase == (const void *)_dyld_get_image_header(0);
#elif LJ_TARGET_LINUX
  ProfileMainImageProbe probe;
  UNUSED(info);
  probe.address = address;
  probe.found = 0;
  (void)dl_iterate_phdr(profile_main_image_probe, &probe);
  return probe.found;
#else
  UNUSED(info);
  UNUSED(address);
  return 0;
#endif
}

static int profile_image_pin_matches(void *handle, const Dl_info *expected)
{
#if LJ_TARGET_LINUX && defined(__GLIBC__)
  struct link_map *map = NULL;
  return dlinfo(handle, RTLD_DI_LINKMAP, &map) == 0 && map != NULL &&
         (uintptr_t)map->l_addr == (uintptr_t)expected->dli_fbase;
#else
  /* dlsym(handle, ...) begins with the named image. luaJIT_profile_start is a
  ** public symbol in every loadable image containing this implementation, so
  ** its dladdr base proves that dlopen retained this image, not a replacement
  ** which appeared at the same pathname. */
  Dl_info actual;
  void *symbol = dlsym(handle, "luaJIT_profile_start");
  memset(&actual, 0, sizeof(actual));
  return symbol != NULL && dladdr(symbol, &actual) != 0 &&
         actual.dli_fbase == expected->dli_fbase;
#endif
}

/* Permanently retain the image containing profile_signal before installing
** the first kernel-visible handler. Even a successful restore cannot prove
** that no already-selected kernel entry will reach the old address, so the
** reference is intentionally never released. Main executables cannot be
** dlclosed and need no extra loader reference. */
static int profile_image_pin(void)
{
  for (;;) {
    uint32_t state = la_load32_acq(&profile_image_pin_state);
    if (state == PROFILE_COLD_READY)
      return 1;
    if (state == PROFILE_COLD_EMPTY) {
      uint32_t expected = PROFILE_COLD_EMPTY;
      if (la_cas32(&profile_image_pin_state, &expected,
                   PROFILE_COLD_BUILDING, LA_ACQ_REL, LA_ACQ)) {
        Dl_info info;
        void *handle = NULL;
        int ok = 0;
#if defined(LJ_PROFILE_TIMER_TEST_HELPERS)
        if (profile_test_fail_now(&profile_test_fail_image_pin_after))
          goto pin_done;
#endif
        memset(&info, 0, sizeof(info));
#if LJ_TARGET_LINUX
        /* A static or deleted main executable need not have a usable dynamic
        ** link-map name (and dladdr may fail outright). Its program headers
        ** are nevertheless enough to prove that this address cannot be
        ** dlclosed, so perform that proof before consulting the loader. */
        if (profile_image_is_main(&info,
                                  (uintptr_t)(const void *)&profile_signal))
          ok = 1;
#endif
        if (!ok &&
            dladdr((const void *)(uintptr_t)&profile_signal, &info) != 0 &&
            info.dli_fname != NULL) {
#if LJ_TARGET_OSX
          if (profile_image_is_main(&info,
                                    (uintptr_t)(const void *)&profile_signal))
            ok = 1;
          else
#endif
          {
            int pin_flags = RTLD_NOW | RTLD_LOCAL;
#if LJ_TARGET_OSX
            /* Keep handle-relative base verification from resolving an
            ** interposed luaJIT_profile_start in another loaded image. */
            pin_flags |= RTLD_FIRST;
#endif
            handle = dlopen(info.dli_fname, pin_flags);
            ok = handle != NULL && profile_image_pin_matches(handle, &info);
#if defined(LJ_PROFILE_TIMER_TEST_HELPERS)
            if (ok && profile_test_fail_now(
                        &profile_test_fail_image_match_after))
              ok = 0;
#endif
            if (!ok && handle != NULL) {
              (void)dlclose(handle);
              handle = NULL;
            }
          }
        }
#if defined(LJ_PROFILE_TIMER_TEST_HELPERS)
pin_done:
#endif
        if (ok)
          la_storeptr_rel(&profile_image_pin_handle, handle);
        la_store32_rel(&profile_image_pin_state,
                       ok ? PROFILE_COLD_READY : PROFILE_COLD_EMPTY);
        return ok;
      }
      continue;
    }
    (void)sched_yield();
    if (!profile_process_repair())
      return 0;
  }
}

static int profile_sigaction(int sig, const struct sigaction *action,
                             struct sigaction *oldaction)
{
#if defined(LJ_PROFILE_TIMER_TEST_HELPERS)
  (void)la_add32_acqrel(&profile_test_sigaction_count, 1);
  if (profile_test_fail_now(&profile_test_fail_sigaction_after)) {
    errno = EIO;
    return -1;
  }
#endif
  return sigaction(sig, action, oldaction);
}

static int profile_setitimer(int which, const struct itimerval *value,
                             struct itimerval *oldvalue)
{
#if defined(LJ_PROFILE_TIMER_TEST_HELPERS)
  (void)la_add32_acqrel(&profile_test_setitimer_count, 1);
  if (profile_test_fail_now(&profile_test_fail_setitimer_after)) {
    errno = EIO;
    return -1;
  }
#endif
  return setitimer(which, value, oldvalue);
}

static int profile_signal_enter(ProfileState *ps)
{
  uint32_t handlers = la_load32_acq(&ps->signal_handlers);
  while (handlers != UINT32_MAX) {
    uint32_t next = handlers + 1u;
    if (la_cas32(&ps->signal_handlers, &handlers, next,
                 LA_ACQ_REL, LA_ACQ))
      return 1;
  }
  return 0;
}

/* SIGPROF handler. */
PROFILE_SIGNAL_SCOPE void profile_signal(int sig)
{
  ProfileState *ps = &profile_state;
#if LJ_THR_TG_SIGNAL_CACHE
  TGState *tg;
  global_State *g;
  int32_t st;
#endif
  int *errno_address = &errno;
  int saved_errno = *errno_address;
  UNUSED(sig);
#if LJ_THR_TG_SIGNAL_CACHE
  /* The exact/transitional lookup performs the mandatory eager getpid
  ** incarnation check. No compiler TLS/TLV state is reachable; tag 2 remains
  ** a documented same-thread bridge until production exact migration. */
  tg = lj_thr_get_tg_profile_signal();
  if (!tg || !profile_signal_enter(ps))
    goto signal_out;
#if defined(LJ_PROFILE_TIMER_TEST_HELPERS)
  if (la_load32_acq(&profile_test_signal_pause)) {
    (void)la_add32_acqrel(&profile_test_signal_entered_count, 1);
    while (la_load32_acq(&profile_test_signal_pause)) {
      /* Atomic-only deterministic pause, never enabled in production. */
    }
  }
#endif
  /* The handler publishes only atomics. profile_request is the final release
  ** edge; the owner VM's poll/request acquire check enters normal context,
  ** exchanges the request, and only there installs a dispatch overlay. */
  g = profile_g_load_acq(ps);
  if (g && profile_state_active_g(ps, g) && profile_tg_eligible(g, tg)) {
    lj_tg_profile_samples_add(tg, 1);
    st = profile_sample_vmstate_tg(g, tg);
    lj_tg_profile_vmstate_store_rel(tg, st);
    lj_tg_profile_request_rel(tg, 1);
  }
  (void)la_sub32_acqrel(&ps->signal_handlers, 1);
#else
  /* Unsupported legacy targets retain their existing timer backend until an
  ** equivalent process-stable exact binding is available. */
  if (profile_signal_enter(ps)) {
    profile_trigger(ps);
    (void)la_sub32_acqrel(&ps->signal_handlers, 1);
  }
#endif
signal_out:
  *errno_address = saved_errno;
}

#if defined(LJ_PROFILE_TIMER_TEST_HELPERS) && !LJ_TARGET_OSX
LUA_API void luaJIT_profile_timer_test_signal_entry(int sig)
  __attribute__((alias("profile_signal")));
#endif

static uint32_t profile_signals_wait(lua_State *L, ProfileState *ps)
{
  /* Temporary safety bridge: final nonblocking close must defer teardown and
  ** let the last handler publish completion instead of waiting here. */
  uint32_t actions = 0;
  while (la_load32_acq(&ps->signal_handlers) != 0) {
#if defined(LJ_PROFILE_TIMER_TEST_HELPERS)
    (void)la_add32_acqrel(&profile_test_signal_drain_wait_count, 1);
#endif
    actions |= lj_thr_retry_yield(L);
  }
  return actions;
}

/* Install the handler before arming the timer. A failed arm either restores a
** newly-installed old action or leaves our inert handler installed if that
** rollback itself fails. In both cases the caller can safely return to IDLE. */
static int profile_timer_start(ProfileState *ps, TGState *expected_tg)
{
  int interval = ps->interval;
  int installed_now = 0;
  int saved_errno = errno;
  struct itimerval tm;
  struct sigaction sa;
  /* Seed process identity before any cold state becomes BUILDING, then
  ** register child repair and permanently pin the containing image before the
  ** first kernel-visible handler address. A raw fork can therefore always
  ** distinguish and reset a copied image-pin builder. */
  if (!profile_process_repair() || !profile_image_pin() ||
      !lj_thr_tg_signal_activate() ||
      !profile_atfork_ensure() ||
      !lj_thr_tg_signal_prepare_current(expected_tg)) {
    errno = saved_errno;
    return 0;
  }
  /* Resolve handler-reachable errno/getter entries in normal context. */
  (void)errno;
  (void)lj_thr_get_tg_profile_signal();
  errno = saved_errno;
  tm.it_value.tv_sec = tm.it_interval.tv_sec = interval / 1000;
  tm.it_value.tv_usec = tm.it_interval.tv_usec = (interval % 1000) * 1000;
#if LJ_TARGET_QNX
  sa.sa_flags = 0;
#else
  sa.sa_flags = SA_RESTART;
#endif
  sa.sa_handler = profile_signal;
  if (sigemptyset(&sa.sa_mask) != 0) {
    errno = saved_errno;
    return 0;
  }
  if (!la_load32_acq(&ps->signal_handler_installed)) {
    if (profile_sigaction(SIGPROF, &sa, &ps->oldsa) != 0) {
      errno = saved_errno;
      return 0;
    }
    la_store32_rel(&ps->signal_handler_installed, 1);
    installed_now = 1;
  }
#if defined(LJ_PROFILE_TIMER_TEST_HELPERS)
  if (la_load32_acq(&profile_test_before_arm_pause)) {
    (void)la_add32_acqrel(&profile_test_before_arm_entered_count, 1);
    while (la_load32_acq(&profile_test_before_arm_pause)) {
      /* Deterministic normal-context start/handler overlap. */
    }
  }
#endif
  if (profile_setitimer(ITIMER_PROF, &tm, NULL) != 0) {
    if (installed_now &&
        profile_sigaction(SIGPROF, &ps->oldsa, NULL) == 0)
      la_store32_rel(&ps->signal_handler_installed, 0);
    errno = saved_errno;
    return 0;
  }
  errno = saved_errno;
  return 1;
}

/* Stop profiling timer. */
static uint32_t profile_timer_stop(ProfileState *ps, lua_State *L)
{
  UNUSED(L);
  struct itimerval tm;
  tm.it_value.tv_sec = tm.it_interval.tv_sec = 0;
  tm.it_value.tv_usec = tm.it_interval.tv_usec = 0;
  /* Never restore the old action until disarm succeeded. If either syscall
  ** fails, our static handler remains installed and observes STOPPING/IDLE
  ** with a NULL global state, so a still-armed or external signal is inert. */
  if (profile_setitimer(ITIMER_PROF, &tm, NULL) == 0 &&
      la_load32_acq(&ps->signal_handler_installed) &&
      profile_sigaction(SIGPROF, &ps->oldsa, NULL) == 0)
    la_store32_rel(&ps->signal_handler_installed, 0);
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
#if LJ_PROFILE_SIGPROF
  int start_had_stopreq = lj_safepoint_had_stopreq(L);
#endif
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
#if LJ_THR_TG_SIGNAL_CACHE
  int flush_status;
  int flush_errcode;
  /* Publish the recording policy before flushing every pre-policy trace.
  ** Concurrent recordings either predate the flush or observe poll_g and emit
  ** an XPOLL. The timer is not armed until this boundary is complete. */
  L2J(L)->prof_mode = prof_mode;
  profile_poll_g_store_rel(ps, g);
  flush_errcode = profile_trace_flush_catch(L, &flush_status);
  if (flush_errcode != 0 || flush_status != 0) {
    profile_poll_g_store_rel(ps, NULL);
    L2J(L)->prof_mode = 0;
    profile_state_store_rel(ps, LJ_PROFILE_IDLE);
    if (flush_errcode != 0)
      lj_err_throw(L, flush_errcode);
    return;  /* Never arm while a pre-policy trace can still survive. */
  }
#else
  if (prof_mode) {
    L2J(L)->prof_mode = prof_mode;
    (void)lj_trace_flushall_hs(L);
  }
#endif
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
#if LJ_PROFILE_SIGPROF
  if (!profile_timer_start(ps, L2TG(L))) {
    uint32_t start_actions = profile_signals_wait(L, ps);
    /* STARTING keeps any inherited/partially-installed handler inert while
    ** every publication made above is rolled back. */
#if LJ_HASJIT
#if LJ_THR_TG_SIGNAL_CACHE
    int flush_status;
    int flush_errcode;
    profile_poll_g_store_rel(ps, NULL);
    G2J(g)->prof_mode = 0;
    flush_errcode = profile_trace_flush_catch(L, &flush_status);
    UNUSED(flush_status);  /* Residual poll traces are safe, only slower. */
#else
    if (G2J(g)->prof_mode != 0) {
      G2J(g)->prof_mode = 0;
      (void)lj_trace_flushall_hs(L);
    }
#endif
#endif
    lj_buf_free(g, &ps->sb);
    ps->sb.b = ps->sb.w = ps->sb.e = NULL;
    profile_cb_store_rel(ps, NULL);
    profile_data_store_rel(ps, NULL);
    (void)profile_samples_xchg(ps, 0);
    profile_vmstate_store_rel(ps, 'N');
    profile_state_store_rel(ps, LJ_PROFILE_IDLE);
#if LJ_HASJIT && LJ_THR_TG_SIGNAL_CACHE
    if (flush_errcode != 0)
      lj_err_throw(L, flush_errcode);
#endif
    lj_safepoint_checkstop_fresh(L, start_actions, start_had_stopreq);
    return;
  }
#else
  profile_timer_start(ps);
#endif
  profile_g_store_rel(ps, g);
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
#if LJ_HASJIT && LJ_THR_TG_SIGNAL_CACHE
  int flush_status = 0;
  int flush_errcode = 0;
#endif
#if LJ_PROFILE_SIGPROF
  /* A raw/foreign fork may bypass atfork. Repair its copied handler count
  ** before an inherited ACTIVE profiler can enter the drain path. */
  if (profile_state_load_acq(ps) == LJ_PROFILE_IDLE)
    return 0;
  (void)profile_process_repair();
#endif
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
#if LJ_PROFILE_SIGPROF
  actions |= profile_signals_wait(L, ps);
#endif
#if LJ_PROFILE_TGLOCAL
  profile_tg_drop_all(g);
#endif
  hookmask_update(g, HOOK_PROFILE, 0);
  lj_dispatch_update(g, 0);
  actions |= profile_callbacks_wait(L, ps);
#if LJ_HASJIT
#if LJ_THR_TG_SIGNAL_CACHE
  profile_poll_g_store_rel(ps, NULL);
  G2J(g)->prof_mode = 0;
  flush_errcode = profile_trace_flush_catch(L, &flush_status);
  UNUSED(flush_status);  /* Residual poll traces are safe, only slower. */
#else
  if (G2J(g)->prof_mode != 0) {
    G2J(g)->prof_mode = 0;
    (void)lj_trace_flushall_hs(L);
  }
#endif
#endif
  lj_buf_free(g, &ps->sb);
  ps->sb.b = ps->sb.w = ps->sb.e = NULL;
  profile_cb_store_rel(ps, NULL);
  profile_data_store_rel(ps, NULL);
  (void)profile_samples_xchg(ps, 0);
  profile_vmstate_store_rel(ps, 'N');
  profile_g_store_rel(ps, NULL);
  profile_state_store_rel(ps, LJ_PROFILE_IDLE);
#if LJ_HASJIT && LJ_THR_TG_SIGNAL_CACHE
  if (flush_errcode != 0)
    lj_err_throw(L, flush_errcode);
#endif
  return actions;
}

/* Stop profiling. */
LUA_API void luaJIT_profile_stop(lua_State *L)
{
  int had_stopreq = lj_safepoint_had_stopreq(L);
  uint32_t actions = lj_profile_stop_hs(L);
  lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
}

typedef struct ProfileDumpstackCtx {
  SBuf *sb;
  const char *fmt;
  int depth;
  size_t len;
} ProfileDumpstackCtx;

static TValue *profile_dumpstack_cp(lua_State *L, lua_CFunction dummy,
				    void *ud)
{
  ProfileDumpstackCtx *ctx = (ProfileDumpstackCtx *)ud;
  UNUSED(dummy);
  setsbufL(ctx->sb, L);
  lj_buf_reset(ctx->sb);
  lj_debug_dumpstack(L, ctx->sb, ctx->fmt, ctx->depth);
  ctx->len = (size_t)sbuflen(ctx->sb);
  return NULL;
}

/* Return a compact stack dump. */
LUA_API const char *luaJIT_profile_dumpstack(lua_State *L, const char *fmt,
					     int depth, size_t *len)
{
  LJStateClaim claim;
  ProfileState *ps = &profile_state;
  SBuf *sb = &ps->sb;
  ProfileDumpstackCtx ctx;
  int errcode;
  if (!lj_state_tryclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(profile_errstate(L), "thread busy");
  ctx.sb = sb;
  ctx.fmt = fmt;
  ctx.depth = depth;
  ctx.len = 0;
  errcode = lj_vm_cpcall(L, NULL, &ctx, profile_dumpstack_cp);
  lj_state_dropclaim(&claim);
  if (LJ_UNLIKELY(errcode))
    lj_err_throw(L, errcode);
  *len = ctx.len;
  return sb->b;
}

#endif
