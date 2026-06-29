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
    "local arr = ffi.new(lj_m7_istype_snapshot_arr)\n"
    "local ptr = ffi.cast(lj_m7_istype_snapshot_pct, arr)\n"
    "assert(ffi.istype(lj_m7_istype_snapshot_ct, ptr) == true)\n");
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.saw_native);
  assert(ljt_ctype_parse_seq(cts) == ctx.release_seq);
}

static void assert_istype_predefined_avoids_wait(lua_State *L, CTState *cts)
{
  uint32_t seq0 = ljt_ctype_parse_seq(cts);
  uint32_t release_seq = ljt_ctype_hold_parse_token(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(ffi.istype(lj_m7_istype_snapshot_int, "
    "lj_m7_istype_snapshot_int_obj) == true)\n"
    "assert(ffi.istype(lj_m7_istype_snapshot_int, "
    "lj_m7_istype_snapshot_uint8_obj) == false)\n"
    "assert(ffi.istype(lj_m7_istype_snapshot_int, "
    "lj_m7_istype_snapshot_int) == true)\n");

  ljt_ctype_release_parse_token(cts, release_seq);
  assert(ljt_ctype_parse_seq(cts) == seq0 + 2u);
}

static void assert_istype_trace_records_exact_id(lua_State *L, CTState *cts)
{
  ljt_ctype_arm_trace_abort(L, cts);
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local ct = lj_m7_istype_snapshot_ct\n"
    "local obj = lj_m7_istype_snapshot_obj\n"
    "local function run(n)\n"
    "  local count = 0\n"
    "  for i = 1, n do\n"
    "    if ffi.istype(ct, obj) then count = count + 1 end\n"
    "  end\n"
    "  return count\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 8) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_stop_count() >= 1)\n"
    "assert(lj_m7_trace_parse_token_abort_count() == 0)\n"
    "assert(lj_m7_trace_parse_token_ctbusy_count() == 0)\n");
  assert(ljt_ctype_trace_start_count != 0);
  assert(ljt_ctype_trace_stop_count != 0);
  assert(ljt_ctype_trace_abort_count == 0);
  assert(ljt_ctype_trace_ctbusy_count == 0);
  assert(ljt_ctype_trace_cts == NULL);
  assert(ljt_ctype_trace_seq == 0);
  assert((ctype_parse_token_acq(cts) & 1u) == 0);
}

static void assert_istype_trace_snapshot_ctbusy(lua_State *L, CTState *cts)
{
  ljt_ctype_arm_trace_abort(L, cts);
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local ct = lj_m7_istype_snapshot_ct\n"
    "local pct = lj_m7_istype_snapshot_pct\n"
    "local arr_t = lj_m7_istype_snapshot_arr\n"
    "local arr = ffi.new(arr_t)\n"
    "local ptr = ffi.cast(pct, arr)\n"
    "local function run(n)\n"
    "  local count = 0\n"
    "  for i = 1, n do\n"
    "    if ffi.istype(ct, ptr) then count = count + 1 end\n"
    "  end\n"
    "  return count\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 8) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n"
    "assert(lj_m7_trace_parse_token_ctbusy_count() >= 1)\n");
  assert(ljt_ctype_trace_start_count != 0);
  assert(ljt_ctype_trace_abort_count != 0);
  assert(ljt_ctype_trace_ctbusy_count != 0);
  assert(ljt_ctype_trace_cts == NULL);
  assert(ljt_ctype_trace_seq == 0);
  assert((ctype_parse_token_acq(cts) & 1u) == 0);
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
    "lj_m7_istype_snapshot_obj = lj_m7_istype_snapshot_ct()\n"
    "lj_m7_istype_snapshot_int_obj = lj_m7_istype_snapshot_int()\n"
    "lj_m7_istype_snapshot_uint8_obj = lj_m7_istype_snapshot_uint8()\n");

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

  assert_istype_predefined_avoids_wait(L, cts);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);

  assert_istype_waits_without_lock(L, cts, tg);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 4u);

  assert_istype_trace_records_exact_id(L, cts);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 6u);

  assert_istype_trace_snapshot_ctbusy(L, cts);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 8u);

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
