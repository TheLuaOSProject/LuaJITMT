/*
** Focused guard for ffi.istype() ctype comparison snapshots.
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
    "ffi.cdef('typedef struct { int x; } lj_m7_istype_snapshot_t;')\n"
    "lj_m7_istype_snapshot_ct = ffi.typeof('lj_m7_istype_snapshot_t')\n"
    "lj_m7_istype_snapshot_pct = ffi.typeof('lj_m7_istype_snapshot_t *')\n"
    "lj_m7_istype_snapshot_arr = ffi.typeof('lj_m7_istype_snapshot_t[1]')\n"
    "lj_m7_istype_snapshot_int = ffi.typeof('int')\n"
    "lj_m7_istype_snapshot_uint8 = ffi.typeof('uint8_t')\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  seq0 = parse_seq(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ct = lj_m7_istype_snapshot_ct\n"
    "local pct = lj_m7_istype_snapshot_pct\n"
    "local arr_t = lj_m7_istype_snapshot_arr\n"
    "local int_t = lj_m7_istype_snapshot_int\n"
    "local uint8_t = lj_m7_istype_snapshot_uint8\n"
    "local arr = ffi.new(arr_t)\n"
    "local ptr = ffi.cast(pct, arr)\n"
    "local ival = int_t()\n"
    "local uval = uint8_t()\n"
    "for i = 1, 100 do\n"
    "  assert(ffi.istype(ct, arr[0]) == true)\n"
    "  assert(ffi.istype(ct, ptr) == true)\n"
    "  assert(ffi.istype(pct, ptr) == true)\n"
    "  assert(ffi.istype(pct, arr[0]) == false)\n"
    "  assert(ffi.istype(int_t, ival) == true)\n"
    "  assert(ffi.istype(int_t, uval) == false)\n"
    "  assert(ffi.istype(int_t, int_t) == true)\n"
    "end\n");
  seq1 = parse_seq(cts);
  assert(seq1 == seq0);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local obj = ffi.new(lj_m7_istype_snapshot_ct)\n"
    "assert(ffi.istype('struct { int lock_path; }', obj) == false)\n");
  seq2 = parse_seq(cts);
  assert(seq2 != seq1);

  lua_close(L);
  printf("t-ffi-istype-snapshot OK: stable istype comparisons avoid cparser sequence\n");
  return 0;
}
