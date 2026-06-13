/*
** Focused guard for the M6 JIT recorder token.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_jit.h"

static void dostring(lua_State *L, const char *src)
{
  if (luaL_dostring(L, src) != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "lua error: %s\n", err ? err : "(non-string)");
    assert(0);
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  assert(la_load32_acq(&g->jit_token) == 0);

  dostring(L,
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 32 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(tracecount, true)\n"
    "local function f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 20 do assert(f(80) == 3240) end\n"
    "assert(tracecount() > 0, 'expected token-owned recording')\n");
  assert(la_load32_acq(&g->jit_token) == 0);

  dostring(L, "jit.flush()");
  la_store32_rel(&g->jit_token, 0x7fffffffu);
  dostring(L,
    "local util = require'jit.util'\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 32 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(tracecount, true)\n"
    "local function f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 40 do assert(f(80) == 3240) end\n"
    "assert(tracecount() == 0, 'busy recorder token must skip tracing')\n");
  assert(la_load32_acq(&g->jit_token) == 0x7fffffffu);

  la_store32_rel(&g->jit_token, 0);
  dostring(L,
    "local util = require'jit.util'\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 32 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(tracecount, true)\n"
    "local function f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 20 do assert(f(80) == 3240) end\n"
    "assert(tracecount() > 0, 'recording should resume after token release')\n");
  assert(la_load32_acq(&g->jit_token) == 0);

  lua_close(L);
  printf("t-jit-token OK: recorder token skips busy recording and releases\n");
  return 0;
}
