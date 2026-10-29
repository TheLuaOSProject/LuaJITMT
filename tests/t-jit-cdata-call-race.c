#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_meta.h"
#include "lj_record.h"

static int test_arm(lua_State *L)
{
  UNUSED(L);
  lj_record_test_arm_cdata_call_capture();
  return 0;
}

static int test_paused(lua_State *L)
{
  lua_pushboolean(L, lj_meta_test_mt_capture_paused() != 0);
  return 1;
}

static int test_release(lua_State *L)
{
  UNUSED(L);
  lj_meta_test_mt_capture_release();
  return 0;
}

static int run_script(lua_State *L, const char *path)
{
  lua_newtable(L);
  lua_pushcfunction(L, test_arm);
  lua_setfield(L, -2, "arm");
  lua_pushcfunction(L, test_paused);
  lua_setfield(L, -2, "paused");
  lua_pushcfunction(L, test_release);
  lua_setfield(L, -2, "release");
  lua_setglobal(L, "call_race_test");

  if (luaL_dostring(L,
      "package.path = './tests/lib/?.lua;./src/?.lua;"
      "./src/jit/?.lua;' .. package.path") != 0)
    return 0;
  if (luaL_dofile(L, path) != 0)
    return 0;
  return 1;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  int ok;
  if (L == NULL) return 1;
  luaL_openlibs(L);
  ok = run_script(L, "tests/t-ffi-callxs-remote-flush.lua");
  if (!ok) {
    const char *message = lua_tostring(L, -1);
    fprintf(stderr, "remote cdata __call recorder race failed: %s\n",
            message ? message : "unknown Lua error");
    lua_close(L);
    return 1;
  }
  lua_close(L);
  return 0;
}
