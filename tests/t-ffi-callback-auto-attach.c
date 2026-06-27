/*
** Focused guard for TLS-less foreign-thread FFI callback auto-attach.
*/

#include <assert.h>
#include <pthread.h>
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

typedef int (*AutoCallback)(int, int, int, int, int,
			    int, int, int, int, int);
typedef double (*AutoFpCallback)(double);

typedef struct AutoCtx {
  int status;
  int result;
  double fp_result;
  TGState *after_tg;
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

int main(void)
{
  lua_State *L = luaL_newstate();
  AutoCtx ctx;
  pthread_t pt;
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
    "typedef int (*lj_m7_auto_cb_t)(int, int, int, int, int,\n"
    "                               int, int, int, int, int);\n"
    "typedef void (*lj_m7_capture_auto_cb_t)(lj_m7_auto_cb_t);\n"
    "typedef double (*lj_m7_auto_fp_cb_t)(double);\n"
    "typedef void (*lj_m7_capture_auto_fp_cb_t)(lj_m7_auto_fp_cb_t);\n"
    "]]\n"
    "local cb = ffi.cast('lj_m7_auto_cb_t',\n"
    "  function(a, b, c, d, e, f, g, h, i, j)\n"
    "    if not lj_m7_check_auto_context() then return -7007 end\n"
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

  ctx.status = 99;
  ctx.result = 0;
  ctx.fp_result = -1.0;
  ctx.after_tg = (TGState *)1;
  assert(pthread_create(&pt, NULL, foreign_worker, &ctx) == 0);
  assert(pthread_join(pt, NULL) == 0);
  assert(ctx.status == 0);
  assert(ctx.result == 55);
  assert(ctx.after_tg == NULL);
  assert(callbackL == saved_owner);
  assert(saved_owner->tg_hint == NULL);
  assert(context_checks == 1);
  assert(slot_owner() == saved_owner);

  ljt_lua_dostring(L,
    "m7_auto_keep_cb:free()\n"
    "m7_auto_keep_cb = nil\n"
    "m7_auto_keep_fp_cb:free()\n"
    "m7_auto_keep_fp_cb = nil\n");
  assert(slot_owner() == NULL);
  assert(slot_owner_at(saved_fp_slot) == NULL);

  ctx.status = 99;
  ctx.result = -1;
  ctx.fp_result = -1.0;
  ctx.after_tg = (TGState *)1;
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
  printf("t-ffi-callback-auto-attach OK: TLS-less pthread callback auto-attach and stale return verified\n");
  return 0;
}
