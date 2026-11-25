/* Public one-unit GC steps with an unacknowledged string retirement epoch. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_gc.h"
#include "lj_safepoint.h"
#include "lj_str.h"
#include "lj_tg.h"
#include "lj_thr.h"

static atomic_int enabled, target, parked, resume_peer, timed_out;
static atomic_int allow_native, became_native;
static int late_native;
static uint64_t epoch0;

static void pause_briefly(void)
{
  const struct timespec delay = { 0, 1000000 };
  nanosleep(&delay, NULL);
}

static int peer_poll(lua_State *L)
{
  if (!atomic_load(&target)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  assert(lj_tg_in_native_acq(L2TG(L)) == 0);
  atomic_store(&parked, 1);
  while (!atomic_load(&resume_peer) && !atomic_load(&allow_native))
    pause_briefly();
  if (atomic_load(&allow_native)) {
    LJNativeFrame frame;
    lj_native_enter_l(L, &frame);
    atomic_store(&became_native, 1);
    while (!atomic_load(&resume_peer))
      pause_briefly();
    (void)lj_native_leave_l(L, &frame);
  }
  assert(!strcmp(lua_tostring(L, 1), "parked string root"));
  lua_pushboolean(L, 1);
  return 1;
}

int __real_lj_str_gc2_reclaim_checkpoint(global_State *, uint32_t);
int __wrap_lj_str_gc2_reclaim_checkpoint(global_State *g, uint32_t limit)
{
  if (atomic_load(&enabled) && !atomic_load(&target) &&
      gc2_phase_acq(g) == LJ_GC2_IDLE && !gc2_cycle_leader_acq(g) &&
      lj_str_body_retired_head_acq(g) && lj_str_qcount_acq(g) &&
      (uint32_t)lj_str_reclaim_scope_acq(g) == LJ_STR_RECLAIM_STEP) {
    epoch0 = gc2_hs_epoch_acq(g);
    atomic_store(&target, 1);
    while (!atomic_load(&parked) && !atomic_load(&timed_out))
      pause_briefly();
  }
  return __real_lj_str_gc2_reclaim_checkpoint(g, limit);
}

static int step(lua_State *L)
{
  atomic_store(&enabled, 1);
  (void)lua_gc(L, LUA_GCSTEP, 1);
  lua_pushboolean(L, atomic_load(&target));
  return 1;
}

static int finish(lua_State *L)
{
  global_State *g = G(L);
  if (!atomic_load(&timed_out)) {
    uint64_t epoch = gc2_hs_epoch_acq(g);
    assert(atomic_load(&parked) && gc2_phase_acq(g) == LJ_GC2_IDLE);
    assert(epoch > epoch0 && gc2_hs_pending_acq(g) != 0);
    assert(gc2_hs_actions_acq(g) & LJ_GC2_HS_FLUSH_SSB);
    if (late_native) {
      /* Neither mutator polls after this switch. The pool has to retry its
      ** pending IDLE checkpoint and finish the same epoch remotely. */
      assert(gc2_idle_retire_epoch_acq(g) < epoch);
      atomic_store(&allow_native, 1);
      while ((!atomic_load(&became_native) ||
	      gc2_idle_retire_epoch_acq(g) < epoch ||
	      gc2_cycle_leader_acq(g) == LJ_THREAD_GCSCAN) &&
	     !atomic_load(&timed_out))
	pause_briefly();
      assert(gc2_idle_retire_epoch_acq(g) == epoch && gc2_hs_pending_acq(g) == 0);
    }
  }
  atomic_store(&resume_peer, 1);
  return 0;
}

static void *watchdog(void *unused)
{
  unsigned n;
  (void)unused;
  for (n = 0; n < 10000 && !atomic_load(&parked) && !atomic_load(&resume_peer); n++)
    pause_briefly();
  for (n = 0; n < 2000 && !atomic_load(&resume_peer); n++)
    pause_briefly();
  if (!atomic_load(&resume_peer)) {
    atomic_store(&timed_out, 1);
    atomic_store(&resume_peer, 1);
  }
  return NULL;
}

int main(int argc, char **argv)
{
  static const char code[] =
    "local poll, step, finish, pool = ...\n"
    "jit.off(); local th=require('threading'); collectgarbage('stop')\n"
    "assert(th.gcworkers(pool)==0)\n"
    "local peer=assert(th.spawn(function(poll)\n"
    "  local t=0; repeat for i=1,500 do t=t+i end until poll('parked string root'); return t>0\n"
    "end,poll))\n"
    "local done=false\n"
    "for round=1,64 do\n"
    "  collectgarbage('stop')\n"
    "  for i=1,230 do assert(#('retired/'..round..'/'..i):rep(22)>99) end\n"
    "  for j=1,5000 do if step() then done=true; break end end\n"
    "  if done then break end\n"
    "end\n"
    "assert(done,'no active string quarantine reached idle')\n"
    "local keep={}; for i=1,200 do keep[i]={i+7} end\n"
    "finish(); local ok,v=peer:join(10); assert(ok and v); collectgarbage()\n"
    "for i=1,200 do assert(keep[i][1]==i+7) end\n";
  lua_State *L = luaL_newstate();
  pthread_t timer;
  int status, pool = 0;
  if (argc > 1) {
    assert(argc == 2);
    late_native = !strcmp(argv[1], "pool-native");
    assert(late_native || !strcmp(argv[1], "pool"));
    pool = 2;
  }
  assert(L);
  luaL_openlibs(L);
  assert(luaL_loadbuffer(L, code, sizeof(code)-1, "=idle string step") == 0);
  lua_pushcfunction(L, peer_poll);
  lua_pushcfunction(L, step);
  lua_pushcfunction(L, finish);
  lua_pushinteger(L, pool);
  assert(pthread_create(&timer, NULL, watchdog, NULL) == 0);
  status = lua_pcall(L, 4, 0, 0);
  atomic_store(&resume_peer, 1);
  assert(pthread_join(timer, NULL) == 0);
  if (status)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  lua_close(L);
  assert(!status && atomic_load(&target));
  if (atomic_load(&timed_out)) {
    fputs("public string step waited for a parked peer\n", stderr);
    return 1;
  }
  return 0;
}
