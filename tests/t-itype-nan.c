/*
** Focused guard for NaN TValue tag classification.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"

static void assert_lua_ok(lua_State *L, int status, const char *what)
{
  if (status != LUA_OK) {
    fprintf(stderr, "%s: %s\n", what, lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void assert_nan_number(double n)
{
  assert(n != n);
}

static void assert_nan_tv(cTValue *tv)
{
  assert(tvisnumber(tv));
  assert(tvisnum(tv));
  assert(!tvisint(tv));
  assert(!tvisgcv(tv));
  assert(tvisnan(tv));
  assert(itypemap(tv) == ~LJ_TNUMX);
}

int main(void)
{
  TValue tv;
  lua_State *L;
  lua_Number n;

  setnanV(&tv);
  assert_nan_tv(&tv);

  n = 0.0 / 0.0;
  setnumV(&tv, n);
  assert_nan_tv(&tv);

  L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  lua_pushnumber(L, n);
  assert(lua_type(L, -1) == LUA_TNUMBER);
  assert_nan_number(lua_tonumber(L, -1));
  lua_pop(L, 1);

  assert_lua_ok(L, luaL_loadstring(L,
    "local a = 0/0\n"
    "local b = tonumber('nan')\n"
    "return a, b, type(a), type(b), a ~= a, b ~= b\n"),
    "luaL_loadstring");
  assert_lua_ok(L, lua_pcall(L, 0, 6, 0), "lua_pcall");

  assert(lua_type(L, 1) == LUA_TNUMBER);
  assert(lua_type(L, 2) == LUA_TNUMBER);
  assert_nan_number(lua_tonumber(L, 1));
  assert_nan_number(lua_tonumber(L, 2));
  assert(lua_isstring(L, 3) && lua_isstring(L, 4));
  assert(strcmp(lua_tostring(L, 3), "number") == 0);
  assert(strcmp(lua_tostring(L, 4), "number") == 0);
  assert(lua_toboolean(L, 5));
  assert(lua_toboolean(L, 6));

  lua_close(L);
  printf("t-itype-nan OK: NaNs remain Lua numbers\n");
  return 0;
}
