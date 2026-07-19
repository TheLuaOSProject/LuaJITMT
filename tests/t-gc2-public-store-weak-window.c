/*
** Regression for public scalar stores retaining an outer weak-write token.
**
** The keyed transaction has its own exact weak window. Public API/metamethod
** wrappers must not add another one around lj_tab_trystoretv_cas_keyed(),
** because that helper may take an L-aware CAS_CHANGED retry. A stop request at
** that retry can park for weak closure, while weak closure waits for the outer
** token: a circular wait.
**
** The guard gate pause makes the nesting directly observable. Each collectable
** store must expose exactly the guard's one token, never guard + wrapper.
*/

#ifndef LJ_GC2_TEST_HELPERS
#define LJ_GC2_TEST_HELPERS
#endif

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_meta.h"
#include "lj_tab.h"
#include "lj_thr.h"

typedef struct WeakWindowObserve {
  global_State *g;
  uint32_t active;
} WeakWindowObserve;

static LJGC2ActivationSnap gate_move(global_State *g, uint8_t gate)
{
  LJGC2ActivationSnap from =
    lj_gc2_activation_snapshot(&g->gc2.activation);
  LJGC2ActivationSnap to;
  assert(lj_gc2_activation_try_gate(&g->gc2.activation, &from, gate, &to) ==
         LJ_GC2_TRANSITION_OK);
  return to;
}

static void gate_reopen(global_State *g)
{
  LJGC2ActivationSnap snap =
    lj_gc2_activation_snapshot(&g->gc2.activation);
  if (snap.gate != LJ_GC2_ROOT_GATE_OPEN)
    (void)gate_move(g, LJ_GC2_ROOT_GATE_OPEN);
}

static void *observe_weak_window(void *ud)
{
  WeakWindowObserve *ctx = (WeakWindowObserve *)ud;
  uint32_t spin;
  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_gc2_test_table_store_gate_pause_waiting()) {
      ctx->active = gc2_weak_write_active_acq(ctx->g);
      lj_gc2_test_table_store_gate_pause_release();
      return NULL;
    }
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"table-store guard did not reach gate pause");
  return NULL;
}

static pthread_t observe_begin(global_State *g, WeakWindowObserve *ctx)
{
  pthread_t thread;
  LJGC2ActivationSnap snap =
    lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(snap.gate == LJ_GC2_ROOT_GATE_OPEN);
  (void)gate_move(g, LJ_GC2_ROOT_GATE_CLOSING);
  ctx->g = g;
  ctx->active = UINT32_MAX;
  lj_gc2_test_table_store_gate_pause_arm();
  assert(pthread_create(&thread, NULL, observe_weak_window, ctx) == 0);
  return thread;
}

static void observe_end(global_State *g, pthread_t thread,
                        const WeakWindowObserve *ctx)
{
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx->active == 1u);  /* Central guard only; no public outer token. */
  assert(gc2_weak_write_active_acq(g) == 0u);
  gate_reopen(g);
}

static void push_collectable(lua_State *L)
{
  lua_newtable(L);
}

static void test_rawseti(lua_State *L, global_State *g)
{
  WeakWindowObserve ctx;
  pthread_t thread;
  push_collectable(L);
  thread = observe_begin(g, &ctx);
  lua_rawseti(L, 1, 1);
  observe_end(g, thread, &ctx);
}

static void test_rawset(lua_State *L, global_State *g)
{
  WeakWindowObserve ctx;
  pthread_t thread;
  push_collectable(L);  /* key */
  push_collectable(L);  /* value */
  thread = observe_begin(g, &ctx);
  lua_rawset(L, 1);
  observe_end(g, thread, &ctx);
}

static void test_setfield(lua_State *L, global_State *g)
{
  WeakWindowObserve ctx;
  pthread_t thread;
  push_collectable(L);
  thread = observe_begin(g, &ctx);
  lua_setfield(L, 1, "field");
  observe_end(g, thread, &ctx);
}

static void test_settable(lua_State *L, global_State *g)
{
  WeakWindowObserve ctx;
  pthread_t thread;
  push_collectable(L);  /* key */
  push_collectable(L);  /* value */
  thread = observe_begin(g, &ctx);
  lua_settable(L, 1);
  observe_end(g, thread, &ctx);
}

static void test_meta_pair(lua_State *L, global_State *g)
{
  WeakWindowObserve ctx;
  pthread_t thread;
  TValue *o, *key, *value;
  push_collectable(L);  /* key */
  push_collectable(L);  /* value */
  o = L->top - 3;
  key = L->top - 2;
  value = L->top - 1;
  thread = observe_begin(g, &ctx);
  assert(lj_meta_tsettv_pair(L, o, key, value) != NULL);
  observe_end(g, thread, &ctx);
  lua_settop(L, 1);
}

static void test_primitive_and_delete_need_no_window(lua_State *L,
                                                      global_State *g)
{
  assert(gc2_weak_write_active_acq(g) == 0u);
  lua_pushinteger(L, 42);
  lua_rawseti(L, 1, 9);
  assert(gc2_weak_write_active_acq(g) == 0u);
  lua_rawgeti(L, 1, 9);
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  push_collectable(L);
  lua_pushvalue(L, -1);  /* Keep an identical key rooted for the readback. */
  lua_pushinteger(L, 1);
  lua_rawset(L, 1);
  lua_pushvalue(L, -1);
  lua_pushnil(L);
  lua_rawset(L, 1);
  assert(gc2_weak_write_active_acq(g) == 0u);
  lua_pushvalue(L, -1);
  lua_rawget(L, 1);
  assert(lua_isnil(L, -1));
  lua_pop(L, 2);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  GCtab *weak;
  assert(L != NULL);
  g = G(L);

  lua_newtable(L);  /* target at stable public index 1 */
  weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "kv");
  lua_rawset(L, -3);
  assert(lua_setmetatable(L, 1));
  assert(lj_gc2_weak_write_candidate(L, weak) != 0);

  test_rawseti(L, g);
  test_rawset(L, g);
  test_setfield(L, g);
  test_settable(L, g);
  test_meta_pair(L, g);
  test_primitive_and_delete_need_no_window(L, g);

  assert(gc2_weak_write_active_acq(g) == 0u);
  lua_close(L);
  puts("gc2 public scalar weak-window regression passed");
  return 0;
}
