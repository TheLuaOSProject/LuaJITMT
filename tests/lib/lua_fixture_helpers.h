/*
** Shared helpers for C fixtures that drive Lua chunks.
*/

#ifndef TESTS_LIB_LUA_FIXTURE_HELPERS_H
#define TESTS_LIB_LUA_FIXTURE_HELPERS_H

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static inline void ljt_lua_fail(lua_State *L, const char *what)
{
  const char *err = lua_tostring(L, -1);
  fprintf(stderr, "%s: %s\n", what, err ? err : "(non-string)");
  assert(0);
}

static inline void ljt_lua_assert_ok(lua_State *L, int status,
				     const char *what)
{
  if (status != LUA_OK)
    ljt_lua_fail(L, what ? what : "lua status");
}

static inline lua_State *ljt_lua_newstate_openlibs(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  return L;
}

static inline void ljt_lua_dostring(lua_State *L, const char *src)
{
  ljt_lua_assert_ok(L, luaL_dostring(L, src), "lua error");
}

static inline void ljt_lua_loadstring(lua_State *L, const char *src)
{
  ljt_lua_assert_ok(L, luaL_loadstring(L, src), "luaL_loadstring");
}

static inline void ljt_lua_pcall(lua_State *L, int nargs, int nresults,
				 const char *what)
{
  ljt_lua_assert_ok(L, lua_pcall(L, nargs, nresults, 0),
		    what ? what : "lua_pcall");
}

#endif
