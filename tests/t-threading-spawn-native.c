/*
** Focused public threading.spawn native STOPREQ handling fixture.
*/

#include <assert.h>
#include <errno.h>
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
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_vm.h"

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
static uint32_t worker_cpcall_seen;
static uint32_t fail_spawn_create;
static uint32_t block_unstarted_fini;
static uint32_t blocked_unstarted_fini_calls;
static uint32_t retried_unstarted_fini;
static size_t failed_spawn_stack_bytes;
static TGState *blocked_unstarted_tg;
static pthread_t fixture_main_thread;


extern int __real_pthread_create(pthread_t *thread,
				 const pthread_attr_t *attr,
				 void *(*start_routine)(void *), void *arg);
extern int __real_lj_vm_cpcall(lua_State *L, lua_CFunction func, void *ud,
			       lua_CPFunction cp);
extern int __real_lj_tg_fini_thread(global_State *g, TGState *tg);

int __wrap_lj_tg_fini_thread(global_State *g, TGState *tg)
{
  uint32_t remaining = la_load32_acq(&block_unstarted_fini);
  while (remaining != 0) {
    uint32_t expect = remaining;
    if (la_cas32(&block_unstarted_fini, &expect, remaining - 1u,
		 LA_ACQ_REL, LA_ACQ)) {
      if (blocked_unstarted_tg)
	assert(blocked_unstarted_tg == tg);
      else
	blocked_unstarted_tg = tg;
      la_add32_rlx(&blocked_unstarted_fini_calls, 1);
      /* Model the checked finalizer's real BLOCKED publication. The linker
      ** wrapper bypasses its LIVE/RETRY -> BUSY -> RETRY state machine. */
      lj_tg_fini_state_rel(tg, TG_FINI_RETRY);
      return 0;
    }
    remaining = expect;
  }
  if (tg == blocked_unstarted_tg)
    la_store32_rel(&retried_unstarted_fini, 1);
  return __real_lj_tg_fini_thread(g, tg);
}

int __wrap_lj_vm_cpcall(lua_State *L, lua_CFunction func, void *ud,
			lua_CPFunction cp)
{
  global_State *g = G(L);
  if (L != mainthread_acq(g) &&
      !pthread_equal(pthread_self(), fixture_main_thread)) {
    TGState *tg = lj_thr_get_tg();
    uint32_t owner = lj_state_owner_acq(L);
    assert(tg != NULL);
    assert(tg->gl == g);
    assert(lj_thr_id_is_owner(owner));
    assert(owner == lj_tg_tid_acq(tg));
    assert(lj_tg_find_owner(g, owner) == tg);
    assert(lj_tg_load_cur_L(tg) == L);
    assert(lj_tg_load_thread_L(tg) == L);
    la_add32_rlx(&worker_cpcall_seen, 1);
  }
  return __real_lj_vm_cpcall(L, func, ud, cp);
}

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
			  void *(*start_routine)(void *), void *arg)
{
  uint32_t expect = 1;
  if (la_cas32(&fail_spawn_create, &expect, 0, LA_ACQ_REL, LA_ACQ)) {
    LJThread *th = (LJThread *)arg;
    lua_State *child = lj_thread_state_load_acq(th);
    assert(child != NULL);
    failed_spawn_stack_bytes =
      (size_t)child->stacksize * sizeof(TValue);
    return EAGAIN;
  }
  expect = 1;
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
  uint32_t seen0 = la_load32_acq(&worker_cpcall_seen);
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
  assert(la_load32_acq(&worker_cpcall_seen) > seen0);
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

static void churn_main_allocator_after_failed_spawn(lua_State *L)
{
  int i, j;
  lua_settop(L, 0);
  for (i = 0; i < 128; i++) {
    lua_createtable(L, 64, 0);
    for (j = 1; j <= 64; j++) {
      lua_pushinteger(L, i * 64 + j);
      lua_rawseti(L, -2, j);
    }
    lua_pop(L, 1);
    if ((i & 15) == 15)
      lua_gc(L, LUA_GCSTEP, 1);
  }
  lua_gc(L, LUA_GCCOLLECT, 0);
}

static void run_create_failure_retry_case(uint32_t nargs, uint32_t blocks)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  uint32_t i;
  int rc;

  require_threading(L);
  assert(lua_checkstack(L, (int)nargs + 4));
  lua_getfield(L, -1, "spawn");
  lua_pushcfunction(L, child_marker);
  for (i = 0; i < nargs; i++)
    lua_pushinteger(L, (lua_Integer)i);
  blocked_unstarted_tg = NULL;
  failed_spawn_stack_bytes = 0;
  la_store32_rel(&blocked_unstarted_fini_calls, 0);
  la_store32_rel(&retried_unstarted_fini, 0);
  /* The first veto is spawn error cleanup. With two blocks, lua_close's
  ** all-userdata finalization also vetoes and the joined-worker live-root
  ** drain must own the third try; with one, finalization closes the live node. */
  assert(blocks == 1 || blocks == 2);
  la_store32_rel(&block_unstarted_fini, blocks);
  la_store32_rel(&fail_spawn_create, 1);
  rc = lua_pcall(L, (int)nargs + 1, 1, 0);
  assert(rc != LUA_OK);
  assert(lua_tostring(L, -1) != NULL);
  if (nargs == 0)
    assert(failed_spawn_stack_bytes < LJ_HUGE_THRESHOLD);
  else
    assert(failed_spawn_stack_bytes >= LJ_HUGE_THRESHOLD);
  assert(la_load32_acq(&fail_spawn_create) == 0);
  assert(la_load32_acq(&block_unstarted_fini) == blocks - 1u);
  assert(la_load32_acq(&blocked_unstarted_fini_calls) == 1);
  assert(la_load32_acq(&retried_unstarted_fini) == 0);
  assert(blocked_unstarted_tg != NULL);
  assert(la_load32_acq(&g->threading_live_count) == 1);

  /* Generic freeing used to route the provisional child stack into the main
  ** allocator. Exercise small-bin reuse before close and a separate huge
  ** child stack so the exact child allocd/HugeTab route is required. */
  churn_main_allocator_after_failed_spawn(L);
  lua_close(L);
  assert(la_load32_acq(&block_unstarted_fini) == 0);
  assert(la_load32_acq(&blocked_unstarted_fini_calls) == blocks);
  assert(la_load32_acq(&retried_unstarted_fini) == 1);
  blocked_unstarted_tg = NULL;
}

static void test_create_failure_retains_retry_tg_until_joined_close(void)
{
  run_create_failure_retry_case(0, 1);
  run_create_failure_retry_case(0, 2);
  run_create_failure_retry_case(3000, 2);
}

int main(void)
{
  fixture_main_thread = pthread_self();
  test_sticky_stopreq_spawn_ok();
  test_fresh_stopreq_aborts_before_child_runs();
  test_spawn_preserves_active_gc_cycle();
  test_create_failure_retains_retry_tg_until_joined_close();
  printf("t-threading-spawn-native OK: STOPREQ and retry teardown verified\n");
  return 0;
}
