/*
** Focused public threading.spawn native STOPREQ handling fixture.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lib/test_sleep.h"
#include "lib/lua_fixture_helpers.h"
#include "lib/tg_stopreq_fixture_helpers.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

typedef struct SpawnStopReqCtx {
  global_State *g;
  TGState *tg;
  uint32_t saw_native;
  uint32_t published;
  int err;
} SpawnStopReqCtx;

static TGState *pause_tg;
static uint32_t pause_spawn_create;
static uint32_t pause_seen;
static uint32_t pause_release;
static uint32_t child_ran;


extern int __real_pthread_create(pthread_t *thread,
				 const pthread_attr_t *attr,
				 void *(*start_routine)(void *), void *arg);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
			  void *(*start_routine)(void *), void *arg)
{
  uint32_t expect = 1;
  if (pause_tg != NULL && lj_tg_in_native_acq(pause_tg) &&
      la_cas32(&pause_spawn_create, &expect, 2, LA_ACQ_REL, LA_ACQ)) {
    la_store32_rel(&pause_seen, 1);
    while (la_load32_acq(&pause_release) == 0)
      sleep_ns(100000L);
    la_store32_rel(&pause_seen, 0);
  }
  return __real_pthread_create(thread, attr, start_routine, arg);
}

static void *publish_stopreq_while_paused(void *arg)
{
  SpawnStopReqCtx *ctx = (SpawnStopReqCtx *)arg;
  int i;
  for (i = 0; i < 1000; i++) {
    if (la_load32_acq(&pause_seen) != 0 && lj_tg_in_native_acq(ctx->tg)) {
      la_store32_rel(&ctx->saw_native, 1);
      break;
    }
    sleep_ns(100000L);
  }
  if (la_load32_acq(&ctx->saw_native) == 0) {
    ctx->err = 1;
    la_store32_rel(&pause_release, 1);
    return NULL;
  }
  if (lj_safepoint_handshake(ctx->g, LJ_GC2_HS_STOPREQ) == 0)
    ctx->err = 2;
  la_store32_rel(&ctx->published, 1);
  la_store32_rel(&pause_release, 1);
  return NULL;
}

static int child_marker(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&child_ran, 1);
  lua_pushliteral(L, "ran");
  return 1;
}

static void require_threading(lua_State *L)
{
  lua_getglobal(L, "require");
  lua_pushliteral(L, "threading");
  assert(lua_pcall(L, 1, 1, 0) == LUA_OK);
  assert(lua_istable(L, -1));
}

static uint32_t start_active_gc_cycle(lua_State *L)
{
  global_State *g = G(L);
  int i;
  assert(luaL_dostring(L, "jit.off()") == LUA_OK);
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(luaL_dostring(L,
    "local hold = {}\n"
    "for i=1,2000 do hold[i] = { i, tostring(i), i % 17 } end\n"
    "_G.__spawn_gc_hold = hold\n") == LUA_OK);
  lua_gc(L, LUA_GCRESTART, 0);
  g->gc.stepmul = 1;
  g->gc.threshold = 0;
  for (i = 0; i < 1000 && gc2_phase_acq(g) == LJ_GC2_IDLE; i++)
    lj_gc_step(L);
  assert(g->gc.state == GCSpause);  /* Legacy state is compatibility-only. */
  assert(gc2_phase_acq(g) != LJ_GC2_IDLE);
  g->gc.threshold = LJ_MAX_MEM;
  return gc2_cycle_acq(g);
}

static void test_sticky_stopreq_spawn_ok(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  TGState *tg = L2TG(L);
  static const char script[] =
    "local threading = require('threading')\n"
    "local th = threading.spawn(function() return 41 end)\n"
    "local ok, v = th:join()\n"
    "assert(ok == true and v == 41)\n";

  assert(tg != NULL);
  ljt_tg_set_stopreq(tg);
  assert(luaL_loadbuffer(L, script, sizeof(script) - 1, "sticky-spawn") ==
	 LUA_OK);
  assert(lua_pcall(L, 0, 0, 0) == LUA_OK);
  assert(ljt_tg_has_stopreq(tg));
  ljt_tg_clear_stopreq(tg);
  lua_close(L);
}

static void test_fresh_stopreq_aborts_before_child_runs(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  SpawnStopReqCtx ctx;
  pthread_t thread;
  int err, rc;

  assert(tg != NULL);
  memset(&ctx, 0, sizeof(ctx));
  ctx.g = g;
  ctx.tg = tg;
  pause_tg = tg;
  la_store32_rel(&pause_spawn_create, 0);
  la_store32_rel(&pause_seen, 0);
  la_store32_rel(&pause_release, 0);
  la_store32_rel(&child_ran, 0);

  require_threading(L);
  lua_getfield(L, -1, "spawn");
  lua_pushcfunction(L, child_marker);

  err = pthread_create(&thread, NULL, publish_stopreq_while_paused, &ctx);
  assert(err == 0);
  la_store32_rel(&pause_spawn_create, 1);
  rc = lua_pcall(L, 1, 1, 0);
  err = pthread_join(thread, NULL);
  assert(err == 0);

  assert(ctx.err == 0);
  assert(la_load32_acq(&ctx.saw_native) == 1);
  assert(la_load32_acq(&ctx.published) == 1);
  assert(rc != LUA_OK);
  assert(lua_tostring(L, -1) != NULL);
  assert(strstr(lua_tostring(L, -1), "thread interrupted: VM shutdown") !=
	 NULL);
  assert(la_load32_acq(&child_ran) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(ljt_tg_has_stopreq(tg));
  assert(mt_live_acq(g) == 0);
  assert(la_load32_acq(&pause_seen) == 0);
  ljt_tg_clear_stopreq(tg);
  pause_tg = NULL;
  lua_close(L);
}

static void test_spawn_preserves_active_gc_cycle(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  uint32_t cycle;
  static const char script[] =
    "local threading = require('threading')\n"
    "local th = threading.spawn(function() return true end)\n"
    "local ok, v = th:join(5)\n"
    "assert(ok == true and v == true)\n";

  require_threading(L);
  lua_pop(L, 1);
  cycle = start_active_gc_cycle(L);
  assert(luaL_loadbuffer(L, script, sizeof(script) - 1, "spawn-active-gc") ==
	 LUA_OK);
  assert(lua_pcall(L, 0, 0, 0) == LUA_OK);
  /* Background GC2 may advance or complete this cycle while the child runs,
  ** but spawn must not replace it with a new collection cycle. */
  assert(gc2_cycle_acq(g) == cycle);

  g->gc.stepmul = 200;
  g->gc.threshold = 0;
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.state == GCSpause);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_close(L);
}

int main(void)
{
  test_sticky_stopreq_spawn_ok();
  test_fresh_stopreq_aborts_before_child_runs();
  test_spawn_preserves_active_gc_cycle();
  printf("t-threading-spawn-native OK: spawn native STOPREQ verified\n");
  return 0;
}
