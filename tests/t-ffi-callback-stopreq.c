/*
** Focused guard for FFI callback native STOPREQ freshness.
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
#include "lj_atomic.h"
#include "lj_ctype.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

#include "lib/lua_fixture_helpers.h"

typedef int (*CallbackStopReqFn)(int);

typedef struct CallbackStopReqCtx {
  global_State *g;
  TGState *tg;
  pthread_t thread;
  uint32_t saw_native;
  uint32_t handshook;
  int err;
} CallbackStopReqCtx;

static global_State *test_g;
static TGState *test_tg;

static void callback_stopreq_sleep(void)
{
  struct timespec delay;
  delay.tv_sec = 0;
  delay.tv_nsec = 1000000;
  (void)nanosleep(&delay, NULL);
}

static void *callback_stopreq_worker(void *arg)
{
  CallbackStopReqCtx *ctx = (CallbackStopReqCtx *)arg;
  int i;
  for (i = 0; i < 1000 && lj_tg_in_native_acq(ctx->tg) == 0; i++)
    callback_stopreq_sleep();
  if (lj_tg_in_native_acq(ctx->tg) == 0) {
    ctx->err = 1;
    la_store32_rel(&ctx->handshook, 1);
    return NULL;
  }
  la_store32_rel(&ctx->saw_native, 1);
  if (lj_safepoint_handshake(ctx->g, LJ_GC2_HS_STOPREQ) == 0)
    ctx->err = 2;
  else if (!lj_tg_flags_test_acq(ctx->tg, TGF_STOPREQ))
    ctx->err = 3;
  else if (lj_tg_poll_acq(ctx->tg) != 0 ||
	   lj_tg_reqmask_acq(ctx->tg) != 0)
    ctx->err = 4;
  la_store32_rel(&ctx->handshook, 1);
  return NULL;
}

static int callback_fresh_stopreq_call(CallbackStopReqFn cb)
{
  CallbackStopReqCtx ctx;
  int err;
  assert(test_g != NULL);
  assert(test_tg != NULL);
  assert(cb != NULL);
  assert(lj_tg_in_native_acq(test_tg) != 0);
  memset(&ctx, 0, sizeof(ctx));
  ctx.g = test_g;
  ctx.tg = test_tg;
  err = pthread_create(&ctx.thread, NULL, callback_stopreq_worker, &ctx);
  assert(err == 0);
  err = pthread_join(ctx.thread, NULL);
  assert(err == 0);
  assert(la_load32_acq(&ctx.saw_native) == 1);
  assert(la_load32_acq(&ctx.handshook) == 1);
  assert(ctx.err == 0);
  assert(lj_tg_flags_test_acq(test_tg, TGF_STOPREQ));
  assert(lj_tg_poll_acq(test_tg) == 0);
  assert(lj_tg_reqmask_acq(test_tg) == 0);
  return cb(41);
}

static int callback_plain_call(CallbackStopReqFn cb)
{
  assert(test_tg != NULL);
  assert(cb != NULL);
  assert(lj_tg_in_native_acq(test_tg) != 0);
  return cb(11);
}

static int callback_mark_stopreq_c(lua_State *L)
{
  (void)L;
  assert(test_tg != NULL);
  assert(!lj_tg_flags_test_acq(test_tg, TGF_STOPREQ));
  lj_tg_flags_or_rlx(test_tg, TGF_STOPREQ);
  return 0;
}

static int callback_clear_stopreq_c(lua_State *L)
{
  (void)L;
  assert(test_tg != NULL);
  assert(lj_tg_flags_test_acq(test_tg, TGF_STOPREQ));
  assert(lj_tg_poll_acq(test_tg) == 0);
  assert(lj_tg_reqmask_acq(test_tg) == 0);
  lj_tg_flags_and_rlx(test_tg, (uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
  return 0;
}

static int callback_assert_clean_c(lua_State *L)
{
  (void)L;
  assert(test_tg != NULL);
  assert(!lj_tg_flags_test_acq(test_tg, TGF_STOPREQ));
  assert(lj_tg_poll_acq(test_tg) == 0);
  assert(lj_tg_reqmask_acq(test_tg) == 0);
  assert(lj_tg_in_native_acq(test_tg) == 0);
  assert(ccallback_native_had_stopreq_acq(&test_tg->cb) == 0);
  assert(ccallback_depth_acq(&test_tg->cb) == 0);
  return 0;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  test_g = G(L);
  test_tg = L2TG(L);
  assert(test_g != NULL);
  assert(test_tg != NULL);

  lua_pushlightuserdata(L, (void *)callback_fresh_stopreq_call);
  lua_setglobal(L, "lj_m7_callback_fresh_stopreq_call");
  lua_pushlightuserdata(L, (void *)callback_plain_call);
  lua_setglobal(L, "lj_m7_callback_plain_call");
  lua_pushcfunction(L, callback_mark_stopreq_c);
  lua_setglobal(L, "lj_m7_callback_mark_stopreq");
  lua_pushcfunction(L, callback_clear_stopreq_c);
  lua_setglobal(L, "lj_m7_callback_clear_stopreq");
  lua_pushcfunction(L, callback_assert_clean_c);
  lua_setglobal(L, "lj_m7_callback_assert_clean");

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef int (*lj_m7_callback_stopreq_cb_t)(int);\n"
    "typedef int (*lj_m7_callback_stopreq_call_t)"
    "(lj_m7_callback_stopreq_cb_t);\n"
    "]]\n"
    "local call_fresh = ffi.cast('lj_m7_callback_stopreq_call_t',\n"
    "                            lj_m7_callback_fresh_stopreq_call)\n"
    "local call_plain = ffi.cast('lj_m7_callback_stopreq_call_t',\n"
    "                           lj_m7_callback_plain_call)\n"
    "local sticky_entered = false\n"
    "local sticky_cb = ffi.cast('lj_m7_callback_stopreq_cb_t', function(x)\n"
    "  sticky_entered = true\n"
    "  return x + 1\n"
    "end)\n"
    "lj_m7_callback_mark_stopreq()\n"
    "local ok, value = pcall(function() return call_plain(sticky_cb) end)\n"
    "lj_m7_callback_clear_stopreq()\n"
    "assert(ok, tostring(value))\n"
    "assert(sticky_entered)\n"
    "assert(value == 12, tostring(value))\n"
    "sticky_cb:free()\n"
    "lj_m7_callback_assert_clean()\n"
    "local sticky_error_cb = ffi.cast('lj_m7_callback_stopreq_cb_t',\n"
    "  function()\n"
    "    error('sticky callback error')\n"
    "  end)\n"
    "lj_m7_callback_mark_stopreq()\n"
    "ok, value = pcall(function() return call_plain(sticky_error_cb) end)\n"
    "lj_m7_callback_clear_stopreq()\n"
    "sticky_error_cb:free()\n"
    "assert(not ok)\n"
    "assert(tostring(value):find('sticky callback error', 1, true),\n"
    "       tostring(value))\n"
    "lj_m7_callback_assert_clean()\n"
    "local fresh_entered = false\n"
    "local fresh_cb = ffi.cast('lj_m7_callback_stopreq_cb_t', function(x)\n"
    "  fresh_entered = true\n"
    "  return x + 1\n"
    "end)\n"
    "ok, value = pcall(function() return call_fresh(fresh_cb) end)\n"
    "lj_m7_callback_clear_stopreq()\n"
    "fresh_cb:free()\n"
    "assert(not ok)\n"
    "assert(not fresh_entered)\n"
    "assert(tostring(value):find('thread interrupted: VM shutdown', 1, true),\n"
    "       tostring(value))\n"
    "lj_m7_callback_assert_clean()\n");

  assert(!lj_tg_flags_test_acq(test_tg, TGF_STOPREQ));
  assert(lj_tg_in_native_acq(test_tg) == 0);
  assert(ccallback_depth_acq(&test_tg->cb) == 0);
  lua_close(L);
  printf("t-ffi-callback-stopreq OK: callback native STOPREQ freshness verified\n");
  return 0;
}
