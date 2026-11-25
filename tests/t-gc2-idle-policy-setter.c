/* A public mode setter publishes a poll after another TG has ACKed it. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

static _Atomic(TGState *) peer_tg, setter_tg;
static atomic_int start_setter, release_peer, setter_done, timeout_hit;
static uint64_t epoch;
static const uint32_t policy = LJ_GC2_HS_ALLOC_WHITE | LJ_GC2_HS_ENABLE_BARRIER;

static void pause_briefly(void)
{
  const struct timespec delay = { 0, 1000000 };
  nanosleep(&delay, NULL);
}

static int hold_peer(lua_State *L)
{
  atomic_store(&peer_tg, L2TG(L));
  while (!atomic_load(&release_peer))
    pause_briefly();
  assert(strcmp(lua_tostring(L, 1), "keeper") == 0);
  return 0;
}

static int wait_setter(lua_State *L)
{
  atomic_store(&setter_tg, L2TG(L));
  while (!atomic_load(&start_setter) && !atomic_load(&release_peer))
    pause_briefly();
  return 0;
}

static int finish_setter(lua_State *L)
{
  assert(lj_tg_mark_active_acq(L2TG(L)) && !lj_tg_alloc_black_acq(L2TG(L)));
  atomic_store(&setter_done, 1);
  return 0;
}

static int start(lua_State *L)
{
  global_State *g = G(L);
  TGState *self = L2TG(L), *peer = atomic_load(&peer_tg);
  TGState *setter = atomic_load(&setter_tg);
  if (!peer || !setter) {
    lua_pushboolean(L, 0);
    return 1;
  }
  lj_safepoint_test_signal_pause_arm(self);
  atomic_store(&start_setter, 1);
  while (!lj_safepoint_test_signal_paused() && !atomic_load(&release_peer))
    pause_briefly();
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE && gc2_hs_actions_acq(g) == policy);
  assert(gc2_hs_leader_acq(g) == lj_tg_tid_acq(setter));
  assert(lj_tg_reqmask_acq(self) == policy && lj_tg_poll_acq(self) == 0);
  epoch = gc2_hs_epoch_acq(g);
  assert(lj_safepoint_poll(L) == policy);
  assert(lj_tg_hs_epoch_ack_acq(self) == epoch);
  assert(lj_tg_reqmask_acq(self) == 0 && lj_tg_poll_acq(self) == 0);
  lj_safepoint_test_signal_pause_release();
  /* The leader acknowledges itself after the first real signaling pass. */
  while (lj_tg_hs_epoch_ack_acq(setter) != epoch && !atomic_load(&release_peer))
    pause_briefly();
  assert(lj_safepoint_test_signal_resumed_poll_stores() == 1);
  assert(lj_tg_reqmask_acq(peer) == policy);
  assert(lj_tg_hs_epoch_ack_acq(peer) != epoch);
  lua_pushboolean(L, 1);
  return 1;
}

static int finish(lua_State *L)
{
  global_State *g = G(L);
  TGState *peer = atomic_load(&peer_tg);
  if (!atomic_load(&release_peer)) {
    assert(gc2_phase_acq(g) == LJ_GC2_IDLE && gc2_hs_epoch_acq(g) == epoch);
    assert(lj_tg_reqmask_acq(peer) == policy && gc2_hs_pending_acq(g) != 0);
    assert(lj_tg_hs_epoch_ack_acq(peer) != epoch && !atomic_load(&setter_done));
    assert(lj_tg_reqmask_acq(L2TG(L)) == 0 && lj_tg_poll_acq(L2TG(L)) == 0);
    assert(lj_safepoint_test_signal_consumed_clears() == 1);
    assert(lj_tg_mark_active_acq(L2TG(L)) && !lj_tg_alloc_black_acq(L2TG(L)));
  }
  atomic_store(&release_peer, 1);
  return 0;
}

static void *watchdog(void *unused)
{
  unsigned i;
  (void)unused;
  for (i = 0; i < 5000 && !atomic_load(&release_peer); i++)
    pause_briefly();
  if (!atomic_load(&release_peer)) {
    atomic_store(&timeout_hit, 1);
    atomic_store(&release_peer, 1);
    lj_safepoint_test_signal_pause_release();
  }
  return NULL;
}

int main(void)
{
  static const char source[] =
    "local hold, wait, done, start, finish = ...\n"
    "if jit then jit.off() end\n"
    "collectgarbage('stop')\n"
    "local th = require('threading')\n"
    "local peer = assert(th.spawn(function(hold) hold('keeper') end, hold))\n"
    "local setter = assert(th.spawn(function(wait, done)\n"
    "  wait()\n"
    "  assert(require('threading').gcmode('generational') == 'incremental')\n"
    "  done()\n"
    "end, wait, done))\n"
    "while not start() do th.sleep(0.001) end\n"
    "local keep = {}\n"
    "for i=1,200 do keep[i]={answer=i+23} end\n"
    "finish()\n"
    "assert(peer:join(5))\n"
    "assert(setter:join(5))\n"
    "for i=1,200 do assert(keep[i].answer==i+23) end\n";
  lua_State *L = luaL_newstate();
  pthread_t timer;
  int status;
  assert(L);
  luaL_openlibs(L);
  assert(luaL_loadbuffer(L, source, sizeof(source)-1, "=idle policy") == LUA_OK);
  lua_pushcfunction(L, hold_peer);
  lua_pushcfunction(L, wait_setter);
  lua_pushcfunction(L, finish_setter);
  lua_pushcfunction(L, start);
  lua_pushcfunction(L, finish);
  assert(pthread_create(&timer, NULL, watchdog, NULL) == 0);
  status = lua_pcall(L, 5, 0, 0);
  atomic_store(&release_peer, 1);
  assert(pthread_join(timer, NULL) == 0);
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(status == LUA_OK && atomic_load(&setter_done));
  assert(gc2_hs_leader_acq(G(L)) == 0 && gc2_hs_pending_acq(G(L)) == 0);
  lj_safepoint_test_signal_pause_reset();
  lua_close(L);
  if (atomic_load(&timeout_hit)) {
    fprintf(stderr, "allocation waited for a parked mode-policy peer\n");
    return 1;
  }
  return 0;
}
