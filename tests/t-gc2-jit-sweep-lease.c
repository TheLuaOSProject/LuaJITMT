/* A completed machine trace consumes the open SWEEP scheduling turn. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lib/lua_fixture_helpers.h"

int main(void)
{
#if LJ_HASJIT
  lua_State *L = luaL_newstate();
  global_State *g;
  uint32_t i;
  assert(L);
  luaL_openlibs(L);
  g = G(L);
  ljt_lua_dostring(L,
    "collectgarbage('stop')\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function sum(n)\n"
    "  local x = 0\n"
    "  for i = 1, n do x = x + i end\n"
    "  return x\n"
    "end\n"
    "assert(sum(4000) == 8002000)\n"
    "assert(require('jit.util').traceinfo(1), 'sum did not trace')\n"
    "_G.test_sum = sum\n"
    "jit.off(true)\n"
    "for i = 1, 3000 do local dead = {i}; assert(dead[1] == i) end\n");
  lua_getglobal(L, "test_sum");
  assert(lua_isfunction(L, -1));
  for (i = 0; i < 10000u; i++) {
    if (gc2_phase_acq(g) == LJ_GC2_SWEEP &&
        gc2_sweep_bridge_ready_acq(g) && gc2_jit_phase_gate_acq(g))
      break;
    (void)lj_gc2_step_explicit(L, 1);
  }
  assert(i < 10000u);
  /* Hold the real gate's deadline in the future for the entire observation. */
  gc2_jit_sweep_yield_until_ns_rel(g, UINT64_MAX);
  assert(lj_gc2_worker_drain(g, 1) == 0);
  assert(!lj_tg_any_jit_active(g));
  lua_pushinteger(L, 4000);
  ljt_lua_pcall(L, 1, 1, "return through a cooperative SWEEP trace");
  assert(lua_tointeger(L, -1) == 8002000);
  assert(!lj_tg_any_jit_active(g));
  assert(gc2_jit_sweep_yield_until_ns_acq(g) == UINT64_MAX);
  assert(lj_gc2_worker_drain(g, 1) == 1);
  lua_close(L);
  puts("t-gc2-jit-sweep-lease OK");
#else
  puts("t-gc2-jit-sweep-lease skipped");
#endif
  return 0;
}
