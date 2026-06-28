/*
** Focused guard for nonblocking recorder string ctype parsing.
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

static void assert_busy_trace_records(lua_State *L, CTState *cts,
				      const char *chunk)
{
  ljt_ctype_arm_trace_abort(L, cts);
  ljt_lua_dostring(L, chunk);
  assert(ljt_ctype_trace_start_count != 0);
  assert(ljt_ctype_trace_stop_count != 0);
  assert(ljt_ctype_trace_abort_count == 0);
  assert(ljt_ctype_trace_ctbusy_count == 0);
  assert(ljt_ctype_trace_cts == NULL);
  assert(ljt_ctype_trace_seq == 0);
  assert((ctype_parse_token_acq(cts) & 1u) == 0);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(ffi.sizeof('int') == 4)\n"
    "local a = ffi.new('int[1]')\n"
    "a[0] = 17\n"
    "assert(a[0] == 17)\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);

  assert_busy_trace_records(L, cts,
    "local ffi = require('ffi')\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    sum = sum + ffi.sizeof('int')\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 160) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_stop_count() >= 1)\n"
    "assert(lj_m7_trace_parse_token_ctbusy_count() == 0)\n");

  assert_busy_trace_releases(L, cts,
    "local ffi = require('ffi')\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    local a = ffi.new('int[1]')\n"
    "    a[0] = i\n"
    "    sum = sum + a[0]\n"
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
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    sum = sum + ffi.sizeof('int')\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 160) end\n"
    "local a = ffi.new('int[1]')\n"
    "a[0] = 23\n"
    "assert(a[0] == 23)\n");

  lua_close(L);
  printf("t-ffi-recorder-string-ctype-busy OK: recorder exact ctype strings bypass parser token and general strings abort instead of waiting\n");
  return 0;
}
