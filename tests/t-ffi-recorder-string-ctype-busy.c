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

static void assert_trace_records_without_parse(lua_State *L, CTState *cts,
					       const char *chunk)
{
  uint32_t seq = ctype_parse_token_acq(cts);
  assert((seq & 1u) == 0);
  ljt_lua_dostring(L, chunk);
  assert(ctype_parse_token_acq(cts) == seq);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef int lj_m7_rec_named_int_t;\n"
    "typedef struct { int x; } lj_m7_rec_named_struct_t;\n"
    "struct lj_m7_rec_tagged { int x; };\n"
    "union lj_m7_rec_union { int x; double y; };\n"
    "enum lj_m7_rec_enum { LJ_M7_REC_ENUM_A = 7 };\n"
    "]]\n"
    "assert(ffi.sizeof('int') == 4)\n"
    "assert(ffi.sizeof('int *') == 8)\n"
    "assert(ffi.alignof('int * const') == 8)\n"
    "assert(ffi.sizeof('int *[2]') == 16)\n"
    "assert(ffi.sizeof('const char * const [3]') == 24)\n"
    "assert(ffi.sizeof('lj_m7_rec_named_int_t') == 4)\n"
    "assert(ffi.sizeof('const lj_m7_rec_named_struct_t *') == 8)\n"
    "assert(ffi.alignof('struct lj_m7_rec_tagged * const') == 8)\n"
    "assert(ffi.sizeof('union lj_m7_rec_union') == 8)\n"
    "assert(ffi.sizeof('enum lj_m7_rec_enum[2]') == 8)\n"
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

  assert_busy_trace_records(L, cts,
    "local ffi = require('ffi')\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    sum = sum + ffi.sizeof('int *') + ffi.alignof('int * const')\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 640) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_stop_count() >= 1)\n"
    "assert(lj_m7_trace_parse_token_ctbusy_count() == 0)\n");

  assert_busy_trace_records(L, cts,
    "local ffi = require('ffi')\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    sum = sum + ffi.sizeof('int[1]')\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 160) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_stop_count() >= 1)\n"
    "assert(lj_m7_trace_parse_token_ctbusy_count() == 0)\n");

  assert_busy_trace_records(L, cts,
    "local ffi = require('ffi')\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    sum = sum + ffi.sizeof('const char * const [3]')\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 960) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_stop_count() >= 1)\n"
    "assert(lj_m7_trace_parse_token_ctbusy_count() == 0)\n");

  assert_busy_trace_records(L, cts,
    "local ffi = require('ffi')\n"
    "local deep = 'int' .. string.rep('[1]', 9)\n"
    "assert(ffi.sizeof(deep) == 4)\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    sum = sum + ffi.sizeof(deep)\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 160) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_stop_count() >= 1)\n"
    "assert(lj_m7_trace_parse_token_ctbusy_count() == 0)\n");

  assert_trace_records_without_parse(L, cts,
    "local ffi = require('ffi')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local named = 'lj_m7_rec_named_int_t'\n"
    "local namedp = 'const lj_m7_rec_named_struct_t *'\n"
    "local tagp = 'struct lj_m7_rec_tagged * const'\n"
    "local enumarr = 'enum lj_m7_rec_enum[2]'\n"
    "local uni = 'union lj_m7_rec_union'\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    local v = ffi.new(named, i)\n"
    "    sum = sum + tonumber(v) + ffi.sizeof(namedp)\n"
    "    sum = sum + ffi.alignof(tagp) + ffi.sizeof(enumarr)\n"
    "    sum = sum + ffi.sizeof(uni)\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 2100) end\n");

  assert_busy_trace_releases(L, cts,
    "local ffi = require('ffi')\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + ffi.sizeof('lj_m7_rec_named_int_t') end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 32) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_ctbusy_count() >= 1)\n");

  assert_busy_trace_releases(L, cts,
    "local ffi = require('ffi')\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    sum = sum + ffi.sizeof('struct { int x; }')\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 32) end\n"
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
  printf("t-ffi-recorder-string-ctype-busy OK: recorder direct ctype strings bypass parser token when stable and abort instead of waiting when busy\n");
  return 0;
}
