/*
** Focused regression test for bit.* cdata ctype snapshots.
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

static void assert_carith_waits_without_lock(lua_State *L, CTState *cts,
					     TGState *tg, const char *chunk)
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

static void assert_predefined_carith_avoids_wait(lua_State *L, CTState *cts)
{
  uint32_t seq0 = ljt_ctype_parse_seq(cts);
  uint32_t release_seq = ljt_ctype_hold_parse_token(cts);

  ljt_lua_dostring(L,
    "local bit = require('bit')\n"
    "assert(tonumber(bit.band(lj_m7_carith_u64_a, lj_m7_carith_u64_b)) == 0x0f00)\n"
    "assert(bit.tohex(lj_m7_carith_u64_a, 4) == 'ff00')\n");

  ljt_ctype_release_parse_token(cts, release_seq);
  assert(ljt_ctype_parse_seq(cts) == seq0 + 2u);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;
  uint32_t seq0, seq1, seq2;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef enum { LJ_M7_CARITH_A = 0x33, "
    "LJ_M7_CARITH_B = 0x55 } lj_m7_carith_e;\n"
    "]]\n"
    "lj_m7_carith_enum_ct = ffi.typeof('lj_m7_carith_e')\n"
    "lj_m7_carith_enum_a = ffi.cast(lj_m7_carith_enum_ct, 'LJ_M7_CARITH_A')\n"
    "lj_m7_carith_enum_b = ffi.cast(lj_m7_carith_enum_ct, 'LJ_M7_CARITH_B')\n"
    "lj_m7_carith_u64 = ffi.typeof('uint64_t')\n"
    "lj_m7_carith_u64_a = lj_m7_carith_u64(0xff00)\n"
    "lj_m7_carith_u64_b = lj_m7_carith_u64(0x0ff0)\n"
    "jit.off()\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  assert_predefined_carith_avoids_wait(L, cts);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);

  assert_carith_waits_without_lock(L, cts, tg,
    "local bit = require('bit')\n"
    "assert(tonumber(bit.band(lj_m7_carith_enum_a, 0x0f)) == 0x03)\n"
    "assert(tonumber(bit.bxor(lj_m7_carith_enum_a, lj_m7_carith_enum_b)) == "
    "bit.bxor(0x33, 0x55))\n"
    "assert(bit.tohex(lj_m7_carith_enum_b, 2) == '55')\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 4u);

  ljt_lua_dostring(L,
    "local bit = require('bit')\n"
    "assert(tonumber(bit.band(lj_m7_carith_enum_a, 0x0f)) == 0x03)\n"
    "assert(bit.tohex(lj_m7_carith_enum_b, 2) == '55')\n");
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 == seq1);

  lua_close(L);
  printf("t-ffi-carith-check64-snapshot OK: bit.* waits on parser-owned ctypes\n");
  return 0;
}
