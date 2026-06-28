/*
** Focused guard for lock-free enum string constant snapshots.
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

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('enum lj_m7_enum_snapshot_t { LJ_M7_ENUM_A = 5, LJ_M7_ENUM_B = 7 };')\n"
    "lj_m7_ffi = ffi\n"
    "lj_m7_enum_snapshot_ct = ffi.typeof('enum lj_m7_enum_snapshot_t')\n"
    "lj_m7_enum_snapshot_a = ffi.cast(lj_m7_enum_snapshot_ct, 'LJ_M7_ENUM_A')\n"
    "jit.off()\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ct = lj_m7_enum_snapshot_ct\n"
    "for i = 1, 100 do\n"
    "  assert(tonumber(ffi.cast(ct, 'LJ_M7_ENUM_B')) == 7)\n"
    "  assert(tonumber(ffi.cast(ct, 5)) == 5)\n"
    "end\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  {
    ljt_ctype_arm_trace_abort(L, cts);
    ljt_lua_dostring(L,
      "local ffi = lj_m7_ffi\n"
      "local ct = lj_m7_enum_snapshot_ct\n"
      "local a = lj_m7_enum_snapshot_a\n"
      "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
      "jit.flush()\n"
      "jit.on()\n"
      "jit.opt.start('hotloop=1', 'hotexit=1')\n"
      "local function run(n)\n"
      "  local sum = 0\n"
      "  for i = 1, n do\n"
      "    sum = sum + tonumber(ffi.cast(ct, 'LJ_M7_ENUM_A'))\n"
      "    if a == 'LJ_M7_ENUM_A' then sum = sum + 1 end\n"
      "  end\n"
      "  return sum\n"
      "end\n"
      "for i = 1, 3 do assert(run(8) == 48) end\n"
      "jit.attach(lj_m7_trace_parse_token)\n"
      "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");
    ljt_ctype_assert_trace_abort_released(cts);
  }
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 == seq1 + 2u);

  ljt_lua_dostring(L,
    "local ffi = lj_m7_ffi\n"
    "local ct = lj_m7_enum_snapshot_ct\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local a = lj_m7_enum_snapshot_a\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    sum = sum + tonumber(ffi.cast(ct, 'LJ_M7_ENUM_A'))\n"
    "    if a == 'LJ_M7_ENUM_A' then sum = sum + 1 end\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 240) end\n");
  seq3 = ljt_ctype_parse_seq(cts);
  assert(seq3 == seq2);

  lua_close(L);
  printf("t-ffi-enum-snapshot OK: stable enum string constants avoid cparser sequence\n");
  return 0;
}
