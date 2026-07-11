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
  TGLeaseCtx ctx = {0};
  pthread_t reader;
  uint32_t live0;

  assert(L != NULL);
  g = G(L);
  main_tg = G2TG(g);
  assert(main_tg != NULL);
  lj_thr_set_tg(main_tg);

  live0 = gc2_n_threads_acq(g);
  lj_tg_init_thread(g, &dead_tg, NULL, 0);
  lj_tg_tid_rel(&dead_tg, lj_thr_newid());
  dead_tg.alloc.owner_tid = lj_tg_tid_acq(&dead_tg);
  lj_tg_attach(g, &dead_tg);
  assert(gc2_n_threads_acq(g) == live0 + 1u);
  assert(tg_list_contains(g, &dead_tg));

  lj_tg_detach(g, &dead_tg);
  assert(gc2_n_threads_acq(g) == live0);
  assert(lj_tg_flags_test_acq(&dead_tg, TGF_DEAD));

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

  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(!tg_list_contains(g, &dead_tg));
  assert(lj_tg_find_owner(g, lj_tg_tid_acq(&dead_tg)) != &dead_tg);
  lj_tg_fini_thread(g, &dead_tg);

  lj_thr_set_tg(NULL);
  lua_close(L);
  printf("t-tg-registry-lease OK: reader lease defers TG reclamation\n");
  return 0;
}
