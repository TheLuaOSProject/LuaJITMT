/* The last public step keeps JIT entry closed until its sweep commits. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tg.h"

static void check_close(int suffix)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  unsigned i;
  assert(L);
  g = G(L);
  lua_gc(L, LUA_GCSTOP, 0);
  if (suffix)
    assert(luaL_loadstring(L, "return 'terminal sweep suffix'") == LUA_OK);
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
  for (i = 0; i != 10000; i++)
    if (!lj_gc2_worker_drain(g, LJ_GC2_SWEEP_BATCH) &&
	!lj_gc2_sweep_pending(g) && !gc2_sweep_grace_needed_acq(g))
      break;
  assert(i != 10000);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  if (suffix) {
    TGState *tg = L2TG(L);
    assert(lj_tg_ssb_next_acq(tg) == lj_tg_ssb_base_acq(tg));
    lj_gc_pubroot(L, L->top - 1);
    assert(lj_tg_ssb_next_acq(tg) == lj_tg_ssb_base_acq(tg) + 1);
    (void)gc2_marks_this_round_xchg_acqrel(g, 0);
    (void)lj_gc2_sweep_to_idle(g);
    assert(lj_tg_ssb_next_acq(tg) == lj_tg_ssb_base_acq(tg));
    if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
      assert(lj_gc2_sweep_to_idle(g));
  } else {
    gc2_jit_sweep_yield_until_ns_rel(g, 0);
    gc2_jit_sweep_displaced_rel(g, 1);
    assert(lua_gc(L, LUA_GCSTEP, 1) == 1);
  }
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  if (suffix) {
    assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
    assert(strcmp(lua_tostring(L, -1), "terminal sweep suffix") == 0);
  }
  lua_close(L);
}

int main(void)
{
  check_close(0);
  check_close(1);
  puts("sweep terminal gate handoff OK");
  return 0;
}
