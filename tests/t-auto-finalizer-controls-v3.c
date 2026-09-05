#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_dispatch.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"

typedef struct Probe {
  lua_State *L, *child;
  global_State *g;
  uint32_t main_tid, depth, calls, nested, attached, go, done, leave;
  int peer, inner_throw, outer_throw, outer_stop, exact;
} Probe;
static Probe probe;

static unsigned auto_flags(global_State *g)
{
#ifdef LJ_GC_AUTO_FINPAUSE
  return lj_gc_auto_flags_load(g);
#else
  return lj_gc_auto_stopped_load(g);
#endif
}

static void wait_for(const uint32_t *p)
{
  while (!la_load32_acq(p)) la_cpu_pause();
}

static void snapshot(lua_State *L, const char *stage)
{
  global_State *g = G(L);
  printf("{\"stage\":\"%s\",\"flags\":%u,\"running\":%d,"
         "\"owner\":%u,\"current\":%u,\"active\":%u,\"depth\":%u,"
         "\"calls\":%u,\"phase\":%u,\"starts\":%" PRIu64
         ",\"completed\":%" PRIu64 ",\"live\":%u,\"hook\":%u}\n",
         stage, auto_flags(g), lua_gc(L, LUA_GCISRUNNING, 0),
         gc2_finalizer_owner_acq(g), lj_thr_current_id(g),
         gc2_finalizer_active_acq(g), la_load32_acq(&probe.depth),
         la_load32_acq(&probe.calls), gc2_phase_acq(g),
         gc2_cycle_starts_acq(g), gc2_sweep_to_idle_acq(g), mt_live_acq(g),
         (unsigned)hookmask_load(g));
}

static void check_paused(lua_State *L, unsigned flags)
{
  assert(gc2_finalizer_active_acq(G(L)) != 0);
  if (probe.exact) {
    assert(auto_flags(G(L)) == flags);
    assert(!lj_gc_auto_running(G(L)));
    assert(lua_gc(L, LUA_GCISRUNNING, 0) == !(flags & LJ_GC_AUTO_STOPPED));
  }
}

static void *peer_main(void *unused)
{
  TGState *tg;
  uint32_t i;
  (void)unused;
  assert(lj_threading_attach(probe.child));
  tg = lj_thr_get_tg(); assert(tg != NULL);
  lj_native_enter(tg);
  la_store32_rel(&probe.attached, 1);
  wait_for(&probe.go);
  (void)lj_native_leave(probe.child);
  snapshot(probe.child, "peer_overlaps_owned_callback");
  check_paused(probe.child, 2);
  /* Real public STEP attempts may drive work but cannot enter a foreign-owned
  ** callback. Full collection must retain its existing callback deferral. */
  for (i = 0; i < 16; i++) (void)lua_gc(probe.child, LUA_GCSTEP, 1);
  (void)lua_gc(probe.child, LUA_GCCOLLECT, 0);
  assert(la_load32_acq(&probe.calls) == 1);
  assert(lua_gc(probe.child, LUA_GCSTOP, 0) == 0);
  check_paused(probe.child, 3);
  assert(lua_gc(probe.child, LUA_GCRESTART, 0) == 0);
  check_paused(probe.child, 2);
  snapshot(probe.child, "peer_restart_preserves_owned_pause");
  lj_native_enter(tg);
  la_store32_rel(&probe.done, 1);
  wait_for(&probe.leave);
  (void)lj_native_leave(probe.child);
  lj_threading_detach(probe.child, 1);
  return NULL;
}

static int finalizer(lua_State *L)
{
  global_State *g = G(L);
  uint32_t depth = la_add32_rlx(&probe.depth, 1) + 1u;
  uint32_t i;
  (void)la_add32_rlx(&probe.calls, 1);
  assert(lj_thr_current_id(g) == probe.main_tid);
  assert(lj_gc2_finalizer_owned_by_current(g));
  assert(depth <= 2);
  snapshot(L, depth == 1 ? "outer_enter" : "inner_enter");
  check_paused(L, 2);
  if (depth == 1) {
    if (probe.peer) {
      TGState *tg = L2TG(L);
      la_store32_rel(&probe.go, 1);
      lj_native_enter(tg);
      wait_for(&probe.done);
      (void)lj_native_leave(L);
      check_paused(L, 2);
    }
    for (i = 0; i < 4096 && !la_load32_acq(&probe.nested); i++)
      (void)lua_gc(L, LUA_GCSTEP, 1);
    assert(la_load32_acq(&probe.nested) == 1);
    assert(la_load32_acq(&probe.calls) == 2);
    check_paused(L, 3);  /* Inner STOP survived its own pause cleanup. */
    snapshot(L, "outer_after_inner_stop_and_return");
    assert(lua_gc(L, LUA_GCRESTART, 0) == 0);
    check_paused(L, 2);
    (void)lua_gc(L, LUA_GCCOLLECT, 0);
    check_paused(L, 2);  /* Nested public collect returns through deferral. */
    snapshot(L, "outer_after_restart_and_nested_collect");
    if (probe.outer_stop) {
      assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
      check_paused(L, 3);
    }
    (void)la_sub32_rlx(&probe.depth, 1);
    if (probe.outer_throw) {
      lua_pushliteral(L, "intentional outer finalizer error");
      return lua_error(L);
    }
  } else {
    la_store32_rel(&probe.nested, 1);
    assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
    check_paused(L, 3);
    snapshot(L, "inner_stop_before_return_or_throw");
    (void)la_sub32_rlx(&probe.depth, 1);
    if (probe.inner_throw) {
      lua_pushliteral(L, "intentional inner finalizer error");
      return lua_error(L);
    }
  }
  return 0;
}

static void make_finalized(lua_State *L)
{
  (void)lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
}

int main(int argc, char **argv)
{
  lua_State *L;
  global_State *g;
  pthread_t peer;
  uint32_t i;
  uint8_t oldhook;
  int child_ref = LUA_NOREF;
  assert(argc == 6);
  probe.peer = atoi(argv[1]); probe.inner_throw = atoi(argv[2]);
  probe.outer_throw = atoi(argv[3]); probe.outer_stop = atoi(argv[4]);
  probe.exact = atoi(argv[5]);
  setvbuf(stdout, NULL, _IOLBF, 0); alarm(20);
  L = luaL_newstate(); assert(L); luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF));
  g = G(L); probe.L = L; probe.g = g;
  probe.main_tid = lj_thr_current_id(g);
  lua_gc(L, LUA_GCCOLLECT, 0); lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  if (probe.peer) {
    probe.child = lua_newthread(L);
    child_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    assert(pthread_create(&peer, NULL, peer_main, NULL) == 0);
    wait_for(&probe.attached);
  }
  make_finalized(L); make_finalized(L); lua_settop(L, 0);
  oldhook = hookmask_load(g);
  lua_gc(L, LUA_GCRESTART, 0);
  snapshot(L, "before_explicit_step");
  for (i = 0; i < 4096 && la_load32_acq(&probe.calls) < 2; i++)
    (void)lua_gc(L, LUA_GCSTEP, 1);
  assert(la_load32_acq(&probe.calls) == 2);
  assert(la_load32_acq(&probe.nested) == 1);
  assert(la_load32_acq(&probe.depth) == 0);
  assert(gc2_finalizer_active_acq(g) == 0);
  assert(gc2_finalizer_owner_acq(g) == 0);
  assert(hookmask_load(g) == oldhook);
  assert(lua_gettop(L) == 0);
  assert(lua_gc(L, LUA_GCISRUNNING, 0) == !probe.outer_stop);
  if (probe.exact) assert(auto_flags(g) == (unsigned)probe.outer_stop);
  snapshot(L, "after_owned_pause_cleanup");
  if (probe.peer) {
    la_store32_rel(&probe.leave, 1);
    assert(pthread_join(peer, NULL) == 0);
    luaL_unref(L, LUA_REGISTRYINDEX, child_ref);
  }
  lua_gc(L, LUA_GCCOLLECT, 0); lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lua_gc(L, LUA_GCISRUNNING, 0));
  assert(auto_flags(g) == 0);
  snapshot(L, "cleaned_up");
  lua_close(L);
  puts("FINALIZER_CONTROL passed");
  return 0;
}
