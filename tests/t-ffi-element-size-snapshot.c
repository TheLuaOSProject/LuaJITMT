/*
** Focused guard for lock-free cdata element-size snapshots.
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
    "ffi.cdef('typedef int lj_m7_elem_snapshot_t;')\n"
    "lj_m7_elem_arr = ffi.new('lj_m7_elem_snapshot_t[4]', {10, 20, 30, 40})\n"
    "lj_m7_elem_ptr = ffi.cast('lj_m7_elem_snapshot_t *', lj_m7_elem_arr)\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  seq0 = parse_seq(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local p = lj_m7_elem_ptr\n"
    "for i = 1, 100 do\n"
    "  assert(p[2] == 30)\n"
    "  local q = p + 3\n"
    "  assert(q - p == 3)\n"
    "end\n");
  seq1 = parse_seq(cts);
  assert(seq1 == seq0);

  ljt_lua_dostring(L,
    "local p = lj_m7_elem_ptr\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    local k = (i - 1) % 4\n"
    "    local q = p + k\n"
    "    sum = sum + p[k] + tonumber(q - p)\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 1060) end\n");
  seq2 = parse_seq(cts);
  assert(seq2 == seq1);

  lua_close(L);
  printf("t-ffi-element-size-snapshot OK: stable cdata element-size readers avoid cparser sequence\n");
  return 0;
}
