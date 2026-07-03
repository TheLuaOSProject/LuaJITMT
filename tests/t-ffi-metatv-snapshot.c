/*
** Focused regression test for lock-free ctype metamethod lookup snapshots.
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

static void assert_metatv_waits_without_lock(lua_State *L, CTState *cts,
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
