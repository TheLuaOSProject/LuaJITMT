/*
** Focused public threading.spawn native STOPREQ handling fixture.
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
#include "lj_atomic.h"
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

static void sleep_ns(long ns)
{
  struct timespec ts;
  ts.tv_sec = ns / 1000000000L;
  ts.tv_nsec = ns % 1000000000L;
  while (nanosleep(&ts, &ts) != 0) {
  }
}

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

static lua_State *new_open_state(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  return L;
}

static void clear_stopreq(TGState *tg)
{
  assert(lj_tg_flags_test_acq(tg, TGF_STOPREQ));
  (void)lj_tg_flags_and_rlx(tg, (uint8_t)~TGF_STOPREQ);
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

static void test_sticky_stopreq_spawn_ok(void)
{
  lua_State *L = new_open_state();
  TGState *tg = L2TG(L);
  static const char script[] =
    "local threading = require('threading')\n"
    "local th = threading.spawn(function() return 41 end)\n"
    "local ok, v = th:join()\n"
    "assert(ok == true and v == 41)\n";

  assert(tg != NULL);
  (void)lj_tg_flags_or_rlx(tg, TGF_STOPREQ);
  assert(luaL_loadbuffer(L, script, sizeof(script) - 1, "sticky-spawn") ==
	 LUA_OK);
  assert(lua_pcall(L, 0, 0, 0) == LUA_OK);
  assert(lj_tg_flags_test_acq(tg, TGF_STOPREQ));
  clear_stopreq(tg);
  lua_close(L);
}

static void test_fresh_stopreq_aborts_before_child_runs(void)
{
  lua_State *L = new_open_state();
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
  assert(lj_tg_flags_test_acq(tg, TGF_STOPREQ));
  assert(mt_live_acq(g) == 0);
  assert(la_load32_acq(&pause_seen) == 0);
  clear_stopreq(tg);
  pause_tg = NULL;
  lua_close(L);
}

int main(void)
{
  test_sticky_stopreq_spawn_ok();
  test_fresh_stopreq_aborts_before_child_runs();
  printf("t-threading-spawn-native OK: spawn native STOPREQ verified\n");
  return 0;
}
