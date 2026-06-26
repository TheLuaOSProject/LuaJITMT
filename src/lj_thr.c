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
#include <time.h>
#include <unistd.h>

static __thread TGState *lj_tls_tg;
static uint32_t lj_thr_next_tid;

uint32_t lj_thr_newid(void)
{
  uint32_t tid = la_add32_rlx(&lj_thr_next_tid, 1) + 1u;  /* 09 section 9.2. */
  if (tid == 0)
    tid = la_add32_rlx(&lj_thr_next_tid, 1) + 1u;
  return tid;
}

int lj_thr_create(LJThr *thr, LJThrFunc func, void *arg)
{
  int rc;
  if (!thr || !func)
    return EINVAL;
  if (thr->tid == 0)
    thr->tid = lj_thr_newid();
  rc = pthread_create(&thr->handle, NULL, func, arg);  /* 09 section 9.3. */
  if (rc != 0)
    thr->tid = 0;
  return rc;
}

int lj_thr_join(LJThr *thr, void **ret)
{
  if (!thr)
    return EINVAL;
  return pthread_join(thr->handle, ret);  /* 09 section 9.4 substrate. */
}

uint32_t lj_thr_id(const LJThr *thr)
{
  return thr ? thr->tid : 0;
}

uint32_t lj_thr_current_id(global_State *g)
{
  TGState *tg = lj_thr_get_tg_fallback(g);
  return tg ? tg->tid : 0;
}

void lj_thr_set_tg(TGState *tg)
{
  lj_tls_tg = tg;  /* 03 section 3.2: one TLS TG pointer per OS thread. */
}

TGState *lj_thr_get_tg(void)
{
  return lj_tls_tg;
}

TGState *lj_thr_get_tg_fallback(global_State *g)
{
  TGState *tg = lj_tls_tg;
  if (!g)
    return tg;
  return tg && tg->gl == g ? tg : g->main_tg;
}

int lj_state_claim(lua_State *L, uint32_t tid)
{
  uint32_t owner;
  if (!L || tid == 0 || tid == LJ_THREAD_GCSCAN)
    return 0;
  for (;;) {
    owner = la_load32_acq(&L->thr_owner);
    if (owner == tid)
      return 1;
    if (owner == 0) {
      uint32_t expect = 0;
      if (la_cas32(&L->thr_owner, &expect, tid, LA_ACQ_REL, LA_ACQ))
	return 1;
      continue;
    }
    if (owner == LJ_THREAD_GCSCAN) {
      la_cpu_pause();
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
    owner = la_load32_acq(&L->thr_owner);
    if (owner == tid) {
      if (claim) {
	claim->L = L;
	claim->tid = tid;
      }
      return 1;
    }
    if (owner == 0) {
      uint32_t expect = 0;
      if (la_cas32(&L->thr_owner, &expect, tid, LA_ACQ_REL, LA_ACQ)) {
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
      la_cpu_pause();
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
    owner = la_load32_acq(&L->thr_owner);
    if (owner == 0) {
      uint32_t expect = 0;
      if (la_cas32(&L->thr_owner, &expect, LJ_THREAD_GCSCAN,
		   LA_ACQ_REL, LA_ACQ)) {
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
      la_cpu_pause();  /* 05 section 5.7.2: scan claim is short-lived. */
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
    la_add64_rlx(&tg->stack_dirty_epoch, 1);
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
    uint32_t owner = la_load32_acq(&L->thr_owner);
    lj_assertX(owner == tid, "lua_State owner mismatch");
    UNUSED(owner);
    state_stack_dirty(L, tid);
    la_store32_rel(&L->thr_owner, 0);
  }
}

uint32_t lj_thr_cpucount(void)
{
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (uint32_t)n : 1u;
}

void lj_thr_fence(void)
{
  la_fence_seq();  /* 09 section 9.1 threading.fence memory edge. */
}

uint32_t lj_thr_sleep_ns(lua_State *L, int64_t ns)
{
  TGState *tg = L ? L2TG(L) : lj_tls_tg;
  uint32_t actions = 0;
  if (tg)
    lj_native_enter(tg);  /* 09 section 9.1 sleep is a native region. */
  if (ns > 0) {
    struct timespec req;
    req.tv_sec = (time_t)(ns / 1000000000ll);
    req.tv_nsec = (long)(ns % 1000000000ll);
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
      ;
  }
  if (L) {
    actions = lj_native_leave(L);
  } else if (tg) {
    lj_tg_in_native_store_rlx(tg, 0);  /* No Lua stack is available to poll. */
  }
  return actions;
}
