/*
** M8 finalizer callback state ownership coverage.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static lua_State *expected_L;
static int expected_slot;
static int finalized[3];

static int state_finalizer(lua_State *L)
{
  assert(L == expected_L);
  finalized[expected_slot]++;
  return 0;
}

static void drive_full_gc(lua_State *L)
{
  int i;
  for (i = 0; i < 4; i++)
    lua_gc(L, LUA_GCCOLLECT, 0);
}

static void push_state_udata(lua_State *L)
{
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, state_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
}

static void check_udata_on_state(lua_State *L, int slot)
{
  expected_L = L;
  expected_slot = slot;
  push_state_udata(L);
  drive_full_gc(L);
  assert(finalized[slot] == 1);
}

static void check_cdata_on_state(lua_State *L, int slot)
{
  static const char script[] =
    "local ffi = require('ffi')\n"
    "ffi.cdef[[typedef struct { int x; } lj_m8_fin_state_t;]]\n"
    "ffi.gc(ffi.new('lj_m8_fin_state_t'), m8_state_finalizer)\n";
  int status;
  expected_L = L;
  expected_slot = slot;
  lua_pushcfunction(L, state_finalizer);
  lua_setglobal(L, "m8_state_finalizer");
  status = luaL_dostring(L, script);
  if (status != LUA_OK) {
    fprintf(stderr, "cdata setup failed: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  drive_full_gc(L);
  assert(finalized[slot] == 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  assert(L != NULL);
  luaL_openlibs(L);

  check_udata_on_state(L, 0);
  check_cdata_on_state(L, 1);

  co = lua_newthread(L);
  assert(co != NULL);
  check_udata_on_state(co, 2);
  lua_pop(L, 1);

  lua_close(L);
  printf("t-m8-finalizer-state OK: finalizer callbacks run on caller state\n");
  return 0;
}
