/*
** M8 close-time finalizer drain coverage.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static int cdata_finalized;
static int udata_finalized;

static int close_cdata_finalizer(lua_State *L)
{
  (void)L;
  cdata_finalized++;
  return 0;
}

static int close_udata_finalizer(lua_State *L)
{
  udata_finalized++;
  if (udata_finalized == 1) {
    lua_newuserdata(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, close_udata_finalizer);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    lua_pop(L, 1);
  }
  return 0;
}

static void push_close_udata(lua_State *L)
{
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, close_udata_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  lua_pushcfunction(L, close_cdata_finalizer);
  lua_setglobal(L, "m8_close_cdata_finalizer");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[typedef struct { int x; } lj_m8_close_fin_t;]]\n"
    "ffi.gc(ffi.new('lj_m8_close_fin_t'), m8_close_cdata_finalizer)\n") ==
	 LUA_OK);
  push_close_udata(L);

  lua_close(L);
  assert(cdata_finalized == 1);
  assert(udata_finalized == 2);
  printf("t-m8-close-finalizers OK: lua_close drains cdata and chained udata finalizers\n");
  return 0;
}
