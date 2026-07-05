/*
** Focused behavior test for lock-free cdata string-key field snapshots.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_ctype.h"
#include "lj_tg.h"

#include "lib/ctype_parse_fixture_helpers.h"
#include "lib/lua_fixture_helpers.h"

static void assert_field_waits_without_lock(lua_State *L, CTState *cts,
					    TGState *tg, const char *chunk)
{
  LJTCTypeParseReleaseCtx ctx;
  pthread_t thread;

  ljt_ctype_release_when_native_start(&ctx, &thread, cts, tg);
  ljt_lua_dostring(L, chunk);
  ljt_ctype_release_when_native_join(&ctx, thread);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;
  uint32_t seq0, seq1, seq2, seq3;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef struct {\n"
    "  struct { int ax; };\n"
    "  int y;\n"
    "} lj_m7_field_snapshot_t;\n"
    "typedef struct { int x; } lj_m7_field_meta_t;\n"
    "typedef struct {\n"
    "  static const int K = 42;\n"
    "  enum { E = 39 };\n"
    "  int x;\n"
    "} lj_m7_field_const_t;\n"
    "]]\n"
    "lj_m7_field_snapshot_obj = ffi.new('lj_m7_field_snapshot_t')\n"
    "lj_m7_field_snapshot_ptr = ffi.cast('lj_m7_field_snapshot_t *',\n"
    "                                     lj_m7_field_snapshot_obj)\n"
    "lj_m7_field_meta_ct = ffi.metatype('lj_m7_field_meta_t', {\n"
    "  __index = function(self, key)\n"
    "    if key == 'missing' then return 731 end\n"
    "  end,\n"
    "  __newindex = function(self, key, value)\n"
    "    if key == 'missing' then lj_m7_meta_newindex_value = value; return end\n"
    "    error('unexpected key: '..tostring(key))\n"
    "  end\n"
    "})\n"
    "lj_m7_field_meta_obj = lj_m7_field_meta_ct()\n"
    "lj_m7_field_const_ct = ffi.typeof('lj_m7_field_const_t')\n"
    "lj_m7_field_const_ptr_ct = ffi.typeof('lj_m7_field_const_t *')\n"
    "lj_m7_field_snapshot_obj.ax = 11\n"
    "lj_m7_field_snapshot_obj.y = 13\n"
    "jit.off()\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  ljt_lua_dostring(L,
    "local obj = lj_m7_field_snapshot_obj\n"
    "local ptr = lj_m7_field_snapshot_ptr\n"
    "for i = 1, 100 do\n"
    "  assert(obj.ax == 10 + i)\n"
    "  assert(ptr.y == 12 + i)\n"
    "  obj.ax = obj.ax + 1\n"
    "  ptr.y = ptr.y + 1\n"
    "end\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  assert_field_waits_without_lock(L, cts, tg,
    "assert(lj_m7_field_snapshot_obj.ax == 111)\n");

  assert_field_waits_without_lock(L, cts, tg,
    "lj_m7_field_snapshot_obj.ax = 31\n"
    "assert(lj_m7_field_snapshot_obj.ax == 31)\n");

  assert_field_waits_without_lock(L, cts, tg,
    "assert(lj_m7_field_snapshot_ptr.y == 113)\n");

  assert_field_waits_without_lock(L, cts, tg,
    "lj_m7_field_snapshot_ptr.y = 41\n"
    "assert(lj_m7_field_snapshot_ptr.y == 41)\n");

  assert_field_waits_without_lock(L, cts, tg,
    "assert(lj_m7_field_snapshot_ptr.ax == 31)\n");

  assert_field_waits_without_lock(L, cts, tg,
    "local ok, err = pcall(function()\n"
    "  return lj_m7_field_snapshot_obj.no_such_field\n"
    "end)\n"
    "assert(not ok and tostring(err):match('no_such_field'))\n");

  assert_field_waits_without_lock(L, cts, tg,
    "assert(lj_m7_field_meta_obj.missing == 731)\n");

  assert_field_waits_without_lock(L, cts, tg,
    "lj_m7_field_meta_obj.missing = 864\n"
    "assert(lj_m7_meta_newindex_value == 864)\n");

  assert_field_waits_without_lock(L, cts, tg,
    "assert(lj_m7_field_const_ct.K == 42)\n"
    "assert(lj_m7_field_const_ct.E == 39)\n");

  assert_field_waits_without_lock(L, cts, tg,
    "assert(lj_m7_field_const_ptr_ct.K == 42)\n"
    "assert(lj_m7_field_const_ptr_ct.E == 39)\n");

  assert_field_waits_without_lock(L, cts, tg,
    "local ok, err = pcall(function() return lj_m7_field_const_ct.x end)\n"
    "assert(not ok and tostring(err):match('x'))\n");

  assert_field_waits_without_lock(L, cts, tg,
    "local ok, err = pcall(function() lj_m7_field_const_ct.K = 1 end)\n"
    "assert(not ok and tostring(err):match('constant'))\n");

  seq1 = ljt_ctype_parse_seq(cts);

  {
    ljt_ctype_arm_trace_abort(L, cts);
    ljt_lua_dostring(L,
      "local obj = lj_m7_field_snapshot_obj\n"
      "local ptr = lj_m7_field_snapshot_ptr\n"
      "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
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
      "for i = 1, 3 do assert(run(8) == 76) end\n"
      "jit.attach(lj_m7_trace_parse_token)\n"
      "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");
    ljt_ctype_assert_trace_abort_released(cts);
  }
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 == seq1 + 2u);

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
  seq3 = ljt_ctype_parse_seq(cts);
  assert(seq3 == seq2);

  lua_close(L);
  printf("t-ffi-field-snapshot OK: stable cdata fields avoid cparser sequence\n");
  return 0;
}
