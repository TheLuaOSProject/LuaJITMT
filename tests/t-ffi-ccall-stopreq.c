/*
** Focused regression test for interpreted FFI C-call native STOPREQ freshness.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

typedef struct CCallStopReqCtx {
  global_State *g;
  TGState *tg;
  uint32_t published;
  uint32_t saw_native;
} CCallStopReqCtx;

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
  CCallStopReqCtx *ctx = (CCallStopReqCtx *)arg;
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

static void run_sticky_stopreq_ok(lua_State *L, TGState *tg)
{
  set_stopreq(tg);
  run_lua_ok(L,
    "local ffi = require('ffi')\n"
    "local ok, pid = pcall(ffi.C.getpid)\n"
    "assert(ok == true and type(pid) == 'number' and pid > 0)\n");
  assert(lj_tg_flags_test_acq(tg, TGF_STOPREQ));
  clear_stopreq(tg);
}

static void run_fresh_stopreq_interrupt(lua_State *L, global_State *g,
					TGState *tg)
{
  CCallStopReqCtx ctx;
  pthread_t thread;
  int rc;

  ctx.g = g;
  ctx.tg = tg;
  ctx.published = 0;
  ctx.saw_native = 0;

  assert(pthread_create(&thread, NULL, publish_stopreq_while_native, &ctx) == 0);
  rc = luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "local ok, err = pcall(function()\n"
    "  return ffi.C.poll(nil, 0, 200)\n"
    "end)\n"
    "assert(ok == false, 'fresh STOPREQ did not interrupt FFI ccall')\n"
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
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);
  assert(lj_tg_flags_test_acq(tg, TGF_STOPREQ));
  clear_stopreq(tg);
}

static void run_traced_fresh_stopreq_interrupt(lua_State *L, global_State *g,
					       TGState *tg)
{
  CCallStopReqCtx ctx;
  pthread_t thread;
  int rc;

  if (getenv("LJ_M7_FFI_CCALL_JIT_SO") == NULL)
    return;

  run_lua_ok(L,
    "local ffi = require('ffi')\n"
    "local util = require('jit.util')\n"
    "local lib = assert(lj_m7_ccall_jit_lib)\n"
    "local sleep_i32 = lib.lj_m7_ccall_jit_sleep_i32\n"
    "function lj_m7_traced_sleep(n, ms)\n"
    "  local r = 0\n"
    "  for _ = 1, n do r = sleep_i32(ms) end\n"
    "  return r\n"
    "end\n"
    "jit.on()\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "assert(lj_m7_traced_sleep(80, 0) == 7)\n"
    "assert(util.traceinfo(1), 'traced FFI ccall sleep probe did not compile')\n");

  ctx.g = g;
  ctx.tg = tg;
  ctx.published = 0;
  ctx.saw_native = 0;

  assert(pthread_create(&thread, NULL, publish_stopreq_while_native, &ctx) == 0);
  rc = luaL_dostring(L,
    "local ok, err = pcall(function()\n"
    "  return lj_m7_traced_sleep(2, 200)\n"
    "end)\n"
    "assert(ok == false, 'fresh STOPREQ did not interrupt traced FFI ccall')\n"
    "assert(tostring(err):find('thread interrupted: VM shutdown', 1, true),\n"
    "       tostring(err))\n");
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.published);
  if (rc != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "traced fresh STOPREQ chunk failed: %s\n",
	    err ? err : "(nil)");
  }
  assert(rc == LUA_OK);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);
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
    "ffi.cdef[[\n"
    "int getpid(void);\n"
    "int poll(void *fds, unsigned long nfds, int timeout);\n"
    "int lj_m7_ccall_jit_sleep_i32(int);\n"
    "]]\n");
  if (getenv("LJ_M7_FFI_CCALL_JIT_SO") != NULL) {
    run_lua_ok(L,
      "local ffi = require('ffi')\n"
      "lj_m7_ccall_jit_lib = ffi.load(\n"
      "  assert(os.getenv('LJ_M7_FFI_CCALL_JIT_SO')))\n");
  }

  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);

  run_sticky_stopreq_ok(L, tg);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) == 0);

  run_fresh_stopreq_interrupt(L, g, tg);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) == 0);

  run_traced_fresh_stopreq_interrupt(L, g, tg);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) == 0);

  lua_close(L);
  printf("t-ffi-ccall-stopreq OK: FFI ccall STOPREQ freshness verified\n");
  return 0;
}
