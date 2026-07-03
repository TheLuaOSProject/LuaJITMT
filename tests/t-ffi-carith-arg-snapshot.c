/*
** Focused regression test for lock-free cdata arithmetic argument snapshots.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_tg.h"

#include "lib/ctype_parse_fixture_helpers.h"
#include "lib/lua_fixture_helpers.h"

typedef struct ParseReleaseCtx {
  CTState *cts;
  TGState *tg;
  uint32_t release_seq;
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
    if (lj_tg_in_native_acq(ctx->tg))
      break;
    sleep_ns(100000);
  }
  ljt_ctype_release_parse_token(ctx->cts, ctx->release_seq);
  return NULL;
}

static void assert_carith_arg_runs_with_parser_release(lua_State *L,
						       CTState *cts,
						       TGState *tg,
						       const char *chunk)
{
  ParseReleaseCtx ctx;
  pthread_t thread;
  uint32_t seq0;

  assert(luaL_loadstring(L, chunk) == 0);
  seq0 = ljt_ctype_parse_seq(cts);

  ctx.cts = cts;
  ctx.tg = tg;
  ctx.release_seq = ljt_ctype_hold_parse_token(cts);
  assert(ctx.release_seq == seq0 + 2u);

  assert(pthread_create(&thread, NULL, release_parse_token, &ctx) == 0);
  assert(lua_pcall(L, 0, 0, 0) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(ljt_ctype_parse_seq(cts) == ctx.release_seq);
}

static void assert_predefined_carith_arg_avoids_wait(lua_State *L,
						     CTState *cts)
{
  uint32_t seq0 = ljt_ctype_parse_seq(cts);
  uint32_t release_seq = ljt_ctype_hold_parse_token(cts);

  ljt_lua_dostring(L,
    "local i64_5 = lj_m7_carith_arg_i64_5\n"
    "local i64_7 = lj_m7_carith_arg_i64_7\n"
    "assert(i64_5 + i64_7 == lj_m7_carith_arg_i64_12)\n"
    "assert(i64_7 - i64_5 == lj_m7_carith_arg_i64_2)\n"
    "assert(i64_7 > i64_5)\n"
    "local p = lj_m7_carith_arg_predef_ptr\n"
    "local q = p + 3\n"
    "assert(q[0] == 44)\n"
    "assert(tonumber(q - p) == 3)\n"
    "assert(p < q)\n");

  ljt_ctype_release_parse_token(cts, release_seq);
  assert(ljt_ctype_parse_seq(cts) == seq0 + 2u);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;
  uint32_t seq0, seq1, seq2, seq3, seq4;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef enum { LJ_M7_CARITH_ARG_A = 21, "
    "LJ_M7_CARITH_ARG_B = 9 } lj_m7_carith_arg_e;\n"
    "typedef int lj_m7_carith_arg_i;\n"
    "]]\n"
    "lj_m7_carith_arg_enum_ct = ffi.typeof('lj_m7_carith_arg_e')\n"
    "lj_m7_carith_arg_enum_a = ffi.cast(lj_m7_carith_arg_enum_ct, "
    "'LJ_M7_CARITH_ARG_A')\n"
    "lj_m7_carith_arg_enum_b = ffi.cast(lj_m7_carith_arg_enum_ct, "
    "'LJ_M7_CARITH_ARG_B')\n"
    "lj_m7_carith_arg_arr = ffi.new('lj_m7_carith_arg_i[4]', "
    "{ 1, 2, 3, 4 })\n"
    "lj_m7_carith_arg_ptr = ffi.cast('lj_m7_carith_arg_i *', "
    "lj_m7_carith_arg_arr)\n"
    "lj_m7_carith_arg_predef_arr = ffi.new('int[4]', { 11, 22, 33, 44 })\n"
    "lj_m7_carith_arg_predef_ptr = ffi.cast('int *', "
    "lj_m7_carith_arg_predef_arr)\n"
    "lj_m7_carith_arg_i64 = ffi.typeof('int64_t')\n"
    "lj_m7_carith_arg_i64_2 = lj_m7_carith_arg_i64(2)\n"
    "lj_m7_carith_arg_i64_5 = lj_m7_carith_arg_i64(5)\n"
    "lj_m7_carith_arg_i64_7 = lj_m7_carith_arg_i64(7)\n"
    "lj_m7_carith_arg_i64_12 = lj_m7_carith_arg_i64(12)\n"
    "jit.off()\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  assert_predefined_carith_arg_avoids_wait(L, cts);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);

  assert_carith_arg_runs_with_parser_release(L, cts, tg,
    "local a = lj_m7_carith_arg_enum_a\n"
    "local b = lj_m7_carith_arg_enum_b\n"
    "assert(tonumber(a + b) == 30)\n"
    "assert(tonumber(a - b) == 12)\n"
    "assert(a > b)\n");
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 == seq1 + 2u);

  assert_carith_arg_runs_with_parser_release(L, cts, tg,
    "local a = lj_m7_carith_arg_enum_a\n"
    "local b = lj_m7_carith_arg_enum_b\n"
    "assert('LJ_M7_CARITH_ARG_A' == a)\n"
    "assert(b == 'LJ_M7_CARITH_ARG_B')\n"
    "assert((a == 'NO_SUCH_CARITH_ARG') == false)\n");
  seq3 = ljt_ctype_parse_seq(cts);
  assert(seq3 == seq2 + 2u);

  assert_carith_arg_runs_with_parser_release(L, cts, tg,
    "local p = lj_m7_carith_arg_ptr\n"
    "assert(tonumber((p + 2) - p) == 2)\n"
    "assert((p + 2)[0] == 3)\n"
    "assert(p < p + 3)\n");
  seq4 = ljt_ctype_parse_seq(cts);
  assert(seq4 == seq3 + 2u);

  ljt_lua_dostring(L,
    "local a = lj_m7_carith_arg_enum_a\n"
    "local b = lj_m7_carith_arg_enum_b\n"
    "local p = lj_m7_carith_arg_ptr\n"
    "assert(tonumber(a + b) == 30)\n"
    "assert('LJ_M7_CARITH_ARG_A' == a)\n"
    "assert(tonumber((p + 1) - p) == 1)\n");
  assert(ljt_ctype_parse_seq(cts) == seq4);

  lua_close(L);
  printf("t-ffi-carith-arg-snapshot OK: cdata arithmetic uses ctype snapshots\n");
  return 0;
}
