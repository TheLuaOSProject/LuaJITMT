/* An explicit collection can widen its next scan after a fixed Lua-to-C call. */
#include <assert.h>
#include <stdio.h>

#include "lj_state.h"
#include "lib/lua_fixture_helpers.h"

static int check_slots(lua_State *L)
{
  ptrdiff_t offset;
  TValue *scratch;
  int collect = lua_toboolean(L, 2), clear = lua_toboolean(L, 3);
  assert(lua_gettop(L) == 3 && lua_istable(L, 1));
  assert(lua_checkstack(L, 192));
  lua_settop(L, 128);
  lua_pushvalue(L, 1);
  offset = savestack(L, L->top - 1);
  lua_settop(L, 3);
  assert(tvistab(restorestack(L, offset)));
  (void)lua_gc(L, collect ? LUA_GCCOLLECT : LUA_GCSTEP, 1);
  scratch = restorestack(L, offset);
  assert(scratch >= L->top && scratch < tvref(L->maxstack));
  if (clear)
    assert(tvisnil(scratch));
  else
    assert(tvistab(scratch) && tabV(scratch) == tabV(L->base));
  assert(lua_gettop(L) == 3 && lua_istable(L, 1));
  lua_pushvalue(L, 1);
  return 1;
}

static int nested_call(lua_State *L)
{
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  ljt_lua_pcall(L, 1, 0, "nested collection");
  return 0;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  lua_register(L, "check_slots", check_slots);
  lua_register(L, "nested_call", nested_call);
  ljt_lua_dostring(L,
    "local function fixed(collect, clear)\n"
    "  local obj, guard = {}, {token = 93}\n"
    "  local function read() return guard.token end\n"
    "  local result = check_slots(obj, collect, clear)\n"
    "  assert(result == obj and read() == 93)\n"
    "end\n"
    "local stripped = assert(loadstring(string.dump(fixed, true)))\n"
    "for _, collect in ipairs({false, true}) do\n"
    "  fixed(collect, true)\n"
    "  stripped(collect, true)\n"
    "  nested_call(function(mode) fixed(mode, false) end, collect)\n"
    "  local a = {{}, collect, false}\n"
    "  local result = check_slots(unpack(a))\n"
    "  assert(result == a[1])\n"
    "end\n"
    "local ffi = require('ffi')\n"
    "ffi.cdef[[void qsort(void *, size_t, size_t,\n"
    "  int (*)(const void *, const void *));]]\n"
    "local cmp = ffi.cast('int (*)(const void *, const void *)', function(a, b)\n"
    "  fixed(true, false)\n"
    "  return ffi.cast('const int32_t *', a)[0] - ffi.cast('const int32_t *', b)[0]\n"
    "end)\n"
    "local values = ffi.new('int32_t[2]', 9, 2)\n"
    "ffi.C.qsort(values, 2, ffi.sizeof('int32_t'), cmp)\n"
    "assert(values[0] == 2 and values[1] == 9)\n"
    "cmp:free()");
  lua_close(L);
  puts("t-gc2-call-slots OK");
  return 0;
}
