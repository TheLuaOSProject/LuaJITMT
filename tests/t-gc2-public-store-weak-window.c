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
#include <sched.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_meta.h"
#include "lj_tab.h"
#include "lj_tg.h"
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

typedef struct StoreReclaimPeer {
  lua_State *peer;
  uint32_t ready, start;
} StoreReclaimPeer;

static void *retire_metadata(void *ud)
{
  StoreReclaimPeer *run = (StoreReclaimPeer *)ud;
  lua_State *peer = run->peer;
  global_State *g = G(peer);
  assert(lj_threading_attach(peer));
  la_store32_rel(&run->ready, 1);
  while (!la_load32_acq(&run->start))
    sched_yield();
  (void)lj_gc2_reclaim_retired(g, lj_gc2_retire_epoch(g) + 1);
  lj_threading_detach(peer, 1);
  return NULL;
}

static void test_replacing_during_retirement(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  StoreReclaimPeer run = {0};
  pthread_t worker;
  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  assert(luaL_dostring(L,
    "local t = setmetatable({}, {__mode='kv'})\n"
    "local key, value = {}, require('ffi').new('char[20000]')\n"
    "for i=1,3072 do t[i]=i; t['h'..i]=i end\n"
    "t[key]=true\n"
    "local store,eq,clear = rawset,rawequal,require('table.clear')\n"
    "local grow, labels = setmetatable({}, {__mode='kv'}), {}\n"
    "for i=1,80 do labels[i] = i%2==0 and {} or ('k'..i) end\n"
    "return t,key,value,function()\n"
    "  t.h19=value; assert(eq(t.h19,value))\n"
    "  store(t,key,value); assert(eq(t[key],value))\n"
    "  t[key]=false; assert(t[key]==false)\n"
    "  t[3071]=value; assert(eq(t[3071],value))\n"
    "  t.new_field=value; assert(eq(t.new_field,value))\n"
    "  local other={}; store(t,other,value); assert(eq(t[other],value))\n"
    "  for i=1,80 do grow[i]=value; grow[labels[i]]=i end\n"
    "  for i=1,80 do assert(eq(grow[i],value) and grow[labels[i]]==i) end\n"
    "  clear(grow); assert(next(grow)==nil)\n"
    "  grow[labels[1]]=value; assert(eq(grow[labels[1]],value))\n"
    "end") == 0);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  lua_gc(L, LUA_GCSTOP, 0);
  run.peer = lua_newthread(L);
  assert(pthread_create(&worker, NULL, retire_metadata, &run) == 0);
  while (!la_load32_acq(&run.ready))
    (void)lj_thr_retry_yield(L);
  lj_gc2_test_idle_reclaim_pause_after_jit_quiescence();
  la_store32_rel(&run.start, 1);
  while (!lj_gc2_test_idle_reclaim_paused())
    (void)lj_thr_retry_yield(NULL);
  assert(mt_active_or_entering_acq(g));
  lua_newtable(L);
  for (int i = 1; i <= 256; i++) {
    lua_pushvalue(L, 3);
    lua_rawseti(L, -2, i);
  }
  for (int i = 1; i <= 256; i++) {
    lua_rawgeti(L, -1, i);
    assert(lua_rawequal(L, -1, 3));
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  lua_pushvalue(L, 3);
  lua_rawseti(L, 1, 3072);
  lua_rawgeti(L, 1, 3072);
  assert(lua_rawequal(L, -1, 3));
  lua_pop(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_rawset(L, 1);
  lua_pushvalue(L, 2);
  lua_rawget(L, 1);
  assert(lua_rawequal(L, -1, 3));
  lua_pop(L, 1);
  lua_pushvalue(L, 2);
  lua_pushnil(L);
  lua_rawset(L, 1);
  lua_pushvalue(L, 2);
  lua_rawget(L, 1);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);
  lua_pushvalue(L, 3);
  lua_setfield(L, 1, "h18");
  lua_getfield(L, 1, "h18");
  assert(lua_rawequal(L, -1, 3));
  lua_pop(L, 1);
  lua_pushvalue(L, 2);
  lua_pushboolean(L, 1);
  lua_settable(L, 1);
  lua_pushvalue(L, 4);
  assert(lua_pcall(L, 0, 0, 0) == 0);
  assert(lj_gc2_test_idle_reclaim_paused());
  lj_gc2_test_idle_reclaim_release();
  assert(pthread_join(worker, NULL) == 0);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  lua_rawgeti(L, 1, 3072);
  assert(lua_rawequal(L, -1, 3));
  lua_close(L);
}

typedef struct PausedResize {
  lua_State *peer;
  GCtab *table;
  uint32_t ready, start, publishing, resume;
} PausedResize;

static PausedResize *resize_pause;

static void pause_before_root_change(lua_State *L, GCtab *t, TValue *array, MSize size)
{
  PausedResize *run = resize_pause;
  UNUSED(L); UNUSED(array); UNUSED(size);
  if (run->table != t)
    return;
  la_store32_rel(&run->publishing, 1);
  while (!la_load32_acq(&run->resume))
    sched_yield();
}

static void *resize_while_reclaiming(void *ud)
{
  PausedResize *run = (PausedResize *)ud;
  assert(lj_threading_attach(run->peer));
  la_store32_rel(&run->ready, 1);
  while (!la_load32_acq(&run->start))
    sched_yield();
  lj_tab_resize(run->peer, run->table, 257, 5);
  lj_threading_detach(run->peer, 1);
  return NULL;
}

static void test_helping_during_retirement(int operation)
{
  lua_State *L = luaL_newstate();
  StoreReclaimPeer collector = {0};
  PausedResize resize = {0};
  pthread_t reclaimer, issuer;
  TabResizeDesc *desc;
  TabNodeRetire *oldnodes;
  TabArrayRetire *oldarray;
  global_State *g;
  const void *node_body, *array_body;
  uint64_t epoch, id;
  assert(L);
  g = G(L);
  lua_createtable(L, 64, 4);
  resize.table = tabV(L->top - 1);
  lua_pushinteger(L, 42); lua_rawseti(L, 1, 1);
  lua_pushinteger(L, 99); lua_rawseti(L, 1, -1);
  if (operation == 4) {
    luaL_openlibs(L);
    assert(luaL_dostring(L, "return require('table.clear')") == 0);
    assert(lua_isfunction(L, 2));
  }
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  lua_gc(L, LUA_GCSTOP, 0);
  resize.peer = lua_newthread(L);
  lua_pushvalue(L, 1);
  lua_xmove(L, resize.peer, 1);
  collector.peer = lua_newthread(L);
  resize_pause = &resize;
  lj_tab_test_set_resize_hash_cutover_hook(pause_before_root_change);
  assert(pthread_create(&issuer, NULL, resize_while_reclaiming, &resize) == 0);
  assert(pthread_create(&reclaimer, NULL, retire_metadata, &collector) == 0);
  while (!la_load32_acq(&resize.ready) || !la_load32_acq(&collector.ready))
    (void)lj_thr_retry_yield(L);
  lj_gc2_test_idle_reclaim_pause_after_jit_quiescence();
  la_store32_rel(&collector.start, 1);
  while (!lj_gc2_test_idle_reclaim_paused())
    sched_yield();
  la_store32_rel(&resize.start, 1);
  while (!la_load32_acq(&resize.publishing))
    sched_yield();
  desc = lj_tab_resize_desc_control_acq(resize.table);
  assert(desc && desc->oldret && desc->oldaret);
  oldnodes = desc->oldret;
  oldarray = desc->oldaret;
  id = desc->id;
  node_body = lj_tab_node_hdr(desc->oldnode);
  array_body = lj_tab_array_hdr(desc->oldarray);
  switch (operation) {
  case 0:
    lua_rawgeti(L, 1, -1);
    assert(lua_tointeger(L, -1) == 99);
    lua_pop(L, 1);
    break;
  case 1:
    lua_pushinteger(L, 111);
    lua_rawseti(L, 1, -1);
    lua_rawgeti(L, 1, -1);
    assert(lua_tointeger(L, -1) == 111);
    lua_pop(L, 1);
    break;
  case 2:
    lua_pushnil(L);
    assert(lua_next(L, 1));
    assert(lua_tointeger(L, -1) == (lua_tointeger(L, -2) == 1 ? 42 : 99));
    lua_pop(L, 2);
    break;
  case 3:
    assert(lua_objlen(L, 1) == 1);
    break;
  case 4:
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 1);
    assert(lua_pcall(L, 1, 0, 0) == 0);
    lua_pushnil(L);
    assert(!lua_next(L, 1));
    break;
  }
  assert(lj_tab_resize_desc_control_acq(resize.table) == NULL);
  assert(lj_gc2_test_idle_reclaim_paused());
  lj_gc2_test_idle_reclaim_release();
  assert(pthread_join(reclaimer, NULL) == 0);
  epoch = lj_tab_resize_desc_epoch_acq(desc) + LJ_TAB_RETIRE_EPOCHS;
  assert(lj_gc2_test_idle_reclaim_enter(g));
  (void)lj_tab_reclaim_retired(g, epoch);
  assert(lj_gc2_mem_registered_known_reclaim_held(g, desc));
  assert(desc->retire_records == 2 && lj_tab_resize_desc_phase_acq(desc) ==
         TAB_RESIZE_DESC_TERMINAL);
  assert(lj_gc2_mem_registered_known_reclaim_held(g, oldnodes) &&
         lj_gc2_mem_registered_known_reclaim_held(g, oldarray));
  assert(lj_gc2_mem_registered_known_reclaim_held(g, node_body) &&
         lj_gc2_mem_registered_known_reclaim_held(g, array_body));
  lj_gc2_test_idle_reclaim_leave(g);
  la_store32_rel(&resize.resume, 1);
  assert(pthread_join(issuer, NULL) == 0);
  lj_tab_test_set_resize_hash_cutover_hook(NULL);
  resize_pause = NULL;
  assert(lj_gc2_test_idle_reclaim_enter(g));
  (void)lj_tab_reclaim_retired(g, epoch);
  lj_gc2_test_idle_reclaim_leave(g);
  assert(lj_gc2_smr_read_try(g));
  assert(lj_tab_resize_desc_find_held(g, resize.table, id) == NULL);
  lj_gc2_smr_read_leave(g);
  (void)lj_arena_remote_free_drain_force(&g->main_tg->alloc);
  assert(!lj_gc2_mem_registered_known(g, node_body) &&
         !lj_gc2_mem_registered_known(g, array_body));
  lua_close(L);
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
  test_replacing_during_retirement();
  for (int i = 0; i < 5; i++)
    test_helping_during_retirement(i);
  puts("gc2 public scalar weak-window regression passed");
  return 0;
}
