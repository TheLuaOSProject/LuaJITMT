/*
** Focused guard for FFI library memory helpers' native STOPREQ behavior.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

typedef struct FFILibStopReqCtx {
  global_State *g;
  TGState *tg;
  uint32_t published;
  uint32_t saw_native;
} FFILibStopReqCtx;

static void sleep_ns(long ns)
{
  struct timespec ts;
  ts.tv_sec = ns / 1000000000L;
  ts.tv_nsec = ns % 1000000000L;
  while (nanosleep(&ts, &ts) != 0)
    ;
}

static void clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg, (uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
}

static void set_stopreq(TGState *tg)
{
  (void)lj_tg_flags_or_rlx(tg, TGF_STOPREQ);
}

static void *publish_stopreq_while_native(void *arg)
{
  FFILibStopReqCtx *ctx = (FFILibStopReqCtx *)arg;
  int i;
  for (i = 0; i < 50000; i++) {
    if (lj_tg_in_native_acq(ctx->tg)) {
      ctx->saw_native = 1;
      break;
    }
    sleep_ns(100000);
  }
  assert(ctx->saw_native);
  assert(lj_safepoint_handshake(ctx->g, LJ_GC2_HS_STOPREQ) >= 1u);
  ctx->published = 1;
  return NULL;
}

static void run_lua_ok(lua_State *L, const char *chunk)
{
  int rc = luaL_dostring(L, chunk);
  if (rc != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "unexpected Lua error: %s\n", err ? err : "(nil)");
  }
  assert(rc == LUA_OK);
}

static void run_sticky_stopreq_ok(lua_State *L, TGState *tg)
{
  set_stopreq(tg);
  run_lua_ok(L,
    "local ffi = require('ffi')\n"
    "local dst = ffi.new('char[16]')\n"
    "local src = ffi.new('char[16]')\n"
    "ffi.fill(src, 16, 0)\n"
    "ffi.copy(src, 'sticky')\n"
    "ffi.fill(dst, 16, 0)\n"
    "ffi.copy(dst, src, 16)\n"
    "assert(ffi.string(dst) == 'sticky')\n"
    "ffi.copy(dst, 'stock')\n"
    "assert(ffi.string(dst) == 'stock')\n");
  assert(lj_tg_flags_test_acq(tg, TGF_STOPREQ));
  assert(lj_tg_in_native_acq(tg) == 0);
  clear_stopreq(tg);
}

static void run_fresh_stopreq_interrupt(lua_State *L, global_State *g,
					TGState *tg)
{
  FFILibStopReqCtx ctx;
  pthread_t thread;
  int rc;

  ctx.g = g;
  ctx.tg = tg;
  ctx.published = 0;
  ctx.saw_native = 0;

  assert(pthread_create(&thread, NULL, publish_stopreq_while_native, &ctx) == 0);
  rc = luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "local n = 64 * 1024 * 1024\n"
    "local buf = ffi.new('uint8_t[?]', n)\n"
    "local ok, err = pcall(function()\n"
    "  for i = 1, 16 do ffi.fill(buf, n, i) end\n"
    "end)\n"
    "assert(ok == false, 'fresh STOPREQ did not interrupt ffi.fill')\n"
    "assert(tostring(err):find('thread interrupted: VM shutdown', 1, true),\n"
    "       tostring(err))\n");
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.published);
  if (rc != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "fresh STOPREQ chunk failed: %s\n", err ? err : "(nil)");
  }
  assert(rc == LUA_OK);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_flags_test_acq(tg, TGF_STOPREQ));
  clear_stopreq(tg);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;

  assert(L != NULL);
  luaL_openlibs(L);
  run_lua_ok(L,
    "local ffi = require('ffi')\n"
    "jit.off()\n"
    "assert(ffi.abi('64bit'))\n");

  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert(lj_tg_in_native_acq(tg) == 0);

  run_sticky_stopreq_ok(L, tg);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) == 0);

  run_fresh_stopreq_interrupt(L, g, tg);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) == 0);

  lua_close(L);
  printf("t-ffi-lib-native-stopreq OK: FFI library native STOPREQ verified\n");
  return 0;
}
