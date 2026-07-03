/*
** Focused regression test for lock-free cdata __tostring type snapshots.
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

static void assert_tostring_waits_without_lock(lua_State *L, CTState *cts,
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

static void assert_predefined_tostring_avoids_wait(lua_State *L, CTState *cts)
{
  uint32_t seq0 = ljt_ctype_parse_seq(cts);
  uint32_t release_seq = ljt_ctype_hold_parse_token(cts);

  ljt_lua_dostring(L,
    "assert(tostring(lj_m7_tostring_i64):find('-42', 1, true))\n"
    "assert(tostring(lj_m7_tostring_u64):find('42', 1, true))\n");

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
    "typedef int64_t lj_m7_tostring_i64_t;\n"
    "typedef lj_m7_tostring_i64_t &lj_m7_tostring_i64_ref_t;\n"
    "typedef complex lj_m7_tostring_complex_t;\n"
    "typedef lj_m7_tostring_complex_t &lj_m7_tostring_complex_ref_t;\n"
    "enum lj_m7_tostring_snapshot_e { LJ_M7_TOSTRING_A = 7 };\n"
    "]]\n"
    "lj_m7_tostring_i64 = ffi.new('int64_t', -42)\n"
    "lj_m7_tostring_u64 = ffi.new('uint64_t', 42)\n"
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

  lua_close(L);
  printf("t-ffi-tostring-snapshot OK: cdata tostring waits on ctype snapshots\n");
  return 0;
}
