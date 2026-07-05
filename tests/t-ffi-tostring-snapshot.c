/*
** Focused regression test for lock-free cdata __tostring type snapshots.
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

static void assert_tostring_waits_without_lock(lua_State *L, CTState *cts,
					       TGState *tg, const char *chunk)
{
  LJTCTypeParseReleaseCtx ctx;
  pthread_t thread;

  ljt_ctype_release_when_native_start(&ctx, &thread, cts, tg);
  ljt_lua_dostring(L, chunk);
  ljt_ctype_release_when_native_join(&ctx, thread);
}

static void assert_predefined_tostring_avoids_wait(lua_State *L, CTState *cts)
{
  uint32_t seq0 = ljt_ctype_parse_seq(cts);
  uint32_t release_seq = ljt_ctype_hold_parse_token(cts);

  ljt_lua_dostring(L,
    "assert(tostring(lj_m7_tostring_int_ct) == 'ctype<int>')\n"
    "assert(tostring(lj_m7_tostring_voidp_ct) == 'ctype<void *>')\n"
    "assert(tostring(lj_m7_tostring_int_obj):find("
    "'cdata<int>:', 1, true))\n");

  ljt_ctype_release_parse_token(cts, release_seq);
  assert(ljt_ctype_parse_seq(cts) == seq0 + 2u);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;
  uint32_t seq0, seq1;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef struct { int x; } lj_m7_tostring_snapshot_t;\n"
    "struct lj_m7_tostring_plain_s { int y; };\n"
    "typedef struct lj_m7_tostring_plain_s lj_m7_tostring_plain_t;\n"
    "typedef int64_t lj_m7_tostring_i64_t;\n"
    "typedef lj_m7_tostring_i64_t &lj_m7_tostring_i64_ref_t;\n"
    "typedef complex lj_m7_tostring_complex_t;\n"
    "typedef lj_m7_tostring_complex_t &lj_m7_tostring_complex_ref_t;\n"
    "enum lj_m7_tostring_snapshot_e { LJ_M7_TOSTRING_A = 7 };\n"
    "]]\n"
    "lj_m7_tostring_i64 = ffi.new('int64_t', -42)\n"
    "lj_m7_tostring_u64 = ffi.new('uint64_t', 42)\n"
    "lj_m7_tostring_int_ct = ffi.typeof('int')\n"
    "lj_m7_tostring_voidp_ct = ffi.typeof('void *')\n"
    "lj_m7_tostring_int_obj = ffi.new('int', -42)\n"
    "lj_m7_tostring_plain_ct = ffi.typeof('lj_m7_tostring_plain_t')\n"
    "lj_m7_tostring_plain_obj = lj_m7_tostring_plain_ct(11)\n"
    "lj_m7_tostring_i64_slot = ffi.new('lj_m7_tostring_i64_t[1]', -77)\n"
    "lj_m7_tostring_i64_ref = "
    "ffi.new('lj_m7_tostring_i64_ref_t', lj_m7_tostring_i64_slot)\n"
    "lj_m7_tostring_complex_slot = ffi.new('lj_m7_tostring_complex_t[1]', "
    "ffi.new('lj_m7_tostring_complex_t', 1, 2))\n"
    "lj_m7_tostring_complex_ref = ffi.new('lj_m7_tostring_complex_ref_t', "
    "lj_m7_tostring_complex_slot)\n"
    "lj_m7_tostring_enum = ffi.cast('enum lj_m7_tostring_snapshot_e',\n"
    "                               'LJ_M7_TOSTRING_A')\n"
    "local ct = ffi.metatype('lj_m7_tostring_snapshot_t', {\n"
    "  __tostring = function(self) return 'snap:' .. tonumber(self.x) end,\n"
    "})\n"
    "lj_m7_tostring_obj = ct(42)\n"
    "lj_m7_tostring_ptr = ffi.cast('lj_m7_tostring_snapshot_t *',\n"
    "                              lj_m7_tostring_obj)\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  ljt_lua_dostring(L,
    "assert(tostring(lj_m7_tostring_i64):find('-42', 1, true))\n"
    "assert(tostring(lj_m7_tostring_u64):find('42', 1, true))\n"
    "assert(tostring(lj_m7_tostring_i64_ref):find('-77', 1, true))\n"
    "assert(tostring(lj_m7_tostring_complex_ref) == '1+2i')\n"
    "assert(tostring(lj_m7_tostring_enum):find(': 7', 1, true))\n"
    "assert(tostring(lj_m7_tostring_plain_ct) == "
    "'ctype<struct lj_m7_tostring_plain_s>')\n"
    "assert(tostring(lj_m7_tostring_plain_obj):find("
    "'cdata<struct lj_m7_tostring_plain_s>:', 1, true))\n"
    "assert(tostring(lj_m7_tostring_obj) == 'snap:42')\n"
    "assert(tostring(lj_m7_tostring_ptr) == 'snap:42')\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  assert_predefined_tostring_avoids_wait(L, cts);
  assert_tostring_waits_without_lock(L, cts, tg,
    "assert(tostring(lj_m7_tostring_ptr) == 'snap:42')\n");
  assert_tostring_waits_without_lock(L, cts, tg,
    "assert(tostring(lj_m7_tostring_i64_ref):find('-77', 1, true))\n");
  assert_tostring_waits_without_lock(L, cts, tg,
    "assert(tostring(lj_m7_tostring_complex_ref) == '1+2i')\n");
  assert_tostring_waits_without_lock(L, cts, tg,
    "assert(tostring(lj_m7_tostring_plain_ct) == "
    "'ctype<struct lj_m7_tostring_plain_s>')\n");
  assert_tostring_waits_without_lock(L, cts, tg,
    "assert(tostring(lj_m7_tostring_plain_obj):find("
    "'cdata<struct lj_m7_tostring_plain_s>:', 1, true))\n");
  assert_tostring_waits_without_lock(L, cts, tg,
    "assert(tostring(lj_m7_tostring_enum):find(': 7', 1, true))\n");

  lua_close(L);
  printf("t-ffi-tostring-snapshot OK: cdata tostring waits on ctype snapshots\n");
  return 0;
}
