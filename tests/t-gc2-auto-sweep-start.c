/* Ordinary allocations during the handoff from weak marking to sweep. */
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
#include "lj_gc.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"

static atomic_int ready, enabled, requested, parked, release_peer, timeout_hit;

static void pause_briefly(void)
{
  const struct timespec delay = { 0, 1000000 };
  nanosleep(&delay, NULL);
}

/* The peer services earlier epochs from a real published native frame. It
** leaves that frame just before the sweep policy is signalled and keeps its
** Lua stack untouched inside a managed call until the allocating TG returns. */
static int hold_peer(lua_State *L)
{
  LJNativeFrame frame;
  lj_native_enter_l(L, &frame);
  atomic_store(&ready, 1);
  while (!atomic_load(&requested) && !atomic_load(&release_peer))
    pause_briefly();
  (void)lj_native_leave_l(L, &frame);
  atomic_store(&parked, 1);
  while (!atomic_load(&release_peer))
    pause_briefly();
  assert(strcmp(lua_tostring(L, 1), "live sweep peer") == 0);
  return 0;
}

static void before_actions(uint32_t actions)
{
  if (!atomic_load(&enabled) ||
      (actions & (LJ_GC2_HS_DISABLE_BARRIER | LJ_GC2_HS_ALLOC_BLACK)) !=
        (LJ_GC2_HS_DISABLE_BARRIER | LJ_GC2_HS_ALLOC_BLACK) ||
      atomic_exchange(&requested, 1))
    return;
  while (!atomic_load(&parked) && !atomic_load(&release_peer))
    pause_briefly();
}

uint32_t __real_lj_safepoint_handshake(global_State *, uint32_t);
uint32_t __wrap_lj_safepoint_handshake(global_State *g, uint32_t actions)
{
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
    before_actions(actions);
  return __real_lj_safepoint_handshake(g, actions);
}

int __real_lj_safepoint_resumable_epoch_try(global_State *, uint32_t, int);
int __wrap_lj_safepoint_resumable_epoch_try(global_State *g, uint32_t actions,
                                             int start)
{
  if (start && gc2_phase_acq(g) == LJ_GC2_SWEEP)
    before_actions(actions);
  return __real_lj_safepoint_resumable_epoch_try(g, actions, start);
}

static int begin(lua_State *L)
{
  if (!atomic_load(&ready)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  atomic_store(&enabled, 1);
  lua_gc(L, LUA_GCRESTART, 0);
  lj_gc_threshold_store(G(L), 0);
  lua_pushboolean(L, 1);
  return 1;
}

static int park_seen(lua_State *L)
{
  lua_pushboolean(L, atomic_load(&parked));
  return 1;
}

static int finish(lua_State *L)
{
  global_State *g = G(L);
  if (!atomic_load(&release_peer)) {
    assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
    assert(gc2_cycle_leader_acq(g) == LJ_THREAD_GCSCAN);
    assert(gc2_sweep_bridge_ready_acq(g) == 0 && gc2_jit_phase_gate_acq(g) == 0);
    assert(gc2_hs_pending_acq(g) != 0 && !lj_tg_mark_active_acq(L2TG(L)));
    assert(lj_tg_alloc_black_acq(L2TG(L)));
  }
  lua_gc(L, LUA_GCSTOP, 0);
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
  }
  return NULL;
}

int main(void)
{
  static const char source[] =
    "local hold, begin, parked, finish = ...\n"
    "if jit then jit.off() end\n"
    "collectgarbage('stop')\n"
    "local th = require('threading')\n"
    "local peer = assert(th.spawn(function(hold) hold('live sweep peer') end, hold))\n"
    "while not begin() do th.sleep(0.001) end\n"
    "local keep = {}\n"
    "for i=1,100000 do keep[i%8+1]={i}; if parked() then break end end\n"
    "assert(parked())\n"
    "for i=1,200 do keep[i]={answer=i+23} end\n"
    "finish()\n"
    "assert(peer:join(5))\n"
    "collectgarbage('collect')\n"
    "for i=1,200 do assert(keep[i].answer==i+23) end\n";
  lua_State *L = luaL_newstate();
  pthread_t timer;
  int status;
  assert(L);
  luaL_openlibs(L);
  assert(luaL_loadbuffer(L, source, sizeof(source)-1, "=sweep entry") == LUA_OK);
  lua_pushcfunction(L, hold_peer);
  lua_pushcfunction(L, begin);
  lua_pushcfunction(L, park_seen);
  lua_pushcfunction(L, finish);
  assert(pthread_create(&timer, NULL, watchdog, NULL) == 0);
  status = lua_pcall(L, 4, 0, 0);
  atomic_store(&release_peer, 1);
  assert(pthread_join(timer, NULL) == 0);
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  lua_close(L);
  assert(status == LUA_OK);
  if (atomic_load(&timeout_hit)) {
    fputs("allocation waited for a parked owner at sweep entry\n", stderr);
    return 1;
  }
  return 0;
}
