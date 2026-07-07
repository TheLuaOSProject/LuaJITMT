/*
** Focused regression test for exported FFI C-call native-state helpers.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lib/test_sleep.h"
#include "lib/lua_fixture_helpers.h"
#include "lib/tg_stopreq_fixture_helpers.h"

#include "lj_obj.h"
#include "lj_ccall.h"
#include "lj_ctype.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

typedef struct NativeHelperStopReqCtx {
  global_State *g;
  TGState *tg;
  uint32_t published;
  uint32_t saw_native;
} NativeHelperStopReqCtx;

static CCallNativeState pending_native;
static global_State *pending_g;
static TGState *pending_tg;
static uint32_t pending_actions;

static void dummy_foreign(void)
{
}

static void dummy_callback_foreign(void)
{
}

static double dummy_num_i32_i32(int32_t a, int32_t b)
{
  return (double)a * 2.0 + (double)b + 0.5;
}

static double dummy_num_u32_u32(uint32_t a, uint32_t b)
{
  return (double)(a & 255u) + (double)(b & 255u) + 0.875;
}

static double dummy_num_i64_u32(int64_t a, uint32_t b)
{
  return (double)((uint64_t)a & 255u) + (double)(b & 255u) + 1.125;
}

static void queue_stopreq_request(global_State *g, TGState *tg)
{
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  gc2_hs_actions_rel(g, LJ_GC2_HS_STOPREQ);
  gc2_hs_pending_rel(g, 1);
  gc2_hs_epoch_rel(g, gc2_hs_epoch_rlx(g) + 1u);
  lj_tg_reqmask_rel(tg, LJ_GC2_HS_STOPREQ);
  lj_tg_poll_rel(tg, 1);
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

static int checkstop_from_lua(lua_State *L)
{
  lj_ccall_native_checkstop(L, pending_actions, &pending_native);
  return 0;
}

static int queue_then_checkstop_from_lua(lua_State *L)
{
  queue_stopreq_request(pending_g, pending_tg);
  lj_ccall_native_checkstop(L, pending_actions, &pending_native);
  return 0;
}

static void run_restore_state(lua_State *L, CTState *cts, TGState *tg)
{
  CCallNativeState native;
  void *old_func = (void *)(uintptr_t)&run_restore_state;
  void *func = (void *)(uintptr_t)&dummy_foreign;
  uint32_t actions;

  ljt_tg_clear_stopreq(tg);
  lj_tg_ffi_call_func_rel(tg, old_func);
  ccallback_slot_rel(&tg->cb, 17);
  ccallback_native_had_stopreq_rel(&tg->cb, 1);

  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  assert(lj_tg_in_native_acq(tg) != 0);
  assert(lj_tg_ffi_call_func_acq(tg) == func);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == 0);
  assert(ccallback_slot_acq(&tg->cb) == (MSize)~0u);

  actions = lj_ccall_native_leave(L, cts, &native, func);
  assert(actions == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == old_func);
  assert(ccallback_slot_acq(&tg->cb) == 17);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == 1);

  lj_tg_ffi_call_func_rel(tg, NULL);
  ccallback_slot_rel(&tg->cb, 0);
  ccallback_native_had_stopreq_rel(&tg->cb, 0);
}

static void run_callback_blacklist(lua_State *L, CTState *cts, TGState *tg)
{
  CCallNativeState native;
  void *func = (void *)(uintptr_t)&dummy_callback_foreign;

  ljt_tg_clear_stopreq(tg);
  ccallback_slot_rel(&tg->cb, 23);
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ccallback_slot_rel(&tg->cb, 0);  /* Simulate a callback trampoline reaching Lua. */
  (void)lj_ccall_native_leave(L, cts, &native, func);

  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);
  assert(ccallback_slot_acq(&tg->cb) == 23);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == 0);
  assert(lj_ctype_cb_isblacklisted(cts, func));
  ccallback_slot_rel(&tg->cb, 0);
}

static void run_fresh_stopreq_check(lua_State *L, CTState *cts,
				    global_State *g, TGState *tg)
{
  NativeHelperStopReqCtx ctx;
  CCallNativeState native;
  pthread_t thread;
  void *func = (void *)(uintptr_t)&dummy_foreign;

  ljt_tg_clear_stopreq(tg);
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
  assert(ccallback_slot_acq(&tg->cb) == 0);
  assert(ljt_tg_has_stopreq(tg));

  pending_native = native;
  lua_pushcfunction(L, checkstop_from_lua);
  lua_setglobal(L, "ccall_native_checkstop_probe");
  ljt_lua_dostring(L,
    "local ok, err = pcall(ccall_native_checkstop_probe)\n"
    "assert(ok == false, 'fresh STOPREQ helper check did not interrupt')\n"
    "assert(tostring(err):find('thread interrupted: VM shutdown', 1, true),\n"
    "       tostring(err))\n");
  ljt_tg_clear_stopreq(tg);
}

static void run_post_leave_stopreq_check(lua_State *L, CTState *cts,
					 global_State *g, TGState *tg)
{
  CCallNativeState native;
  void *func = (void *)(uintptr_t)&dummy_foreign;

  ljt_tg_clear_stopreq(tg);
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  pending_actions = lj_ccall_native_leave(L, cts, &native, func);
  assert(pending_actions == 0);
  assert(!ljt_tg_has_stopreq(tg));

  pending_native = native;
  pending_g = g;
  pending_tg = tg;
  lua_pushcfunction(L, queue_then_checkstop_from_lua);
  lua_setglobal(L, "ccall_post_leave_checkstop_probe");
  ljt_lua_dostring(L,
    "local ok, err = pcall(ccall_post_leave_checkstop_probe)\n"
    "assert(ok == false, 'post-leave STOPREQ helper check did not poll')\n"
    "assert(tostring(err):find('thread interrupted: VM shutdown', 1, true),\n"
    "       tostring(err))\n");
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(ljt_tg_has_stopreq(tg));
  ljt_tg_clear_stopreq(tg);
}

static void run_num_gpr_helper(lua_State *L, TGState *tg)
{
  double r;

  ljt_tg_clear_stopreq(tg);
  r = lj_ccall_jit_num_gpr(L, (void *)(uintptr_t)&dummy_num_i32_i32,
			   (uintptr_t)3, (uintptr_t)4,
			   LJ_CCALL_JIT_SIG_I32_I32);
  assert(r == 10.5);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);

  r = lj_ccall_jit_num_gpr(L, (void *)(uintptr_t)&dummy_num_u32_u32,
			   (uintptr_t)UINT32_C(0xfffffff0), (uintptr_t)11,
			   LJ_CCALL_JIT_SIG_U32_U32);
  assert(r == 240 + 11 + 0.875);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);

  r = lj_ccall_jit_num_gpr(L, (void *)(uintptr_t)&dummy_num_i64_u32,
			   (uintptr_t)(int64_t)-15,
			   (uintptr_t)UINT32_C(0xfffffff2),
			   LJ_CCALL_JIT_SIG_I64_U32);
  assert(r == 241 + 242 + 1.125);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g;
  TGState *tg;
  CTState *cts;

  ljt_lua_dostring(L,
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
  run_post_leave_stopreq_check(L, cts, g, tg);
  run_num_gpr_helper(L, tg);

  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) == 0);

  lua_close(L);
  printf("t-ffi-ccall-native-helpers OK: exported native helper ABI verified\n");
  return 0;
}
