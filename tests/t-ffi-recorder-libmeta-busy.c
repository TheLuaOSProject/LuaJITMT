/*
** Focused guard for nonblocking recorder FFI library ctype metadata reads.
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

static void assert_busy_trace_releases(lua_State *L, CTState *cts,
				       const char *chunk)
{
  ljt_ctype_arm_trace_abort(L, cts);
  ljt_lua_dostring(L, chunk);
  ljt_ctype_assert_trace_abort_released(cts);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef enum { LJ_M7_RECMETA_A = -7, LJ_M7_RECMETA_B = 5 } "
    "lj_m7_recmeta_e;\n"
    "]]\n"
    "lj_m7_recmeta_i64 = ffi.new('int64_t', 1234567)\n"
    "lj_m7_recmeta_enum = ffi.new('lj_m7_recmeta_e', 'LJ_M7_RECMETA_A')\n"
    "lj_m7_recmeta_u64 = ffi.new('uint64_t', 0x12345678ULL)\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);

  assert_busy_trace_releases(L, cts,
    "local v = lj_m7_recmeta_i64\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + tonumber(v) end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 9876536) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  assert_busy_trace_releases(L, cts,
    "local v = lj_m7_recmeta_enum\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + tonumber(v) end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == -56) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  assert_busy_trace_releases(L, cts,
    "local bit = require('bit')\n"
    "local ffi = require('ffi')\n"
    "local v = lj_m7_recmeta_u64\n"
    "local mask = ffi.new('uint64_t', 0xffULL)\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local x = ffi.new('uint64_t', 0)\n"
    "  for i = 1, n do x = bit.bxor(x, bit.band(v, mask)) end\n"
    "  return tostring(x)\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == '0ULL') end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  ljt_lua_dostring(L,
    "local bit = require('bit')\n"
    "local ffi = require('ffi')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local i64 = lj_m7_recmeta_i64\n"
    "local enum = lj_m7_recmeta_enum\n"
    "local u64 = lj_m7_recmeta_u64\n"
    "local mask = ffi.new('uint64_t', 0xffULL)\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  local x = ffi.new('uint64_t', 0)\n"
    "  for i = 1, n do\n"
    "    sum = sum + tonumber(i64) + tonumber(enum)\n"
    "    x = bit.bxor(x, bit.band(u64, mask))\n"
    "  end\n"
    "  return sum, tostring(x)\n"
    "end\n"
    "for i = 1, 30 do\n"
    "  local sum, x = run(40)\n"
    "  assert(sum == 49382400 and x == '0ULL')\n"
    "end\n");

  lua_close(L);
  printf("t-ffi-recorder-libmeta-busy OK: recorder FFI metadata reads abort instead of waiting\n");
  return 0;
}
