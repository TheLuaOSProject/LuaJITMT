/*
** OS-thread substrate for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_THR_H
#define _LJ_THR_H

#include <pthread.h>
#include <stdint.h>

#include "lj_atomic.h"
#include "lj_obj.h"

typedef void *(*LJThrFunc)(void *);

typedef struct LJThr {
  pthread_t handle;
  uint32_t tid;
} LJThr;

typedef struct LJStateClaim {
  lua_State *L;
  uint32_t tid;
  uint8_t release;
} LJStateClaim;

#define LJ_THREAD_RUNNING	1u
#define LJ_THREAD_DONE		2u
#define LJ_THREAD_GCSCAN	0xffffffffu

struct LJThreadLive {
  struct LJThreadLive *next;
  GCRef ud;
};

static LJ_AINLINE LJThreadLive *
lj_thread_live_next_acq(const LJThreadLive *node)
{
  return (LJThreadLive *)la_loadptr_acq((void *const *)&node->next);
}

static LJ_AINLINE void lj_thread_live_next_rel(LJThreadLive *node,
					       LJThreadLive *next)
{
  la_storeptr_rel((void **)&node->next, next);
}

typedef struct LJThread {
  LJThr thr;
  lua_State *L;
  GCudata *ud;
  TGState *tg;
  LJThreadLive *live_node;
  uint32_t state;
  uint32_t joined;
  uint32_t futex;
  uint32_t status;
  uint32_t nargs;
  uint32_t nresults;
  uint32_t main_thread;
} LJThread;

static LJ_AINLINE lua_State *lj_thread_state_load_acq(const LJThread *th)
{
  return (lua_State *)la_loadptr_acq((void *const *)&th->L);
}

static LJ_AINLINE void lj_thread_state_store_rel(LJThread *th, lua_State *L)
{
  la_storeptr_rel((void **)&th->L, (void *)L);
}

LJ_FUNC int lj_thr_create(LJThr *thr, LJThrFunc func, void *arg);
LJ_FUNC int lj_thr_join(LJThr *thr, void **ret);
LJ_FUNC uint32_t lj_thr_newid(void);
LJ_FUNC uint32_t lj_thr_id(const LJThr *thr);
LJ_FUNC uint32_t lj_thr_current_id(global_State *g);
LJ_FUNC void lj_thr_set_tg(TGState *tg);
LJ_FUNC TGState *lj_thr_get_tg(void);
LJ_FUNC TGState *lj_thr_get_tg_fallback(global_State *g);
LJ_FUNC int lj_threading_attach(lua_State *L);
LJ_FUNC void lj_threading_detach(lua_State *L, int disown_callbacks);
LJ_FUNC int lj_state_claim(lua_State *L, uint32_t tid);
LJ_FUNC int lj_state_tryclaim(lua_State *L, uint32_t tid, LJStateClaim *claim);
LJ_FUNC int lj_state_gcscan_claim(lua_State *L, LJStateClaim *claim);
LJ_FUNC void lj_state_dropclaim(LJStateClaim *claim);
LJ_FUNC void lj_state_release(lua_State *L, uint32_t tid);
LJ_FUNC uint32_t lj_thr_cpucount(void);
LJ_FUNC void lj_thr_fence(void);
LJ_FUNC uint32_t lj_thr_sleep_ns(lua_State *L, int64_t ns);
LJ_FUNC void lj_threading_shutdown(lua_State *L);

#endif
