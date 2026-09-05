#include <assert.h>
#include <inttypes.h>
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

static void snapshot(lua_State *L, const char *stage)
{
  global_State *g = G(L);
  printf("{\"stage\":\"%s\",\"running\":%d,\"total\":%" PRIu64
         ",\"threshold\":%" PRIu64 ",\"pause\":%u,\"leader\":%u,"
         "\"phase\":%u,\"starts\":%" PRIu64 ",\"completed\":%" PRIu64 "}\n",
         stage, lua_gc(L, LUA_GCISRUNNING, 0), (uint64_t)lj_gc_total_load(g),
         (uint64_t)lj_gc_threshold_load(g), (unsigned)lj_gc_pause_load(g),
         gc2_cycle_leader_acq(g), gc2_phase_acq(g), gc2_cycle_starts_acq(g),
         gc2_sweep_to_idle_acq(g));
}

int main(void)
{
  const uint64_t target = (UINT64_C(1) << 17) * 100u;
  lua_State *L = luaL_newstate();
  global_State *g;
  uint64_t total, done;
  uint32_t i;
  int fn, missed;
  assert(L != NULL); setvbuf(stdout, NULL, _IOLBF, 0); alarm(20);
  luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF));
  assert(luaL_dostring(L, "return function() local t={} return t end") == 0);
  fn = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_gc(L, LUA_GCCOLLECT, 0); lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0); g = G(L);
  total = (uint64_t)lj_gc_total_load(g);
  assert(total + sizeof(GCudata) < target);
  /* One ordinary, stack-anchored userdata makes the public pause arithmetic
  ** exactly MAX. No allocation counter or threshold is written by the test. */
  (void)lua_newuserdata(L, (size_t)(target - total - sizeof(GCudata)));
  total = (uint64_t)lj_gc_total_load(g);
  snapshot(L, "anchored_allocation_before_restart");
  assert(total / 100u == (UINT64_C(1) << 17));
  lua_gc(L, LUA_GCSETPAUSE, 1 << 30);
  assert((total / 100u) * lj_gc_pause_load(g) == LJ_MAX_MEM);
  done = gc2_sweep_to_idle_acq(g);
  assert(lua_gc(L, LUA_GCRESTART, -1) == 0);
  assert(lj_gc_threshold_load(g) == LJ_MAX_MEM);
  missed = !lua_gc(L, LUA_GCISRUNNING, 0);
  snapshot(L, "public_restart_with_numeric_max");
  for (i = 0; i < 8192; i++) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, fn);
    assert(lua_pcall(L, 0, 1, 0) == 0);
    assert(lua_istable(L, -1)); lua_pop(L, 1);
  }
  if (gc2_sweep_to_idle_acq(g) == done) missed = 1;
  snapshot(L, "after_8192_automatic_tnew");
  lua_gc(L, LUA_GCSETPAUSE, 200);
  lua_settop(L, 0);
  lua_gc(L, LUA_GCCOLLECT, 0); lua_gc(L, LUA_GCCOLLECT, 0);
  snapshot(L, "cleaned_up");
  lua_close(L);
  printf("NUMERIC_MAX_RESTART missed=%d\n", missed);
  return missed ? 45 : 0;
}
