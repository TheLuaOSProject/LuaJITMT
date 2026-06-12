/*
** Focused test for the C-only thread substrate.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_thr.h"
#include "lj_tg.h"

typedef struct ThrCtx {
  global_State *g;
  lua_State *L;
  TGState tg;
  uint32_t attached;
  uint32_t release;
  uint32_t detached;
} ThrCtx;

static void publish_manual(global_State *g, TGState *tg, uint32_t actions)
{
  uint64_t epoch = la_load64_rlx(&g->gc2.hs_epoch) + 1u;
  la_store32_rel(&g->gc2.hs_actions, actions);
  la_store32_rel(&g->gc2.hs_pending, 1);
  la_store64_rel(&g->gc2.hs_epoch, epoch);
  la_store32_rel(&tg->reqmask, actions);
  la_store32_rel(&tg->poll, 1);
}

static void *worker_main(void *arg)
{
  ThrCtx *ctx = (ThrCtx *)arg;
  lj_tg_init_thread(ctx->g, &ctx->tg, ctx->L, 0);
  lj_thr_set_tg(&ctx->tg);
  assert(lj_thr_get_tg() == &ctx->tg);
  lj_native_enter(&ctx->tg);
  lj_tg_attach(ctx->g, &ctx->tg);
  la_store32_rel(&ctx->attached, 1);
  while (la_load32_acq(&ctx->release) == 0)
    la_cpu_pause();
  assert(ctx->tg.in_native == 1);
  lj_tg_detach(ctx->g, &ctx->tg);
  lj_thr_set_tg(NULL);
  lj_tg_fini_thread(ctx->g, &ctx->tg);
  la_store32_rel(&ctx->detached, 1);
  return (void *)(uintptr_t)0x4a;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  LJThr thr;
  ThrCtx ctx = {0};
  void *ret = NULL;
  uint64_t epoch0;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);

  lj_thr_set_tg(tg);
  assert(lj_thr_get_tg() == tg);
  assert(lj_thr_cpucount() >= 1u);
  lj_thr_fence();

  tg->alloc.alloc_black = 1;
  epoch0 = g->gc2.hs_epoch;
  publish_manual(g, tg, LJ_GC2_HS_ALLOC_WHITE);
  assert(lj_thr_sleep_ns(L, 0) == LJ_GC2_HS_ALLOC_WHITE);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(tg->alloc.alloc_black == 0);
  assert(lj_thr_sleep_ns(NULL, 0) == 0);

  ctx.g = g;
  ctx.L = lua_newthread(L);
  assert(ctx.L != NULL);
  assert(lj_thr_create(&thr, worker_main, &ctx) == 0);
  assert(lj_thr_id(&thr) != 0);
  while (la_load32_acq(&ctx.attached) == 0)
    la_cpu_pause();
  assert(la_load32_acq(&g->gc2.n_threads) == 2u);
  assert(!(ctx.tg.tg_flags & TGF_DEAD));
  la_store32_rel(&ctx.release, 1);
  assert(lj_thr_join(&thr, &ret) == 0);
  assert(ret == (void *)(uintptr_t)0x4a);
  assert(la_load32_acq(&ctx.detached) == 1u);
  assert(la_load32_acq(&g->gc2.n_threads) == 1u);
  assert(ctx.tg.tg_flags & TGF_DEAD);

  lj_thr_set_tg(NULL);
  lua_pop(L, 1);
  lua_close(L);

  printf("t-thr-substrate OK: pthread create/join, TG TLS, sleep verified\n");
  return 0;
}
