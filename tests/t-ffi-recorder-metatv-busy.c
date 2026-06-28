/*
** Focused guard for nonblocking recorder ctype metamethod lookup.
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

static void assert_predefined_no_meta_trace_completes(lua_State *L,
						      CTState *cts)
{
  ljt_ctype_arm_trace_abort(L, cts);
  ljt_lua_dostring(L,
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + tonumber(lj_m7_rec_metatv_int_ct(i)) end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 36) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_start_count() >= 1)\n"
    "assert(lj_m7_trace_parse_token_ctbusy_count() == 0)\n");
  assert((ctype_parse_token_acq(cts) & 1u) == 0);

  ljt_ctype_arm_trace_abort(L, cts);
  ljt_lua_dostring(L,
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1')\n"
    "local function run(n)\n"
    "  local caught = 0\n"
    "  for i = 1, n do\n"
    "    local v = lj_m7_rec_metatv_int_ct(i)\n"
    "    local ok, err = pcall(function() return v + 'x' end)\n"
    "    if not ok and tostring(err):match('arithmetic') then caught = caught + 1 end\n"
    "  end\n"
    "  return caught\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 8) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_start_count() >= 1)\n"
    "assert(lj_m7_trace_parse_token_ctbusy_count() == 0)\n");
  assert((ctype_parse_token_acq(cts) & 1u) == 0);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[typedef struct { int x; } lj_m7_rec_metatv_busy_t;]]\n"
    "lj_m7_rec_metatv_gc_count = 0\n"
    "local ct\n"
    "ct = ffi.metatype('lj_m7_rec_metatv_busy_t', {\n"
    "  __call = function(self, y) return self.x + y end,\n"
    "  __add = function(a, b) return ct(a.x + b.x) end,\n"
    "  __index = { extra = 5 },\n"
    "  __gc = function() lj_m7_rec_metatv_gc_count = "
    "lj_m7_rec_metatv_gc_count + 1 end,\n"
    "})\n"
    "lj_m7_rec_metatv_ct = ct\n"
    "lj_m7_rec_metatv_int_ct = ffi.typeof('int')\n"
    "lj_m7_rec_metatv_obj = ct(40)\n"
    "lj_m7_rec_metatv_rhs = ct(2)\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);

  assert_predefined_no_meta_trace_completes(L, cts);

  assert_busy_trace_releases(L, cts,
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local obj = lj_m7_rec_metatv_obj\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + obj(2) end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 336) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  assert_busy_trace_releases(L, cts,
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local lhs = lj_m7_rec_metatv_obj\n"
    "local rhs = lj_m7_rec_metatv_rhs\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + (lhs + rhs).x end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 336) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  assert_busy_trace_releases(L, cts,
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local obj = lj_m7_rec_metatv_obj\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + obj.extra end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 40) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  assert_busy_trace_releases(L, cts,
    "local ffi = require('ffi')\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local ct = lj_m7_rec_metatv_ct\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    local obj = ffi.new(ct, i)\n"
    "    sum = sum + obj.x\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 36) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local obj = lj_m7_rec_metatv_obj\n"
    "local rhs = lj_m7_rec_metatv_rhs\n"
    "local ct = lj_m7_rec_metatv_ct\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    sum = sum + obj(2) + (obj + rhs).x + obj.extra\n"
    "    sum = sum + ffi.new(ct, i).x\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 20 do assert(run(12) == 1146) end\n"
    "lj_m7_rec_metatv_obj = nil\n"
    "lj_m7_rec_metatv_rhs = nil\n"
    "collectgarbage('collect')\n"
    "assert(lj_m7_rec_metatv_gc_count >= 1)\n");

  lua_close(L);
  printf("t-ffi-recorder-metatv-busy OK: recorder ctype metamethod lookup aborts instead of waiting\n");
  return 0;
}
