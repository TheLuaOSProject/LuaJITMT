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

#include "lib/lua_fixture_helpers.h"

static uint32_t parse_seq(CTState *cts)
{
  uint32_t seq = la_load32_acq(&cts->parse_token);
  assert((seq & 1u) == 0);
  return seq;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  uint32_t seq0, seq1, seq2;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef([[\n"
    "typedef struct { int a; double b; } lj_m7_layout_snapshot_t;\n"
    "typedef struct { unsigned int a; unsigned int b:5; } lj_m7_layout_bits_t;\n"
    "]])\n"
    "lj_m7_layout_snapshot_ct = ffi.typeof('lj_m7_layout_snapshot_t')\n"
    "lj_m7_layout_bits_ct = ffi.typeof('lj_m7_layout_bits_t')\n"
    "lj_m7_layout_vla_ct = ffi.typeof('int [?]')\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  seq0 = parse_seq(cts);

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
  seq1 = parse_seq(cts);
  assert(seq1 == seq0);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(ffi.sizeof('struct { int lock_path; }') == 4)\n");
  seq2 = parse_seq(cts);
  assert(seq2 != seq1);

  lua_close(L);
  printf("t-ffi-layout-snapshot OK: stable layout queries avoid cparser sequence\n");
  return 0;
}
