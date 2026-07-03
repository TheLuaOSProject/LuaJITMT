/*
** Focused regression test for lock-free JIT snapshot cdata restore ctype reads.
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

#include "lib/ctype_parse_fixture_helpers.h"
#include "lib/lua_fixture_helpers.h"

typedef struct ParseReleaseCtx {
  CTState *cts;
  uint32_t release_seq;
  volatile int released;
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
  sleep_ns(200000000);
  ljt_ctype_release_parse_token(ctx->cts, ctx->release_seq);
  ctx->released = 1;
  return NULL;
}

static void assert_snap_restore_waits_for_release(lua_State *L, CTState *cts,
						  const char *chunk)
{
  ParseReleaseCtx ctx;
  pthread_t thread;
  uint32_t seq0 = ljt_ctype_parse_seq(cts);
  uint32_t seq1;

  ctx.cts = cts;
  ctx.release_seq = ljt_ctype_hold_parse_token(cts);
  ctx.released = 0;
  assert(ctx.release_seq == seq0 + 2u);

  assert(pthread_create(&thread, NULL, release_parse_token, &ctx) == 0);
  ljt_lua_dostring(L, chunk);
  seq1 = ctype_parse_token_acq(cts);
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.released);
  assert(seq1 == ctx.release_seq);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  uint32_t seq0, seq1, seq2, seq3;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef struct { int x; double y; } lj_m7_snap_restore_snapshot_t;\n"
    "typedef const int64_t lj_m7_snap_restore_snapshot_i64_t;\n"
    "]]\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1000')\n"
    "lj_m7_snap_restore_struct_t = "
    "ffi.typeof('lj_m7_snap_restore_snapshot_t')\n"
    "lj_m7_snap_restore_i64_t = "
    "ffi.typeof('lj_m7_snap_restore_snapshot_i64_t')\n"
    "function lj_m7_snap_restore_make_struct(n, stop)\n"
    "  local struct_t = lj_m7_snap_restore_struct_t\n"
    "  for i = 1, n do\n"
    "    local obj = struct_t(i, i + 0.5)\n"
    "    if i == stop then return obj end\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    "function lj_m7_snap_restore_make_i64(n, stop)\n"
    "  local int64_t = lj_m7_snap_restore_i64_t\n"
    "  for i = 1, n do\n"
    "    local v = int64_t(i)\n"
    "    if i == stop then return v end\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    "for _ = 1, 60 do\n"
    "  assert(lj_m7_snap_restore_make_struct(80, 0) == nil)\n"
    "  assert(lj_m7_snap_restore_make_i64(80, 0) == nil)\n"
    "end\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  assert_snap_restore_waits_for_release(L, cts,
    "local obj = lj_m7_snap_restore_make_struct(80, 37)\n"
    "assert(obj.x == 37)\n"
    "assert(obj.y == 37.5)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);

  assert_snap_restore_waits_for_release(L, cts,
    "local v = lj_m7_snap_restore_make_i64(80, 41)\n"
    "assert(tonumber(v) == 41)\n");
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 == seq0 + 4u);

  ljt_lua_dostring(L,
    "local obj = lj_m7_snap_restore_make_struct(80, 43)\n"
    "assert(obj.x == 43)\n"
    "assert(obj.y == 43.5)\n"
    "local v = lj_m7_snap_restore_make_i64(80, 47)\n"
    "assert(tonumber(v) == 47)\n");
  seq3 = ljt_ctype_parse_seq(cts);
  assert(seq3 == seq2);

  lua_close(L);
  printf("t-ffi-snap-restore-snapshot OK: JIT cdata restore waits on parser-owned ctypes\n");
  return 0;
}
