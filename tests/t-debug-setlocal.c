/* Debug writes to a captured local must invalidate constified upvalues. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_trace.h"
#include "lib/lua_fixture_helpers.h"

static int expect_trace;

static int warm_getter(lua_State *L)
{
  int i;
  assert(lua_isfunction(L, 1));
  for (i = 0; i < 100; i++) {
    lua_pushvalue(L, 1);
    lua_call(L, 0, 1);
    assert(lua_tonumber(L, -1) == 13);
    lua_pop(L, 1);
  }
  if (expect_trace)
    assert(lj_trace_hasany(G(L)));
  return 0;
}

static int set_owner(lua_State *L)
{
  lua_Debug ar;
  lua_Number value = lua_tonumber(L, 1);
  const char *name;
  assert(lua_getstack(L, 1, &ar));
  name = lua_getlocal(L, &ar, 1);
  assert(name != NULL && strcmp(name, "value") == 0);
  assert(lua_tonumber(L, -1) == 13);
  lua_pop(L, 1);
  if (expect_trace)
    assert(lj_trace_hasany(G(L)));
  lua_pushnumber(L, value);
  name = lua_setlocal(L, &ar, 1);
  assert(name != NULL && strcmp(name, "value") == 0);
  /* Repeating the current value must not retire the warmed getter. */
  if (expect_trace && value == 13)
    assert(lj_trace_hasany(G(L)));
  return 0;
}

static void run_case(int jit_on)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
#if LJ_HASJIT && LJ_TARGET_X64
  expect_trace = jit_on;
#else
  expect_trace = 0;
#endif
  lua_pushcfunction(L, warm_getter);
  lua_setglobal(L, "warm_getter");
  lua_pushcfunction(L, set_owner);
  lua_setglobal(L, "set_owner");
  ljt_lua_dostring(L, jit_on ?
    "if jit and jit.status() then jit.opt.start('hotloop=1') end" :
    "if jit then jit.off() end");
  ljt_lua_dostring(L,
    "local value = 13\n"
    "local a = function() return value end\n"
    "local b = function() return value end\n"
    "assert(debug.upvalueid(a, 1) == debug.upvalueid(b, 1))\n"
    "warm_getter(a)\n"
    "set_owner(13)\n"
    "assert(a() == 13 and b() == 13 and value == 13)\n"
    "set_owner(33)\n"
    "assert(a() == 33 and b() == 33 and value == 33)\n");
  lua_close(L);
}

int main(void)
{
  run_case(0);
  run_case(1);
  puts("t-debug-setlocal OK: C API preserves captured local identity and JIT values");
  return 0;
}
