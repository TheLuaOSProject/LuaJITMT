/*
** Focused guard for lock-free cdata string-key field snapshots.
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
    "ffi.cdef[[\n"
    "typedef struct {\n"
    "  struct { int ax; };\n"
    "  int y;\n"
    "} lj_m7_field_snapshot_t;\n"
    "]]\n"
    "lj_m7_field_snapshot_obj = ffi.new('lj_m7_field_snapshot_t')\n"
    "lj_m7_field_snapshot_ptr = ffi.cast('lj_m7_field_snapshot_t *',\n"
    "                                     lj_m7_field_snapshot_obj)\n"
    "lj_m7_field_snapshot_obj.ax = 11\n"
    "lj_m7_field_snapshot_obj.y = 13\n"
    "jit.off()\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  seq0 = parse_seq(cts);

  ljt_lua_dostring(L,
    "local obj = lj_m7_field_snapshot_obj\n"
    "local ptr = lj_m7_field_snapshot_ptr\n"
    "for i = 1, 100 do\n"
    "  assert(obj.ax == 10 + i)\n"
    "  assert(ptr.y == 12 + i)\n"
    "  obj.ax = obj.ax + 1\n"
    "  ptr.y = ptr.y + 1\n"
    "end\n");
  seq1 = parse_seq(cts);
  assert(seq1 == seq0);

  ljt_lua_dostring(L,
    "local obj = lj_m7_field_snapshot_obj\n"
    "local ptr = lj_m7_field_snapshot_ptr\n"
    "obj.ax = 5\n"
    "obj.y = 0\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    obj.y = i\n"
    "    sum = sum + obj.ax + ptr.y\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 1020) end\n");
  seq2 = parse_seq(cts);
  assert(seq2 == seq1);

  lua_close(L);
  printf("t-ffi-field-snapshot OK: stable cdata fields avoid cparser sequence\n");
  return 0;
}
