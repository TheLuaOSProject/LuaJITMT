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

#include "lib/ctype_growth_fixture_helpers.h"
#include "lib/lua_fixture_helpers.h"

static void assert_new_range_abandoned(CTState *cts, CTypeID first, CTypeID last)
{
  CTypeID id;
  for (id = first; id < last; id++) {
    CType snap;
    assert(lj_ctype_snapshot(cts, id, &snap) == 0);
  }
}

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
  ljt_ctype_force_table_growth(L, cts, "lj_m7_cparse_rollback_grow");
  top0 = ctype_top_acq(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ok = pcall(ffi.cdef, 'struct m7_badrollback { int x; ')\n"
    "assert(not ok)\n");

  top1 = ctype_top_acq(cts);
  assert(top1 > top0);
  assert_new_range_abandoned(cts, top0, top1);

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

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('struct m7_layoutrollback; enum m7_enumrollback;')\n"
    "local st = ffi.typeof('struct m7_layoutrollback')\n"
    "local et = ffi.typeof('enum m7_enumrollback')\n"
    "local ok = pcall(ffi.cdef, [[\n"
    "struct m7_layoutrollback { char c; int x; unsigned b:3; };\n"
    "enum m7_enumrollback { M7_ENUMROLLBACK_TMP = -4 };\n"
    "@\n"
    "]])\n"
    "assert(not ok)\n"
    "assert(ffi.sizeof(st) == nil)\n"
    "assert(ffi.sizeof(et) == nil)\n"
    "assert(ffi.offsetof(st, 'x') == nil)\n"
    "assert(not pcall(ffi.cast, et, 'M7_ENUMROLLBACK_TMP'))\n"
    "ffi.cdef[[\n"
    "struct m7_layoutrollback { double d; int y; };\n"
    "enum m7_enumrollback { M7_ENUMROLLBACK_OK = 5 };\n"
    "]]\n"
    "assert(ffi.offsetof(st, 'y') == 8)\n"
    "assert(ffi.sizeof(st) >= 12)\n"
    "assert(tonumber(ffi.cast(et, 'M7_ENUMROLLBACK_OK')) == 5)\n");

  top0 = ctype_top_acq(cts);
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ok = pcall(ffi.cdef, [[\n"
    "extern int lj_m7_rb_redir_gap __asm__(\"lj_m7_rb_redir_real\");\n"
    "@\n"
    "]])\n"
    "assert(not ok)\n");
  top1 = ctype_top_acq(cts);
  assert(top1 > top0);
  assert_new_range_abandoned(cts, top0, top1);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef int lj_m7_rb_redir_gap;')\n"
    "assert(ffi.sizeof('lj_m7_rb_redir_gap') == 4)\n");

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "struct lj_m7_empty_typedef_decl;\n"
    "typedef struct lj_m7_empty_typedef_decl lj_m7_empty_typedef_decl_t;\n"
    "lj_m7_empty_typedef_decl_t;\n"
    "struct lj_m7_empty_typedef_decl { int x; };\n"
    "lj_m7_empty_typedef_decl_t;\n"
    "]]\n"
    "assert(ffi.sizeof('lj_m7_empty_typedef_decl_t') == 4)\n");

  lua_close(L);
  printf("t-ffi-cparse-rollback OK: failed parses abandon new ctype records\n");
  return 0;
}
