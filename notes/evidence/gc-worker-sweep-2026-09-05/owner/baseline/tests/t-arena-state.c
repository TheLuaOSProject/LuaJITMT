/*
** Focused state lifecycle test for the internal arena allocator backend.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static const char *state_churn =
  "local acc = 0\n"
  "for r=1,20 do\n"
  "  local t = {}\n"
  "  for i=1,1800 do\n"
  "    local s = tostring(r)..':'..string.rep(string.char(65+(i%26)),"
  "                                           (i%83)+1)\n"
  "    t[s] = {i, s, i%7}\n"
  "    acc = acc + #s\n"
  "  end\n"
  "  local huge = string.rep('z', 20000 + r)\n"
  "  assert(#huge > 16384)\n"
  "  collectgarbage()\n"
  "end\n"
  "local ok, ffi = pcall(require, 'ffi')\n"
  "if ok then\n"
  "  ffi.cdef('typedef struct { int x; double y; } arena_state_pair;')\n"
  "  local arr = ffi.new('arena_state_pair[?]', 512)\n"
  "  for i=0,511 do arr[i].x = i; arr[i].y = i + 0.25 end\n"
  "  assert(arr[511].x == 511)\n"
  "end\n"
  "assert(acc > 0)\n";

int main(void)
{
  int i;
  for (i = 0; i < 24; i++) {
    lua_State *L = luaL_newstate();
    assert(L != NULL);
    luaL_openlibs(L);
    if (luaL_dostring(L, state_churn) != LUA_OK) {
      const char *msg = lua_tostring(L, -1);
      fprintf(stderr, "%s\n", msg ? msg : "lua error");
      return 1;
    }
    lua_close(L);
  }
  printf("t-arena-state OK: repeated luaL_newstate churn verified\n");
  return 0;
}
