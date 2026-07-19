/*
** Physical OS-actor authority for TG and lua_State ownership.
**
** This fixture deliberately supplies a target universe's logical tid from
** unrelated OS threads. A tid match alone must never authorize a claim. It
** also observes repeated owner claim/release cycles through the packed atomic
** word and verifies that lazily admitted actor IDs survive TLS use but are
** never reused when pthread implementations recycle their native handles.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_atomic.h"
#include "lj_obj.h"
#include "lj_tg.h"
#include "lj_thr.h"

#define ACTOR_REUSE_THREADS 32u
#define CLAIM_STRESS_ITERS 100000u
#define WAIT_SPINS 10000000u

typedef struct SpoofCtx {
  lua_State *target;
  TGState *tg;
  LJStateOwner owner_before;
  uint32_t raw_null_actor;
  uint32_t other_universe_actor;
  int raw_null_claim;
  int raw_null_tryclaim;
  int other_universe_claim;
  int other_universe_tryclaim;
} SpoofCtx;

typedef struct ActorCtx {
  uint32_t before;
  uint32_t actor;
  uint32_t again;
} ActorCtx;

typedef struct ObserveCtx {
  lua_State *co;
  LJStateOwner ownerless;
  LJStateOwner owned;
  uint32_t ready;
  uint32_t stop;
  uint32_t bad;
  uint32_t saw_owned;
  uint32_t observer_actor;
} ObserveCtx;

static void wait_flag(uint32_t *flag, const char *what)
{
  uint32_t spin;
  for (spin = 0; spin < WAIT_SPINS; spin++) {
    if (la_load32_acq(flag) != 0)
      return;
    (void)lj_thr_retry_yield(NULL);
  }
  fprintf(stderr, "timeout waiting for %s\n", what);
  assert(0);
}

static void assert_owner_pair(lua_State *L, uint32_t tid, uint32_t actor)
{
  LJStateOwner owner = lj_state_owner_word_acq(L);
  assert(lj_state_owner_tid(owner) == tid);
  assert(lj_state_owner_actor(owner) == actor);
  assert(owner == lj_state_owner_pack(tid, actor));
}

static void *spoof_thread(void *ud)
{
  SpoofCtx *ctx = (SpoofCtx *)ud;
  LJStateClaim claim;
  lua_State *other;
  uint32_t target_tid = lj_tg_tid_acq(ctx->tg);

  /* A direct pthread starts without an admitted actor or TG. Even after actor
  ** admission, knowing the target's logical tid is not physical authority. */
  assert(lj_thr_get_tg() == NULL);
  assert(lj_thr_actor_current() == 0);
  ctx->raw_null_actor = lj_thr_actor_ensure();
  assert(ctx->raw_null_actor != 0);
  assert(lj_thr_actor_current() == ctx->raw_null_actor);
  memset(&claim, 0xa5, sizeof(claim));
  ctx->raw_null_tryclaim =
    lj_state_tryclaim(ctx->target, target_tid, &claim);
  ctx->raw_null_claim = lj_state_claim(ctx->target, target_tid);
  assert(!ctx->raw_null_tryclaim && !ctx->raw_null_claim);
  assert(lj_state_owner_word_acq(ctx->target) == ctx->owner_before);
  assert(lj_thr_get_tg() == NULL);

  /* Installing an unrelated universe gives this actor a valid raw TG, but
  ** neither that raw carrier nor a forged target tid can impersonate the
  ** target universe's main actor. */
  other = luaL_newstate();
  assert(other != NULL);
  ctx->other_universe_actor = lj_thr_actor_current();
  assert(ctx->other_universe_actor == ctx->raw_null_actor);
  assert(lj_thr_get_tg() == G(other)->main_tg);
  memset(&claim, 0xa5, sizeof(claim));
  ctx->other_universe_tryclaim =
    lj_state_tryclaim(ctx->target, target_tid, &claim);
  ctx->other_universe_claim = lj_state_claim(ctx->target, target_tid);
  assert(!ctx->other_universe_tryclaim && !ctx->other_universe_claim);
  assert(lj_state_owner_word_acq(ctx->target) == ctx->owner_before);
  lua_close(other);
  assert(lj_thr_get_tg() == NULL);
  assert(lj_thr_actor_current() == ctx->raw_null_actor);
  return NULL;
}

static void *actor_thread(void *ud)
{
  ActorCtx *ctx = (ActorCtx *)ud;
  ctx->before = lj_thr_actor_current();
  ctx->actor = lj_thr_actor_ensure();
  ctx->again = lj_thr_actor_ensure();
  assert(ctx->before == 0);
  assert(ctx->actor != 0 && ctx->again == ctx->actor);
  assert(lj_thr_actor_current() == ctx->actor);
  assert(lj_thr_get_tg() == NULL);
  return NULL;
}

static void *owner_observer(void *ud)
{
  ObserveCtx *ctx = (ObserveCtx *)ud;
  ctx->observer_actor = lj_thr_actor_ensure();
  assert(ctx->observer_actor != 0);
  la_store32_rel(&ctx->ready, 1);
  while (la_load32_acq(&ctx->stop) == 0) {
    LJStateOwner owner = lj_state_owner_word_acq(ctx->co);
    if (owner == ctx->owned) {
      la_store32_rel(&ctx->saw_owned, 1);
    } else if (owner != ctx->ownerless) {
      uint32_t tid = lj_state_owner_tid(owner);
      uint32_t actor = lj_state_owner_actor(owner);
      /* Collector and release sentinels are physical-actor claims too. A
      ** background scanner may legitimately use an actor other than main. */
      if ((tid != LJ_THREAD_GCSCAN && tid != LJ_THREAD_GCPREP) || actor == 0) {
        la_store32_rel(&ctx->bad, 1);
        break;
      }
    }
    la_cpu_pause();
  }
  return NULL;
}

static void test_foreign_tid_spoof(lua_State *L)
{
  SpoofCtx ctx;
  pthread_t thread;
  uint32_t actor = lj_thr_actor_current();

  memset(&ctx, 0, sizeof(ctx));
  ctx.target = L;
  ctx.tg = G(L)->main_tg;
  ctx.owner_before = lj_state_owner_word_acq(L);
  assert(actor != 0 && lj_tg_actor_acq(ctx.tg) == actor);
  assert_owner_pair(L, lj_tg_tid_acq(ctx.tg), actor);

  assert(pthread_create(&thread, NULL, spoof_thread, &ctx) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.raw_null_actor != 0 &&
         ctx.other_universe_actor == ctx.raw_null_actor &&
         ctx.raw_null_actor != actor);
  assert(!ctx.raw_null_claim && !ctx.raw_null_tryclaim &&
         !ctx.other_universe_claim && !ctx.other_universe_tryclaim);
  assert(lj_state_owner_word_acq(L) == ctx.owner_before);
}

static void test_actor_tls_nonreuse(uint32_t main_actor)
{
  ActorCtx ctx[ACTOR_REUSE_THREADS];
  uint32_t i, j;
  memset(ctx, 0, sizeof(ctx));

  /* Sequential creation encourages pthread_t/kernel-thread reuse. Actor IDs
  ** are process-lifetime incarnations and therefore must still be unique. */
  for (i = 0; i < ACTOR_REUSE_THREADS; i++) {
    pthread_t thread;
    assert(pthread_create(&thread, NULL, actor_thread, &ctx[i]) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(ctx[i].before == 0 && ctx[i].actor == ctx[i].again);
    assert(ctx[i].actor != main_actor);
    for (j = 0; j < i; j++)
      assert(ctx[i].actor != ctx[j].actor);
  }
}

static void test_atomic_claim_release(lua_State *L)
{
  TGState *tg = G(L)->main_tg;
  lua_State *co = lua_newthread(L);
  uint32_t actor = lj_thr_actor_current();
  uint32_t tid = lj_tg_tid_acq(tg);
  ObserveCtx ctx;
  pthread_t observer;
  uint32_t i;

  assert(co != NULL && actor != 0 && lj_tg_actor_acq(tg) == actor);
  memset(&ctx, 0, sizeof(ctx));
  ctx.co = co;
  ctx.ownerless = lj_state_owner_pack(0, 0);
  ctx.owned = lj_state_owner_pack(tid, actor);
  assert(lj_state_owner_word_acq(co) == ctx.ownerless);

  assert(pthread_create(&observer, NULL, owner_observer, &ctx) == 0);
  wait_flag(&ctx.ready, "owner observer start");

  /* Hold one claim until the observer has definitely sampled the complete
  ** normal pair. It must never observe tid with actor zero or vice versa. */
  {
    LJStateClaim claim;
    assert(lj_state_tryclaim(co, tid, &claim));
    assert(lj_state_owner_word_acq(co) == ctx.owned);
    wait_flag(&ctx.saw_owned, "packed owned state");
    lj_state_dropclaim(&claim);
    assert(lj_state_owner_word_acq(co) == ctx.ownerless);
  }

  for (i = 0; i < CLAIM_STRESS_ITERS; i++) {
    LJStateClaim claim;
    assert(lj_state_tryclaim(co, tid, &claim));
    assert(lj_state_owner_word_acq(co) == ctx.owned);
    lj_state_dropclaim(&claim);
    assert(lj_state_owner_word_acq(co) == ctx.ownerless);
    if ((i & 255u) == 0)
      (void)lj_thr_retry_yield(NULL);
  }
  la_store32_rel(&ctx.stop, 1);
  assert(pthread_join(observer, NULL) == 0);
  assert(ctx.observer_actor != 0 && ctx.observer_actor != actor);
  assert(la_load32_acq(&ctx.bad) == 0);
  assert(lj_state_owner_word_acq(co) == ctx.ownerless);
  lua_pop(L, 1);  /* coroutine root */
}

static void test_detach_terminal_actor(lua_State *L)
{
  global_State *g = G(L);
  TGState extra;
  lua_State *co = lua_newthread(L);
  uint32_t actor = lj_thr_actor_current();
  uint32_t tid = lj_thr_newid();

  assert(co != NULL && actor != 0 && lj_thr_id_is_owner(tid));
  lj_tg_init_thread(g, &extra, co, 0);
  lj_tg_tid_rel(&extra, tid);
  lj_tg_derive_prng(g, &extra, tid);
  lj_tg_attach(g, &extra);
  assert(lj_tg_actor_acq(&extra) == actor);
  assert(!lj_tg_flags_test_acq(&extra, TGF_DEAD));

  lj_tg_detach(g, &extra);
  assert(lj_tg_flags_test_acq(&extra, TGF_DEAD));
  assert(lj_tg_actor_acq(&extra) == LJ_THR_ACTOR_RETIRED);
  assert(!lj_thr_tg_bind_current(&extra));
  assert(lj_tg_actor_acq(&extra) == LJ_THR_ACTOR_RETIRED);
  assert(lj_tg_reclaim_dead(g) >= 1u);
  assert(lj_tg_fini_thread(g, &extra));
  lua_pop(L, 1);  /* coroutine root */
}

static void test_same_actor_nested_universe(lua_State *outer)
{
  lua_State *inner;
  TGState *inner_tg;
  TGState *outer_tg = G(outer)->main_tg;
  TGState *raw = lj_thr_get_tg();
  uint32_t actor = lj_thr_actor_current();
  uint32_t inner_tid;

  assert(raw == outer_tg && actor != 0);
  inner = luaL_newstate();
  assert(inner != NULL);
  inner_tg = G(inner)->main_tg;
  inner_tid = lj_tg_tid_acq(inner_tg);
  assert(lj_thr_actor_current() == actor);
  assert(lj_thr_get_tg() == raw);
  assert(lj_tg_actor_acq(inner_tg) == actor);
  assert_owner_pair(inner, inner_tid, actor);
  /* A nested universe has no raw TLS alias of its own. Its explicit quiescent
  ** handoff must preserve the outer binding, and close may then reacquire the
  ** inner TG without ever clobbering that unrelated carrier. */
  assert(lj_thr_tg_handoff_current(inner_tg));
  assert(lj_thr_get_tg() == raw);
  assert(lj_tg_actor_acq(inner_tg) == 0);
  /* A generic raw/exact binder must not split the handed-off TG from the still
  ** actor-tagged main state. Only the explicit paired close claim may consume
  ** actor zero and retag both authorities as one transaction. */
  assert(!lj_thr_tg_bind_current(inner_tg));
  assert(lj_tg_actor_acq(inner_tg) == 0);
  assert_owner_pair(inner, inner_tid, actor);
  assert(lj_thr_main_close_claim(inner));
  assert(lj_tg_actor_acq(inner_tg) == actor);
  assert_owner_pair(inner, inner_tid, actor);
  lua_close(inner);
  assert(lj_thr_get_tg() == raw && lj_thr_actor_current() == actor);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  TGState *tg;
  uint32_t actor;

  assert(L != NULL);
  tg = G(L)->main_tg;
  actor = lj_thr_actor_current();
  assert(actor != 0 && lj_thr_actor_ensure() == actor);
  assert(lj_thr_get_tg() == tg && lj_tg_actor_acq(tg) == actor);
  assert_owner_pair(L, lj_tg_tid_acq(tg), actor);

  test_foreign_tid_spoof(L);
  test_actor_tls_nonreuse(actor);
  test_atomic_claim_release(L);
  test_detach_terminal_actor(L);
  test_same_actor_nested_universe(L);

  assert(lj_thr_get_tg() == tg && lj_thr_actor_current() == actor);
  assert(lj_tg_actor_acq(tg) == actor);
  lua_close(L);
  assert(lj_thr_get_tg() == NULL);
  assert(lj_thr_actor_current() == actor);
  puts("t-state-actor-carrier OK: physical actor claims and TLS incarnation");
  return 0;
}
