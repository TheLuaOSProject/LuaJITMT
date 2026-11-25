/* A deferred sweep turn must leave allocation debt due. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  uint64_t work;
  unsigned pass;
  assert(L);
  g = G(L);
  lua_gc(L, LUA_GCSTOP, 0);
  for (pass = 0; pass < 40000; pass++) {
    lua_newtable(L);
    lua_pop(L, 1);
  }
  for (pass = 0; pass < 10000; pass++) {
    (void)lj_gc2_step_explicit(L, 1);
    if (gc2_phase_acq(g) == LJ_GC2_SWEEP &&
        gc2_sweep_bridge_ready_acq(g) && lj_gc2_jit_entry_open(g))
      break;
  }
  assert(pass < 10000 && lj_gc2_sweep_pending(g));
  lua_gc(L, LUA_GCRESTART, 0);

  /* Retain the phase's native scheduling lease for this one test turn. */
  gc2_jit_sweep_yield_until_ns_rel(g, UINT64_MAX);
  work = gc2_worker_runs_acq(g);
  lj_gc_step(L);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  assert(gc2_worker_runs_acq(g) == work);
  assert(lj_gc_threshold_load(g) == lj_gc_total_load(g));
  assert(lj_gc2_jit_entry_open(g));

  gc2_jit_sweep_yield_until_ns_rel(g, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_close(L);
  puts("t-gc2-sweep-deferred-debt OK");
  return 0;
}
