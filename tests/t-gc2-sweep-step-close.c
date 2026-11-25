/* The last public step keeps JIT entry closed until its sweep commits. */
#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc2.h"

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  unsigned i;
  assert(L);
  g = G(L);
  lua_gc(L, LUA_GCSTOP, 0);
  for (i = 0; i < 50000; i++) {
    lua_newtable(L);
    lua_pop(L, 1);
  }
  lua_gc(L, LUA_GCRESTART, 0);
  for (i = 0; i != 10000 &&
       !(gc2_phase_acq(g) == LJ_GC2_SWEEP &&
         gc2_sweep_bridge_ready_acq(g)); i++)
    lua_gc(L, LUA_GCSTEP, 1);
  assert(i != 10000);
  for (i = 0; i != 10000 &&
       (lj_gc2_sweep_pending(g) || gc2_sweep_grace_needed_acq(g)); i++)
    lj_gc2_worker_drain(g, LJ_GC2_SWEEP_BATCH);
  assert(i != 10000);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  gc2_jit_sweep_yield_until_ns_rel(g, 0);
  gc2_jit_sweep_displaced_rel(g, 1);
  assert(lua_gc(L, LUA_GCSTEP, 1) == 1);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lua_close(L);
  puts("sweep terminal gate handoff OK");
  return 0;
}
