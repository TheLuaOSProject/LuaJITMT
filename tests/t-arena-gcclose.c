/*
** Assertion-build lua_close churn for small protos, closures and tables.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static const char *close_churn =
  "local keep = {}\n"
  "local count = 0\n"
  "local function nentries(t)\n"
  "  local n = 0\n"
  "  for _ in pairs(t) do n = n + 1 end\n"
  "  return n\n"
  "end\n"
  "local dumped = string.dump(assert(loadstring("
  "  'return function(x) return x * 3 end'))())\n"
  "for i = 1, 320 do\n"
  "  local chunk = assert(loadstring("
  "    'local n = '..i..' return function(x) return x + n end'))\n"
  "  count = count + 1; keep[count] = chunk\n"
  "  count = count + 1; keep[count] = chunk()\n"
  "  count = count + 1; keep[count] = assert(loadstring(dumped))\n"
  "  count = count + 1; keep[count] = assert(loadstring('return '..i))\n"
  "end\n"
  "for i = 1, count, 4 do keep[i] = nil end\n"
  "for i = 1, 120 do\n"
  "  keep[count+i] = {i, i+1, i+2}\n"
  "  for j = 4, 40 do keep[count+i][j] = j end\n"
  "end\n"
  "local wk = setmetatable({}, {__mode='k'})\n"
  "local wv = setmetatable({}, {__mode='v'})\n"
  "local wkv = setmetatable({}, {__mode='kv'})\n"
  "do\n"
  "  local k1, v1 = {}, {}\n"
  "  local v2 = {}\n"
  "  local k3, v3 = {}, {}\n"
  "  wk[k1] = v1\n"
  "  wv.value = v2\n"
  "  wkv[k3] = v3\n"
  "end\n"
  "collectgarbage('collect')\n"
  "assert(nentries(wk) == 0)\n"
  "assert(nentries(wv) == 0)\n"
  "assert(nentries(wkv) == 0)\n"
  "assert(count == 1280)\n";

static int table_churn_finalizer(lua_State *L)
{
  int status = luaL_dostring(L,
    "local hold = {}\n"
    "for i = 1, 160 do\n"
    "  local t = {i, i+1, i+2}\n"
    "  for j = 4, 96 do t[j] = j end\n"
    "  t['k'..i] = {i}\n"
    "  hold[i] = t\n"
    "end\n"
    "collectgarbage('collect')\n");
  if (status != LUA_OK)
    lua_pop(L, 1);
  return 0;
}

int main(void)
{
  int i;
  for (i = 0; i < 12; i++) {
    lua_State *L = luaL_newstate();
    assert(L != NULL);
    luaL_openlibs(L);
    if (luaL_dostring(L, close_churn) != LUA_OK) {
      const char *msg = lua_tostring(L, -1);
      fprintf(stderr, "%s\n", msg ? msg : "lua error");
      return 1;
    }
    lua_newuserdata(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, table_churn_finalizer);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    lua_pop(L, 1);
    lua_close(L);
  }
  printf("t-arena-gcclose OK: assertion-build lua_close churn verified\n");
  return 0;
}
