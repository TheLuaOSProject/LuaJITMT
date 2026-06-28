/*
** Focused guard for lock-free callback set/free type snapshots.
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

static void assert_callback_waits_without_lock(lua_State *L, CTState *cts,
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

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;
  uint32_t seq0, seq1;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[typedef int (*lj_m7_callback_snapshot_t)(int);]]\n"
    "lj_m7_callback_snapshot_cb = "
    "ffi.cast('lj_m7_callback_snapshot_t', function(x) return x + 1 end)\n"
    "lj_m7_callback_snapshot_free = "
    "ffi.cast('lj_m7_callback_snapshot_t', function(x) return x + 4 end)\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  ljt_lua_dostring(L,
    "lj_m7_callback_snapshot_cb:set(function(x) return x + 2 end)\n"
    "assert(lj_m7_callback_snapshot_cb(40) == 42)\n"
    "assert(lj_m7_callback_snapshot_free(38) == 42)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  assert_callback_waits_without_lock(L, cts, tg,
    "lj_m7_callback_snapshot_cb:set(function(x) return x + 3 end)\n"
    "assert(lj_m7_callback_snapshot_cb(39) == 42)\n");

  assert_callback_waits_without_lock(L, cts, tg,
    "lj_m7_callback_snapshot_free:free()\n"
    "assert(not pcall(lj_m7_callback_snapshot_free))\n"
    "lj_m7_callback_snapshot_free = nil\n");

  ljt_lua_dostring(L,
    "lj_m7_callback_snapshot_cb:free()\n"
    "lj_m7_callback_snapshot_cb = nil\n"
    "collectgarbage('collect')\n");

  lua_close(L);
  printf("t-ffi-callback-snapshot OK: callback set/free waits on ctype snapshots\n");
  return 0;
}
