/*
** OS-thread substrate for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_THR_H
#define _LJ_THR_H

#include <pthread.h>
#include <stdint.h>

#include "lj_obj.h"

typedef void *(*LJThrFunc)(void *);

typedef struct LJThr {
  pthread_t handle;
  uint32_t tid;
} LJThr;

#define LJ_THREAD_RUNNING	1u
#define LJ_THREAD_DONE		2u

typedef struct LJThread {
  LJThr thr;
  lua_State *L;
  GCudata *ud;
  TGState *tg;
  uint32_t state;
  uint32_t joined;
  uint32_t futex;
  uint32_t status;
  uint32_t nargs;
  uint32_t nresults;
  uint32_t main_thread;
} LJThread;

LJ_FUNC int lj_thr_create(LJThr *thr, LJThrFunc func, void *arg);
LJ_FUNC int lj_thr_join(LJThr *thr, void **ret);
LJ_FUNC uint32_t lj_thr_id(const LJThr *thr);
LJ_FUNC void lj_thr_set_tg(TGState *tg);
LJ_FUNC TGState *lj_thr_get_tg(void);
LJ_FUNC TGState *lj_thr_get_tg_fallback(global_State *g);
LJ_FUNC uint32_t lj_thr_cpucount(void);
LJ_FUNC void lj_thr_fence(void);
LJ_FUNC uint32_t lj_thr_sleep_ns(lua_State *L, int64_t ns);

#endif
