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

typedef struct HandshakeCtx {
  global_State *g;
  uint32_t actions;
  uint32_t signaled;
} HandshakeCtx;

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
  assert(G2TG(ctx->g) == &ctx->tg);
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

static void *handshake_main(void *arg)
{
  HandshakeCtx *ctx = (HandshakeCtx *)arg;
  ctx->signaled = lj_gc2_handshake(ctx->g, ctx->actions);
  return NULL;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  LJThr thr;
  ThrCtx ctx = {0};
  ThrCtx catch = {0};
  HandshakeCtx hs = {0};
  pthread_t hs_thread;
  void *ret = NULL;
  uint64_t epoch0;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);

  lj_thr_set_tg(tg);
  assert(tg->tid != 0);
  assert(L->thr_owner == tg->tid);
  assert(lj_thr_get_tg() == tg);
  assert(G2TG(g) == tg);
  assert(lj_thr_cpucount() >= 1u);
  lj_thr_fence();

  {
    lua_State *Lclaim = lua_newthread(L);
    uint32_t owner = lj_thr_current_id(g);
    assert(owner == tg->tid);
    assert(Lclaim->thr_owner == 0);
    assert(lj_state_claim(Lclaim, owner) == 1);
    assert(Lclaim->thr_owner == owner);
    assert(lj_state_claim(Lclaim, owner + 1u) == 0);
    lj_state_release(Lclaim, owner);
    assert(Lclaim->thr_owner == 0);
    lua_pop(L, 1);
  }

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

  hs.g = g;
  hs.actions = LJ_GC2_HS_ALLOC_BLACK|LJ_GC2_HS_STOPREQ;
  tg->cur_L = NULL;  /* Hold the leader handshake open on the main TG. */
  epoch0 = la_load64_acq(&g->gc2.hs_epoch);
  assert(pthread_create(&hs_thread, NULL, handshake_main, &hs) == 0);
  while (la_load64_acq(&g->gc2.hs_epoch) == epoch0)
    la_cpu_pause();
  while (la_load32_acq(&g->gc2.hs_pending) == 0)
    la_cpu_pause();

  catch.g = g;
  catch.L = lua_newthread(L);
  assert(catch.L != NULL);
  assert(lj_thr_create(&thr, worker_main, &catch) == 0);
  while (la_load32_acq(&catch.attached) == 0)
    la_cpu_pause();
  assert(catch.tg.hs_epoch_ack == g->gc2.hs_epoch);
  assert(catch.tg.alloc.alloc_black == 1);
  assert(catch.tg.tg_flags & TGF_STOPREQ);

  tg->cur_L = L;
  assert(lj_safepoint_ack(L) == hs.actions);
  assert(pthread_join(hs_thread, NULL) == 0);
  assert(hs.signaled == 1u);
  assert(g->gc2.hs_pending == 0);
  assert(catch.tg.hs_epoch_ack == g->gc2.hs_epoch);

  la_store32_rel(&catch.release, 1);
  assert(lj_thr_join(&thr, &ret) == 0);
  assert(ret == (void *)(uintptr_t)0x4a);
  assert(la_load32_acq(&catch.detached) == 1u);
  assert(la_load32_acq(&g->gc2.n_threads) == 1u);
  lua_pop(L, 1);

  lj_thr_set_tg(NULL);
  lua_pop(L, 1);
  lua_close(L);

  printf("t-thr-substrate OK: pthread create/join, TG TLS, sleep verified\n");
  return 0;
}
