/*
** Focused guard for lock-free C library extern variable snapshots.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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

static void assert_clib_extern_waits_without_lock(lua_State *L, CTState *cts,
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

static void assert_busy_trace_releases(lua_State *L, CTState *cts,
				       const char *chunk)
{
  ljt_ctype_arm_trace_abort(L, cts);
  ljt_lua_dostring(L, chunk);
  ljt_ctype_assert_trace_abort_released(cts);
}

int main(void)
{
  const char *so = getenv("LJ_M7_FFI_CLIB_EXTERN_SO");
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;
  uint32_t seq0, seq1;

  assert(so != NULL);
  lua_pushstring(L, so);
  lua_setglobal(L, "lj_m7_clib_extern_so");

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef struct { int x; } lj_m7_clib_snapshot_struct_t;\n"
    "extern int lj_m7_clib_snapshot_value;\n"
    "extern const int lj_m7_clib_snapshot_const;\n"
    "extern lj_m7_clib_snapshot_struct_t lj_m7_clib_snapshot_struct;\n"
    "]]\n"
    "lj_m7_clib_extern_cl = ffi.load(assert(lj_m7_clib_extern_so))\n"
    "lj_m7_clib_snapshot_struct_ct = "
    "ffi.typeof('lj_m7_clib_snapshot_struct_t')\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local cl = lj_m7_clib_extern_cl\n"
    "assert(cl.lj_m7_clib_snapshot_value == 37)\n"
    "cl.lj_m7_clib_snapshot_value = 41\n"
    "assert(cl.lj_m7_clib_snapshot_value == 41)\n"
    "assert(cl.lj_m7_clib_snapshot_struct.x == 9)\n"
    "cl.lj_m7_clib_snapshot_struct = lj_m7_clib_snapshot_struct_ct({ x = 55 })\n"
    "assert(cl.lj_m7_clib_snapshot_struct.x == 55)\n"
    "assert(cl.lj_m7_clib_snapshot_const == 73)\n"
    "local ok = pcall(function()\n"
    "  cl.lj_m7_clib_snapshot_const = 1\n"
    "end)\n"
    "assert(not ok)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  assert_clib_extern_waits_without_lock(L, cts, tg,
    "local cl = lj_m7_clib_extern_cl\n"
    "assert(cl.lj_m7_clib_snapshot_value == 41)\n");

  assert_clib_extern_waits_without_lock(L, cts, tg,
    "local ffi = require('ffi')\n"
    "local cl = lj_m7_clib_extern_cl\n"
    "cl.lj_m7_clib_snapshot_value = 42\n"
    "cl.lj_m7_clib_snapshot_struct = lj_m7_clib_snapshot_struct_ct({ x = 56 })\n"
    "assert(cl.lj_m7_clib_snapshot_value == 42)\n"
    "assert(cl.lj_m7_clib_snapshot_struct.x == 56)\n");

  assert_busy_trace_releases(L, cts,
    "local cl = lj_m7_clib_extern_cl\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + cl.lj_m7_clib_snapshot_value end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 336) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  assert_busy_trace_releases(L, cts,
    "local cl = lj_m7_clib_extern_cl\n"
    "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  for i = 1, n do cl.lj_m7_clib_snapshot_value = 100 + i end\n"
    "  return cl.lj_m7_clib_snapshot_value\n"
    "end\n"
    "for i = 1, 3 do assert(run(8) == 108) end\n"
    "jit.attach(lj_m7_trace_parse_token)\n"
    "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");

  lua_close(L);
  printf("t-ffi-clib-extern-snapshot OK: extern variables wait on ctype snapshots\n");
  return 0;
}
