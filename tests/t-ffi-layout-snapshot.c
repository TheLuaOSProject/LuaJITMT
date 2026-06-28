/*
** Focused guard for lock-free ffi.sizeof/alignof/offsetof layout snapshots.
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
  uint32_t seq0, seq1, seq2, seq3, seq4, seq5;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef([[\n"
    "typedef struct { int a; double b; } lj_m7_layout_snapshot_t;\n"
    "typedef struct { unsigned int a; unsigned int b:5; } lj_m7_layout_bits_t;\n"
    "]])\n"
    "lj_m7_ffi = ffi\n"
    "lj_m7_layout_snapshot_ct = ffi.typeof('lj_m7_layout_snapshot_t')\n"
    "lj_m7_layout_bits_ct = ffi.typeof('lj_m7_layout_bits_t')\n"
    "lj_m7_layout_vla_ct = ffi.typeof('int [?]')\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "for i = 1, 100 do\n"
    "  assert(ffi.sizeof(lj_m7_layout_snapshot_ct) == 16)\n"
    "  assert(ffi.alignof(lj_m7_layout_snapshot_ct) == 8)\n"
    "  assert(ffi.offsetof(lj_m7_layout_snapshot_ct, 'a') == 0)\n"
    "  assert(ffi.offsetof(lj_m7_layout_snapshot_ct, 'b') == 8)\n"
    "  local ofs, bitpos, bitsz = ffi.offsetof(lj_m7_layout_bits_ct, 'b')\n"
    "  assert(ofs == 4 and bitpos == 0 and bitsz == 5)\n"
    "  assert(ffi.sizeof(lj_m7_layout_vla_ct, 7) == 28)\n"
    "  local obj = ffi.new(lj_m7_layout_snapshot_ct, { a = i, b = i + 0.5 })\n"
    "  assert(obj.a == i and obj.b == i + 0.5)\n"
    "  local arr = ffi.new(lj_m7_layout_vla_ct, 7)\n"
    "  arr[6] = i\n"
    "  assert(ffi.sizeof(arr) == 28 and arr[6] == i)\n"
    "end\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  {
    ljt_ctype_arm_trace_abort(L, cts);
    ljt_lua_dostring(L,
      "local ffi = lj_m7_ffi\n"
      "local ct = lj_m7_layout_snapshot_ct\n"
      "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
      "jit.flush()\n"
      "jit.on()\n"
      "jit.opt.start('hotloop=1', 'hotexit=1')\n"
      "local function run(n)\n"
      "  local sum = 0\n"
      "  for i = 1, n do\n"
      "    local obj = ffi.new(ct)\n"
      "    obj.a = i\n"
      "    obj.b = i + 0.5\n"
      "    sum = sum + obj.a\n"
      "  end\n"
      "  return sum\n"
      "end\n"
      "for i = 1, 3 do assert(run(8) == 36) end\n"
      "jit.attach(lj_m7_trace_parse_token)\n"
      "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");
    ljt_ctype_assert_trace_abort_released(cts);
  }
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 == seq1 + 2u);

  ljt_lua_dostring(L,
    "local ffi = lj_m7_ffi\n"
    "local ct = lj_m7_layout_snapshot_ct\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    local obj = ffi.new(ct)\n"
    "    obj.a = i\n"
    "    obj.b = i + 0.5\n"
    "    sum = sum + obj.a\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 820) end\n");
  seq3 = ljt_ctype_parse_seq(cts);
  assert(seq3 == seq2);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(ffi.offsetof('struct { int a; double b; }', 'b') == 8)\n");
  seq4 = ljt_ctype_parse_seq(cts);
  assert(seq4 == seq3 + 2u);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(ffi.sizeof('struct { int lock_path; }') == 4)\n");
  seq5 = ljt_ctype_parse_seq(cts);
  assert(seq5 != seq4);

  lua_close(L);
  printf("t-ffi-layout-snapshot OK: stable layout queries avoid cparser sequence\n");
  return 0;
}
