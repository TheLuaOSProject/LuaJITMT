/*
** Focused regression test for exported FFI C-call native-state helpers.
*/

#include <assert.h>
#include <errno.h>
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
#if LJ_TARGET_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include "lj_ccall.h"
#include "lj_ctype.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_state.h"
#include "lj_tg.h"

typedef struct NativeHelperStopReqCtx {
  global_State *g;
  TGState *tg;
  uint32_t published;
  uint32_t saw_native;
} NativeHelperStopReqCtx;

typedef struct NativeHelperRedispatchCtx {
  global_State *g;
  TGState *tg;
  uint32_t published;
  uint32_t saw_native;
} NativeHelperRedispatchCtx;

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
typedef struct NativeHelperAllocCtx {
  lua_Alloc oldf;
  void *oldud;
  uint32_t clobbers;
} NativeHelperAllocCtx;
#endif

#define STOPREQ_WINERR UINT32_C(0x52a7)

static CCallNativeState pending_native;
static global_State *pending_g;
static TGState *pending_tg;
static uint32_t pending_actions;
static NativeHelperRedispatchCtx *errno_redispatch_ctx;

static const char fresh_stopreq_chunk[] =
  "local ok, err = pcall(ccall_native_checkstop_probe)\n"
  "local e, w = ccall_error_state_probe()\n"
  "assert(ok == false, 'fresh STOPREQ helper check did not interrupt')\n"
  "assert(e == ccall_expected_errno, e)\n"
  "assert(w == ccall_expected_winerr, w)\n"
  "assert(tostring(err):find('thread interrupted: VM shutdown', 1, true),\n"
  "       tostring(err))\n";

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
static void *clobber_error_alloc(void *ud, void *ptr, size_t osize,
				 size_t nsize)
{
  NativeHelperAllocCtx *ctx = (NativeHelperAllocCtx *)ud;
  void *p = ctx->oldf(ctx->oldud, ptr, osize, nsize);
  ctx->clobbers++;
  errno = EILSEQ;
#if LJ_TARGET_WINDOWS
  SetLastError((DWORD)(STOPREQ_WINERR + 1u));
#endif
  return p;
}
#endif

static int error_state_from_lua(lua_State *L)
{
  uint32_t winerr;
  int errnum;
#if LJ_TARGET_WINDOWS
  winerr = (uint32_t)GetLastError();
#else
  winerr = 0;
#endif
  errnum = errno;
  lua_pushinteger(L, errnum);
  lua_pushinteger(L, (lua_Integer)winerr);
  return 2;
}

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

static int32_t dummy_errno_after_redispatch(void)
{
  NativeHelperRedispatchCtx *ctx = errno_redispatch_ctx;
  while (la_load32_acq(&ctx->published) == 0)
    sleep_ns(1000000);
  errno = EDOM;
  return 73;
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

static void queue_redispatch_request(global_State *g, TGState *tg)
{
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  gc2_hs_actions_rel(g, LJ_GC2_HS_REDISPATCH);
  gc2_hs_pending_rel(g, 1);
  gc2_hs_epoch_rel(g, gc2_hs_epoch_rlx(g) + 1u);
  lj_tg_reqmask_rel(tg, LJ_GC2_HS_REDISPATCH);
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

static void *publish_redispatch_while_native(void *arg)
{
  NativeHelperRedispatchCtx *ctx = (NativeHelperRedispatchCtx *)arg;
  int i;
  for (i = 0; i < 500; i++) {
    if (lj_tg_in_native_acq(ctx->tg)) {
      la_store32_rel(&ctx->saw_native, 1);
      break;
    }
    sleep_ns(1000000);
  }
  assert(la_load32_acq(&ctx->saw_native) != 0);
  queue_redispatch_request(ctx->g, ctx->tg);
  la_store32_rel(&ctx->published, 1);
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

  errno = EAGAIN;
  lj_ccall_native_save(L, &native);
  /* Simulate interpreted argument conversion clobbering errno between the
  ** outer save and native publication. enter must expose the saved value. */
  errno = EILSEQ;
  lj_ccall_native_enter(L, &native, func);
  assert(errno == EAGAIN);
  assert(lj_tg_in_native_acq(tg) != 0);
  assert(lj_tg_ffi_call_func_acq(tg) == func);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == 0);
  assert(ccallback_slot_acq(&tg->cb) == (MSize)~0u);

  actions = lj_ccall_native_leave(L, cts, &native, func);
  assert(actions == 0);
  assert(errno == EAGAIN);
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
#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
  NativeHelperAllocCtx alloc;
#endif
  NativeHelperStopReqCtx ctx;
  CCallNativeState native;
  pthread_t thread;
  void *func = (void *)(uintptr_t)&dummy_foreign;

  ljt_tg_clear_stopreq(tg);
  /* Compile before publishing STOPREQ. The loader is itself an L-aware
  ** safepoint boundary and correctly throws a pending request before this
  ** chunk can establish its inner pcall frame. Keep the compiled closure on
  ** the Lua stack through the allocation-free native-state probe, then test
  ** only the intended post-call checkstop unwind below. */
  ljt_lua_loadstring(L, fresh_stopreq_chunk);
  ctx.g = g;
  ctx.tg = tg;
  ctx.published = 0;
  ctx.saw_native = 0;

  assert(pthread_create(&thread, NULL, publish_stopreq_while_native,
			&ctx) == 0);
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  sleep_ns(20000000);
  errno = EDOM;
#if LJ_TARGET_WINDOWS
  SetLastError((DWORD)STOPREQ_WINERR);
#endif
  pending_actions = lj_ccall_native_leave(L, cts, &native, func);
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.published);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);
  assert(ccallback_slot_acq(&tg->cb) == 0);
  assert(ljt_tg_has_stopreq(tg));

  pending_native = native;
  /* Native leave returned the STOP action without checking it, so the generic
  ** protocol deliberately retains a synthetic VM-dispatch poll. This fixture
  ** wants the nested probe below to perform the checked throw inside its pcall;
  ** cancel only that dispatch edge while retaining FRESH and pending_actions. */
  assert((pending_actions & LJ_GC2_HS_STOPREQ) != 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 1);
  lj_tg_poll_rel(tg, 0);
  lua_pushcfunction(L, checkstop_from_lua);
  lua_setglobal(L, "ccall_native_checkstop_probe");
  lua_pushcfunction(L, error_state_from_lua);
  lua_setglobal(L, "ccall_error_state_probe");
  lua_pushinteger(L, EDOM);
  lua_setglobal(L, "ccall_expected_errno");
  lua_pushinteger(L,
	(lua_Integer)(LJ_TARGET_WINDOWS ? STOPREQ_WINERR : 0));
  lua_setglobal(L, "ccall_expected_winerr");
#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
  alloc.oldf = lua_getallocf(L, &alloc.oldud);
  alloc.clobbers = 0;
  lua_setallocf(L, clobber_error_alloc, &alloc);
#endif
  ljt_lua_pcall(L, 0, 0, "fresh STOPREQ pcall probe");
#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
  lua_setallocf(L, alloc.oldf, alloc.oldud);
  assert(alloc.clobbers != 0);
#endif
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

static void run_scalar_native_protocol(lua_State *L, CTState *cts,
			       TGState *tg)
{
  CCallNativeState native;
  uint32_t actions;
  void *func;
  double r;

  ljt_tg_clear_stopreq(tg);
  func = (void *)(uintptr_t)&dummy_num_i32_i32;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  r = ((double (*)(int32_t, int32_t))(uintptr_t)func)(3, 4);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  assert(r == 10.5);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);

  func = (void *)(uintptr_t)&dummy_num_u32_u32;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  r = ((double (*)(uint32_t, uint32_t))(uintptr_t)func)
    (UINT32_C(0xfffffff0), 11);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  assert(r == 240 + 11 + 0.875);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);

  func = (void *)(uintptr_t)&dummy_num_i64_u32;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  r = ((double (*)(int64_t, uint32_t))(uintptr_t)func)
    ((int64_t)-15, UINT32_C(0xfffffff2));
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  assert(r == 241 + 242 + 1.125);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);
}

static void run_errno_redispatch_helper(lua_State *L, CTState *cts,
					global_State *g, TGState *tg)
{
  CCallNativeState native;
  NativeHelperRedispatchCtx ctx;
  pthread_t thread;
  uint32_t actions;
  void *func;
  int32_t result;

  ljt_tg_clear_stopreq(tg);
  ctx.g = g;
  ctx.tg = tg;
  ctx.published = 0;
  ctx.saw_native = 0;
  errno_redispatch_ctx = &ctx;
  assert(pthread_create(&thread, NULL, publish_redispatch_while_native,
			&ctx) == 0);
  errno = EAGAIN;
  func = (void *)(uintptr_t)&dummy_errno_after_redispatch;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  result = ((int32_t (*)(void))(uintptr_t)func)();
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  assert(pthread_join(thread, NULL) == 0);
  errno_redispatch_ctx = NULL;

  assert(result == 73);
  assert(errno == EDOM);
  assert(la_load32_acq(&ctx.saw_native) != 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
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
  run_scalar_native_protocol(L, cts, tg);
  run_errno_redispatch_helper(L, cts, g, tg);

  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) == 0);

  lua_close(L);
  printf("t-ffi-ccall-native-helpers OK: compact native protocol verified\n");
  return 0;
}
