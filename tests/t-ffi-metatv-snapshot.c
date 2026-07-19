/*
** Focused regression test for lock-free ctype metamethod lookup snapshots.
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

static void assert_metatv_waits_without_lock(lua_State *L, CTState *cts,
					     TGState *tg, const char *chunk)
{
  LJTCTypeParseReleaseCtx ctx;
  pthread_t thread;

  ljt_ctype_release_when_native_start(&ctx, &thread, cts, tg);
  ljt_lua_dostring(L, chunk);
  ljt_ctype_release_when_native_join(&ctx, thread);
}

static void assert_predefined_metatv_avoids_wait(lua_State *L, CTState *cts)
{
  uint32_t release_seq = ljt_ctype_hold_parse_token(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local v = lj_m7_metatv_int_ct(42)\n"
    "assert(ffi.istype(lj_m7_metatv_int_ct, v))\n"
    "local ok, err = pcall(function() return v.no_such_field end)\n"
    "assert(not ok and tostring(err):match('no_such_field'))\n"
    "ok, err = pcall(function() return v + 'x' end)\n"
    "assert(not ok and tostring(err):match('arithmetic'))\n"
    "assert(tostring(v + 1) == '43LL')\n");
  assert((ctype_parse_token_acq(cts) & 1u) != 0);
  ljt_ctype_release_parse_token(cts, release_seq);
}

static void assert_caught_metatv_errors_balance_roots(lua_State *L,
					       TGState *tg)
{
  uint32_t roots0 = lj_tg_root_anchor_top_acq(tg);
  uint32_t i;

  for (i = 0; i < 64u; i++) {
    ljt_lua_dostring(L, "lj_m7_metatv_error_round()\n");
    /* Each error is caught by an inner Lua pcall. The anchor top must already
    ** be repaired there, not by this fixture's outer protected boundary. */
    assert(lj_tg_root_anchor_top_acq(tg) == roots0);
  }
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;
  uint32_t seq0, seq1;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[typedef struct { int x; } lj_m7_metatv_snapshot_t;]]\n"
    "lj_m7_metatv_gc_count = 0\n"
    "local ct\n"
    "ct = ffi.metatype('lj_m7_metatv_snapshot_t', {\n"
    "  __call = function(self, y) return self.x + y end,\n"
    "  __add = function(a, b) return ct(a.x + b.x) end,\n"
    "  __pairs = function(self)\n"
    "    local done = false\n"
    "    return function()\n"
    "      if done then return nil end\n"
    "      done = true\n"
    "      return 'x', tonumber(self.x)\n"
    "    end, nil, nil\n"
    "  end,\n"
    "  __gc = function() lj_m7_metatv_gc_count = lj_m7_metatv_gc_count + 1 end,\n"
    "})\n"
    "lj_m7_metatv_ct = ct\n"
    "lj_m7_metatv_int_ct = ffi.typeof('int')\n"
    "lj_m7_metatv_obj = ct(40)\n"
    "lj_m7_metatv_rhs = ct(2)\n");

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef struct { int x; } lj_m7_metatv_index_loop_t;\n"
    "typedef struct { int x; } lj_m7_metatv_newindex_loop_t;\n"
    "typedef struct { int x; } lj_m7_metatv_index_call_t;\n"
    "typedef struct { int x; } lj_m7_metatv_newindex_call_t;\n"
    "]]\n"
    "local ri, rj = {}, {}\n"
    "setmetatable(ri, { __index = rj })\n"
    "setmetatable(rj, { __index = ri })\n"
    "local wi, wj = {}, {}\n"
    "setmetatable(wi, { __newindex = wj })\n"
    "setmetatable(wj, { __newindex = wi })\n"
    "local rcall = setmetatable({}, { __index = function()\n"
    "  collectgarbage('collect'); error('rooted ffi index call')\n"
    "end })\n"
    "local wcall = setmetatable({}, { __newindex = function()\n"
    "  collectgarbage('collect'); error('rooted ffi newindex call')\n"
    "end })\n"
    "local rloopct = ffi.metatype('lj_m7_metatv_index_loop_t',\n"
    "  { __index = ri })\n"
    "local wloopct = ffi.metatype('lj_m7_metatv_newindex_loop_t',\n"
    "  { __newindex = wi })\n"
    "local rcallct = ffi.metatype('lj_m7_metatv_index_call_t',\n"
    "  { __index = rcall })\n"
    "local wcallct = ffi.metatype('lj_m7_metatv_newindex_call_t',\n"
    "  { __newindex = wcall })\n"
    "local rloop, wloop = rloopct(), wloopct()\n"
    "local rcallobj, wcallobj = rcallct(), wcallct()\n"
    "lj_m7_metatv_error_round = function()\n"
    "  local ok, err = pcall(function() return rloop.absent end)\n"
    "  assert(not ok and tostring(err):find('loop in gettable', 1, true))\n"
    "  ok, err = pcall(function() wloop.absent = true end)\n"
    "  assert(not ok and tostring(err):find('loop in settable', 1, true))\n"
    "  ok, err = pcall(function() return rcallobj.absent end)\n"
    "  assert(not ok and tostring(err):find('rooted ffi index call', 1, true))\n"
    "  ok, err = pcall(function() wcallobj.absent = true end)\n"
    "  assert(not ok and tostring(err):find('rooted ffi newindex call', 1, true))\n"
    "end\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  ljt_lua_dostring(L,
    "assert(lj_m7_metatv_obj(2) == 42)\n"
    "assert((lj_m7_metatv_obj + lj_m7_metatv_rhs).x == 42)\n"
    "local seen = 0\n"
    "for k, v in pairs(lj_m7_metatv_obj) do\n"
    "  assert(k == 'x' and v == 40)\n"
    "  seen = seen + 1\n"
    "end\n"
    "assert(seen == 1)\n"
    "local tmp = lj_m7_metatv_ct(77)\n"
    "assert(tmp.x == 77)\n"
    "tmp = nil\n"
    "collectgarbage('collect')\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  assert_caught_metatv_errors_balance_roots(L, tg);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  assert_predefined_metatv_avoids_wait(L, cts);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);

  assert_metatv_waits_without_lock(L, cts, tg,
    "assert(lj_m7_metatv_obj(2) == 42)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 4u);
  assert_metatv_waits_without_lock(L, cts, tg,
    "assert((lj_m7_metatv_obj + lj_m7_metatv_rhs).x == 42)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 6u);
  assert_metatv_waits_without_lock(L, cts, tg,
    "local seen = 0\n"
    "for k, v in pairs(lj_m7_metatv_obj) do\n"
    "  assert(k == 'x' and v == 40)\n"
    "  seen = seen + 1\n"
    "end\n"
    "assert(seen == 1)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 8u);

  ljt_lua_dostring(L,
    "lj_m7_metatv_obj = nil\n"
    "lj_m7_metatv_rhs = nil\n"
    "collectgarbage('collect')\n"
    "assert(lj_m7_metatv_gc_count >= 1)\n");

  lua_close(L);
  printf("t-ffi-metatv-snapshot OK: ctype metamethods wait on snapshots\n");
  return 0;
}
