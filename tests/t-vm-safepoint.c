/*
** Focused test for x64 VM safepoint polling.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_tg.h"

static void publish_manual(global_State *g, TGState *tg, uint32_t actions)
{
  uint64_t epoch = la_load64_rlx(&g->gc2.hs_epoch) + 1u;
  g->gc2.hs_actions = actions;
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

static void load_error_after_publish(lua_State *L)
{
  int status = luaL_loadstring(L, "publish_error()\n");
  if (status != LUA_OK) {
    fprintf(stderr, "load_error_after_publish failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

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
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);
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
  lua_pushcfunction(L, publish_poll_c);
  lua_setglobal(L, "publish_poll");
  lua_pushcfunction(L, assert_pending_c);
  lua_setglobal(L, "assert_pending");
  lua_pushcfunction(L, publish_error_c);
  lua_setglobal(L, "publish_error");

  load_loop(L);
  epoch0 = g->gc2.hs_epoch;
  actions = LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK;
  publish_manual(g, tg, actions);
  call_expect(L, 64, "call_loop");
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

  lua_close(L);

  printf("t-vm-safepoint OK: x64 branch, function-entry, return, unwind polls acked\n");
  return 0;
}
