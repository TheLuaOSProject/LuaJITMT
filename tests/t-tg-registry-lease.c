/*
** Deterministic TG-registry reader/reclaimer exclusion regression.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_thr.h"
#include "lj_tg.h"

typedef struct TGLeaseCtx {
  global_State *g;
  TGState *target;
  uint32_t entered;
  uint32_t writer_probed;
  uint32_t validated;
} TGLeaseCtx;

static void wait_flag(uint32_t *flag)
{
  while (la_load32_acq(flag) == 0)
    (void)lj_thr_retry_yield(NULL);
}

static int tg_list_contains(global_State *g, TGState *target)
{
  TGState *tg;
  uint32_t n = 0;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (tg == target)
      return 1;
    assert(tg != lj_tg_next_acq(tg));
    assert(++n < 1000000u);
  }
  return 0;
}

static int stable_list_contains(global_State *g, LJTGRegistrySlot *target)
{
  LJTGRegistrySlot *slot;
  uint32_t n = 0;
  for (slot = gc2_tg_registry_head_acq(g); slot != NULL;
       slot = lj_tgregistry_slot_next_all(slot)) {
    if (slot == target)
      return 1;
    assert(slot != lj_tgregistry_slot_next_all(slot));
    assert(++n < 1000000u);
  }
  return 0;
}

static void release_stable_borrow(LJTGRegistryBorrow *borrow)
{
  for (;;) {
    LJTGSlotResult result = lj_tgregistry_try_release(borrow, NULL);
    if (result == LJ_TGSLOT_OK)
      return;
    assert(result == LJ_TGSLOT_LOST);
  }
}

static void *reader_main(void *arg)
{
  TGLeaseCtx *ctx = (TGLeaseCtx *)arg;
  uint32_t tid;

  assert(lj_gc2_smr_read_try(ctx->g));
  assert(tg_list_contains(ctx->g, ctx->target));
  tid = lj_tg_tid_acq(ctx->target);
  assert(tid != 0 && tid != LJ_THREAD_GCSCAN);
  assert(lj_tg_flags_test_acq(ctx->target, TGF_DEAD));
  la_store32_rel(&ctx->entered, 1);

  /* The writer attempt must return without invalidating the admitted node. */
  wait_flag(&ctx->writer_probed);
  assert(tg_list_contains(ctx->g, ctx->target));
  assert(lj_tg_tid_acq(ctx->target) == tid);
  assert(lj_tg_flags_test_acq(ctx->target, TGF_DEAD));
  la_store32_rel(&ctx->validated, 1);
  lj_gc2_smr_read_leave(ctx->g);
  return NULL;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *main_tg;
  TGState dead_tg;
  TGState missed_tg;
  LJTGRegistryKey dead_key;
  LJTGRegistryBorrow stable_borrow;
  LJTGRegistryBodySnap body;
  LJTGSlotSnap snap;
  TGLeaseCtx ctx = {0};
  pthread_t reader;
  uint32_t failures0, live0, stable_nodes0;

  assert(L != NULL);
  g = G(L);
  main_tg = G2TG(g);
  assert(main_tg != NULL);
  lj_thr_set_tg(main_tg);
  assert(lj_tgregistry_key_valid(&main_tg->registry_key));
  assert(lj_tgregistry_key_snapshot(&main_tg->registry_key, &snap) ==
	 LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_LIVE && snap.lease_count == 1u);
  assert(stable_list_contains(g, main_tg->registry_key.slot));

  live0 = gc2_n_threads_acq(g);
  stable_nodes0 = gc2_tg_registry_nodes_acq(g);
  lj_tg_init_thread(g, &dead_tg, NULL, 0);
  lj_tg_tid_rel(&dead_tg, lj_thr_newid());
  dead_tg.alloc.owner_tid = lj_tg_tid_acq(&dead_tg);
  lj_tg_attach(g, &dead_tg);
  assert(gc2_n_threads_acq(g) == live0 + 1u);
  assert(tg_list_contains(g, &dead_tg));
  dead_key = dead_tg.registry_key;
  assert(lj_tgregistry_key_valid(&dead_key));
  assert(stable_list_contains(g, dead_key.slot));
  assert(gc2_tg_registry_nodes_acq(g) == stable_nodes0 + 1u);
  assert(lj_tgregistry_key_snapshot(&dead_key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_LIVE && snap.lease_count == 1u);
  body = lj_tgregistry_slot_body_snapshot(dead_key.slot);
  assert(body.body == &dead_tg && body.incarnation == dead_key.incarnation);
  lj_tgregistry_borrow_init(&stable_borrow);
  assert(lj_tgregistry_try_borrow(&dead_key, &stable_borrow, &snap) ==
	 LJ_TGSLOT_OK);
  assert(stable_borrow.body == &dead_tg);

  lj_tg_detach(g, &dead_tg);
  assert(gc2_n_threads_acq(g) == live0);
  assert(lj_tg_flags_test_acq(&dead_tg, TGF_DEAD));
  assert(lj_tgregistry_key_snapshot(&dead_key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_RETIRED && snap.lease_count == 2u);

  ctx.g = g;
  ctx.target = &dead_tg;
  assert(pthread_create(&reader, NULL, reader_main, &ctx) == 0);
  wait_flag(&ctx.entered);
  assert(gc2_smr_readers_acq(g) == 1u);

  /* Reclamation is opportunistic: it must fail instead of waiting for the
  ** reader. The reader intentionally cannot leave until this call returns. */
  assert(lj_tg_reclaim_dead(g) == 0u);
  assert(tg_list_contains(g, &dead_tg));
  la_store32_rel(&ctx.writer_probed, 1);
  wait_flag(&ctx.validated);
  assert(pthread_join(reader, NULL) == 0);
  assert(gc2_smr_readers_acq(g) == 0u);

  /* The legacy writer gates now succeed, but the separately admitted stable
  ** lease is an additional negative veto. No token state is positive reclaim
  ** authority on its own. */
  assert(lj_tg_reclaim_dead(g) == 0u);
  assert(tg_list_contains(g, &dead_tg));
  release_stable_borrow(&stable_borrow);
  assert(lj_tgregistry_key_snapshot(&dead_key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_RETIRED && snap.lease_count == 1u);

  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(!tg_list_contains(g, &dead_tg));
  assert(lj_tg_find_owner(g, lj_tg_tid_acq(&dead_tg)) != &dead_tg);
  assert(stable_list_contains(g, dead_key.slot));
  assert(lj_tgregistry_key_snapshot(&dead_key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_EMPTY && snap.lease_count == 0u);
  body = lj_tgregistry_slot_body_snapshot(dead_key.slot);
  assert(body.body == NULL && body.incarnation == dead_key.incarnation);
  lj_tg_fini_thread(g, &dead_tg);

  /* Slot OOM is a shadow-publication failure, not an API failure. The TG must
  ** continue through the unchanged legacy attach/detach/reclaim path while a
  ** permanent universe veto prevents future stable authority from pretending
  ** the spine is complete. */
  stable_nodes0 = gc2_tg_registry_nodes_acq(g);
  failures0 = gc2_tg_registry_alloc_failures_acq(g);
  lj_tg_init_thread(g, &missed_tg, NULL, 0);
  lj_tg_tid_rel(&missed_tg, lj_thr_newid());
  missed_tg.alloc.owner_tid = lj_tg_tid_acq(&missed_tg);
  gc2_tg_registry_test_fail_alloc_rel(g, 1);
  lj_tg_attach(g, &missed_tg);
  assert(gc2_tg_registry_test_fail_alloc_acq(g) == 0);
  assert(tg_list_contains(g, &missed_tg));
  assert(gc2_n_threads_acq(g) == live0 + 1u);
  assert(lj_tg_registry_shadow_missed_acq(&missed_tg));
  assert(!lj_tgregistry_key_valid(&missed_tg.registry_key));
  assert(gc2_tg_registry_incomplete_acq(g) == 1u);
  assert(gc2_tg_registry_alloc_failures_acq(g) == failures0 + 1u);
  assert(gc2_tg_registry_nodes_acq(g) == stable_nodes0);
  lj_tg_detach(g, &missed_tg);
  assert(gc2_n_threads_acq(g) == live0);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(!tg_list_contains(g, &missed_tg));
  lj_tg_fini_thread(g, &missed_tg);

  /* A duplicate legacy attach may fill in the missing shadow slot. The
  ** universe-wide incomplete veto stays sticky, but this exact body must stop
  ** taking fallback detach/reclaim paths once its tagged slot is published. */
  lj_tg_init_thread(g, &missed_tg, NULL, 0);
  lj_tg_tid_rel(&missed_tg, lj_thr_newid());
  missed_tg.alloc.owner_tid = lj_tg_tid_acq(&missed_tg);
  gc2_tg_registry_test_fail_alloc_rel(g, 1);
  lj_tg_attach(g, &missed_tg);
  assert(lj_tg_registry_shadow_missed_acq(&missed_tg));
  assert(gc2_tg_registry_alloc_failures_acq(g) == failures0 + 2u);
  assert(gc2_tg_registry_nodes_acq(g) == stable_nodes0);
  lj_tg_attach(g, &missed_tg);
  assert(!lj_tg_registry_shadow_missed_acq(&missed_tg));
  assert(lj_tgregistry_key_valid(&missed_tg.registry_key));
  assert(gc2_tg_registry_nodes_acq(g) == stable_nodes0 + 1u);
  assert(gc2_n_threads_acq(g) == live0 + 1u);
  assert(lj_tgregistry_key_snapshot(&missed_tg.registry_key, &snap) ==
	 LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_LIVE && snap.lease_count == 1u);
  lj_tg_detach(g, &missed_tg);
  assert(gc2_n_threads_acq(g) == live0);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(!tg_list_contains(g, &missed_tg));
  lj_tg_fini_thread(g, &missed_tg);

  lj_thr_set_tg(NULL);
  /* Main subordinate teardown must not begin under an admitted stable borrow.
  ** The first prepare closes admission at RETIRED and returns a nonblocking
  ** veto; after release, the exact owner lease is consumed into RECLAIMING.
  ** lua_close repeats the prepare idempotently before destroying main storage. */
  lj_tgregistry_borrow_init(&stable_borrow);
  assert(lj_tgregistry_try_borrow(&main_tg->registry_key, &stable_borrow,
	 &snap) == LJ_TGSLOT_OK);
  assert(!lj_tg_registry_main_close_begin(g));
  assert(lj_tgregistry_key_snapshot(&main_tg->registry_key, &snap) ==
	 LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_RETIRED && snap.lease_count == 2u);
  release_stable_borrow(&stable_borrow);
  assert(lj_tg_registry_main_close_begin(g));
  assert(lj_tgregistry_key_snapshot(&main_tg->registry_key, &snap) ==
	 LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_RECLAIMING && snap.lease_count == 0u);
  lua_close(L);
  printf("t-tg-registry-lease OK: dual publication and leases defer TG reclamation\n");
  return 0;
}
