/*
** Focused guard for attached-thread FFI callback carrier selection.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_tg.h"
#include "lj_thr.h"

#include "lib/lua_fixture_helpers.h"

typedef int (*CarrierCallback)(int, int, int, int, int,
			       int, int, int, int, int);

typedef struct CarrierCtx {
  lua_State *L;
  int status;
  int result;
  MSize depth;
  uint32_t tid;
} CarrierCtx;

static lua_State *mainL;
static lua_State *workerL;
static CarrierCallback saved_cb;
static int context_checks;

static void capture_cb(CarrierCallback cb)
{
  saved_cb = cb;
}

static int check_callback_context(lua_State *L)
{
  TGState *tg = lj_thr_get_tg();
  int ok = tg != NULL && L == workerL && L != mainL &&
	   L2TG(L) == tg && tg->cur_L == L && tg->thread_L == L &&
	   tg->gl == G(L);
  context_checks++;
  lua_pushboolean(L, ok);
  return 1;
}

static void *attached_worker(void *arg)
{
  CarrierCtx *ctx = (CarrierCtx *)arg;
  TGState *tg;
  if (!lj_threading_attach(ctx->L)) {
    ctx->status = 1;
    return NULL;
  }
  tg = lj_thr_get_tg();
  ctx->tid = tg ? tg->tid : 0;
  ctx->result = saved_cb(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  ctx->depth = tg ? ccallback_depth_acq(&tg->cb) : ~(MSize)0;
  lj_threading_detach(ctx->L, 1);
  ctx->status = 0;
  return NULL;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  CarrierCtx ctx;
  pthread_t pt;
  assert(L != NULL);
  luaL_openlibs(L);
  mainL = L;

  lua_pushcfunction(L, check_callback_context);
  lua_setglobal(L, "lj_m7_check_callback_context");
  lua_pushlightuserdata(L, (void *)capture_cb);
  lua_setglobal(L, "lj_m7_capture_attached_cb");

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef int (*lj_m7_carrier_cb_t)(int, int, int, int, int,\n"
    "                                   int, int, int, int, int);\n"
    "typedef void (*lj_m7_capture_attached_cb_t)(lj_m7_carrier_cb_t);\n"
    "]]\n"
    "local cb = ffi.cast('lj_m7_carrier_cb_t',\n"
    "  function(a, b, c, d, e, f, g, h, i, j)\n"
    "    if not lj_m7_check_callback_context() then return -7007 end\n"
    "    return a + b + c + d + e + f + g + h + i + j\n"
    "  end)\n"
    "local capture = ffi.cast('lj_m7_capture_attached_cb_t',\n"
    "                         lj_m7_capture_attached_cb)\n"
    "capture(cb)\n"
    "m7_attached_keep_cb = cb\n");
  assert(saved_cb != NULL);

  workerL = lua_newthread(L);
  assert(workerL != NULL);
  ctx.L = workerL;
  ctx.status = 99;
  ctx.result = 0;
  ctx.depth = ~(MSize)0;
  ctx.tid = 0;
  assert(pthread_create(&pt, NULL, attached_worker, &ctx) == 0);
  assert(pthread_join(pt, NULL) == 0);
  assert(ctx.status == 0);
  assert(ctx.tid != 0);
  assert(ctx.result == 55);
  assert(ctx.depth == 0);
  assert(context_checks == 1);

  ljt_lua_dostring(L,
    "m7_attached_keep_cb:free()\n"
    "m7_attached_keep_cb = nil\n"
    "collectgarbage('collect')\n");
  lua_pop(L, 1);
  lua_close(L);
  printf("t-ffi-callback-attached-carrier OK: main callback runs on attached carrier TG\n");
  return 0;
}
