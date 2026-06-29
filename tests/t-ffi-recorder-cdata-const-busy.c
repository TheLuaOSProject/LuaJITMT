/*
** Focused guard for nonblocking recorder cdata constant ctype reads.
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
    "ffi.cdef[[\n"
    "typedef struct { int x; } lj_m7_rec_const_s;\n"
    "typedef int *lj_m7_rec_const_named_intp;\n"
    "]]\n"
    "lj_m7_rec_const_small = ffi.new('lj_m7_rec_const_s', { 37 })\n"
    "lj_m7_rec_const_words = ffi.new('int[4]', { 11, 13, 17, 19 })\n"
    "lj_m7_rec_const_pint = ffi.cast('int *', lj_m7_rec_const_words)\n"
    "lj_m7_rec_const_named_pint = "
    "ffi.cast('lj_m7_rec_const_named_intp', lj_m7_rec_const_words)\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);

  assert_busy_trace_records(L, cts,
    "local p = lj_m7_rec_const_pint\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + tonumber((p + 1)[0]) end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 104) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_stop_count() >= 1)\n"
    "assert(lj_m7_trace_parse_token_ctbusy_count() == 0)\n");

  assert_busy_trace_records(L, cts,
    "local p = lj_m7_rec_const_named_pint\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + tonumber((p + 1)[0]) end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 104) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_stop_count() >= 1)\n"
    "assert(lj_m7_trace_parse_token_ctbusy_count() == 0)\n");

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local v = lj_m7_rec_const_small\n"
    "local p = lj_m7_rec_const_named_pint\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    if v == v then sum = sum + tonumber((p + 1)[0]) end\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 520) end\n");

  lua_close(L);
  printf("t-ffi-recorder-cdata-const-busy OK: cdata constants snapshot ctype metadata during recording\n");
  return 0;
}
