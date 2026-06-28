/*
** Focused guard for exported FFI C-call native-state helpers.
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
#include "lj_ccall.h"
#include "lj_ctype.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

typedef struct NativeHelperStopReqCtx {
  global_State *g;
  TGState *tg;
  uint32_t published;
  uint32_t saw_native;
} NativeHelperStopReqCtx;

static CCallNativeState pending_native;
static uint32_t pending_actions;

static void dummy_foreign(void)
{
}

static void dummy_callback_foreign(void)
{
}

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
  (void)lj_tg_flags_and_rlx(tg, (uint8_t)~TGF_STOPREQ);
}

static void *publish_stopreq_while_native(void *arg)
{
  NativeHelperStopReqCtx *ctx = (NativeHelperStopReqCtx *)arg;
  int i;
  for (i = 0; i < 500; i++) {
    if (lj_tg_in_native_acq(ctx->tg)) {
      ctx->saw_native = 1;
      break;
    }
    sleep_ns(1000000);
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

static int checkstop_from_lua(lua_State *L)
{
  lj_ccall_native_checkstop(L, pending_actions, &pending_native);
  return 0;
}

static void run_restore_state(lua_State *L, CTState *cts, TGState *tg)
{
  CCallNativeState native;
  void *old_func = (void *)(uintptr_t)&run_restore_state;
  void *func = (void *)(uintptr_t)&dummy_foreign;
  uint32_t actions;

  clear_stopreq(tg);
  lj_tg_ffi_call_func_rel(tg, old_func);
  ccallback_native_had_stopreq_rel(&tg->cb, 1);

  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  assert(lj_tg_in_native_acq(tg) != 0);
  assert(lj_tg_ffi_call_func_acq(tg) == func);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == 0);
  assert(tg->cb.slot == (MSize)~0u);

  actions = lj_ccall_native_leave(L, cts, &native, func);
  assert(actions == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == old_func);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == 1);

  lj_tg_ffi_call_func_rel(tg, NULL);
  ccallback_native_had_stopreq_rel(&tg->cb, 0);
}

static void run_callback_blacklist(lua_State *L, CTState *cts, TGState *tg)
{
  CCallNativeState native;
  void *func = (void *)(uintptr_t)&dummy_callback_foreign;

  clear_stopreq(tg);
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  tg->cb.slot = 0;  /* Simulate a callback trampoline reaching Lua. */
  (void)lj_ccall_native_leave(L, cts, &native, func);

  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == 0);
  assert(lj_ctype_cb_isblacklisted(cts, func));
}

static void run_fresh_stopreq_check(lua_State *L, CTState *cts,
				    global_State *g, TGState *tg)
{
  NativeHelperStopReqCtx ctx;
  CCallNativeState native;
  pthread_t thread;
  void *func = (void *)(uintptr_t)&dummy_foreign;

  clear_stopreq(tg);
  ctx.g = g;
  ctx.tg = tg;
  ctx.published = 0;
  ctx.saw_native = 0;

  assert(pthread_create(&thread, NULL, publish_stopreq_while_native,
			&ctx) == 0);
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  sleep_ns(20000000);
  pending_actions = lj_ccall_native_leave(L, cts, &native, func);
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.published);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);
  assert(lj_tg_flags_test_acq(tg, TGF_STOPREQ));

  pending_native = native;
  lua_pushcfunction(L, checkstop_from_lua);
  lua_setglobal(L, "ccall_native_checkstop_probe");
  run_lua_ok(L,
    "local ok, err = pcall(ccall_native_checkstop_probe)\n"
    "assert(ok == false, 'fresh STOPREQ helper check did not interrupt')\n"
    "assert(tostring(err):find('thread interrupted: VM shutdown', 1, true),\n"
    "       tostring(err))\n");
  clear_stopreq(tg);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  CTState *cts;

  assert(L != NULL);
  luaL_openlibs(L);
  run_lua_ok(L,
    "local ffi = require('ffi')\n"
    "jit.off()\n"
    "ffi.cdef'int getpid(void);'\n");

  g = G(L);
  tg = G2TG(g);
  cts = ctype_cts(L);
  assert(tg != NULL);
  assert(cts != NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);

  run_restore_state(L, cts, tg);
  run_callback_blacklist(L, cts, tg);
  run_fresh_stopreq_check(L, cts, g, tg);

  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) == 0);

  lua_close(L);
  printf("t-ffi-ccall-native-helpers OK: exported native helper ABI verified\n");
  return 0;
}
