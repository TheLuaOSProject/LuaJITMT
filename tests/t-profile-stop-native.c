/*
** jit.profile stop native-state STOPREQ guard.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"

#if LJ_HASPROFILE

#include <unistd.h>

#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_thr.h"
#include "lj_tg.h"

#define KEY_PROFILE_THREAD	(U64x(81000000,00000000)|'t')

static void clear_stopreq(TGState *tg)
{
  uint8_t flags = la_load8_acq(&tg->tg_flags);
  la_store8_rel(&tg->tg_flags, (uint8_t)(flags & ~TGF_STOPREQ));
}

static void set_stopreq(TGState *tg)
{
  (void)la_or8_rlx(&tg->tg_flags, TGF_STOPREQ);
}

static void run_ok(lua_State *L, const char *chunk)
{
  int rc = luaL_dostring(L, chunk);
  if (rc != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "unexpected Lua error: %s\n", err ? err : "(nil)");
  }
  assert(rc == LUA_OK);
}

static void expect_stopreq_error(lua_State *L, int rc)
{
  const char *err;
  assert(rc != LUA_OK);
  err = lua_tostring(L, -1);
  assert(err && strstr(err, "thread interrupted: VM shutdown"));
  lua_pop(L, 1);
}

static void run_deferred_cleanup_test(lua_State *L, TGState *tg)
{
  run_ok(L,
    "local profile = require('jit.profile')\n"
    "profile.start('i1000', function() end)\n");
  set_stopreq(tg);
  expect_stopreq_error(L, luaL_dostring(L,
    "local profile = require('jit.profile')\n"
    "profile.stop()\n"));
  clear_stopreq(tg);
  run_ok(L,
    "local profile = require('jit.profile')\n"
    "profile.start('i1', function() end)\n"
    "profile.stop()\n");
}

static void run_callback_error_test(lua_State *L)
{
  run_ok(L,
    "local profile = require('jit.profile')\n"
    "local clock = os.clock\n"
    "local seen = 0\n"
    "profile.start('i1', function()\n"
    "  seen = seen + 1\n"
    "  error('profile callback containment smoke')\n"
    "end)\n"
    "local deadline = clock() + 0.5\n"
    "local x = 0\n"
    "while seen == 0 and clock() < deadline do\n"
    "  for i = 1, 10000 do x = x + i end\n"
    "end\n"
    "assert(seen > 0, 'profile callback did not run')\n"
    "profile.stop()\n"
    "profile.start('i1', function() seen = seen + 1 end)\n"
    "profile.stop()\n");
}

#if LJ_PROFILE_PTHREAD

#include <pthread.h>

#include "lj_safepoint.h"

typedef struct ProfileStopCtx {
  global_State *g;
  TGState *tg;
  uint32_t saw_native;
  uint32_t handshook;
  uint32_t signaled;
  uint32_t stopreq_seen;
} ProfileStopCtx;

static void *stopreq_thread(void *arg)
{
  ProfileStopCtx *ctx = (ProfileStopCtx *)arg;
  int i;
  for (i = 0; i < 5000; i++) {
    if (la_load8_acq(&ctx->tg->in_native)) {
      la_store32_rel(&ctx->saw_native, 1);
      break;
    }
    usleep(1000);
  }
  if (!la_load32_acq(&ctx->saw_native))
    return NULL;

  la_store32_rel(&ctx->signaled,
		 lj_safepoint_handshake(ctx->g, LJ_GC2_HS_STOPREQ));
  la_store32_rel(&ctx->stopreq_seen,
		 (la_load8_acq(&ctx->tg->tg_flags) & TGF_STOPREQ) != 0);
  la_store32_rel(&ctx->handshook, 1);
  return NULL;
}

static void run_native_join_test(lua_State *L, global_State *g, TGState *tg)
{
  ProfileStopCtx ctx;
  pthread_t stopper;
  int rc;

  memset(&ctx, 0, sizeof(ctx));
  ctx.g = g;
  ctx.tg = tg;
  assert(pthread_create(&stopper, NULL, stopreq_thread, &ctx) == 0);

  alarm(20);
  rc = luaL_dostring(L,
    "local profile = require('jit.profile')\n"
    "profile.start('i1000', function() end)\n"
    "profile.stop()\n");
  assert(pthread_join(stopper, NULL) == 0);
  alarm(0);

  assert(la_load32_acq(&ctx.saw_native) != 0);
  assert(la_load32_acq(&ctx.handshook) != 0);
  assert(la_load32_acq(&ctx.signaled) != 0);
  assert(la_load32_acq(&ctx.stopreq_seen) != 0);
  assert(la_load8_acq(&tg->in_native) == 0);
  expect_stopreq_error(L, rc);

  clear_stopreq(tg);
  assert(g->gc2.hs_pending == 0);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);

  run_ok(L,
    "local profile = require('jit.profile')\n"
    "profile.start('i1', function() end)\n"
    "profile.stop()\n");
}

static lua_State *profile_callback_thread(lua_State *L)
{
  TValue key;
  cTValue *tv;
  key.u64 = KEY_PROFILE_THREAD;
  tv = lj_tab_get(L, tabV(registry(L)), &key);
  assert(tvisthread(tv));
  return threadV(tv);
}

static void run_busy_callback_state_test(lua_State *L, global_State *g)
{
  lua_State *L2;
  uint32_t saved_owner, fake_owner;

  run_ok(L,
    "local profile = require('jit.profile')\n"
    "profile.start('i1', function() end)\n");

  L2 = profile_callback_thread(L);
  saved_owner = lj_state_owner_acq(L2);
  fake_owner = lj_thr_current_id(g) + 7000u;
  if (fake_owner == 0 || fake_owner == lj_thr_current_id(g) ||
      fake_owner == LJ_THREAD_GCSCAN)
    fake_owner = 7000u;

  lj_state_owner_rel(L2, fake_owner);
  run_ok(L,
    "local x = 0\n"
    "for i = 1, 2000000 do x = x + i end\n"
    "assert(x > 0)\n");
  assert(lj_state_owner_acq(L2) == fake_owner);
  lj_state_owner_rel(L2, saved_owner);

  run_ok(L,
    "local profile = require('jit.profile')\n"
    "profile.stop()\n"
    "profile.start('i1', function() end)\n"
    "profile.stop()\n");
}

#endif

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);

  run_ok(L, "assert(require('jit.profile'))");
  run_deferred_cleanup_test(L, tg);
  run_callback_error_test(L);
#if LJ_PROFILE_PTHREAD
  run_native_join_test(L, g, tg);
  run_busy_callback_state_test(L, g);
#endif

  lua_close(L);
  printf("t-profile-stop-native OK: profile.stop STOPREQ cleanup guarded\n");
  return 0;
}

#else

int main(void)
{
  printf("t-profile-stop-native SKIP: profiler unavailable\n");
  return 0;
}

#endif
