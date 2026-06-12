/*
** Assertion-build lua_close churn for many small protos and closures.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static const char *close_churn =
  "local keep = {}\n"
  "local count = 0\n"
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
  "assert(count == 1280)\n";

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
    lua_close(L);
  }
  printf("t-arena-gcclose OK: assertion-build lua_close churn verified\n");
  return 0;
}
