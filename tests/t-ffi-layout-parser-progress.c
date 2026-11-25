/* Suspend an actual cdef after its provisional definition becomes visible
** to the parser. Public layout readers must still see the previous type. */
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_ctype.h"
#include "lib/lua_fixture_helpers.h"

static uint32_t stage, target;

CType *__real_lj_ctype_publish(CTState *cts, CTypeID id, CType *src);
CType *__wrap_lj_ctype_publish(CTState *cts, CTypeID id, CType *src)
{
  CType *ct = __real_lj_ctype_publish(cts, id, src);
  if (id == la_load32_acq(&target) && ctype_size_acq(ct) == 4 &&
      la_load32_acq(&stage) == 0) {
    la_store32_rel(&stage, 1);
    while (la_load32_acq(&stage) == 1)
      sched_yield();
  }
  return ct;
}

static int wait_for_parser(lua_State *L)
{
  UNUSED(L);
  while (!la_load32_acq(&stage)) sched_yield();
  return 0;
}

static int release_parser(lua_State *L)
{
  UNUSED(L);
  assert(la_load32_acq(&stage) == 1);
  la_store32_rel(&stage, 2);
  return 0;
}

static int watch_type(lua_State *L)
{
  la_store32_rel(&target, (uint32_t)luaL_checkinteger(L, 1));
  la_store32_rel(&stage, 0);
  return 0;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  lua_register(L, "wait_for_parser", wait_for_parser);
  lua_register(L, "release_parser", release_parser);
  lua_register(L, "watch_type", watch_type);
  ljt_lua_dostring(L,
    "local ffi, th = require('ffi'), require('threading')\n"
    "ffi.cdef('struct layout_ready { char a; double b; };')\n"
    "local ready = ffi.typeof('struct layout_ready')\n"
    "for n = 1, 2 do\n"
    "  local name = 'struct layout_pending_'..n\n"
    "  ffi.cdef(name..';')\n"
    "  local pending = ffi.typeof(name)\n"
    "  watch_type(tonumber(pending))\n"
    "  local src = name..' { int x; };'..(n == 1 and '@' or '')\n"
    "  local w = th.spawn(function(src)\n"
    "    local ok = pcall(require('ffi').cdef, src); return ok\n"
    "  end, src)\n"
    "  wait_for_parser()\n"
    "  assert(ffi.sizeof(ready) == 16 and ffi.alignof(ready) == 8)\n"
    "  assert(ffi.offsetof(ready, 'b') == 8)\n"
    "  assert(ffi.sizeof(pending) == nil)\n"
    "  assert(ffi.offsetof(pending, 'x') == nil)\n"
    "  release_parser()\n"
    "  local joined, committed = w:join(10)\n"
    "  assert(joined and committed == (n == 2))\n"
    "  if committed then\n"
    "    assert(ffi.sizeof(pending) == 4 and ffi.offsetof(pending, 'x') == 0)\n"
    "  else assert(ffi.sizeof(pending) == nil) end\n"
    "  collectgarbage()\n"
    "end\n");
  lua_close(L);
  puts("t-ffi-layout-parser-progress OK");
  return 0;
}
