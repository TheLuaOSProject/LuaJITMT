/* Verify that semantic CLibrary close retains native handles until the
** joined-world trace teardown in lua_close(). */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_clib.h"

static void run(lua_State *L, const char *chunk)
{
  int rc = luaL_loadstring(L, chunk);
  if (rc == 0)
    rc = lua_pcall(L, 0, 0, 0);
  if (rc != 0) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    abort();
  }
}

int main(void)
{
  const char *so = getenv("LJ_M7_FFI_CLIB_CLOSE_SO");
  lua_State *L = luaL_newstate();
  assert(so != NULL && L != NULL);
  luaL_openlibs(L);
  lua_pushstring(L, so);
  lua_setglobal(L, "clib_close_so");
  lj_clib_test_counters_reset();
  run(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[extern int lj_m7_clib_snapshot_value;\n"
    "int lj_m7_clib_snapshot_read(void);]]\n"
    "local cl = ffi.load(assert(clib_close_so))\n"
    "assert(cl.lj_m7_clib_snapshot_value == 37)\n"
    "local read = cl.lj_m7_clib_snapshot_read\n"
    "assert(read() == 37)\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function traced_read(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + read() end\n"
    "  return sum\n"
    "end\n"
    "assert(traced_read(80) == 37 * 80)\n"
    "local gc = assert(debug.getmetatable(cl).__gc)\n"
    "gc(cl); gc(cl)\n"
    "-- The trace embeds the native symbol address. Semantic namespace close\n"
    "-- must not turn that already-published machine code into a dangling call.\n"
    "assert(read() == 37)\n"
    "assert(traced_read(80) == 37 * 80)\n"
    "clib_close_keepalive = cl\n");
  assert(lj_clib_test_retired_handles() == 1);
  assert(lj_clib_test_native_closes() == 0);
  lua_close(L);
  assert(lj_clib_test_retired_handles() == 1);
  assert(lj_clib_test_native_closes() == 1);
  printf("t-ffi-clib-deferred-close OK: native close followed trace teardown\n");
  return 0;
}
