/*
** Focused guard for ffi.typeinfo() ctype snapshots.
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

static void assert_typedef_name_waits_without_lock(lua_State *L, CTState *cts,
						   TGState *tg)
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
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ct = ffi.typeof('lj_m7_typeinfo_snapshot_t')\n"
    "assert(tonumber(ct) == lj_m7_typeinfo_snapshot_id)\n"
    "assert(ffi.sizeof('lj_m7_typeinfo_snapshot_t') == 4)\n");
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.saw_native);
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
    "ffi.cdef('typedef struct { int x; } lj_m7_typeinfo_snapshot_t;')\n"
    "lj_m7_typeinfo_snapshot_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t'))\n"
    "lj_m7_typeinfo_int_id = tonumber(ffi.typeof('int'))\n"
    "assert(type(lj_m7_typeinfo_snapshot_id) == 'number')\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "for i = 1, 100 do\n"
    "  local ct = ffi.typeof('lj_m7_typeinfo_snapshot_t')\n"
    "  assert(tonumber(ct) == lj_m7_typeinfo_snapshot_id)\n"
    "  assert(ffi.sizeof('lj_m7_typeinfo_snapshot_t') == 4)\n"
    "  assert(ffi.alignof('lj_m7_typeinfo_snapshot_t') == 4)\n"
    "end\n"
    "for i = 1, 100 do\n"
    "  local ti = ffi.typeinfo(lj_m7_typeinfo_snapshot_id)\n"
    "  assert(ti and ti.size == 4)\n"
    "end\n"
    "assert(ffi.typeinfo(0) == nil)\n"
    "assert(ffi.typeinfo(1000000000) == nil)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  assert_typedef_name_waits_without_lock(L, cts, tg);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);

  {
    uint32_t release_seq = ljt_ctype_hold_parse_token(cts);
    ljt_lua_dostring(L,
      "local ffi = require('ffi')\n"
      "local ti = ffi.typeinfo(lj_m7_typeinfo_int_id)\n"
      "assert(ti and ti.size == 4)\n"
      "assert(ffi.typeinfo(lj_m7_typeinfo_snapshot_id) == nil)\n");
    assert((ctype_parse_token_acq(cts) & 1u) != 0);
    ljt_ctype_release_parse_token(cts, release_seq);
  }

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ti = ffi.typeinfo(lj_m7_typeinfo_snapshot_id)\n"
    "assert(ti and ti.size == 4)\n");
  assert(ljt_ctype_parse_seq(cts) == seq1 + 2u);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef int lj_m7_typeinfo_snapshot_seq_t;')\n"
    "lj_m7_typeinfo_snapshot_seq_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_seq_t'))\n");
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 != seq1);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ti = ffi.typeinfo(lj_m7_typeinfo_snapshot_seq_id)\n"
    "assert(ti and ti.info ~= nil)\n");
  seq3 = ljt_ctype_parse_seq(cts);
  assert(seq3 == seq2);

  lua_close(L);
  printf("t-ffi-typeinfo-snapshot OK: stable typeinfo reads avoid parser locking\n");
  return 0;
}
