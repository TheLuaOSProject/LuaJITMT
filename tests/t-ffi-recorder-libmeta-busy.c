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

static void assert_trace_avoids_ctbusy(lua_State *L, CTState *cts,
				       const char *chunk)
{
  ljt_ctype_arm_trace_abort(L, cts);
  ljt_lua_dostring(L, chunk);
  assert((ctype_parse_token_acq(cts) & 1u) == 0);
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
    "typedef struct { lj_m7_recmeta_e e; } lj_m7_recmeta_enum_s;\n"
    "typedef struct { int a; double b; } lj_m7_recmeta_copy_s;\n"
    "typedef int lj_m7_recmeta_copy_a4[4];\n"
    "typedef struct { int a; double b; } lj_m7_recmeta_cnew_s;\n"
    "typedef int lj_m7_recmeta_cnew_a4[4];\n"
    "int abs(int);\n"
    "]]\n"
    "lj_m7_recmeta_i64 = ffi.new('int64_t', 1234567)\n"
    "lj_m7_recmeta_enum = ffi.new('lj_m7_recmeta_e', 'LJ_M7_RECMETA_A')\n"
    "lj_m7_recmeta_u64 = ffi.new('uint64_t', 0x12345678ULL)\n"
    "lj_m7_recmeta_enum_box = ffi.new('lj_m7_recmeta_enum_s', "
    "{ e = 'LJ_M7_RECMETA_A' })\n"
    "lj_m7_recmeta_enum_arr = ffi.new('lj_m7_recmeta_e[1]', "
    "{ 'LJ_M7_RECMETA_B' })\n"
    "lj_m7_recmeta_copy_src = ffi.new('lj_m7_recmeta_copy_s')\n"
    "lj_m7_recmeta_copy_dst = ffi.new('lj_m7_recmeta_copy_s[1]')\n"
    "lj_m7_recmeta_array_src = ffi.new('lj_m7_recmeta_copy_a4')\n"
    "lj_m7_recmeta_array_dst = ffi.new('lj_m7_recmeta_copy_a4[1]')\n"
    "lj_m7_recmeta_cnew_struct_t = ffi.typeof('lj_m7_recmeta_cnew_s')\n"
    "lj_m7_recmeta_cnew_array_t = ffi.typeof('lj_m7_recmeta_cnew_a4')\n"
    "lj_m7_recmeta_cnew_struct_src = ffi.new('lj_m7_recmeta_cnew_s')\n"
    "lj_m7_recmeta_cnew_array_src = ffi.new('lj_m7_recmeta_cnew_a4')\n"
    "lj_m7_recmeta_abs = ffi.C.abs\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);

  assert_trace_avoids_ctbusy(L, cts,
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
    "assert(lj_m7_trace_parse_token_start_count() >= 1)\n"
    "assert(lj_m7_trace_parse_token_ctbusy_count() == 0)\n");

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
    "local box = lj_m7_recmeta_enum_box\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + tonumber(box.e) end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == -56) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  assert_trace_avoids_ctbusy(L, cts,
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
    "assert(lj_m7_trace_parse_token_start_count() >= 1)\n"
    "assert(lj_m7_trace_parse_token_ctbusy_count() == 0)\n");

  assert_busy_trace_releases(L, cts,
    "local ffi = require('ffi')\n"
    "local buf = ffi.new('uint64_t[4]')\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  for i = 1, n do ffi.fill(buf, 32, i) end\n"
    "  return tonumber(buf[0])\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 0x0808080808080808) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  assert_busy_trace_releases(L, cts,
    "local src = lj_m7_recmeta_copy_src\n"
    "local dst = lj_m7_recmeta_copy_dst\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum, last = 0, 0\n"
    "  for i = 1, n do\n"
    "    src.a = i\n"
    "    src.b = i + 0.5\n"
    "    dst[0] = src\n"
    "    sum = sum + dst[0].a\n"
    "    last = dst[0].b\n"
    "  end\n"
    "  return sum, last\n"
    "end\n"
    "for i = 1, 3 do\n"
    "  local sum, last = run(8)\n"
    "  assert(sum == 36 and last == 8.5)\n"
    "end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  assert_busy_trace_releases(L, cts,
    "local src = lj_m7_recmeta_array_src\n"
    "local dst = lj_m7_recmeta_array_dst\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    local k = i % 4\n"
    "    src[k] = i\n"
    "    dst[0] = src\n"
    "    sum = sum + dst[0][k]\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 36) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  assert_busy_trace_releases(L, cts,
    "local ct = lj_m7_recmeta_cnew_struct_t\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum, last = 0, 0\n"
    "  for i = 1, n do\n"
    "    local obj = ct(i, i + 0.5)\n"
    "    sum = sum + obj.a\n"
    "    last = obj.b\n"
    "  end\n"
    "  return sum, last\n"
    "end\n"
    "for i = 1, 3 do\n"
    "  local sum, last = run(8)\n"
    "  assert(sum == 36 and last == 8.5)\n"
    "end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  assert_busy_trace_releases(L, cts,
    "local ct = lj_m7_recmeta_cnew_array_t\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    local arr = ct(i, i + 1, i + 2, i + 3)\n"
    "    sum = sum + arr[0] + arr[3]\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 96) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  assert_busy_trace_releases(L, cts,
    "local ct = lj_m7_recmeta_cnew_struct_t\n"
    "local src = lj_m7_recmeta_cnew_struct_src\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum, last = 0, 0\n"
    "  for i = 1, n do\n"
    "    src.a = i\n"
    "    src.b = i + 0.5\n"
    "    local obj = ct(src)\n"
    "    sum = sum + obj.a\n"
    "    last = obj.b\n"
    "  end\n"
    "  return sum, last\n"
    "end\n"
    "for i = 1, 3 do\n"
    "  local sum, last = run(8)\n"
    "  assert(sum == 36 and last == 8.5)\n"
    "end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  assert_busy_trace_releases(L, cts,
    "local ct = lj_m7_recmeta_cnew_array_t\n"
    "local src = lj_m7_recmeta_cnew_array_src\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    src[0] = i\n"
    "    src[3] = i + 3\n"
    "    local arr = ct(src)\n"
    "    sum = sum + arr[0] + arr[3]\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 96) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  assert_busy_trace_releases(L, cts,
    "local abs = lj_m7_recmeta_abs\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + abs(-i) end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 36) end\n"
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
    "local ebox = lj_m7_recmeta_enum_box\n"
    "local earr = lj_m7_recmeta_enum_arr\n"
    "local u64 = lj_m7_recmeta_u64\n"
    "local mask = ffi.new('uint64_t', 0xffULL)\n"
    "local buf = ffi.new('uint64_t[4]')\n"
    "local csrc = lj_m7_recmeta_copy_src\n"
    "local cdst = lj_m7_recmeta_copy_dst\n"
    "local asrc = lj_m7_recmeta_array_src\n"
    "local adst = lj_m7_recmeta_array_dst\n"
    "local struct_t = lj_m7_recmeta_cnew_struct_t\n"
    "local array_t = lj_m7_recmeta_cnew_array_t\n"
    "local struct_src = lj_m7_recmeta_cnew_struct_src\n"
    "local array_src = lj_m7_recmeta_cnew_array_src\n"
    "local abs = lj_m7_recmeta_abs\n"
    "local function run(n)\n"
    "  local sum, last = 0, 0\n"
    "  local x = ffi.new('uint64_t', 0)\n"
    "  for i = 1, n do\n"
    "    sum = sum + tonumber(i64) + tonumber(enum) + "
    "tonumber(ebox.e) + tonumber(earr[0])\n"
    "    x = bit.bxor(x, bit.band(u64, mask))\n"
    "    ffi.fill(buf, 32, i)\n"
    "    csrc.a = i\n"
    "    csrc.b = i + 0.5\n"
    "    cdst[0] = csrc\n"
    "    local k = i % 4\n"
    "    asrc[k] = i\n"
    "    adst[0] = asrc\n"
    "    local obj = struct_t(i, i + 0.5)\n"
    "    local arr = array_t(i, i + 1, i + 2, i + 3)\n"
    "    struct_src.a = i + 10\n"
    "    struct_src.b = i + 1.5\n"
    "    array_src[0] = i + 4\n"
    "    array_src[3] = i + 7\n"
    "    local clone = struct_t(struct_src)\n"
    "    local arrclone = array_t(array_src)\n"
    "    sum = sum + cdst[0].a + adst[0][k] + obj.a + arr[0] + arr[3] + "
    "clone.a + arrclone[0] + arrclone[3] + abs(-i)\n"
    "    last = obj.b\n"
    "  end\n"
    "  return sum, tostring(x), tonumber(buf[0]), last\n"
    "end\n"
    "for i = 1, 30 do\n"
    "  local sum, x, b, last = run(40)\n"
    "  assert(sum == 49390660 and x == '0ULL' and b == "
    "0x2828282828282828 and last == 40.5)\n"
    "end\n");

  lua_close(L);
  printf("t-ffi-recorder-libmeta-busy OK: recorder FFI metadata reads avoid or abort instead of waiting\n");
  return 0;
}
