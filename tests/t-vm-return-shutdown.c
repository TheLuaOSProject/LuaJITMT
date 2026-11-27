/* A shutdown poll at a protected call return must preserve the caller's stack. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lib/lua_fixture_helpers.h"

static uint32_t entered, finished;

enum { USERDATA_FINALIZER, CDATA_FINALIZER, ERROR_EVENT };

static int returning_finalizer(lua_State *L)
{
  TGState *tg = L2TG(L);
  lua_pushinteger(L, 31);
  lua_pushinteger(L, 47);
  assert(la_add32_acqrel(&entered, 1) == 0);
  /* Publication of the counted request precedes its VM poll. Acknowledge
  ** without throwing and wait for the leader to finish clearing/rearming.
  ** The callback return is the first checked poll of the resulting edge. */
  while (!lj_tg_flags_test_acq(tg, TGF_STOPREQ_FRESH) ||
         gc2_hs_leader_acq(G(L)) || !lj_tg_poll_acq(tg)) {
    if (lj_tg_reqmask_acq(tg))
      (void)lj_safepoint_ack(L);
    else
      la_cpu_pause();
  }
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

static int finalizer_error_event(lua_State *L)
{
  assert(lua_gettop(L) == 1);
  assert(strcmp(lua_tostring(L, 1), "ordinary finalizer error") == 0);
  lua_pushcfunction(L, returning_finalizer);
  assert(lua_pcall(L, 0, LUA_MULTRET, 0) == LUA_ERRRUN);
  assert(lua_gettop(L) == 2);
  assert(strstr(lua_tostring(L, 2), "thread interrupted: VM shutdown"));
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

static void run(int pool, int mode)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  la_store32_rlx(&entered, 0);
  la_store32_rlx(&finished, 0);
  lua_pushinteger(L, pool);
  lua_setglobal(L, "pool");
  lua_pushboolean(L, mode == CDATA_FINALIZER);
  lua_setglobal(L, "use_cdata");
  lua_register(L, "returning_finalizer", returning_finalizer);
  if (mode == ERROR_EVENT) {
    lua_register(L, "finalizer_error_event", finalizer_error_event);
    ljt_lua_dostring(L, "jit.attach(finalizer_error_event, 'errfin')");
  }
  lua_register(L, "observe_shutdown", observe_shutdown);
  lua_register(L, "wait_for_callback", wait_for_callback);
  ljt_lua_dostring(L,
    "local th = require('threading')\n"
    "th.gcworkers(pool)\n"
    "th.spawn(function()\n"
    "  local guard = 'caller guard'\n"
    "  local ok, err = pcall(function()\n"
    "    if use_cdata then\n"
    "      local ffi = require('ffi')\n"
    "      ffi.gc(ffi.new('int[1]'), returning_finalizer)\n"
    "    else\n"
    "      local u = newproxy(true)\n"
    "      getmetatable(u).__gc = finalizer_error_event and\n"
    "        function() error('ordinary finalizer error', 0) end or returning_finalizer\n"
    "    end\n"
    "    while true do collectgarbage() end\n"
    "  end)\n"
    "  observe_shutdown(ok, err, guard)\n"
    "end)\n"
    "wait_for_callback()\n");
  lua_close(L);
  assert(la_load32_acq(&entered) == 1 && la_load32_acq(&finished) == 1);
}

#ifdef LJ_GC2_TEST_HELPERS
static uint32_t parked_tid, consumed, waiting, gc_deferred;

static void stop_wait_hook(global_State *g, TGState *tg, uint64_t epoch,
			   uint32_t point)
{
  UNUSED(epoch);
  if (lj_tg_tid_acq(tg) != la_load32_acq(&parked_tid))
    return;
  if (point == LJ_SAFEPOINT_COMPLETION_TEST_CONSUMED &&
      gc2_hs_actions_acq(g) == LJ_GC2_HS_STOPREQ) {
    assert(lj_thr_get_tg() != tg && !lj_tg_reqmask_acq(tg));
    la_store32_rel(&consumed, 1);
    while (!la_load32_acq(&waiting)) la_cpu_pause();
  } else if (point == LJ_SAFEPOINT_COMPLETION_TEST_BEFORE_WAIT &&
	     la_load32_acq(&consumed)) {
    assert(lj_thr_get_tg() == tg);
    la_store32_rel(&waiting, 1);
    /* Let the remote leader finish both stores before the real futex wait. */
    while (gc2_hs_leader_acq(g) || !lj_tg_poll_acq(tg) ||
	   !lj_tg_flags_test_acq(tg, TGF_STOPREQ_FRESH)) la_cpu_pause();
  }
}

static int leave_armed_wait(lua_State *L)
{
  TGState *tg = L2TG(L);
  uint32_t actions;
  uint64_t starts;
  la_store32_rel(&parked_tid, lj_tg_tid_acq(tg));
  lj_native_enter(tg);
  la_store32_rel(&entered, 1);
  while (!la_load32_acq(&consumed)) la_cpu_pause();
  actions = lj_native_leave(L);
  assert(actions == LJ_GC2_HS_STOPREQ);
  starts = gc2_major_cycle_starts_acq(G(L));
  assert(lj_gc2_collect_active(L) == 0);
  assert(lj_gc2_step_explicit(L, 1u << 20) == 0);
  assert(gc2_major_cycle_starts_acq(G(L)) == starts);
  assert(lj_tg_flags_test_acq(tg, TGF_STOPREQ_FRESH));
  la_store32_rel(&gc_deferred, 1);
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  lj_safepoint_checkstop(L, actions);
  abort();
}

static int observe_gc_shutdown(lua_State *L)
{
  assert(la_load32_acq(&G(L)->gc2.explicit_collectors) == 0);
  return observe_shutdown(L);
}

static void run_consumed_stop(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  la_store32_rlx(&entered, 0);
  la_store32_rlx(&finished, 0);
  lua_register(L, "leave_armed_wait", leave_armed_wait);
  lua_register(L, "wait_for_callback", wait_for_callback);
  lua_register(L, "observe_shutdown", observe_gc_shutdown);
  lj_safepoint_test_completion_hook(stop_wait_hook);
  ljt_lua_dostring(L,
    "require('threading').spawn(function()\n"
    "  local ok, err = pcall(leave_armed_wait)\n"
    "  observe_shutdown(ok, err, 'caller guard')\n"
    "end)\n"
    "wait_for_callback()\n");
  lua_close(L);
  lj_safepoint_test_completion_hook(NULL);
  assert(la_load32_acq(&waiting) && la_load32_acq(&gc_deferred) &&
         la_load32_acq(&finished) == 1);
}
#endif

int main(void)
{
  int pool;
  alarm(45);
  for (pool = 0; pool <= 2; pool += 2) {
    run(pool, USERDATA_FINALIZER);
#if LJ_HASFFI
    run(pool, CDATA_FINALIZER);
#endif
#if LJ_HASJIT && !defined(LUAJIT_DISABLE_VMEVENT)
    run(pool, ERROR_EVENT);
#endif
  }
#ifdef LJ_GC2_TEST_HELPERS
  run_consumed_stop();
#endif
  puts("shutdown during a finalizer return preserves the interrupted worker frame");
  return 0;
}
