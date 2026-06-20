/*
** Focused guard for lock-free ffi.C namespace snapshots.
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
    "int abs(int);\n"
    "unsigned long lj_m7_ns_strlen(const char *) asm(\"strlen\");\n"
    "enum { LJ_M7_NS_CONST = 91 };\n"
    "]]\n"
    "jit.off()\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  seq0 = parse_seq(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(ffi.C.LJ_M7_NS_CONST == 91)\n"
    "assert(ffi.C.abs(-44) == 44)\n"
    "assert(tonumber(ffi.C.lj_m7_ns_strlen('abcd')) == 4)\n");
  seq1 = parse_seq(cts);
  assert(seq1 == seq0);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local C = ffi.C\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    sum = sum + C.LJ_M7_NS_CONST + C.abs(-i)\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 4460) end\n");
  seq2 = parse_seq(cts);
  assert(seq2 == seq1);

  lua_close(L);
  printf("t-ffi-namespace-snapshot OK: stable ffi.C lookups avoid cparser sequence\n");
  return 0;
}
