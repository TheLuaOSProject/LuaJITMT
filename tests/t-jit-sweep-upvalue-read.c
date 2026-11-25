/* Repeated rooted UGETs during a real SWEEP keep the barrier queue available
** for mutations. A subsequent collectable store still survives collection. */
#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_tg.h"

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCRef *cursor;
  unsigned i;
  assert(L);
  g = G(L);
  tg = L2TG(L);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(luaL_dostring(L, "local a=314; return function() return a end") == 0);
  lua_pushnil(L);
  lua_createtable(L, 1, 0);
  lua_pushinteger(L, 271);
  lua_rawseti(L, 3, 1);
  for (i = 0; i < 3000; i++) {
    lua_newtable(L);
    lua_pop(L, 1);
  }
  for (i = 0; i < 10000 && (gc2_phase_acq(g) != LJ_GC2_SWEEP ||
                             !gc2_sweep_bridge_ready_acq(g)); i++)
    (void)lj_gc2_step_explicit(L, 1);
  assert(i < 10000);
  cursor = lj_tg_ssb_next_acq(tg);
  for (i = 0; i < 512; i++) {
    assert(lj_func_getuv_rooted_try(L, L->base, 0, L->base+1));
    assert(lua_tointeger(L, 2) == 314);
    assert(lj_tg_ssb_next_acq(tg) == cursor);
  }
  assert(lj_func_storeuv_rooted_try(L, L->base, 0, L->base+2));
  lua_settop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_pushnil(L);
  assert(lj_func_getuv_rooted_try(L, L->base, 0, L->base+1));
  lua_rawgeti(L, 2, 1);
  assert(lua_tointeger(L, -1) == 271);
  lua_close(L);
  puts("t-jit-sweep-upvalue-read OK");
  return 0;
}
