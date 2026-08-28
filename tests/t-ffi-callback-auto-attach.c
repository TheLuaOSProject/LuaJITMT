/*
** Focused regression test for TLS-less foreign-thread FFI callback auto-attach.
*/

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_atomic.h"
#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_ccallback.h"
#include "lj_tg.h"
#include "lj_thr.h"

#include "lib/lua_fixture_helpers.h"

typedef uint64_t (*AutoCallback)(int, int, int, int, int,
				 int, int, int, int, int);
typedef double (*AutoFpCallback)(double);

typedef struct AutoCtx {
  int status;
  uint64_t result;
  double fp_result;
  TGState *after_tg;
  int signal_call;
  int errnum;
} AutoCtx;

static lua_State *mainL;
static CTState *saved_cts;
static AutoCallback saved_cb;
static AutoFpCallback saved_fp_cb;
static MSize saved_slot;
static MSize saved_fp_slot;
static lua_State *saved_owner;
static lua_State *saved_fp_owner;
static lua_State *callbackL;
static int context_checks;
static uint32_t concurrent_hold;
static uint32_t concurrent_release;
static uint32_t concurrent_entered;
static uint32_t concurrent_attempts;
#ifdef LJ_CCALLBACK_TEST_HELPERS
static uint32_t result_detach_entered;
static uint32_t result_detach_release;
#endif

static void assert_dead_tgs_drop_callback_roots(void)
{
  global_State *g = G(mainL);
  TGState *tg;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (lj_tg_flags_test_acq(tg, TGF_DEAD)) {
      assert(ccallback_depth_acq(&tg->cb) == 0);
      assert(ccallback_L_acq(&tg->cb) == NULL);
      assert(ccallback_slot_acq(&tg->cb) == 0);
      assert(ccallback_auto_detach_acq(&tg->cb) == 0);
    }
  }
}

static void init_ctx(AutoCtx *ctx)
{
  ctx->status = 99;
  ctx->result = 0;
  ctx->fp_result = -1.0;
  ctx->after_tg = (TGState *)1;
  ctx->signal_call = 0;
  ctx->errnum = -1;
}

static void wait_u32_at_least(uint32_t *p, uint32_t value)
{
  int i;
  for (i = 0; i < 5000; i++) {
    if (la_load32_acq(p) >= value)
      return;
    (void)lj_thr_sleep_ns(NULL, 1000000);
  }
  assert(la_load32_acq(p) >= value);
}

static lua_State *slot_owner_at(MSize slot)
{
  lua_State **owner = (lua_State **)la_loadptr_acq(
    (void *const *)&saved_cts->cb.owner);
  assert(owner != NULL);
  return (lua_State *)la_loadptr_acq((void *const *)&owner[slot]);
}

static lua_State *slot_owner(void)
{
  return slot_owner_at(saved_slot);
}

static void capture_cb(AutoCallback cb)
{
  CTypeID1 *cbid;
  saved_cts = ctype_cts(mainL);
  saved_cb = cb;
  saved_slot = lj_ccallback_ptr2slot(saved_cts, (void *)cb);
  assert(saved_slot != ~0u);
  assert(saved_slot < la_load32_acq(&saved_cts->cb.sizeid));
  cbid = (CTypeID1 *)la_loadptr_acq((void *const *)&saved_cts->cb.cbid);
  assert(cbid != NULL);
  assert(la_load16_acq(&cbid[saved_slot]) != 0);
  saved_owner = slot_owner();
  assert(saved_owner != NULL);
  assert(saved_owner != mainL);
  assert(saved_owner->tg_hint == NULL);
}

static void capture_fp_cb(AutoFpCallback cb)
{
  CTypeID1 *cbid;
  saved_cts = ctype_cts(mainL);
  saved_fp_cb = cb;
  saved_fp_slot = lj_ccallback_ptr2slot(saved_cts, (void *)cb);
  assert(saved_fp_slot != ~0u);
  assert(saved_fp_slot < la_load32_acq(&saved_cts->cb.sizeid));
  cbid = (CTypeID1 *)la_loadptr_acq((void *const *)&saved_cts->cb.cbid);
  assert(cbid != NULL);
  assert(la_load16_acq(&cbid[saved_fp_slot]) != 0);
  saved_fp_owner = slot_owner_at(saved_fp_slot);
  assert(saved_fp_owner != NULL);
  assert(saved_fp_owner != mainL);
  assert(saved_fp_owner->tg_hint == NULL);
}

static int check_callback_context(lua_State *L)
{
  TGState *tg = lj_thr_get_tg();
  int ok = tg != NULL && L == saved_owner && L != mainL &&
	   L2TG(L) == tg && tg->cur_L == L && tg->thread_L == L &&
	   tg->gl == G(mainL);
  callbackL = L;
  context_checks++;
  if (la_load32_acq(&concurrent_hold) != 0) {
    (void)la_add32_rlx(&concurrent_entered, 1);
    while (la_load32_acq(&concurrent_release) == 0)
      (void)lj_thr_sleep_ns(L, 1000000);
  }
  lua_pushboolean(L, ok);
  return 1;
}

static void *foreign_worker(void *arg)
{
  AutoCtx *ctx = (AutoCtx *)arg;
  if (lj_thr_get_tg() != NULL) {
    ctx->status = 1;
    return NULL;
  }
  if (ctx->signal_call)
    (void)la_add32_rlx(&concurrent_attempts, 1);
  ctx->result = saved_cb(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  ctx->after_tg = lj_thr_get_tg();
  ctx->status = 0;
  return NULL;
}

static void *stale_foreign_worker(void *arg)
{
  AutoCtx *ctx = (AutoCtx *)arg;
  if (lj_thr_get_tg() != NULL) {
    ctx->status = 1;
    return NULL;
  }
  ctx->result = saved_cb(10, 9, 8, 7, 6, 5, 4, 3, 2, 1);
  ctx->fp_result = saved_fp_cb(12.5);
  ctx->after_tg = lj_thr_get_tg();
  ctx->status = 0;
  return NULL;
}

#ifdef LJ_CCALLBACK_TEST_HELPERS
static void *foreign_result_worker(void *arg)
{
  AutoCtx *ctx = (AutoCtx *)arg;
  if (lj_thr_get_tg() != NULL) {
    ctx->status = 1;
    return NULL;
  }
  errno = E2BIG;
  ctx->result = saved_cb(-1, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  ctx->errnum = errno;
  ctx->after_tg = lj_thr_get_tg();
  ctx->status = 0;
  return NULL;
}

static void *foreign_fp_worker(void *arg)
{
  AutoCtx *ctx = (AutoCtx *)arg;
  if (lj_thr_get_tg() != NULL) {
    ctx->status = 1;
    return NULL;
  }
  errno = EILSEQ;
  ctx->fp_result = saved_fp_cb(12.5);
  ctx->errnum = errno;
  ctx->after_tg = lj_thr_get_tg();
  ctx->status = 0;
  return NULL;
}

static void result_after_detach_hold(void)
{
  errno = ERANGE;  /* Finish must restore the callback's exact error pair. */
  la_store32_rel(&result_detach_entered, 1);
  while (la_load32_acq(&result_detach_release) == 0)
    (void)lj_thr_sleep_ns(NULL, 1000000);
}

static void drain_dead_tgs(global_State *g)
{
  while (lj_tg_reclaim_dead(g) != 0)
    ;
}

static void arm_result_after_detach_hold(void)
{
  la_store32_rel(&result_detach_entered, 0);
  la_store32_rel(&result_detach_release, 0);
  lj_ccallback_test_set_after_detach_hook(result_after_detach_hold);
}

static void release_result_after_detach_hold(void)
{
  la_store32_rel(&result_detach_release, 1);
}

static void test_result_reload_after_reclaim(global_State *g)
{
  AutoCtx intctx, fpctx;
  pthread_t intpt, fppt;

  drain_dead_tgs(g);
  assert(mt_live_acq(g) == 0);
  init_ctx(&intctx);
  arm_result_after_detach_hold();
  assert(pthread_create(&intpt, NULL, foreign_result_worker, &intctx) == 0);
  wait_u32_at_least(&result_detach_entered, 1);
  /* Auto-detach is complete, but the assembly continuation has not yet
  ** reloaded the integer result. Reclaim the exact TG in that old UAF window. */
  assert(mt_live_acq(g) == 0);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(lj_tg_reclaim_dead(g) == 0u);
  release_result_after_detach_hold();
  assert(pthread_join(intpt, NULL) == 0);
  assert(intctx.status == 0);
  assert(intctx.result == UINT64_C(0xfedcba9876543210));
  assert(intctx.errnum == E2BIG);
  assert(intctx.after_tg == NULL);

  drain_dead_tgs(g);
  assert(mt_live_acq(g) == 0);
  init_ctx(&fpctx);
  arm_result_after_detach_hold();
  assert(pthread_create(&fppt, NULL, foreign_fp_worker, &fpctx) == 0);
  wait_u32_at_least(&result_detach_entered, 1);
  /* Repeat with the independent floating-point carrier. */
  assert(mt_live_acq(g) == 0);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(lj_tg_reclaim_dead(g) == 0u);
  release_result_after_detach_hold();
  assert(pthread_join(fppt, NULL) == 0);
  assert(fpctx.status == 0);
  assert(fpctx.fp_result == 13.0);
  assert(fpctx.errnum == EILSEQ);
  assert(fpctx.after_tg == NULL);
  assert(saved_owner->tg_hint == NULL);
  assert(saved_fp_owner->tg_hint == NULL);
}
#endif

int main(void)
{
  lua_State *L = luaL_newstate();
  AutoCtx ctx;
  AutoCtx cctx[2];
  pthread_t pt;
  pthread_t cpt[2];
  assert(L != NULL);
  luaL_openlibs(L);
  mainL = L;

  lua_pushcfunction(L, check_callback_context);
  lua_setglobal(L, "lj_m7_check_auto_context");
  lua_pushlightuserdata(L, (void *)capture_cb);
  lua_setglobal(L, "lj_m7_capture_auto_cb");
  lua_pushlightuserdata(L, (void *)capture_fp_cb);
  lua_setglobal(L, "lj_m7_capture_auto_fp_cb");

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef uint64_t (*lj_m7_auto_cb_t)(int, int, int, int, int,\n"
    "                               int, int, int, int, int);\n"
    "typedef void (*lj_m7_capture_auto_cb_t)(lj_m7_auto_cb_t);\n"
    "typedef double (*lj_m7_auto_fp_cb_t)(double);\n"
    "typedef void (*lj_m7_capture_auto_fp_cb_t)(lj_m7_auto_fp_cb_t);\n"
    "]]\n"
    "local cb = ffi.cast('lj_m7_auto_cb_t',\n"
    "  function(a, b, c, d, e, f, g, h, i, j)\n"
    "    if not lj_m7_check_auto_context() then return -7007 end\n"
    "    if a == -1 then return 0xfedcba9876543210ULL end\n"
    "    return a + b + c + d + e + f + g + h + i + j\n"
    "  end)\n"
    "local capture = ffi.cast('lj_m7_capture_auto_cb_t',\n"
    "                         lj_m7_capture_auto_cb)\n"
    "capture(cb)\n"
    "local fp_cb = ffi.cast('lj_m7_auto_fp_cb_t', function(x)\n"
    "  return x + 0.5\n"
    "end)\n"
    "local capture_fp = ffi.cast('lj_m7_capture_auto_fp_cb_t',\n"
    "                            lj_m7_capture_auto_fp_cb)\n"
    "capture_fp(fp_cb)\n"
    "m7_auto_keep_cb = cb\n"
    "m7_auto_keep_fp_cb = fp_cb\n");
  assert(saved_cb != NULL);
  assert(saved_fp_cb != NULL);
  assert(saved_owner != NULL);
  assert(saved_fp_owner != NULL);
  ljt_lua_dostring(L, "collectgarbage('collect')\ncollectgarbage('collect')\n");
  assert(slot_owner() == saved_owner);
  assert(slot_owner_at(saved_fp_slot) == saved_fp_owner);
  assert(saved_owner->tg_hint == NULL);
  assert(saved_fp_owner->tg_hint == NULL);

  init_ctx(&ctx);
  assert(pthread_create(&pt, NULL, foreign_worker, &ctx) == 0);
  assert(pthread_join(pt, NULL) == 0);
  assert(ctx.status == 0);
  assert(ctx.result == 55);
  assert(ctx.after_tg == NULL);
  assert_dead_tgs_drop_callback_roots();
  assert(callbackL == saved_owner);
  assert(saved_owner->tg_hint == NULL);
  assert(context_checks == 1);
  assert(slot_owner() == saved_owner);

#ifdef LJ_CCALLBACK_TEST_HELPERS
  test_result_reload_after_reclaim(G(L));
  assert(slot_owner() == saved_owner);
  assert(slot_owner_at(saved_fp_slot) == saved_fp_owner);
#endif

  la_store32_rel(&concurrent_hold, 1);
  la_store32_rel(&concurrent_release, 0);
  la_store32_rel(&concurrent_entered, 0);
  la_store32_rel(&concurrent_attempts, 0);
  init_ctx(&cctx[0]);
  init_ctx(&cctx[1]);
  cctx[0].signal_call = 1;
  cctx[1].signal_call = 1;
  assert(pthread_create(&cpt[0], NULL, foreign_worker, &cctx[0]) == 0);
  wait_u32_at_least(&concurrent_entered, 1);
  assert(pthread_create(&cpt[1], NULL, foreign_worker, &cctx[1]) == 0);
  wait_u32_at_least(&concurrent_attempts, 2);
  (void)lj_thr_sleep_ns(NULL, 10000000);
  la_store32_rel(&concurrent_release, 1);
  assert(pthread_join(cpt[0], NULL) == 0);
  assert(pthread_join(cpt[1], NULL) == 0);
  la_store32_rel(&concurrent_hold, 0);
  assert(cctx[0].status == 0);
  assert(cctx[1].status == 0);
  assert(cctx[0].result == 55);
  assert(cctx[1].result == 55);
  assert(cctx[0].after_tg == NULL);
  assert(cctx[1].after_tg == NULL);
  assert_dead_tgs_drop_callback_roots();
  assert(callbackL == saved_owner);
  assert(saved_owner->tg_hint == NULL);
#ifdef LJ_CCALLBACK_TEST_HELPERS
  assert(context_checks == 4);
#else
  assert(context_checks == 3);
#endif
  assert(slot_owner() == saved_owner);

  ljt_lua_dostring(L,
    "m7_auto_keep_cb:free()\n"
    "m7_auto_keep_cb = nil\n"
    "m7_auto_keep_fp_cb:free()\n"
    "m7_auto_keep_fp_cb = nil\n");
  assert(slot_owner() == NULL);
  assert(slot_owner_at(saved_fp_slot) == NULL);

  init_ctx(&ctx);
  ctx.result = -1;
  assert(pthread_create(&pt, NULL, stale_foreign_worker, &ctx) == 0);
  assert(pthread_join(pt, NULL) == 0);
  assert(ctx.status == 0);
  assert(ctx.result == 0);
  assert(ctx.fp_result == 0.0);
  assert(ctx.after_tg == NULL);
  assert(slot_owner() == NULL);
  assert(slot_owner_at(saved_fp_slot) == NULL);

  ljt_lua_dostring(L, "collectgarbage('collect')\n");
  lua_close(L);
#ifdef LJ_CCALLBACK_TEST_HELPERS
  printf("t-ffi-callback-auto-attach OK: 64-bit integer/FP results and errno survived post-detach TG reclaim\n");
#else
  printf("t-ffi-callback-auto-attach OK: TLS-less pthread callback auto-attach and stale return verified\n");
#endif
  return 0;
}
