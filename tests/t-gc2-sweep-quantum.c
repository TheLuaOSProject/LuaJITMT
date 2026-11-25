/* Completed arenas consume the existing sweep work budget, one per unit. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_tg.h"

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  enum { BUDGET = 8 };
  uint64_t before, advanced = 0;
  unsigned i;
  char bytes[4096];
  assert(L);
  g = G(L);
  lua_gc(L, LUA_GCSTOP, 0);
  memset(bytes, 's', sizeof(bytes));
  for (i = 0; i < 3000; i++) {
    memcpy(bytes + 16, &i, sizeof(i));
    lua_pushlstring(L, bytes, sizeof(bytes));
    lua_pop(L, 1);
  }
  for (i = 0; i < 10000; i++) {
    if (gc2_phase_acq(g) == LJ_GC2_SWEEP && gc2_sweep_bridge_ready_acq(g))
      break;
    (void)lj_gc2_step_explicit(L, 1);
  }
  assert(i < 10000);
  for (i = 0; i < 4096; i++) {
    unsigned progress;
    before = gc2_sweep_owner_arenas_acq(g);
    progress = lj_gc2_worker_drain(g, BUDGET);
    advanced = gc2_sweep_owner_arenas_acq(g) - before;
    assert(advanced <= progress && progress <= BUDGET);
    if (advanced > 1)
      break;
  }
  assert(advanced > 1);  /* A worker did not give up after its first arena. */
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  before = gc2_sweep_owner_arenas_acq(g);
  (void)lj_gc2_worker_drain(g, 1);
  assert(gc2_sweep_owner_arenas_acq(g) - before <= 1);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  lua_close(L);
  puts("t-gc2-sweep-quantum OK");
  return 0;
}
