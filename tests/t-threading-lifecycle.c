/*
** Focused regression test for foreign TG/state teardown lifetime barriers.
**
** Linker wrappers stop at the two otherwise very narrow lifecycle boundaries:
** immediately before a detacher releases its state, and immediately after a
** provisional TG has registered but then claimed its state. This makes both
** races deterministic without allowing lua_close() to free storage still used
** by the fixture.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_vm.h"

#include "lib/test_sleep.h"

enum {
  RELEASE_NONE,
  RELEASE_DETACH,
  RELEASE_CLOSE
};

typedef struct LifecycleCtx {
  lua_State *L;
  uint32_t ready;
  uint32_t go;
  uint32_t done;
  int attached;
} LifecycleCtx;

typedef struct ReclaimCtx {
  global_State *g;
  uint32_t reclaimed;
  uint32_t started;
  uint32_t done;
} ReclaimCtx;

static global_State *test_g;
static lua_State *target_L;
static uint32_t release_mode;
static uint32_t release_entered;
static uint32_t release_gate;
static uint32_t attach_paused;
static uint32_t provisional_seen;
static uint32_t first_cpcall_owned;
static uint32_t shutdown_returned;
static uint32_t close_escaped_cleanup;
static uint32_t close_worker_done;
static LifecycleCtx *close_ctx;
static TGState *reclaim_victim;
static uint32_t transfer_arrivals;
static uint32_t transfer_release;
static uint32_t transfer_overlap;
static uint32_t transfer_real_claimed;
static uint32_t transfer_real_done;
static uint32_t transfer_result;

extern void __real_lj_state_release(lua_State *L, uint32_t tid);
extern int __real_lj_state_claim(lua_State *L, uint32_t tid);
extern void __real_lj_tg_attach(global_State *g, TGState *tg);
extern int __real_lj_vm_cpcall(lua_State *L, lua_CFunction func, void *ud,
                               lua_CPFunction cp);
extern void __real_lj_threading_shutdown(lua_State *L);
extern uint32_t __real_lj_arena_alloc_transfer(TGAlloc *dst, TGAlloc *src);

static int closing_panic(lua_State *L)
{
  UNUSED(L);
  assert(close_ctx != NULL);
  close_ctx->attached = -1;
  la_store32_rel(&close_ctx->done, 1);
  la_store32_rel(&close_worker_done, 1);
  pthread_exit(NULL);
  return 0;
}

static void wait_flag(uint32_t *flag)
{
  int i;
  for (i = 0; i < 5000 && la_load32_acq(flag) == 0; i++)
    sleep_ns(1000000L);
  assert(la_load32_acq(flag) != 0);
}

void __wrap_lj_state_release(lua_State *L, uint32_t tid)
{
  uint32_t mode = la_load32_acq(&release_mode);
  if (L == target_L && mode != RELEASE_NONE) {
    la_store32_rel(&release_entered, 1);
    if (mode == RELEASE_DETACH) {
      wait_flag(&release_gate);
    } else {
      int i;
      /* If shutdown is protected by an additional lifetime gate, it must not
      ** return while this state release is outstanding. The current live and
      ** entering counters are a fast path; the bounded wait also accepts a
      ** future, equivalent close barrier. */
      if (mt_live_acq(test_g) == 0 && mt_entering_acq(test_g) == 0) {
        for (i = 0; i < 1000 &&
                    la_load32_acq(&shutdown_returned) == 0; i++)
          sleep_ns(1000000L);
      }
      if (la_load32_acq(&shutdown_returned) != 0)
        la_store32_rel(&close_escaped_cleanup, 1);
    }
  }
  __real_lj_state_release(L, tid);
}

void __wrap_lj_tg_attach(global_State *g, TGState *tg)
{
  __real_lj_tg_attach(g, tg);
  if (la_load32_acq(&release_mode) == RELEASE_CLOSE &&
      lj_thr_get_tg() == tg && lj_tg_load_thread_L(tg) == NULL) {
    assert(lj_tg_find_owner(g, lj_tg_tid_acq(tg)) == tg);
    la_store32_rel(&provisional_seen, 1);
  }
}

int __wrap_lj_state_claim(lua_State *L, uint32_t tid)
{
  int claimed = __real_lj_state_claim(L, tid);
  if (claimed && L == target_L &&
      la_load32_acq(&release_mode) == RELEASE_CLOSE) {
    TGState *tg = lj_thr_get_tg();
    assert(la_load32_acq(&provisional_seen) != 0);
    assert(tg != NULL && lj_tg_tid_acq(tg) == tid);
    assert(lj_tg_find_owner(G(L), tid) == tg);
  }
  return claimed;
}

int __wrap_lj_vm_cpcall(lua_State *L, lua_CFunction func, void *ud,
                        lua_CPFunction cp)
{
  if (L == target_L &&
      la_load32_acq(&release_mode) == RELEASE_CLOSE) {
    TGState *tg = lj_thr_get_tg();
    assert(tg != NULL && lj_state_owner_acq(L) == lj_tg_tid_acq(tg));
    assert(lj_tg_find_owner(G(L), lj_tg_tid_acq(tg)) == tg);
    la_store32_rel(&first_cpcall_owned, 1);
    la_store32_rel(&attach_paused, 1);
    while (mt_shutdown_acq(G(L)) == 0) {
      if (lj_tg_reqmask_acq(tg) != 0 || lj_tg_poll_acq(tg) != 0)
        (void)lj_safepoint_poll_tg(tg);
      sleep_ns(100000L);
    }
  }
  return __real_lj_vm_cpcall(L, func, ud, cp);
}

void __wrap_lj_threading_shutdown(lua_State *L)
{
  __real_lj_threading_shutdown(L);
  la_store32_rel(&shutdown_returned, 1);
  wait_flag(&close_worker_done);
  /* Keep later close_state work out of the state-release wrapper. */
  la_store32_rel(&release_mode, RELEASE_NONE);
}

uint32_t __wrap_lj_arena_alloc_transfer(TGAlloc *dst, TGAlloc *src)
{
  if (reclaim_victim != NULL && src == &reclaim_victim->alloc) {
    uint32_t arrival = la_add32_rlx(&transfer_arrivals, 1) + 1u;
    uint32_t expect = 0;
    if (arrival >= 2) {
      la_store32_rel(&transfer_overlap, 1);
      /* Under an un-serialized implementation the controller also reaches
      ** this victim. Let that second arrival release the paused main writer,
      ** so the fixture observes the double count without deadlocking. */
      la_store32_rel(&transfer_release, 1);
    }
    wait_flag(&transfer_release);
    if (la_cas32(&transfer_real_claimed, &expect, 1,
                 LA_ACQ_REL, LA_ACQ)) {
      uint32_t result = __real_lj_arena_alloc_transfer(dst, src);
      la_store32_rel(&transfer_result, result);
      la_store32_rel(&transfer_real_done, 1);
      return result;
    }
    wait_flag(&transfer_real_done);
    return la_load32_acq(&transfer_result);
  }
  return __real_lj_arena_alloc_transfer(dst, src);
}

static void *detach_worker(void *arg)
{
  LifecycleCtx *ctx = (LifecycleCtx *)arg;
  ctx->attached = lj_threading_attach(ctx->L);
  la_store32_rel(&ctx->ready, 1);
  if (!ctx->attached) {
    la_store32_rel(&ctx->done, 1);
    return NULL;
  }
  wait_flag(&ctx->go);
  lj_threading_detach(ctx->L, 1);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void *closing_attach_worker(void *arg)
{
  LifecycleCtx *ctx = (LifecycleCtx *)arg;
  ctx->attached = lj_threading_attach(ctx->L);
  la_store32_rel(&ctx->done, 1);
  la_store32_rel(&close_worker_done, 1);
  return NULL;
}

static void *reclaim_worker(void *arg)
{
  ReclaimCtx *ctx = (ReclaimCtx *)arg;
  la_store32_rel(&ctx->started, 1);
  wait_flag(&transfer_arrivals);
  ctx->reclaimed = lj_tg_reclaim_dead(ctx->g);
  /* A rejected controller is responsible for releasing the runtime-main
  ** writer. With the old missing gate, its second transfer arrival already
  ** releases the same latch above. */
  la_store32_rel(&transfer_release, 1);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static int test_concurrent_reclaim(global_State *g, uint32_t *sum_out)
{
  TGState pinned, victim;
  ReclaimCtx ctx = {g, 0, 0, 0};
  pthread_t thread;
  int arena_internal = lj_tg_flags_test_acq(g->main_tg,
                                             TGF_ARENA_INTERNAL);
  uint32_t main_reclaimed;

  lj_tg_init_thread(g, &victim, NULL, arena_internal);
  lj_tg_init_thread(g, &pinned, NULL, arena_internal);
  lj_tg_attach(g, &victim);
  lj_tg_attach(g, &pinned);
  (void)lj_tg_ssb_refs_add(&pinned, 1);
  lj_tg_detach(g, &victim);
  lj_tg_detach(g, &pinned);
  assert(gc2_n_threads_acq(g) == 1);

  reclaim_victim = &victim;
  la_store32_rel(&transfer_arrivals, 0);
  la_store32_rel(&transfer_release, 0);
  la_store32_rel(&transfer_overlap, 0);
  la_store32_rel(&transfer_real_claimed, 0);
  la_store32_rel(&transfer_real_done, 0);
  la_store32_rel(&transfer_result, 0);

  assert(pthread_create(&thread, NULL, reclaim_worker, &ctx) == 0);
  wait_flag(&ctx.started);
  /* Registry mutation is runtime-main-only. This call pauses at the victim;
  ** the controller must be rejected and then release its transfer latch. */
  main_reclaimed = lj_tg_reclaim_dead(g);
  assert(pthread_join(thread, NULL) == 0);
  reclaim_victim = NULL;

  *sum_out = main_reclaimed + ctx.reclaimed;
  assert(lj_tg_ssb_refs_sub(&pinned, 1) == 1);
  assert(lj_tg_reclaim_dead(g) == 1);
  lj_tg_fini_thread(g, &victim);
  lj_tg_fini_thread(g, &pinned);
  return la_load32_acq(&transfer_overlap) != 0;
}

static void reset_wrapper_state(uint32_t mode, lua_State *L)
{
  target_L = L;
  la_store32_rel(&release_entered, 0);
  la_store32_rel(&release_gate, 0);
  la_store32_rel(&attach_paused, 0);
  la_store32_rel(&provisional_seen, 0);
  la_store32_rel(&first_cpcall_owned, 0);
  la_store32_rel(&shutdown_returned, 0);
  la_store32_rel(&close_escaped_cleanup, 0);
  la_store32_rel(&close_worker_done, 0);
  la_store32_rel(&release_mode, mode);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  LifecycleCtx detach = {0};
  LifecycleCtx closing = {0};
  pthread_t thread;
  uint32_t early_reclaimed, final_reclaimed;
  uint32_t concurrent_reclaimed;
  int concurrent_overlap;

  assert(L != NULL);
  test_g = G(L);
  lua_atpanic(L, closing_panic);

  detach.L = lua_newthread(L);
  assert(detach.L != NULL);
  reset_wrapper_state(RELEASE_DETACH, detach.L);
  assert(pthread_create(&thread, NULL, detach_worker, &detach) == 0);
  wait_flag(&detach.ready);
  assert(detach.attached == 1);
  la_store32_rel(&detach.go, 1);
  wait_flag(&release_entered);

  /* A dead foreign TG is still part of the detacher's state-release lifetime.
  ** Reclamation must remain deferred even though n_threads is already one. */
  early_reclaimed = lj_tg_reclaim_dead(test_g);
  la_store32_rel(&release_gate, 1);
  assert(pthread_join(thread, NULL) == 0);
  assert(la_load32_acq(&detach.done) != 0);
  final_reclaimed = lj_tg_reclaim_dead(test_g);
  lua_settop(L, 0);

  la_store32_rel(&release_mode, RELEASE_NONE);
  concurrent_overlap = test_concurrent_reclaim(test_g,
                                                &concurrent_reclaimed);

  closing.L = lua_newthread(L);
  assert(closing.L != NULL);
  close_ctx = &closing;
  reset_wrapper_state(RELEASE_CLOSE, closing.L);
  assert(pthread_create(&thread, NULL, closing_attach_worker, &closing) == 0);
  wait_flag(&attach_paused);

  /* The cpcall wrapper returns only after shutdown is published, forcing the
  ** unsuccessful-attach cleanup path while lua_close() waits. */
  lua_close(L);
  assert(pthread_join(thread, NULL) == 0);

  if (early_reclaimed != 0 || final_reclaimed != 1 || concurrent_overlap ||
      concurrent_reclaimed != 1 ||
      closing.attached > 0 || la_load32_acq(&release_entered) == 0 ||
      la_load32_acq(&provisional_seen) == 0 ||
      la_load32_acq(&first_cpcall_owned) == 0 ||
      la_load32_acq(&close_escaped_cleanup) != 0) {
    fprintf(stderr, "lifecycle observations: early_reclaimed=%u, "
            "final_reclaimed=%u, concurrent_overlap=%d, "
            "concurrent_reclaimed=%u, close_attach=%d, "
            "release_entered=%u, provisional=%u, first_cpcall_owned=%u, "
            "close_escaped=%u\n", early_reclaimed,
            final_reclaimed, concurrent_overlap, concurrent_reclaimed,
            closing.attached, la_load32_acq(&release_entered),
            la_load32_acq(&provisional_seen),
            la_load32_acq(&first_cpcall_owned),
            la_load32_acq(&close_escaped_cleanup));
    assert(0);
  }

  printf("t-threading-lifecycle OK: detach reclamation and close/attach "
         "cleanup barriers verified\n");
  return 0;
}
