/*
** Focused regression test for lock-free enum string constant snapshots.
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
#include "lj_cdata.h"
#include "lj_cconv.h"
#include "lj_str.h"
#include "lj_tg.h"

#include "lib/ctype_parse_fixture_helpers.h"
#include "lib/lua_fixture_helpers.h"

static CTState *ljt_enum_miss_cts;
static CTypeID ljt_enum_miss_id;

static void assert_enum_string_waits_without_lock(lua_State *L, CTState *cts,
						  TGState *tg,
						  const char *chunk)
{
  LJTCTypeParseReleaseCtx ctx;
  pthread_t thread;

  ljt_ctype_release_when_native_start(&ctx, &thread, cts, tg);
  ljt_lua_dostring(L, chunk);
  ljt_ctype_release_when_native_join(&ctx, thread);
}

static int enum_cconv_miss_lua(lua_State *L)
{
  CTState *cts = ljt_enum_miss_cts;
  CTypeID id = ljt_enum_miss_id;
  CType *ct = ctype_get(cts, id);
  CTSize out = 0;
  lua_pushliteral(L, "NO_SUCH_ENUM_CONSTANT");
  lj_cconv_ct_tv_l(L, cts, ct, id, (uint8_t *)&out, L->top - 1, CCF_CAST);
  return luaL_error(L, "unexpected enum miss conversion success");
}

static void assert_enum_cconv_miss_waits_without_lock(lua_State *L,
						      CTState *cts,
						      TGState *tg)
{
  LJTCTypeParseReleaseCtx ctx;
  pthread_t thread;
  int rc;

  ljt_ctype_release_when_native_start(&ctx, &thread, cts, tg);
  lua_pushcfunction(L, enum_cconv_miss_lua);
  rc = lua_pcall(L, 0, 0, 0);
  ljt_ctype_release_when_native_join(&ctx, thread);
  assert(rc != LUA_OK);
  assert(lua_tostring(L, -1) != NULL);
  lua_settop(L, 0);
}

static void assert_enum_cconv_waits_without_lock(lua_State *L, CTState *cts,
						 TGState *tg)
{
  LJTCTypeParseReleaseCtx ctx;
  pthread_t thread;
  CTypeID id, rid;
  CType *ct;
  CTSize out = 0;

  lua_getglobal(L, "lj_m7_enum_snapshot_ct");
  assert(tviscdata(L->top - 1));
  id = *(CTypeID *)cdataptr(cdataV(L->top - 1));
  L->top--;
  rid = ctype_rawid(cts, id);
  ct = ctype_get(cts, rid);
  ljt_enum_miss_cts = cts;
  ljt_enum_miss_id = rid;

  lua_pushliteral(L, "LJ_M7_ENUM_B");

  ljt_ctype_release_when_native_start(&ctx, &thread, cts, tg);
  lj_cconv_ct_tv_l(L, cts, ct, rid, (uint8_t *)&out, L->top - 1, CCF_CAST);
  L->top--;
  ljt_ctype_release_when_native_join(&ctx, thread);
  assert(out == 7);
}

static void assert_enum_snapshot_direct(lua_State *L, CTState *cts)
{
  CTypeID id, rid, cid = 0;
  CType *ct;
  CTSize val = 0;
  GCstr *hit = lj_str_newz(L, "LJ_M7_ENUM_B");
  GCstr *miss = lj_str_newz(L, "NO_SUCH_ENUM_CONSTANT");
  uint32_t release_seq;

  lua_getglobal(L, "lj_m7_enum_snapshot_ct");
  assert(tviscdata(L->top - 1));
  id = *(CTypeID *)cdataptr(cdataV(L->top - 1));
  L->top--;
  rid = ctype_rawid(cts, id);
  ct = ctype_get(cts, rid);

  assert(lj_ctype_enumconst_snapshot(cts, ct, hit, &val, &cid) == 1);
  assert(val == 7);
  assert(cid != 0);
  val = 123;
  cid = 456;
  assert(lj_ctype_enumconst_snapshot(cts, ct, miss, &val, &cid) == 0);

  release_seq = ljt_ctype_hold_parse_token(cts);
  assert(lj_ctype_enumconst_snapshot(cts, ct, hit, &val, &cid) == -1);
  ljt_ctype_release_parse_token(cts, release_seq);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;
  uint32_t seq0, seq1, seq2, seq3;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('enum lj_m7_enum_snapshot_t { LJ_M7_ENUM_A = 5, LJ_M7_ENUM_B = 7 };')\n"
    "lj_m7_ffi = ffi\n"
    "lj_m7_enum_snapshot_ct = ffi.typeof('enum lj_m7_enum_snapshot_t')\n"
    "lj_m7_enum_snapshot_a = ffi.cast(lj_m7_enum_snapshot_ct, 'LJ_M7_ENUM_A')\n"
    "jit.off()\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  seq0 = ljt_ctype_parse_seq(cts);
  assert_enum_snapshot_direct(L, cts);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);
  seq0 = seq1;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ct = lj_m7_enum_snapshot_ct\n"
    "for i = 1, 100 do\n"
    "  assert(tonumber(ffi.cast(ct, 'LJ_M7_ENUM_B')) == 7)\n"
    "  assert(tonumber(ffi.cast(ct, 5)) == 5)\n"
    "end\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  assert_enum_cconv_waits_without_lock(L, cts, tg);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);

  assert_enum_cconv_miss_waits_without_lock(L, cts, tg);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 4u);

  assert_enum_string_waits_without_lock(L, cts, tg,
    "local a = lj_m7_enum_snapshot_a\n"
    "assert(a == 'LJ_M7_ENUM_A')\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 6u);

  assert_enum_string_waits_without_lock(L, cts, tg,
    "local a = lj_m7_enum_snapshot_a\n"
    "assert((a == 'NO_SUCH_ENUM_CONSTANT') == false)\n"
    "local ok = pcall(function() return a + 'NO_SUCH_ENUM_CONSTANT' end)\n"
    "assert(ok == false)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 8u);

  {
    ljt_ctype_arm_trace_abort(L, cts);
    ljt_lua_dostring(L,
      "local ffi = lj_m7_ffi\n"
      "local ct = lj_m7_enum_snapshot_ct\n"
      "local a = lj_m7_enum_snapshot_a\n"
      "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
      "jit.flush()\n"
      "jit.on()\n"
      "jit.opt.start('hotloop=1', 'hotexit=1')\n"
      "local function run(n)\n"
      "  local sum = 0\n"
      "  for i = 1, n do\n"
      "    sum = sum + tonumber(ffi.cast(ct, 'LJ_M7_ENUM_A'))\n"
      "    if a == 'LJ_M7_ENUM_A' then sum = sum + 1 end\n"
      "  end\n"
      "  return sum\n"
      "end\n"
      "for i = 1, 3 do assert(run(8) == 48) end\n"
      "jit.attach(lj_m7_trace_parse_token)\n"
      "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");
    ljt_ctype_assert_trace_abort_released(cts);
  }
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 == seq1 + 2u);

  ljt_lua_dostring(L,
    "local ffi = lj_m7_ffi\n"
    "local ct = lj_m7_enum_snapshot_ct\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local a = lj_m7_enum_snapshot_a\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    sum = sum + tonumber(ffi.cast(ct, 'LJ_M7_ENUM_A'))\n"
    "    if a == 'LJ_M7_ENUM_A' then sum = sum + 1 end\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 240) end\n");
  seq3 = ljt_ctype_parse_seq(cts);
  assert(seq3 == seq2);

  lua_close(L);
  printf("t-ffi-enum-snapshot OK: stable enum string constants avoid cparser sequence\n");
  return 0;
}
