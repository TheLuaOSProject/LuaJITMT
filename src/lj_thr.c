/*
** OS-thread substrate for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_thr_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"

#include <errno.h>
#if LJ_TARGET_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#if LJ_TARGET_WINDOWS
static DWORD lj_tls_tg_key = TLS_OUT_OF_INDEXES;
static INIT_ONCE lj_tls_tg_once = INIT_ONCE_STATIC_INIT;
#else
static LJ_TLS TGState *lj_tls_tg;
#endif
static uint32_t lj_thr_next_tid;

uint32_t lj_thr_newid(void)
{
  uint32_t tid = la_add32_rlx(&lj_thr_next_tid, 1) + 1u;  /* 09 section 9.2. */
  if (tid == 0)
    tid = la_add32_rlx(&lj_thr_next_tid, 1) + 1u;
  return tid;
}

#if LJ_TARGET_WINDOWS
static BOOL CALLBACK lj_thr_tls_init(PINIT_ONCE once, PVOID param,
				     PVOID *ctx)
{
  DWORD key;
  UNUSED(once);
  UNUSED(param);
  UNUSED(ctx);
  key = TlsAlloc();
  if (key == TLS_OUT_OF_INDEXES)
    return FALSE;
  lj_tls_tg_key = key;
  return TRUE;
}

static DWORD lj_thr_tls_key(void)
{
  return InitOnceExecuteOnce(&lj_tls_tg_once, lj_thr_tls_init, NULL, NULL) ?
    lj_tls_tg_key : TLS_OUT_OF_INDEXES;
}

static void lj_thr_tls_set(TGState *tg)
{
  DWORD key = lj_thr_tls_key();
  if (key != TLS_OUT_OF_INDEXES)
    (void)TlsSetValue(key, tg);
}

static TGState *lj_thr_tls_get(void)
{
  DWORD key = lj_thr_tls_key();
  return key != TLS_OUT_OF_INDEXES ? (TGState *)TlsGetValue(key) : NULL;
}

static DWORD WINAPI lj_thr_windows_main(void *arg)
{
  LJThr *thr = (LJThr *)arg;
  thr->ret = thr->func(thr->arg);
  return 0;
}
#else
static void lj_thr_tls_set(TGState *tg)
{
  lj_tls_tg = tg;
}

static TGState *lj_thr_tls_get(void)
{
  return lj_tls_tg;
}
#endif

int lj_thr_create(LJThr *thr, LJThrFunc func, void *arg)
{
  if (!thr || !func)
    return EINVAL;
  if (thr->tid == 0)
    thr->tid = lj_thr_newid();
#if LJ_TARGET_WINDOWS
  thr->func = func;
  thr->arg = arg;
  thr->ret = NULL;
  thr->handle = CreateThread(NULL, 0, lj_thr_windows_main, thr, 0,
			     &thr->sysid);  /* 09 section 9.3. */
  if (thr->handle == NULL) {
    thr->tid = 0;
    return EAGAIN;
  }
  return 0;
#else
  {
    int rc;
    rc = pthread_create(&thr->handle, NULL, func, arg);  /* 09 section 9.3. */
    if (rc != 0)
      thr->tid = 0;
    return rc;
  }
#endif
}

int lj_thr_join(LJThr *thr, void **ret)
{
  if (!thr)
    return EINVAL;
#if LJ_TARGET_WINDOWS
  if (WaitForSingleObject(thr->handle, INFINITE) != WAIT_OBJECT_0)
    return EINVAL;
  if (ret)
    *ret = thr->ret;
  CloseHandle(thr->handle);
  thr->handle = NULL;
  return 0;
#else
  return pthread_join(thr->handle, ret);  /* 09 section 9.4 substrate. */
#endif
}

uint32_t lj_thr_id(const LJThr *thr)
{
  return thr ? thr->tid : 0;
}

uint32_t lj_thr_current_id(global_State *g)
{
  TGState *tg = lj_thr_get_tg_fallback(g);
  return tg ? lj_tg_tid_acq(tg) : 0;
}

uint64_t lj_thr_now_ns(void)
{
#if LJ_TARGET_WINDOWS
  LARGE_INTEGER freq, ctr;
  if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&ctr) ||
      freq.QuadPart <= 0)
    return 0;
  return (uint64_t)ctr.QuadPart / (uint64_t)freq.QuadPart * 1000000000ull +
    (uint64_t)ctr.QuadPart % (uint64_t)freq.QuadPart * 1000000000ull /
    (uint64_t)freq.QuadPart;
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

void lj_thr_set_tg(TGState *tg)
{
  lj_thr_tls_set(tg);  /* 03 section 3.2: one TLS TG pointer per OS thread. */
}

TGState *lj_thr_get_tg(void)
{
  return lj_thr_tls_get();
}

TGState *lj_thr_get_tg_fallback(global_State *g)
{
  TGState *tg = lj_thr_tls_get();
  if (!g)
    return tg;
  return tg && tg->gl == g ? tg : g->main_tg;
}

static void state_gcscan_wait_no_l(void)
{
  (void)lj_thr_sleep_ns(NULL, 1000000);
}

int lj_state_claim(lua_State *L, uint32_t tid)
{
  uint32_t owner;
  if (!L || tid == 0 || tid == LJ_THREAD_GCSCAN)
    return 0;
  for (;;) {
    owner = lj_state_owner_acq(L);
    if (owner == tid)
      return 1;
    if (owner == 0) {
      uint32_t expect = 0;
      if (lj_state_owner_cas(L, &expect, tid))
	return 1;
      continue;
    }
    if (owner == LJ_THREAD_GCSCAN) {
      state_gcscan_wait_no_l();
      continue;
    }
    return 0;
  }
}

int lj_state_tryclaim(lua_State *L, uint32_t tid, LJStateClaim *claim)
{
  uint32_t owner;
  if (claim) {
    claim->L = NULL;
    claim->tid = 0;
    claim->release = 0;
  }
  if (!L || tid == 0 || tid == LJ_THREAD_GCSCAN)
    return 0;
  for (;;) {
    owner = lj_state_owner_acq(L);
    if (owner == tid) {
      if (claim) {
	claim->L = L;
	claim->tid = tid;
      }
      return 1;
    }
    if (owner == 0) {
      uint32_t expect = 0;
      if (lj_state_owner_cas(L, &expect, tid)) {
	if (claim) {
	  claim->L = L;
	  claim->tid = tid;
	  claim->release = 1;
	}
	return 1;
      }
      continue;
    }
    if (owner == LJ_THREAD_GCSCAN) {
      state_gcscan_wait_no_l();
      continue;
    }
    return 0;
  }
}

int lj_state_gcscan_claim(lua_State *L, LJStateClaim *claim)
{
  uint32_t owner;
  if (claim) {
    claim->L = NULL;
    claim->tid = 0;
    claim->release = 0;
  }
  if (!L)
    return 0;
  for (;;) {
    owner = lj_state_owner_acq(L);
    if (owner == 0) {
      uint32_t expect = 0;
      if (lj_state_owner_cas(L, &expect, LJ_THREAD_GCSCAN)) {
	if (claim) {
	  claim->L = L;
	  claim->tid = LJ_THREAD_GCSCAN;
	  claim->release = 1;
	}
	return 1;
      }
      continue;
    }
    if (owner == LJ_THREAD_GCSCAN) {
      state_gcscan_wait_no_l();  /* 05 section 5.7.2: scan claim handoff. */
      continue;
    }
    return 0;
  }
}

static void state_stack_dirty(lua_State *L, uint32_t tid)
{
  TGState *tg;
  if (!L || tid == 0 || tid == LJ_THREAD_GCSCAN)
    return;
  tg = lj_tg_find_owner(G(L), tid);
  if (tg)
    lj_tg_stack_dirty_epoch_add_rlx(tg, 1);
}

void lj_state_dropclaim(LJStateClaim *claim)
{
  if (claim && claim->release) {
    lj_state_release(claim->L, claim->tid);
    claim->release = 0;
  }
}

void lj_state_release(lua_State *L, uint32_t tid)
{
  if (L && tid != 0) {
    uint32_t owner = lj_state_owner_acq(L);
    lj_assertX(owner == tid, "lua_State owner mismatch");
    UNUSED(owner);
    state_stack_dirty(L, tid);
    lj_state_owner_rel(L, 0);
  }
}

uint32_t lj_thr_cpucount(void)
{
#if LJ_TARGET_WINDOWS
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors ? (uint32_t)si.dwNumberOfProcessors : 1u;
#else
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (uint32_t)n : 1u;
#endif
}

void lj_thr_fence(void)
{
  la_fence_seq();  /* 09 section 9.1 threading.fence memory edge. */
}

uint32_t lj_thr_sleep_ns(lua_State *L, int64_t ns)
{
  TGState *tg = L ? L2TG(L) : lj_thr_tls_get();
  uint32_t actions = 0;
  if (tg)
    lj_native_enter(tg);  /* 09 section 9.1 sleep is a native region. */
  if (ns > 0) {
#if LJ_TARGET_WINDOWS
    uint64_t ms = ((uint64_t)ns + 999999u) / 1000000u;
    if (ms == 0)
      ms = 1;
    Sleep(ms >= INFINITE ? INFINITE - 1u : (DWORD)ms);
#else
    struct timespec req;
    req.tv_sec = (time_t)(ns / 1000000000ll);
    req.tv_nsec = (long)(ns % 1000000000ll);
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
      ;
#endif
  }
  if (L) {
    actions = lj_native_leave(L);
  } else if (tg) {
    (void)lj_tg_in_native_dec_rel(tg);  /* No Lua stack is available to poll. */
  }
  return actions;
}
