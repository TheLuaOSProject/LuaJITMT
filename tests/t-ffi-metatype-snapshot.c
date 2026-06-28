/*
** Focused guard for ffi.metatype() string-type validation snapshots.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_ctype.h"

#include "lib/ctype_parse_fixture_helpers.h"
#include "lib/lua_fixture_helpers.h"

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  uint32_t seq0, seq1, seq2, seq3;

  ljt_lua_dostring(L, "require('ffi')\n");
  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ct = ffi.metatype('struct { int x; }', {\n"
    "  __index = { get = function(self) return self.x + 5 end },\n"
    "})\n"
    "assert(ct(37):get() == 42)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } lj_m7_metatype_snapshot_obj_t;')\n"
    "lj_m7_metatype_snapshot_obj_ct = "
    "ffi.typeof('lj_m7_metatype_snapshot_obj_t')\n");
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 != seq1);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ct = ffi.metatype(lj_m7_metatype_snapshot_obj_ct, {\n"
    "  __index = { get = function(self) return self.x + 3 end },\n"
    "})\n"
    "assert(ct(39):get() == 42)\n");
  seq3 = ljt_ctype_parse_seq(cts);
  assert(seq3 == seq2);

  lua_close(L);
  printf("t-ffi-metatype-snapshot OK: string metatype validates after parse snapshot\n");
  return 0;
}
