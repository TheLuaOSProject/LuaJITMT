/*
** Focused guard for ffi.istype() ctype comparison snapshots.
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

static void assert_istype_waits_without_lock(lua_State *L, CTState *cts,
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
    "assert(ffi.istype(lj_m7_istype_snapshot_ct, "
    "lj_m7_istype_snapshot_obj) == true)\n");
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.saw_native);
  assert(ljt_ctype_parse_seq(cts) == ctx.release_seq);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;
  uint32_t seq0, seq1, seq2;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } lj_m7_istype_snapshot_t;')\n"
    "lj_m7_istype_snapshot_ct = ffi.typeof('lj_m7_istype_snapshot_t')\n"
    "lj_m7_istype_snapshot_pct = ffi.typeof('lj_m7_istype_snapshot_t *')\n"
    "lj_m7_istype_snapshot_arr = ffi.typeof('lj_m7_istype_snapshot_t[1]')\n"
    "lj_m7_istype_snapshot_int = ffi.typeof('int')\n"
    "lj_m7_istype_snapshot_uint8 = ffi.typeof('uint8_t')\n"
    "lj_m7_istype_snapshot_obj = lj_m7_istype_snapshot_ct()\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ct = lj_m7_istype_snapshot_ct\n"
    "local pct = lj_m7_istype_snapshot_pct\n"
    "local arr_t = lj_m7_istype_snapshot_arr\n"
    "local int_t = lj_m7_istype_snapshot_int\n"
    "local uint8_t = lj_m7_istype_snapshot_uint8\n"
    "local arr = ffi.new(arr_t)\n"
    "local ptr = ffi.cast(pct, arr)\n"
    "local ival = int_t()\n"
    "local uval = uint8_t()\n"
    "for i = 1, 100 do\n"
    "  assert(ffi.istype(ct, arr[0]) == true)\n"
    "  assert(ffi.istype(ct, ptr) == true)\n"
    "  assert(ffi.istype(pct, ptr) == true)\n"
    "  assert(ffi.istype(pct, arr[0]) == false)\n"
    "  assert(ffi.istype(int_t, ival) == true)\n"
    "  assert(ffi.istype(int_t, uval) == false)\n"
    "  assert(ffi.istype(int_t, int_t) == true)\n"
    "end\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  assert_istype_waits_without_lock(L, cts, tg);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local obj = ffi.new(lj_m7_istype_snapshot_ct)\n"
    "assert(ffi.istype('struct { int lock_path; }', obj) == false)\n");
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 != seq1);

  lua_close(L);
  printf("t-ffi-istype-snapshot OK: stable istype comparisons avoid cparser sequence\n");
  return 0;
}
