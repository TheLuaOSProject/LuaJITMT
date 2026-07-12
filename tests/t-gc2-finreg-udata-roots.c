/*
** Focused regression for GC2 userdata FINREG raw-list roots.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc2.h"

static int empty_finalizer(lua_State *L)
{
  (void)L;
  return 0;
}

static GC2FinRegUDataNode *active_node(global_State *g, GCobj *target)
{
  GC2FinRegUDataNode *node;
  for (node = gc2_finreg_udata_head_acq(g);
       node != NULL && lj_gc2_mem_registered(g, node);
       node = gc2_finreg_udata_next_acq(node)) {
    if (gc2_finreg_udata_active_acq(node) &&
	gc2_finreg_udata_obj_acq(node) == target)
      return node;
  }
  return NULL;
}

static int retired_contains(global_State *g, GC2FinRegUDataNode *target)
{
  GC2FinRegUDataNode *node;
  for (node = gc2_finreg_udata_retired_acq(g);
       node != NULL && lj_gc2_mem_registered(g, node);
       node = gc2_finreg_udata_retired_next_acq(node)) {
    if (node == target)
      return 1;
  }
  return 0;
}

static void full_cycle(lua_State *L)
{
  lua_gc(L, LUA_GCRESTART, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  GC2FinRegUDataNode *node;
  GCobj *o;
  int i;

  assert(L != NULL);
  g = G(L);
  lua_gc(L, LUA_GCSTOP, 0);

  lua_newuserdata(L, 1);
  o = obj2gco(udataV(L->top - 1));
  lua_newtable(L);
  lua_pushcfunction(L, empty_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);

  node = active_node(g, o);
  assert(node != NULL);
  for (i = 0; i < 4; i++) {
    full_cycle(L);
    assert(lj_gc2_mem_registered(g, node));
    assert(active_node(g, o) == node);
  }

  /* Clearing the metatable logically retires and unlinks this raw record. */
  lua_pushnil(L);
  lua_setmetatable(L, -2);
  assert(active_node(g, o) == NULL);
  assert(retired_contains(g, node));
  for (i = 0; i < 4; i++) {
    full_cycle(L);
    assert(lj_gc2_mem_registered(g, node));
    assert(retired_contains(g, node));
  }

  lua_pop(L, 1);
  lua_close(L);
  printf("t-gc2-finreg-udata-roots OK: active and retired raw nodes survived\n");
  return 0;
}
