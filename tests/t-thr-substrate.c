/*
** Focused test for the C-only thread substrate.
*/

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
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

#define ID_STRESS_THREADS 8u
#define ID_STRESS_COUNT 4096u

typedef struct IdStressCtx {
  uint32_t counter;
  uint32_t first;
  uint32_t ready;
  uint32_t go;
  uint32_t successes;
  uint32_t seen[ID_STRESS_COUNT];
} IdStressCtx;

static void publish_manual(global_State *g, TGState *tg, uint32_t actions)
{
  uint64_t epoch = la_load64_rlx(&g->gc2.hs_epoch) + 1u;
  la_store32_rel(&g->gc2.hs_actions, actions);
  la_store32_rel(&g->gc2.hs_pending, 1);
  la_store64_rel(&g->gc2.hs_epoch, epoch);
  la_store32_rel(&tg->reqmask, actions);
  la_store32_rel(&tg->poll, 1);
}

static void attach_without_catchup(global_State *g, TGState *tg)
{
  LJTGRegistrySlot *slot = (LJTGRegistrySlot *)malloc(sizeof(*slot));
  LJTGRegistrySlot *stable_head;
  LJTGRegistryKey key;
  LJTGSlotSnap snap;
  void *head;
  assert(slot != NULL);
  assert(lj_tgregistry_slot_init_unpublished(slot, 0, NULL));
  assert(lj_tgregistry_try_claim(slot, &key, &snap) == LJ_TGSLOT_OK);
  assert(lj_tgregistry_try_publish_body(&key, tg, &snap) == LJ_TGSLOT_OK);
  tg->registry_key = key;
  do {
    stable_head = gc2_tg_registry_head_acq(g);
    slot->next_all = stable_head;
  } while (!gc2_tg_registry_head_cas(g, &stable_head, slot));
  (void)gc2_tg_registry_nodes_add(g, 1);
  do {
    head = la_loadptr_acq((void *const *)&g->gc2.tg_list);
    lj_tg_next_rel(tg, (TGState *)head);
  } while (!la_casptr((void **)&g->gc2.tg_list, &head, tg,
		      LA_ACQ_REL, LA_ACQ));
  la_add32_rlx(&g->gc2.n_threads, 1);
  assert(lj_tgregistry_try_publish(&key, &snap) == LJ_TGSLOT_OK);
}

static int tg_list_contains(TGState *tg, TGState *needle)
{
  while (tg) {
    if (tg == needle)
      return 1;
    tg = lj_tg_next_acq(tg);
  }
  return 0;
}

static void wait_late_remote_ack(TGState *tg, global_State *g)
{
  uint64_t epoch = la_load64_acq(&g->gc2.hs_epoch);
  while (la_load64_acq(&tg->hs_epoch_ack) != epoch)
    la_cpu_pause();
  while (la_load32_acq(&tg->poll) != 0)
    la_cpu_pause();
}

static void *worker_main(void *arg)
{
  ThrCtx *ctx = (ThrCtx *)arg;
  LJGC2RootDescView root_view;
  lj_tg_init_thread(ctx->g, &ctx->tg, ctx->L, 0);
  assert(lj_gc2_rootdesc_snapshot(&ctx->tg.root_desc, &root_view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  assert(root_view.generation == 0);
  lj_thr_set_tg(&ctx->tg);
  assert(lj_thr_get_tg() == &ctx->tg);
  assert(G2TG(ctx->g) == &ctx->tg);
  setgcref(ctx->g->cur_L, obj2gco(mainthread_acq(ctx->g)));
  setmref(ctx->g->jit_base, ctx->L->base);
  ctx->tg.cur_L = NULL;
  ctx->tg.jit_base = NULL;
  assert(lj_tg_cur_L(ctx->g) == NULL);
  assert(lj_tg_jit_base(ctx->g) == NULL);
  ctx->tg.cur_L = ctx->L;
  setmref(ctx->g->jit_base, NULL);
  lj_native_enter(&ctx->tg);
  lj_tg_attach(ctx->g, &ctx->tg);
  la_store32_rel(&ctx->attached, 1);
  while (la_load32_acq(&ctx->release) == 0)
    la_cpu_pause();
  assert(lj_tg_in_native_acq(&ctx->tg) == 1);
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

static void *id_stress_main(void *arg)
{
  IdStressCtx *ctx = (IdStressCtx *)arg;
  uint32_t id;
  (void)la_add32_rlx(&ctx->ready, 1);
  while (la_load32_acq(&ctx->go) == 0)
    la_cpu_pause();
  while ((id = lj_thr_id_alloc(&ctx->counter)) != 0) {
    uint32_t index = id - ctx->first;
    assert(index < ID_STRESS_COUNT);
    assert(la_add32_rlx(&ctx->seen[index], 1) == 0);
    (void)la_add32_rlx(&ctx->successes, 1);
  }
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
  lua_State *hold_L;
  TGState hold_tg;
  lua_State *late_L;
  TGState late_tg;
  HandshakeCtx hs = {0};
  LJThr hs_thread;
  void *ret = NULL;
  uint64_t epoch0;

  {
    uint32_t counter = LJ_THREAD_GCSCAN - 3u;
    LJThr invalid = {0};
    assert(lj_thr_id_alloc(&counter) == LJ_THREAD_GCSCAN - 2u);
    assert(lj_thr_id_alloc(&counter) == LJ_THREAD_GCSCAN - 1u);
    assert(lj_thr_id_alloc(&counter) == 0);
    assert(lj_thr_id_alloc(&counter) == 0);
    assert(counter == LJ_THREAD_GCSCAN - 1u);
    counter = LJ_THREAD_GCSCAN;
    assert(lj_thr_id_alloc(&counter) == 0);
    assert(counter == LJ_THREAD_GCSCAN);
    invalid.tid = LJ_THREAD_GCSCAN;
    assert(lj_thr_create(&invalid, id_stress_main, NULL) == EAGAIN);
    assert(invalid.tid == 0);
  }
  {
    IdStressCtx idctx = {0};
    LJThr idthr[ID_STRESS_THREADS] = {{0}};
    uint32_t i;
    idctx.counter = LJ_THREAD_GCSCAN - 1u - ID_STRESS_COUNT;
    idctx.first = idctx.counter + 1u;
    for (i = 0; i < ID_STRESS_THREADS; i++)
      assert(lj_thr_create(&idthr[i], id_stress_main, &idctx) == 0);
    while (la_load32_acq(&idctx.ready) != ID_STRESS_THREADS)
      la_cpu_pause();
    la_store32_rel(&idctx.go, 1);
    for (i = 0; i < ID_STRESS_THREADS; i++)
      assert(lj_thr_join(&idthr[i], NULL) == 0);
    assert(la_load32_acq(&idctx.successes) == ID_STRESS_COUNT);
    assert(la_load32_acq(&idctx.counter) == LJ_THREAD_GCSCAN - 1u);
    for (i = 0; i < ID_STRESS_COUNT; i++)
      assert(la_load32_acq(&idctx.seen[i]) == 1);
  }

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  {
    LJGC2RootDescView root_view;
    assert(lj_gc2_rootdesc_snapshot(&tg->root_desc, &root_view) ==
           LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
    assert(root_view.generation == 0);
  }

  lj_thr_set_tg(tg);
  assert(tg->tid != 0);
  assert(lj_state_owner_acq(L) == tg->tid);
  assert(lj_thr_get_tg() == tg);
  assert(G2TG(g) == tg);
  assert(lj_tg_cur_L(NULL) == lj_tg_cur_L(g));
  assert(lj_tg_jit_base(NULL) == lj_tg_jit_base(g));
  lj_thr_set_tg(NULL);
  assert(lj_tg_cur_L(NULL) == NULL);
  assert(lj_tg_jit_base(NULL) == NULL);
  lj_tg_setcur_L(NULL, NULL);
  lj_tg_clearcur_L(NULL);
  lj_tg_setjit_base(NULL, NULL);
  lj_thr_set_tg(tg);
  assert(lj_thr_cpucount() >= 1u);
  lj_thr_fence();

  {
    lua_State *Lclaim = lua_newthread(L);
    uint32_t owner = lj_thr_current_id(g);
    uint64_t dirty0;
    assert(owner == tg->tid);
    assert(lj_state_owner_acq(Lclaim) == 0);
    assert(lj_state_claim(Lclaim, owner) == 1);
    assert(lj_state_owner_acq(Lclaim) == owner);
    assert(lj_state_claim(Lclaim, owner + 1u) == 0);
    dirty0 = lj_tg_stack_dirty_epoch_acq(tg);
    lj_state_release(Lclaim, owner);
    assert(lj_state_owner_acq(Lclaim) == 0);
    assert(lj_tg_stack_dirty_epoch_acq(tg) == dirty0 + 1u);
    lua_pop(L, 1);
  }

  {
    TValue slot;
    uint64_t dirty0 = lj_tg_stack_dirty_epoch_acq(tg);
    setnilV(&slot);
    tv_rawstore_rel(&slot, tv_rawload(&slot));
    (void)lj_tg_stack_dirty_epoch_add_rlx(tg, 1);
    lj_gc_pubtvroot_vm(L, &slot);
    assert(lj_tg_stack_dirty_epoch_acq(tg) == dirty0 + 1u);
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
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(!tg_list_contains(g->gc2.tg_list, &ctx.tg));

  hs.g = g;
  hs.actions = LJ_GC2_HS_ALLOC_BLACK;
  hold_L = lua_newthread(L);
  assert(hold_L != NULL);
  lj_tg_init_thread(g, &hold_tg, hold_L, 0);
  lj_tg_attach(g, &hold_tg);
  /* Hold the leader handshake open on a remote TG until cur_L is restored. */
  lj_tg_store_cur_L(&hold_tg, NULL);
  epoch0 = la_load64_acq(&g->gc2.hs_epoch);
  assert(lj_thr_create(&hs_thread, handshake_main, &hs) == 0);
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

  late_L = lua_newthread(L);
  assert(late_L != NULL);
  lj_tg_init_thread(g, &late_tg, late_L, 0);
  late_tg.cur_L = late_L;
  lj_native_enter(&late_tg);
  attach_without_catchup(g, &late_tg);
  wait_late_remote_ack(&late_tg, g);
  assert(la_load64_acq(&late_tg.hs_epoch_ack) == g->gc2.hs_epoch);
  assert(la_load8_acq(&late_tg.alloc.alloc_black) == 1);
  assert(la_load32_acq(&late_tg.poll) == 0);
  assert(la_load32_acq(&late_tg.reqmask) == 0);

  lj_tg_store_cur_L(&hold_tg, hold_L);
  assert(lj_safepoint_ack(hold_L) == hs.actions);
  assert(lj_thr_join(&hs_thread, NULL) == 0);
  assert(hs.signaled == 3u);
  assert(g->gc2.hs_pending == 0);
  assert(la_load64_acq(&catch.tg.hs_epoch_ack) == g->gc2.hs_epoch);
  assert(la_load64_acq(&late_tg.hs_epoch_ack) == g->gc2.hs_epoch);
  assert(la_load64_acq(&hold_tg.hs_epoch_ack) == g->gc2.hs_epoch);

  lj_tg_detach(g, &late_tg);
  assert(lj_tg_reclaim_dead(g) == 0u);
  lj_tg_fini_thread(g, &late_tg);
  lua_pop(L, 1);

  lj_tg_detach(g, &hold_tg);
  lj_tg_fini_thread(g, &hold_tg);
  lua_pop(L, 1);

  la_store32_rel(&catch.release, 1);
  assert(lj_thr_join(&thr, &ret) == 0);
  assert(ret == (void *)(uintptr_t)0x4a);
  assert(la_load32_acq(&catch.detached) == 1u);
  assert(la_load32_acq(&g->gc2.n_threads) == 1u);
  assert(lj_tg_reclaim_dead(g) == 3u);
  assert(!tg_list_contains(g->gc2.tg_list, &catch.tg));
  assert(!tg_list_contains(g->gc2.tg_list, &late_tg));
  assert(!tg_list_contains(g->gc2.tg_list, &hold_tg));
  lua_pop(L, 1);

  lj_thr_set_tg(NULL);
  lua_pop(L, 1);
  lua_close(L);

  printf("t-thr-substrate OK: thread create/join, TG TLS, sleep verified\n");
  return 0;
}
