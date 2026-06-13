/*
** Focused test for x64 VM safepoint polling.
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
#include "lj_tg.h"
#if LJ_HASJIT
#include "lj_dispatch.h"
#include "lj_jit.h"
#endif

static void publish_manual(global_State *g, TGState *tg, uint32_t actions)
{
  uint64_t epoch = la_load64_rlx(&g->gc2.hs_epoch) + 1u;
  la_store32_rel(&g->gc2.hs_actions, actions);
  la_store32_rel(&g->gc2.hs_pending, 1);  /* 05 section 5.4.2. */
  la_store64_rel(&g->gc2.hs_epoch, epoch);  /* 05 section 5.4.2. */
  la_store32_rel(&tg->reqmask, actions);  /* 05 section 5.4.2. */
  la_store32_rel(&tg->poll, 1);  /* 05 section 5.4.2 signal word. */
}

static void load_loop(lua_State *L)
{
  int status = luaL_loadstring(L,
    "local i = 0\n"
    "while i < 64 do\n"
    "  i = i + 1\n"
    "end\n"
    "return i\n");
  if (status != LUA_OK) {
    fprintf(stderr, "load_loop failed: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void load_return(lua_State *L)
{
  int status = luaL_loadstring(L, "return 7\n");
  if (status != LUA_OK) {
    fprintf(stderr, "load_return failed: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void load_return_after_publish(lua_State *L)
{
  int status = luaL_loadstring(L,
    "publish_poll()\n"
    "assert_pending()\n"
    "assert_pending()\n"
    "return 19\n");
  if (status != LUA_OK) {
    fprintf(stderr, "load_return_after_publish failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void load_iter_after_publish(lua_State *L)
{
  int status = luaL_loadstring(L,
    "local t = {1, 2, 3}\n"
    "publish_poll()\n"
    "assert_pending()\n"
    "local n = 0\n"
    "for _ in pairs(t) do\n"
    "  n = n + 1\n"
    "  if n == 2 then assert_acked_now() end\n"
    "end\n"
    "return n\n");
  if (status != LUA_OK) {
    fprintf(stderr, "load_iter_after_publish failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void load_error_after_publish(lua_State *L)
{
  int status = luaL_loadstring(L, "publish_error()\n");
  if (status != LUA_OK) {
    fprintf(stderr, "load_error_after_publish failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

#if LJ_HASJIT
typedef struct TracePollPublisher {
  global_State *g;
  TGState *tg;
  uint32_t start;
} TracePollPublisher;

static void sleep_ns(long ns)
{
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = ns;
  while (nanosleep(&ts, &ts) != 0) {
  }
}

static void *trace_poll_publisher(void *arg)
{
  TracePollPublisher *p = (TracePollPublisher *)arg;
  while (la_load32_acq(&p->start) == 0)
    la_cpu_pause();
  sleep_ns(1000000);
  publish_manual(p->g, p->tg, LJ_GC2_HS_STOPREQ);
  return NULL;
}

static void load_jloop_flush_after_publish(lua_State *L)
{
  int status = luaL_loadstring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function f(n, flush)\n"
    "  if flush then publish_flushj(); assert_pending_flushj() end\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "f(100, false)\n"
    "jit.opt.start('hotloop=1000000', 'hotexit=1000000')\n"
    "return f\n");
  if (status != LUA_OK) {
    fprintf(stderr, "load_jloop_flush_after_publish failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void load_trace_stopreq_loop(lua_State *L)
{
  int status = luaL_loadstring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function f(n)\n"
    "  local x = 0.0\n"
    "  while x < n do x = x + 1.0 end\n"
    "  return x\n"
    "end\n"
    "assert(f(128.0) == 128.0)\n"
    "jit.opt.start('hotloop=1000000', 'hotexit=1000000')\n"
    "return f\n");
  if (status != LUA_OK) {
    fprintf(stderr, "load_trace_stopreq_loop failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}
#endif

static void call_expect(lua_State *L, int expected, const char *name)
{
  int status = lua_pcall(L, 0, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "%s failed: %s\n", name, lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_tointeger(L, -1) == expected);
  lua_pop(L, 1);
}

static void call_expect_error(lua_State *L, const char *name)
{
  int status = lua_pcall(L, 0, 1, 0);
  if (status != LUA_ERRRUN) {
    fprintf(stderr, "%s unexpected status %d: %s\n", name, status,
	    lua_tostring(L, -1));
    assert(status == LUA_ERRRUN);
  }
  lua_pop(L, 1);
}

static void assert_acked(global_State *g, TGState *tg, uint64_t epoch0)
{
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(tg->hs_epoch_ack == g->gc2.hs_epoch);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
}

static void assert_pending(global_State *g, TGState *tg, uint64_t epoch0,
			   uint32_t actions)
{
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 1);
  assert(tg->hs_epoch_ack == epoch0);
  assert(tg->poll == 1);
  assert(tg->reqmask == actions);
}

static int publish_poll_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  publish_manual(g, tg, LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK);
  return 0;
}

static int assert_pending_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  assert_pending(g, tg, g->gc2.hs_epoch - 1u,
		 LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK);
  return 0;
}

static int assert_acked_now_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  assert(g->gc2.hs_pending == 0);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->hs_epoch_ack == g->gc2.hs_epoch);
  assert(tg->mark_active == 1);
  assert(tg->alloc.alloc_black == 1);
  return 0;
}

static int publish_error_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  publish_manual(g, tg, LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_ALLOC_WHITE);
  return luaL_error(L, "published error");
}

#if LJ_HASJIT
static int publish_flushj_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  publish_manual(g, tg, LJ_GC2_HS_FLUSHJ);
  return 0;
}

static int assert_pending_flushj_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  assert_pending(g, tg, g->gc2.hs_epoch - 1u, LJ_GC2_HS_FLUSHJ);
  return 0;
}
#endif

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  uint64_t epoch0;
  uint32_t actions;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert(g->gc2.tg_list == tg);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);

  lua_gc(L, LUA_GCSTOP, 0);
  luaL_openlibs(L);
  lua_pushcfunction(L, publish_poll_c);
  lua_setglobal(L, "publish_poll");
  lua_pushcfunction(L, assert_pending_c);
  lua_setglobal(L, "assert_pending");
  lua_pushcfunction(L, assert_acked_now_c);
  lua_setglobal(L, "assert_acked_now");
  lua_pushcfunction(L, publish_error_c);
  lua_setglobal(L, "publish_error");
#if LJ_HASJIT
  lua_pushcfunction(L, publish_flushj_c);
  lua_setglobal(L, "publish_flushj");
  lua_pushcfunction(L, assert_pending_flushj_c);
  lua_setglobal(L, "assert_pending_flushj");
#endif

  load_loop(L);
  epoch0 = g->gc2.hs_epoch;
  actions = LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK;
  publish_manual(g, tg, actions);
  call_expect(L, 64, "call_loop");
  assert_acked(g, tg, epoch0);
  assert(tg->mark_active == 1);
  assert(tg->alloc.alloc_black == 1);

  load_iter_after_publish(L);
  epoch0 = g->gc2.hs_epoch;
  call_expect(L, 3, "call_iter_after_publish");
  assert_acked(g, tg, epoch0);
  assert(tg->mark_active == 1);
  assert(tg->alloc.alloc_black == 1);

  load_return(L);
  epoch0 = g->gc2.hs_epoch;
  actions = LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_ALLOC_WHITE;
  publish_manual(g, tg, actions);
  call_expect(L, 7, "call_return");
  assert_acked(g, tg, epoch0);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);

  load_return_after_publish(L);
  epoch0 = g->gc2.hs_epoch;
  call_expect(L, 19, "call_return_after_publish");
  assert_acked(g, tg, epoch0);
  assert(tg->mark_active == 1);
  assert(tg->alloc.alloc_black == 1);

  load_error_after_publish(L);
  epoch0 = g->gc2.hs_epoch;
  call_expect_error(L, "call_error_after_publish");
  assert_acked(g, tg, epoch0);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);

#if LJ_HASJIT
  load_jloop_flush_after_publish(L);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  assert(lua_isfunction(L, -1));
  assert(traceref(G2J(g), 1) != NULL || G2J(g)->freetrace > 0);
  epoch0 = g->gc2.hs_epoch;
  lua_pushinteger(L, 20);
  lua_pushboolean(L, 1);
  assert(lua_pcall(L, 2, 1, 0) == LUA_OK);
  assert(lua_tointeger(L, -1) == 210);
  lua_pop(L, 1);
  assert_acked(g, tg, epoch0);
  assert(G2J(g)->cur.traceno == 0);
  epoch0 = g->gc2.hs_epoch;
  assert(lj_gc2_handshake(g, LJ_GC2_HS_FLUSHJ) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(G2J(g)->freetrace == 0);
  epoch0 = g->gc2.hs_epoch;
  assert(luaL_dostring(L, "jit.flush()") == LUA_OK);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(tg->hs_epoch_ack == g->gc2.hs_epoch);
  epoch0 = g->gc2.hs_epoch;
  assert(luaL_dostring(L, "jit.flush(function() end)") == LUA_OK);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(tg->hs_epoch_ack == g->gc2.hs_epoch);
  epoch0 = g->gc2.hs_epoch;
  assert(luaL_dostring(L, "jit.flush(1)") == LUA_OK);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(tg->hs_epoch_ack == g->gc2.hs_epoch);

  load_trace_stopreq_loop(L);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  assert(lua_isfunction(L, -1));
  assert(traceref(G2J(g), 1) != NULL || G2J(g)->freetrace > 0);
  {
    TracePollPublisher pub;
    pthread_t th;
    int status;
    pub.g = g;
    pub.tg = tg;
    pub.start = 0;
    assert(pthread_create(&th, NULL, trace_poll_publisher, &pub) == 0);
    epoch0 = g->gc2.hs_epoch;
    lua_pushnumber(L, 1e100);
    la_store32_rel(&pub.start, 1);
    status = lua_pcall(L, 1, 1, 0);
    assert(pthread_join(th, NULL) == 0);
    if (status != LUA_ERRRUN) {
      fprintf(stderr, "trace_stopreq_loop unexpected status %d: %s\n", status,
	      lua_tostring(L, -1));
      assert(status == LUA_ERRRUN);
    }
    assert(strstr(lua_tostring(L, -1), "thread interrupted: VM shutdown") != NULL);
    lua_pop(L, 1);
    assert_acked(g, tg, epoch0);
    assert((la_load8_acq(&tg->tg_flags) & TGF_STOPREQ) != 0);
    la_store8_rel(&tg->tg_flags,
		  (uint8_t)(la_load8_acq(&tg->tg_flags) & ~TGF_STOPREQ));
  }
#endif

  lua_close(L);

  printf("t-vm-safepoint OK: x64 loop, trace-loop, trace-entry, return, unwind polls acked\n");
  return 0;
}
