/*
** Focused guard for direct registry root replacement.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

static void set_field(lua_State *L, const char *key, int value)
{
  lua_pushstring(L, key);
  lua_pushinteger(L, value);
  lua_rawset(L, -3);
}

static void check_registry_field(lua_State *L, const char *key, int value)
{
  lua_getregistry(L);
  assert(lua_istable(L, -1));
  lua_getfield(L, -1, key);
  assert(lua_tointeger(L, -1) == value);
  lua_pop(L, 2);
}

static void replace_registry(lua_State *L)
{
  lua_newtable(L);
  set_field(L, "replace", 17);
  lua_replace(L, LUA_REGISTRYINDEX);
  lua_gc(L, LUA_GCCOLLECT, 0);
  check_registry_field(L, "replace", 17);
}

static void copy_registry(lua_State *L)
{
  lua_newtable(L);
  set_field(L, "copy", 23);
  lua_copy(L, -1, LUA_REGISTRYINDEX);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  check_registry_field(L, "copy", 23);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  replace_registry(L);
  copy_registry(L);
  lua_close(L);
  printf("t-registry-root OK: direct registry replacement remains rooted\n");
  return 0;
}
