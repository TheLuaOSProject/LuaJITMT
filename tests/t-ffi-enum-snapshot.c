/*
** Focused regression test for lock-free enum string constant snapshots.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_ctype.h"
#include "lj_cdata.h"
#include "lj_cconv.h"
#include "lj_tg.h"

#include "lib/ctype_parse_fixture_helpers.h"
#include "lib/lua_fixture_helpers.h"

static CTState *ljt_enum_miss_cts;
static CTypeID ljt_enum_miss_id;

typedef struct ParseReleaseCtx {
  CTState *cts;
  TGState *tg;
  uint32_t release_seq;
  int saw_native;
} ParseReleaseCtx;

static void sleep_ns(long ns)
{
  struct timespec ts;
  ts.tv_sec = ns / 1000000000l;
  ts.tv_nsec = ns % 1000000000l;
  while (nanosleep(&ts, &ts) != 0)
    ;
}

static void *release_parse_token(void *arg)
{
  ParseReleaseCtx *ctx = (ParseReleaseCtx *)arg;
  int spins;
  for (spins = 0; spins < 1000; spins++) {
    if (lj_tg_in_native_acq(ctx->tg)) {
      ctx->saw_native = 1;
      break;
    }
    sleep_ns(1000000);
  }
  ljt_ctype_release_parse_token(ctx->cts, ctx->release_seq);
  return NULL;
}

static void assert_enum_string_waits_without_lock(lua_State *L, CTState *cts,
						  TGState *tg,
						  const char *chunk)
{
  ParseReleaseCtx ctx;
  pthread_t thread;
  uint32_t seq0 = ljt_ctype_parse_seq(cts);

  ctx.cts = cts;
  ctx.tg = tg;
  ctx.release_seq = ljt_ctype_hold_parse_token(cts);
  ctx.saw_native = 0;
  assert(ctx.release_seq == seq0 + 2u);

  assert(pthread_create(&thread, NULL, release_parse_token, &ctx) == 0);
  ljt_lua_dostring(L, chunk);
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.saw_native);
  assert(ljt_ctype_parse_seq(cts) == ctx.release_seq);
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
  ParseReleaseCtx ctx;
  pthread_t thread;
  uint32_t seq0 = ljt_ctype_parse_seq(cts);
  int rc;

  ctx.cts = cts;
  ctx.tg = tg;
  ctx.release_seq = ljt_ctype_hold_parse_token(cts);
  ctx.saw_native = 0;
  assert(ctx.release_seq == seq0 + 2u);

  assert(pthread_create(&thread, NULL, release_parse_token, &ctx) == 0);
  lua_pushcfunction(L, enum_cconv_miss_lua);
  rc = lua_pcall(L, 0, 0, 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.saw_native);
  assert(rc != LUA_OK);
  assert(lua_tostring(L, -1) != NULL);
  lua_settop(L, 0);
  assert(ljt_ctype_parse_seq(cts) == ctx.release_seq);
}

static void assert_enum_cconv_waits_without_lock(lua_State *L, CTState *cts,
						 TGState *tg)
{
  ParseReleaseCtx ctx;
  pthread_t thread;
  CTypeID id, rid;
  CType *ct;
  CTSize out = 0;
  uint32_t seq0;

  lua_getglobal(L, "lj_m7_enum_snapshot_ct");
  assert(tviscdata(L->top - 1));
  id = *(CTypeID *)cdataptr(cdataV(L->top - 1));
  L->top--;
  rid = ctype_rawid(cts, id);
  ct = ctype_get(cts, rid);
  ljt_enum_miss_cts = cts;
  ljt_enum_miss_id = rid;

  lua_pushliteral(L, "LJ_M7_ENUM_B");
  seq0 = ljt_ctype_parse_seq(cts);

  ctx.cts = cts;
  ctx.tg = tg;
  ctx.release_seq = ljt_ctype_hold_parse_token(cts);
  ctx.saw_native = 0;
  assert(ctx.release_seq == seq0 + 2u);

  assert(pthread_create(&thread, NULL, release_parse_token, &ctx) == 0);
  lj_cconv_ct_tv_l(L, cts, ct, rid, (uint8_t *)&out, L->top - 1, CCF_CAST);
  L->top--;
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.saw_native);
  assert(out == 7);
  assert(ljt_ctype_parse_seq(cts) == ctx.release_seq);
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
