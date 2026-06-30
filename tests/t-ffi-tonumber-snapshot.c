/*
** Focused guard for lock-free tonumber(cdata) ctype snapshots.
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

static void assert_tonumber_waits_without_lock(lua_State *L, CTState *cts,
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

static void assert_predefined_tonumber_avoids_wait(lua_State *L, CTState *cts)
{
  uint32_t seq0;
  uint32_t release_seq;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local buffer = require('string.buffer')\n"
    "lj_m7_tonumber_predef_bytes = ffi.new('uint8_t[64]')\n"
    "lj_m7_tonumber_predef_pvoid = "
    "ffi.cast('void *', lj_m7_tonumber_predef_bytes)\n"
    "lj_m7_tonumber_predef_pcvoid = "
    "ffi.cast('const void *', lj_m7_tonumber_predef_bytes)\n"
    "lj_m7_tonumber_predef_pcchar = "
    "ffi.cast('const char *', lj_m7_tonumber_predef_bytes)\n"
    "lj_m7_tonumber_predef_buffer = buffer.new()\n");

  seq0 = ljt_ctype_parse_seq(cts);
  release_seq = ljt_ctype_hold_parse_token(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(tonumber(ffi.new('int', 33)) == 33)\n"
    "assert(tonumber(ffi.cast('int', 23.75)) == 23)\n"
    "local pvoid = lj_m7_tonumber_predef_pvoid\n"
    "local pcvoid = lj_m7_tonumber_predef_pcvoid\n"
    "local pcchar = lj_m7_tonumber_predef_pcchar\n"
    "ffi.copy(pvoid, 'abcd', 5)\n"
    "assert(ffi.string(pcvoid, 4) == 'abcd')\n"
    "assert(ffi.string(pcchar) == 'abcd')\n"
    "ffi.fill(pvoid, 4, 65)\n"
    "assert(ffi.string(pcvoid, 4) == 'AAAA')\n"
    "local b = lj_m7_tonumber_predef_buffer\n"
    "b:reset()\n"
    "b:set(pvoid, 4)\n"
    "assert(tostring(b) == 'AAAA')\n"
    "ffi.copy(pvoid, 'WXYZ', 5)\n"
    "b:reset()\n"
    "b:putcdata(pvoid, 4)\n"
    "assert(tostring(b) == 'WXYZ')\n");

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
    "typedef enum { LJ_M7_TONUMBER_NEG = -12, "
    "LJ_M7_TONUMBER_POS = 17 } lj_m7_tonumber_e;\n"
    "typedef const int64_t lj_m7_serialize_i64_t;\n"
    "]]\n"
    "lj_m7_tonumber_enum_ct = ffi.typeof('lj_m7_tonumber_e')\n"
    "lj_m7_tonumber_enum_neg = "
    "ffi.cast(lj_m7_tonumber_enum_ct, 'LJ_M7_TONUMBER_NEG')\n"
    "lj_m7_tonumber_enum_pos = "
    "ffi.cast(lj_m7_tonumber_enum_ct, 'LJ_M7_TONUMBER_POS')\n"
    "lj_m7_serialize_i64 = ffi.new('lj_m7_serialize_i64_t', -99)\n"
    "jit.off()\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  assert_predefined_tonumber_avoids_wait(L, cts);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);

  assert_tonumber_waits_without_lock(L, cts, tg,
    "assert(tonumber(lj_m7_tonumber_enum_neg) == -12)\n"
    "assert(tonumber(lj_m7_tonumber_enum_pos) == 17)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 4u);

  assert_tonumber_waits_without_lock(L, cts, tg,
    "local buffer = require('string.buffer')\n"
    "local out = buffer.decode(buffer.encode(lj_m7_serialize_i64))\n"
    "assert(out == lj_m7_serialize_i64)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 6u);

  ljt_lua_dostring(L,
    "assert(tonumber(lj_m7_tonumber_enum_neg) == -12)\n"
    "assert(tonumber(lj_m7_tonumber_enum_pos) == 17)\n"
    "local buffer = require('string.buffer')\n"
    "assert(buffer.decode(buffer.encode(lj_m7_serialize_i64)) == "
    "lj_m7_serialize_i64)\n");
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 == seq1);

  lua_close(L);
  printf("t-ffi-tonumber-snapshot OK: tonumber(cdata) waits on parser-owned ctypes\n");
  return 0;
}
