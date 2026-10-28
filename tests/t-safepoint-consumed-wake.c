/*
** Deterministic lost-wake race for a same-epoch duplicate request arriving
** after an owner consumes reqmask and before it enters the consumed-poll wait.
*/
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"

#include "lib/test_sleep.h"

typedef struct ConsumedWakeProbe {
  global_State *g;
  lua_State *ownerL;
  TGState owner;
  pthread_t owner_thread;
  pthread_t watchdog_thread;
  uint64_t epoch;
  uint32_t armed;
  uint32_t attached;
  uint32_t counted;
  uint32_t consumed;
  uint32_t before_wait;
  uint32_t signaled;
  uint32_t owner_returned;
  uint32_t owner_returned_early;
  uint32_t watchdog_done;
  uint32_t rescue_wakes;
} ConsumedWakeProbe;

static ConsumedWakeProbe probe;

static uint64_t now_ns(void)
{
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void until_value(const uint32_t *value, uint32_t expected,
                        uint64_t timeout_ns)
{
  uint64_t deadline = now_ns() + timeout_ns;
  while (la_load32_acq(value) != expected) {
    assert(now_ns() < deadline);
    la_cpu_pause();
  }
}

static void completion_hook(global_State *g, TGState *tg, uint64_t epoch,
                            uint32_t point)
{
  ConsumedWakeProbe *p = &probe;
  if (g != p->g || tg != &p->owner || !la_load32_acq(&p->armed))
    return;
  assert(epoch == p->epoch);
  assert(gc2_hs_epoch_acq(g) == p->epoch);

  if (point == LJ_SAFEPOINT_COMPLETION_TEST_COUNTED) {
    uint32_t n = la_load32_acq(&p->counted) + 1u;
    assert(gc2_hs_pending_acq(g) != 0);
    if (n == 2) {
      /* The first owner consume is paused. The second count is therefore a
      ** duplicate publication for the same epoch. Let the owner advance to
      ** its pre-wait interlock before reqmask/poll are republished. */
      until_value(&p->consumed, 1, 5000000000ull);
      assert(lj_tg_hs_epoch_ack_acq(tg) != epoch);
      la_store32_rel(&p->counted, n);
      until_value(&p->before_wait, 1, 5000000000ull);
    } else {
      assert(n == 1);
      la_store32_rel(&p->counted, n);
    }
  } else if (point == LJ_SAFEPOINT_COMPLETION_TEST_CONSUMED) {
    assert(lj_thr_get_tg() == tg);
    assert(lj_tg_in_native_acq(tg) == 0);
    {
      uint32_t n = la_add32_rlx(&p->consumed, 1) + 1u;
      if (n == 1) {
        /* This callback runs after reqmask exchange and before epoch claim. */
        assert(lj_tg_reqmask_acq(tg) == 0);
        assert(lj_tg_poll_acq(tg) == 1);
        until_value(&p->counted, 2, 5000000000ull);
      } else {
        assert(n == 2);
        assert(lj_tg_reqmask_acq(tg) == 0);
        assert(lj_tg_hs_epoch_ack_acq(tg) == epoch);
      }
    }
  } else if (point == LJ_SAFEPOINT_COMPLETION_TEST_BEFORE_WAIT) {
    assert(lj_thr_get_tg() == tg);
    if (la_add32_rlx(&p->before_wait, 1) == 0) {
      /* This is immediately after the waiter's reqmask==0 check. Do not let
      ** it call the real futex wait until the duplicate signal has completed. */
      until_value(&p->signaled, 2, 5000000000ull);
    }
  } else if (point == LJ_SAFEPOINT_COMPLETION_TEST_SIGNALED) {
    uint32_t n = la_load32_acq(&p->signaled) + 1u;
    assert(lj_tg_poll_acq(tg) == 1);
    if (n == 2) {
      /* The wake has returned while the futex word remains one. The owner is
      ** still held in BEFORE_WAIT, making the later wait deterministically
      ** vulnerable to a lost wake on the old implementation. */
      assert(lj_tg_reqmask_acq(tg) == LJ_GC2_HS_FLUSH_SSB);
      assert(la_load32_acq(&p->before_wait) == 1);
    }
    /* Publish last: the owner may immediately consume the duplicate. */
    la_store32_rel(&p->signaled, n);
  }
}

static void *owner_main(void *arg)
{
  ConsumedWakeProbe *p = (ConsumedWakeProbe *)arg;
  TGState *tg = &p->owner;
  uint32_t tid;

  lj_tg_init_thread(p->g, tg, p->ownerL, 0);
  tid = lj_thr_newid();
  lj_tg_tid_rel(tg, tid);
  lj_arena_alloc_owner_rel(&tg->alloc, tid);
  lj_tg_derive_prng(p->g, tg, tid);
  lj_thr_set_tg(tg);
  lj_tg_attach(p->g, tg);
  assert(lj_state_claim(p->ownerL, tid));
  la_store32_rel(&p->attached, 1);

  until_value(&tg->reqmask, LJ_GC2_HS_FLUSH_SSB, 5000000000ull);
  until_value(&tg->poll, 1, 5000000000ull);
  (void)lj_safepoint_ack(p->ownerL);
  if (gc2_hs_pending_acq(p->g) != 0 ||
      lj_tg_reqmask_acq(tg) != 0 || lj_tg_poll_acq(tg) != 0 ||
      lj_tg_hs_epoch_ack_acq(tg) != p->epoch)
    la_store32_rel(&p->owner_returned_early, 1);
  la_store32_rel(&p->owner_returned, 1);

  lj_state_release(p->ownerL, tid);
  lj_tg_detach(p->g, tg);
  lj_thr_set_tg(NULL);
  return NULL;
}

static void *watchdog_main(void *arg)
{
  ConsumedWakeProbe *p = (ConsumedWakeProbe *)arg;
  until_value(&p->signaled, 2, 5000000000ull);
  /* Safety recovery for the intentionally tested old indefinite wait. Never
  ** clear poll or decrement a counted request: this is only an extra wake. */
  sleep_ns(1000000000L);
  if (la_load32_acq(&p->consumed) < 2 &&
      lj_tg_reqmask_acq(&p->owner) == LJ_GC2_HS_FLUSH_SSB) {
    (void)la_add32_rlx(&p->rescue_wakes, 1);
    lj_tg_poll_futex_wake(&p->owner, 1);
  }
  la_store32_rel(&p->watchdog_done, 1);
  return NULL;
}

int main(void)
{
  lua_State *L;
  lua_State *ownerL;
  uint32_t signaled;

  alarm(15);
  setvbuf(stdout, NULL, _IOLBF, 0);
  L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);

  memset(&probe, 0, sizeof(probe));
  probe.g = G(L);
  ownerL = lua_newthread(L);  /* Keep the coroutine rooted on L's stack. */
  assert(ownerL != NULL);
  probe.ownerL = ownerL;

  assert(pthread_create(&probe.owner_thread, NULL, owner_main, &probe) == 0);
  until_value(&probe.attached, 1, 5000000000ull);
  probe.epoch = gc2_hs_epoch_acq(probe.g) + 1u;
  la_store32_rel(&probe.armed, 1);
  lj_safepoint_test_completion_hook(completion_hook);
  assert(pthread_create(&probe.watchdog_thread, NULL, watchdog_main,
                        &probe) == 0);

  signaled = lj_safepoint_handshake(probe.g, LJ_GC2_HS_FLUSH_SSB);

  assert(pthread_join(probe.owner_thread, NULL) == 0);
  assert(pthread_join(probe.watchdog_thread, NULL) == 0);
  la_store32_rel(&probe.armed, 0);
  lj_safepoint_test_completion_hook(NULL);

  assert(signaled >= 2);
  assert(la_load32_acq(&probe.counted) == 2);
  assert(la_load32_acq(&probe.consumed) == 2);
  assert(la_load32_acq(&probe.before_wait) >= 1);
  assert(la_load32_acq(&probe.signaled) == 2);
  assert(la_load32_acq(&probe.owner_returned) == 1);
  assert(la_load32_acq(&probe.owner_returned_early) == 0);
  assert(la_load32_acq(&probe.watchdog_done) == 1);
  assert(gc2_hs_pending_acq(probe.g) == 0);
  assert(gc2_hs_leader_acq(probe.g) == 0);
  assert(lj_tg_reqmask_acq(&probe.owner) == 0);
  assert(lj_tg_poll_acq(&probe.owner) == 0);
  assert(lj_tg_hs_epoch_ack_acq(&probe.owner) == probe.epoch);

  printf("consumed duplicate wake: counted=%u consumed=%u before-wait=%u "
         "signaled=%u rescue=%u pending=%u epoch=%llu\n",
         la_load32_acq(&probe.counted), la_load32_acq(&probe.consumed),
         la_load32_acq(&probe.before_wait), la_load32_acq(&probe.signaled),
         la_load32_acq(&probe.rescue_wakes), gc2_hs_pending_acq(probe.g),
         (unsigned long long)probe.epoch);
  assert(la_load32_acq(&probe.rescue_wakes) == 0);

  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_close(L);
  return 0;
}
