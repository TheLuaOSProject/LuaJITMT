/*
** Focused regression test for M7 FFI cparser rollback without CTState top/hash rewind.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ctype.h"

#include "lib/lua_fixture_helpers.h"

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g;
  CTState *cts;
  CTypeID top0, top1;

  g = G(L);

  ljt_lua_dostring(L, "require('ffi')");
  cts = ctype_ctsG(g);
  assert(cts != NULL);
  top0 = ctype_top_acq(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ok = pcall(ffi.cdef, 'struct m7_badrollback { int x; ')\n"
    "assert(not ok)\n");

  top1 = ctype_top_acq(cts);
  assert(top1 > top0);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('struct m7_badrollback;')\n"
    "assert(ffi.typeof('struct m7_badrollback *'))\n"
    "ffi.cdef('struct m7_keep;')\n"
    "local ok = pcall(ffi.cdef, 'struct m7_keep { int a; ')\n"
    "assert(not ok)\n"
    "ffi.cdef('struct m7_keep { int a; };')\n"
    "local v = ffi.new('struct m7_keep')\n"
    "v.a = 42\n"
    "assert(v.a == 42)\n");

  lua_close(L);
  printf("t-ffi-cparse-rollback OK: failed parses abandon new ctype records\n");
  return 0;
}
