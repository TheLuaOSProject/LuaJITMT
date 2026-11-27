/* A shutdown poll at a protected call return must preserve the caller's stack. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"

static uint32_t entered, finished;

static int returning_finalizer(lua_State *L)
{
  TGState *tg = L2TG(L);
  lua_pushinteger(L, 31);
  lua_pushinteger(L, 47);
  assert(la_add32_acqrel(&entered, 1) == 0);
  /* Stay inside this C call until lua_close has published its real request.
  ** The callback return is then the first VM poll that can deliver it. */
  while (!(lj_tg_reqmask_acq(tg) & LJ_GC2_HS_STOPREQ) &&
         !lj_tg_flags_test_acq(tg, TGF_STOPREQ))
    la_cpu_pause();
  return 2;
}

static int wait_for_callback(lua_State *L)
{
  LJNativeFrame frame;
  lj_native_enter_l(L, &frame);
  while (!la_load32_acq(&entered))
    la_cpu_pause();
  (void)lj_native_leave_l(L, &frame);
  return 0;
}

static int observe_shutdown(lua_State *L)
{
  assert(lua_gettop(L) == 3 && !lua_toboolean(L, 1));
  assert(lua_isstring(L, 2));
  assert(strstr(lua_tostring(L, 2), "thread interrupted: VM shutdown"));
  assert(strcmp(lua_tostring(L, 3), "caller guard") == 0);
  assert(la_add32_acqrel(&finished, 1) == 0);
  return 0;
}

static void run(int pool)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  la_store32_rlx(&entered, 0);
  la_store32_rlx(&finished, 0);
  lua_pushinteger(L, pool);
  lua_setglobal(L, "pool");
  lua_register(L, "returning_finalizer", returning_finalizer);
  lua_register(L, "observe_shutdown", observe_shutdown);
  lua_register(L, "wait_for_callback", wait_for_callback);
  ljt_lua_dostring(L,
    "local th = require('threading')\n"
    "th.gcworkers(pool)\n"
    "th.spawn(function()\n"
    "  local guard = 'caller guard'\n"
    "  local ok, err = pcall(function()\n"
    "    do local u = newproxy(true); getmetatable(u).__gc = returning_finalizer end\n"
    "    while true do collectgarbage() end\n"
    "  end)\n"
    "  observe_shutdown(ok, err, guard)\n"
    "end)\n"
    "wait_for_callback()\n");
  lua_close(L);
  assert(la_load32_acq(&entered) == 1 && la_load32_acq(&finished) == 1);
}

int main(void)
{
  alarm(45);
  run(0);
  run(2);
  puts("shutdown during a finalizer return preserves the interrupted worker frame");
  return 0;
}
