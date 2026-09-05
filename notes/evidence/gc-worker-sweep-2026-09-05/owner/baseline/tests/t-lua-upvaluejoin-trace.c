/*
** Focused regression test for lua_upvaluejoin() trace invalidation.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static void check_lua(lua_State *L, int status, const char *what)
{
  if (status != LUA_OK) {
    fprintf(stderr, "%s: %s\n", what, lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  check_lua(L, luaL_dostring(L,
    "local util = require('jit.util')\n"
    "local function make_counter(step)\n"
    "  local x = step\n"
    "  return function(n)\n"
    "    local y = 0\n"
    "    for i = 1, n do y = y + x end\n"
    "    return y\n"
    "  end\n"
    "end\n"
    "f = make_counter(1)\n"
    "g = make_counter(7)\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "assert(f(120) == 120)\n"
    "assert(util.traceinfo(1), 'expected trace over old upvalue cell')\n"),
    "heat trace");

  lua_getglobal(L, "f");
  lua_getglobal(L, "g");
  lua_upvaluejoin(L, -2, 1, -1, 1);
  lua_pop(L, 2);

  check_lua(L, luaL_dostring(L,
    "local util = require('jit.util')\n"
    "assert(not util.traceinfo(1), 'lua_upvaluejoin did not flush trace')\n"
    "assert(f(100) == 700, 'joined upvalue value not observed')\n"),
    "verify joined upvalue");

  lua_close(L);
  printf("t-lua-upvaluejoin-trace OK: lua_upvaluejoin flushes stale traces\n");
  return 0;
}
