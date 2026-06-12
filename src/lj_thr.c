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

static uint32_t thr_next_tid(void)
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
  thr->tid = thr_next_tid();
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

void lj_thr_set_tg(TGState *tg)
{
  lj_tls_tg = tg;  /* 03 section 3.2: one TLS TG pointer per OS thread. */
}

TGState *lj_thr_get_tg(void)
{
  return lj_tls_tg;
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
    la_store8_rlx(&tg->in_native, 0);  /* No Lua stack is available to poll. */
  }
  return actions;
}
