/*
** Verify automatic allocation can progress while a managed owner is parked
** outside a declared native section at MARK and WEAK boundaries. The watchdog
** releases the peer on failure so the embedding can still shut down.
*/
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_thr.h"
#include "lj_tg.h"

static atomic_int owner_parked;
static atomic_int allocation_started;
static atomic_int allocation_finished;
static atomic_int release_owner;
static atomic_int timed_out;
static atomic_int peer_resumed_mark;
static int initial_phase;

static uint64_t monotonic_ns(void)
{
  struct timespec now;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void wait_briefly(void)
{
  const struct timespec ms = { 0, 1000000 };
  nanosleep(&ms, NULL);
}

static int hold_owner(lua_State *L)
{
  assert(lj_tg_in_native_acq(L2TG(L)) == 0);
  atomic_store(&owner_parked, 1);
  while (!atomic_load(&release_owner))
    wait_briefly();
  return 0;
}

static int hold_started(lua_State *L)
{
  lua_pushboolean(L, atomic_load(&owner_parked));
  return 1;
}

static int prepare_mark(lua_State *L)
{
  global_State *g = G(L);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(lua_gc(L, LUA_GCSTEP, 1) == 0);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(gc2_mark_activation_request_acq(g) == 0);
  if (initial_phase == LJ_GC2_WEAK) {
    unsigned round;
    for (round = 0; round < 128 && gc2_phase_acq(g) == LJ_GC2_MARK; round++)
      (void)lua_gc(L, LUA_GCSTEP, 1);
    assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
    assert(gc2_weak_root_scanned_acq(g) == 0);
  }
  return 0;
}

static int start_allocation(lua_State *L)
{
  assert(atomic_load(&owner_parked));
  lj_gc_threshold_store(G(L), 0);
  atomic_store(&allocation_started, 1);
  return 0;
}

static int finish_allocation(lua_State *L)
{
  global_State *g = G(L);
  if (atomic_load(&release_owner)) {
    atomic_store(&timed_out, 1);
  } else {
    assert(gc2_phase_acq(g) ==
	   (initial_phase == LJ_GC2_WEAK ? LJ_GC2_WEAK : LJ_GC2_MARK));
    if (initial_phase != LJ_GC2_IDLE) {
      assert(gc2_fixpoint_epoch_actions_acq(g) & LJ_GC2_HS_SCAN_ROOTS);
      if (initial_phase == LJ_GC2_MARK)
	assert(gc2_mark_root_scanned_acq(g) == 2);
      else
	assert(gc2_weak_root_scanned_acq(g) == 2);
      assert(gc2_cycle_leader_acq(g) == LJ_THREAD_GCSCAN);
    } else {
      assert(gc2_mark_activation_request_acq(g) != 0);
      assert(gc2_cycle_leader_acq(g) == gc2_mark_activation_request_acq(g));
    }
    assert(gc2_jit_phase_gate_acq(g) == 0);
    assert(gc2_hs_pending_acq(g) != 0);
    assert(lj_tg_mark_active_acq(L2TG(L)) && lj_tg_alloc_black_acq(L2TG(L)));
  }
  lua_gc(L, LUA_GCSTOP, 0);
  atomic_store(&allocation_finished, 1);
  atomic_store(&release_owner, 1);
  return 0;
}

static int peer_after_poll(lua_State *L)
{
  global_State *g = G(L);
  if (atomic_load(&timed_out))
    return 0;
  assert(lj_tg_mark_active_acq(L2TG(L)));
  assert(lj_tg_alloc_black_acq(L2TG(L)));
  assert(gc2_fixpoint_epoch_actions_acq(g) == 0);
  if (initial_phase == LJ_GC2_MARK)
    assert(gc2_mark_root_scanned_acq(g) == 1);
  if (initial_phase == LJ_GC2_WEAK)
    assert(gc2_weak_root_scanned_acq(g) == 1);
  assert(gc2_mark_activation_request_acq(g) == 0);
  assert(gc2_cycle_leader_acq(g) == 0);
  atomic_store(&peer_resumed_mark, 1);
  return 0;
}

static void *watchdog(void *unused)
{
  uint64_t started = monotonic_ns();
  (void)unused;
  while (!atomic_load(&allocation_started) &&
         monotonic_ns() - started < UINT64_C(5000000000))
    wait_briefly();
  started = monotonic_ns();
  while (!atomic_load(&allocation_finished) &&
         monotonic_ns() - started < UINT64_C(1000000000))
    wait_briefly();
  if (!atomic_load(&allocation_finished)) {
    atomic_store(&timed_out, 1);
    atomic_store(&release_owner, 1);
  }
  return NULL;
}

static int run_case(int phase)
{
  static const char source[] =
    "local hold, held, start, finish, resumed, prepare = ...\n"
    "if jit then jit.off() end\n"
    "local threading = require('threading')\n"
    "local keep = {answer = 7391}\n"
    "if prepare then collectgarbage('stop') end\n"
    "local go = threading.channel(1)\n"
    "local peer = assert(threading.spawn(function(hold, resumed, go)\n"
    "  assert(select(2, go:recv(5))); hold()\n"
    "  local n = 0; for i = 1, 200 do n = n + i end\n"
    "  assert(n == 20100); resumed()\n"
    "end, hold, resumed, go))\n"
    "if prepare then prepare() end; assert(go:send(true, 5))\n"
    "while not held() do threading.sleep(0.001) end\n"
    "start()\n"
    "for i = 1, 150 do local t = {i}; assert(t[1] == i); end\n"
    "finish()\n"
    "assert(peer:join(5))\n"
    "collectgarbage('collect')\n"
    "assert(keep.answer == 7391)\n";
  lua_State *L = luaL_newstate();
  pthread_t timer;
  int status;
  initial_phase = phase;
  atomic_store(&owner_parked, 0);
  atomic_store(&allocation_started, 0);
  atomic_store(&allocation_finished, 0);
  atomic_store(&release_owner, 0);
  atomic_store(&timed_out, 0);
  atomic_store(&peer_resumed_mark, 0);
  assert(L);
  luaL_openlibs(L);
  assert(luaL_loadbuffer(L, source, sizeof(source)-1,
                         "=managed MARK boundary progress") == LUA_OK);
  lua_pushcfunction(L, hold_owner);
  lua_pushcfunction(L, hold_started);
  lua_pushcfunction(L, start_allocation);
  lua_pushcfunction(L, finish_allocation);
  lua_pushcfunction(L, peer_after_poll);
  if (phase != LJ_GC2_IDLE)
    lua_pushcfunction(L, prepare_mark);
  else
    lua_pushnil(L);
  assert(pthread_create(&timer, NULL, watchdog, NULL) == 0);
  status = lua_pcall(L, 6, 0, 0);
  if (status) fprintf(stderr, "%s\n", lua_tostring(L, -1));
  atomic_store(&allocation_finished, 1);
  atomic_store(&release_owner, 1);
  assert(pthread_join(timer, NULL) == 0);
  lua_close(L);
  if (atomic_load(&timed_out)) {
    static const char *const boundary[] = {
      "MARK activation", "MARK root fixpoint", "WEAK root fixpoint"
    };
    fprintf(stderr, "allocation stalled during %s with a parked owner\n",
            boundary[phase]);
    return 1;
  }
  assert(status == LUA_OK && atomic_load(&peer_resumed_mark));
  return 0;
}

int main(void)
{
  return run_case(LJ_GC2_IDLE) || run_case(LJ_GC2_MARK) ||
         run_case(LJ_GC2_WEAK);
}
