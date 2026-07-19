/*
** Quiescent main-state actor migration and concurrent close claimants.
**
** The creator explicitly releases its TG actor only after making the universe
** quiescent. Two fresh OS actors then race the close-claim transaction. Exactly
** one may transfer both TG and main-state ownership; the loser must return
** before the winner starts destructive teardown.
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

#define CLOSE_WAIT_SPINS 10000000u

typedef struct CloseRace {
  lua_State *L;
  global_State *g;
  TGState *tg;
  LJStateOwner creator_owner;
  uint32_t creator_actor;
  uint32_t tid;
  uint32_t ready;
  uint32_t go;
  uint32_t attempted;
  uint32_t loser_left;
  uint32_t closed;
  uint32_t finalizer_calls;
  uint32_t winner_actor;
  uint32_t actor[2];
  uint32_t won[2];
} CloseRace;

typedef struct CloseThreadCtx {
  CloseRace *race;
  uint32_t index;
} CloseThreadCtx;

static CloseRace *active_race;

static void wait_count(uint32_t *word, uint32_t target, const char *what)
{
  uint32_t spin;
  for (spin = 0; spin < CLOSE_WAIT_SPINS; spin++) {
    if (la_load32_acq(word) >= target)
      return;
    (void)lj_thr_retry_yield(NULL);
  }
  fprintf(stderr, "timeout waiting for %s\n", what);
  assert(0);
}

static void wait_nonzero(uint32_t *word, const char *what)
{
  wait_count(word, 1, what);
}

static int close_actor_finalizer(lua_State *L)
{
  CloseRace *race = active_race;
  LJStateOwner owner;
  uint32_t actor;

  assert(race != NULL && L == race->L);
  actor = lj_thr_actor_current();
  assert(actor != 0 && actor == race->winner_actor);
  assert(lj_thr_get_tg() == NULL);  /* moved-close raw-NULL carrier */
  assert(lj_tg_actor_acq(race->tg) == actor);
  owner = lj_state_owner_word_acq(L);
  assert(lj_state_owner_tid(owner) == race->tid);
  assert(lj_state_owner_actor(owner) == actor);
  assert(owner == lj_state_owner_pack(race->tid, actor));
  assert(la_add32_acqrel(&race->finalizer_calls, 1) == 0);
  return 0;
}

static void install_finalizer(lua_State *L)
{
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, close_actor_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  /* Keep it live until the terminal all-finalizers pass. */
  lua_setglobal(L, "m8_close_actor_handoff_finalizer");
}

static void *close_claimant(void *ud)
{
  CloseThreadCtx *thread = (CloseThreadCtx *)ud;
  CloseRace *race = thread->race;
  uint32_t index = thread->index;
  uint32_t actor;
  LJStateOwner owner;

  assert(index < 2u && lj_thr_get_tg() == NULL);
  assert(lj_thr_actor_current() == 0);
  actor = lj_thr_actor_ensure();
  assert(actor != 0);
  race->actor[index] = actor;
  (void)la_add32_acqrel(&race->ready, 1);
  wait_nonzero(&race->go, "close race release");

  race->won[index] = (uint32_t)lj_thr_main_close_claim(race->L);
  (void)la_add32_acqrel(&race->attempted, 1);
  if (!race->won[index]) {
    /* A losing actor cannot acquire either half of the carrier and performs
    ** no target access after publishing loser_left. */
    assert(lj_tg_actor_acq(race->tg) != actor);
    la_store32_rel(&race->loser_left, 1);
    return NULL;
  }

  assert(lj_thr_actor_current() == actor && lj_thr_get_tg() == NULL);
  assert(lj_tg_actor_acq(race->tg) == actor);
  owner = lj_state_owner_word_acq(race->L);
  assert(owner == lj_state_owner_pack(race->tid, actor));
  race->winner_actor = actor;

  wait_count(&race->attempted, 2, "both close claims");
  wait_nonzero(&race->loser_left, "losing close claimant exit");
  /* lua_close repeats the claim at its first instruction. Reentrant ownership
  ** by this exact actor succeeds and remains sticky through all finalizers. */
  lua_close(race->L);
  assert(lj_thr_get_tg() == NULL && lj_thr_actor_current() == actor);
  la_store32_rel(&race->closed, 1);
  return NULL;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  CloseRace race;
  CloseThreadCtx thread_ctx[2];
  pthread_t thread[2];
  uint32_t winners;

  assert(L != NULL);
  memset(&race, 0, sizeof(race));
  race.L = L;
  race.g = G(L);
  race.tg = race.g->main_tg;
  race.creator_actor = lj_thr_actor_current();
  race.tid = lj_tg_tid_acq(race.tg);
  race.creator_owner = lj_state_owner_word_acq(L);
  assert(race.creator_actor != 0);
  assert(lj_thr_get_tg() == race.tg);
  assert(lj_tg_actor_acq(race.tg) == race.creator_actor);
  assert(race.creator_owner ==
         lj_state_owner_pack(race.tid, race.creator_actor));
  install_finalizer(L);

  /* This is the explicit quiescent handoff LP. TG actor zero prevents both the
  ** old creator and a new closer from having authority during migration; the
  ** main-state pair retains old provenance until a close claimant wins. */
  assert(lj_thr_tg_handoff_current(race.tg));
  assert(lj_thr_get_tg() == NULL);
  assert(lj_thr_actor_current() == race.creator_actor);
  assert(lj_tg_actor_acq(race.tg) == 0);
  assert(lj_state_owner_word_acq(L) == race.creator_owner);

  active_race = &race;
  thread_ctx[0].race = &race;
  thread_ctx[0].index = 0;
  thread_ctx[1].race = &race;
  thread_ctx[1].index = 1;
  assert(pthread_create(&thread[0], NULL, close_claimant, &thread_ctx[0]) == 0);
  assert(pthread_create(&thread[1], NULL, close_claimant, &thread_ctx[1]) == 0);
  wait_count(&race.ready, 2, "close claimant actors");
  assert(race.actor[0] != 0 && race.actor[1] != 0 &&
         race.actor[0] != race.actor[1] &&
         race.actor[0] != race.creator_actor &&
         race.actor[1] != race.creator_actor);
  la_store32_rel(&race.go, 1);

  assert(pthread_join(thread[0], NULL) == 0);
  assert(pthread_join(thread[1], NULL) == 0);
  winners = race.won[0] + race.won[1];
  assert(winners == 1u);
  assert(race.winner_actor ==
         (race.won[0] ? race.actor[0] : race.actor[1]));
  assert(la_load32_acq(&race.closed) == 1u);
  assert(la_load32_acq(&race.finalizer_calls) == 1u);
  assert(lj_thr_get_tg() == NULL);
  assert(lj_thr_actor_current() == race.creator_actor);
  active_race = NULL;

  puts("t-m8-close-actor-handoff OK: one physical close actor won handoff");
  return 0;
}
