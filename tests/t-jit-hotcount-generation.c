/*
** Deterministic TG-local JIT hotcount reset-generation regression.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_dispatch.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"

#ifndef LJ_GC2_TEST_HELPERS
#error "t-jit-hotcount-generation requires LJ_GC2_TEST_HELPERS"
#endif

#define HOTCOUNT_TEST_GEN_SHIFT 16u
#define HOTCOUNT_TEST_VALUE_MASK UINT64_C(0xffff)
#define HOTCOUNT_TEST_WAIT 2000000u

typedef struct AttachResetCtx {
  global_State *g;
  TGState tg;
  uint32_t attached;
  uint32_t release;
  uint32_t done;
} AttachResetCtx;

static uint64_t desired_generation(global_State *g)
{
  return lj_tg_hotcount_reset_word_acq(g->main_tg) >>
    HOTCOUNT_TEST_GEN_SHIFT;
}

static HotCount desired_value(global_State *g)
{
  return (HotCount)(lj_tg_hotcount_reset_word_acq(g->main_tg) &
                    HOTCOUNT_TEST_VALUE_MASK);
}

static void assert_filled(TGState *tg, HotCount value, uint64_t generation)
{
  uint32_t i;
  assert(lj_tg_hotcount_applied_generation_acq(tg) == generation);
  for (i = 0; i < HOTCOUNT_SIZE; i++)
    assert(tg->hotcount[i] == value);
}

static void clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(
    tg, (uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
  lj_tg_poll_rel(tg, 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
}

static void wait_nonzero(uint32_t *value)
{
  uint32_t i;
  for (i = 0; la_load32_acq(value) == 0 && i < HOTCOUNT_TEST_WAIT; i++)
    (void)lj_thr_retry_yield(NULL);
  assert(la_load32_acq(value) != 0);
}

static void wait_attach_pause(void)
{
  uint32_t i;
  for (i = 0;
       lj_tg_test_hotcount_attach_paused() == 0 && i < HOTCOUNT_TEST_WAIT;
       i++)
    (void)lj_thr_retry_yield(NULL);
  assert(lj_tg_test_hotcount_attach_paused() != 0);
}

static void *attach_reset_thread(void *arg)
{
  AttachResetCtx *ctx = (AttachResetCtx *)arg;
  lj_thr_set_tg(&ctx->tg);
  lj_tg_attach(ctx->g, &ctx->tg);
  la_store32_rel(&ctx->attached, 1);
  while (la_load32_acq(&ctx->release) == 0)
    (void)lj_thr_retry_yield(NULL);
  lj_tg_detach(ctx->g, &ctx->tg);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void test_attach_generation_window(global_State *g, jit_State *J)
{
  AttachResetCtx ctx;
  pthread_t thread;
  uint64_t before, published;
  HotCount value;

  ctx.g = g;
  ctx.attached = 0;
  ctx.release = 0;
  ctx.done = 0;
  lj_tg_init_thread(g, &ctx.tg, NULL, 0);
  lj_tg_tid_rel(&ctx.tg, lj_thr_newid());

  before = desired_generation(g);
  lj_tg_test_hotcount_attach_pause(1);
  assert(pthread_create(&thread, NULL, attach_reset_thread, &ctx) == 0);
  wait_attach_pause();

  /* The TG has completed its pre-list catch-up but is not reachable from the
  ** handshake's mandatory legacy list. Publish and complete a reset now. */
  jit_param_rel(J, JIT_P_hotloop, 23);
  published = lj_dispatch_hotcount_publish(g);
  assert(published == before + 1u);
  value = (HotCount)(23u * HOTCOUNT_LOOP - 1u);
  assert(lj_gc2_handshake(g, LJ_GC2_HS_RESET_HOTCOUNT) == 1u);

  lj_tg_test_hotcount_attach_pause(0);
  wait_nonzero(&ctx.attached);
  /* The post-list-CAS recheck, not the completed handshake above, must apply
  ** this generation to the newly visible TG. */
  assert_filled(&ctx.tg, value, published);
  assert(gc2_hs_pending_acq(g) == 0);

  la_store32_rel(&ctx.release, 1);
  wait_nonzero(&ctx.done);
  assert(pthread_join(thread, NULL) == 0);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(lj_tg_fini_thread(g, &ctx.tg));
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  jit_State *J;
  TGState *main_tg;
  TGState peer;
  BCIns pc[HOTCOUNT_SIZE + 1u];
  const BCIns *pc0 = &pc[0];
  const BCIns *pc1 = &pc[HOTCOUNT_SIZE];
  uint32_t bucket0, bucket1, i;
  uint64_t generation, next;
  HotCount value;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  J = G2J(g);
  main_tg = L->tg_hint;
  assert(main_tg != NULL && main_tg == g->main_tg);

  generation = desired_generation(g);
  value = desired_value(g);
  assert(generation != 0);
  assert_filled(main_tg, value, generation);
  assert(gc2_hs_pending_acq(g) == 0);

  /* A full runtime reset may mutate only TG buckets. GG.hotcount is retained
  ** as layout/bootstrap storage and must stay untouched. */
  for (i = 0; i < HOTCOUNT_SIZE; i++)
    G2GG(g)->hotcount[i] = (HotCount)(0x7100u + i);
  jit_param_rel(J, JIT_P_hotloop, 7);
  lj_dispatch_init_hotcount(g);
  generation++;
  value = (HotCount)(7u * HOTCOUNT_LOOP - 1u);
  assert(desired_generation(g) == generation);
  assert_filled(main_tg, value, generation);
  for (i = 0; i < HOTCOUNT_SIZE; i++)
    assert(G2GG(g)->hotcount[i] == (HotCount)(0x7100u + i));

  /* Two different PCs deliberately collide in the hashed table. The runtime
  ** retry helper must update exactly the current L's TG-local bucket. */
  bucket0 = (u32ptr(pc0) >> 2) & (HOTCOUNT_SIZE - 1u);
  bucket1 = (u32ptr(pc1) >> 2) & (HOTCOUNT_SIZE - 1u);
  assert(bucket0 == bucket1);
  assert(hotcount_setl(g, L, pc0, 3));
  assert(main_tg->hotcount[bucket0] == 3u);
  assert(G2GG(g)->hotcount[bucket0] == (HotCount)(0x7100u + bucket0));

  /* A parked peer may be filled by the remote leader. A bare foreign apply
  ** lacks that certificate and must leave both buckets and generation alone. */
  lj_tg_init_thread(g, &peer, NULL, 0);
  lj_tg_tid_rel(&peer, lj_thr_newid());
  lj_native_enter(&peer);
  lj_tg_attach(g, &peer);
  assert(gc2_n_threads_acq(g) == 2u);
  for (i = 0; i < HOTCOUNT_SIZE; i++)
    peer.hotcount[i] = (HotCount)(0x3300u + i);
  {
    uint64_t peer_applied =
      lj_tg_hotcount_applied_generation_acq(&peer);
    jit_param_rel(J, JIT_P_hotloop, 11);
    next = lj_dispatch_hotcount_publish(g);
    assert(next == generation + 1u);
    lj_safepoint_apply_tg(g, &peer, LJ_GC2_HS_RESET_HOTCOUNT);
    assert(lj_tg_hotcount_applied_generation_acq(&peer) == peer_applied);
    for (i = 0; i < HOTCOUNT_SIZE; i++)
      assert(peer.hotcount[i] == (HotCount)(0x3300u + i));
  }
  value = (HotCount)(11u * HOTCOUNT_LOOP - 1u);
  assert(lj_gc2_handshake(g, LJ_GC2_HS_RESET_HOTCOUNT) == 2u);
  generation = next;
  assert_filled(main_tg, value, generation);
  assert_filled(&peer, value, generation);
  assert(gc2_hs_pending_acq(g) == 0);

  /* Runtime collision writes are isolated even when another live TG hashes the
  ** same PC. */
  peer.hotcount[bucket0] = 99u;
  assert(hotcount_setl(g, L, pc1, 5));
  assert(main_tg->hotcount[bucket0] == 5u);
  assert(peer.hotcount[bucket0] == 99u);

  /* The real engine off->on path must publish the mode first and then drive
  ** one REDISPATCH|RESET_HOTCOUNT boundary across both TGs. Turning off does
  ** not reset, while turning back on advances exactly one generation. */
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_OFF|LUAJIT_MODE_ENGINE) == 1);
  assert(desired_generation(g) == generation);
  for (i = 0; i < HOTCOUNT_SIZE; i++) {
    main_tg->hotcount[i] = (HotCount)(0x4400u + i);
    peer.hotcount[i] = (HotCount)(0x5500u + i);
  }
  jit_param_rel(J, JIT_P_hotloop, 13);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ON|LUAJIT_MODE_ENGINE) == 1);
  generation++;
  value = (HotCount)(13u * HOTCOUNT_LOOP - 1u);
  assert(desired_generation(g) == generation);
  assert_filled(main_tg, value, generation);
  assert_filled(&peer, value, generation);
  for (i = 0; i < HOTCOUNT_SIZE; i++)
    assert(G2GG(g)->hotcount[i] == (HotCount)(0x7100u + i));

  /* Prototype/function enable and scoped flush retain their bytecode/trace
  ** semantics without manufacturing a universe-wide reset generation. */
  assert(luaL_dostring(L,
    "local f = function(x) return x + 1 end "
    "jit.off(f); jit.on(f); jit.flush(f)") == 0);
  assert(desired_generation(g) == generation);

  /* STOPREQ composition must retain both semantics: every TG applies the new
  ** generation before hs_pending reaches zero, then keeps its one-shot stop
  ** edge independently armed. */
  jit_param_rel(J, JIT_P_hotloop, 17);
  next = lj_dispatch_hotcount_publish(g);
  value = (HotCount)(17u * HOTCOUNT_LOOP - 1u);
  assert(lj_gc2_handshake(g,
    LJ_GC2_HS_RESET_HOTCOUNT|LJ_GC2_HS_STOPREQ) == 2u);
  assert(gc2_hs_pending_acq(g) == 0);
  assert_filled(main_tg, value, next);
  assert_filled(&peer, value, next);
  assert(lj_tg_flags_all_acq(main_tg, TGF_STOPREQ|TGF_STOPREQ_FRESH));
  assert(lj_tg_flags_all_acq(&peer, TGF_STOPREQ|TGF_STOPREQ_FRESH));
  clear_stopreq(main_tg);
  clear_stopreq(&peer);

  lj_tg_detach(g, &peer);
  assert(gc2_n_threads_acq(g) == 1u);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(lj_tg_fini_thread(g, &peer));

  test_attach_generation_window(g, J);
  lua_close(L);
  printf("t-jit-hotcount-generation OK: TG-local generations and attach closure verified\n");
  return 0;
}
