/* Root handoff, cooperative lifetime and error rollback for scalar stores. */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_tg.h"

#if !defined(LJ_TAB_TEST_HELPERS) || !defined(LJ_TG_ROOT_TEST_HELPERS)
#error "rooted store fixture requires table and TG root test helpers"
#endif

static uint32_t baseline_anchors, baseline_readers, hook_calls;
static uint32_t gc_hook_calls;
static int hook_throw;

static void clean(lua_State *L)
{
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == baseline_anchors);
  assert(gc2_smr_readers_acq(G(L)) == baseline_readers);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
}

static void set_key(lua_State *L, int mode)
{
  if (mode == 0) lua_pushinteger(L, 1);
  else if (mode == 1) lua_pushnumber(L, -2.5);
  else if (mode == 2) lua_pushboolean(L, 0);
  else if (mode == 3) lua_pushliteral(L, "__index");
  else lua_newtable(L);
}

static void basics(lua_State *L)
{
  int mode;
  for (mode = 0; mode < 5; mode++) {
    lua_createtable(L, 4, 4);
    set_key(L, mode);
    lua_pushvalue(L, -1);
    lua_pushnumber(L, 1.25);
    lua_rawset(L, -4);
    errno = 617;
    assert(lj_tab_storenum_rooted(L, L->top-2, L->top-1, 9.5));
    assert(errno == 617);
    clean(L);
    lua_pushvalue(L, -1);
    lua_rawget(L, -3);
    assert(lua_tonumber(L, -1) == 9.5);
    lua_pop(L, 1);
    assert(lj_tab_storebool_rooted(L, L->top-2, L->top-1, 1));
    lua_pushvalue(L, -1);
    lua_rawget(L, -3);
    assert(lua_isboolean(L, -1) && lua_toboolean(L, -1));
    lua_pop(L, 1);
    if (mode == 3) assert(lj_tab_nomm_acq(tabV(L->top-2)) == 0);
    assert(lj_tab_storebool_rooted(L, L->top-2, L->top-1, 0));
    lua_pushvalue(L, -1);
    lua_rawget(L, -3);
    assert(lua_isboolean(L, -1) && !lua_toboolean(L, -1));
    lua_pop(L, 3);
    clean(L);
  }
}

static void refusals(lua_State *L)
{
  uint32_t expected = LJ_GC2_SMR_OPEN;
  int i;
  lua_newtable(L);
  lua_pushinteger(L, 4);
  assert(!lj_tab_storenum_rooted(L, L->top-2, L->top-1, 12.5));
  clean(L);
  lua_pushvalue(L, -1);
  lua_pushnumber(L, 1.25);
  lua_rawset(L, -4);
  assert(gc2_smr_reclaiming_cas(G(L), &expected, LJ_GC2_SMR_META_EXCLUSIVE));
  assert(!lj_tab_storenum_rooted(L, L->top-2, L->top-1, 12.5));
  gc2_smr_reclaiming_rel(G(L), LJ_GC2_SMR_OPEN);
  clean(L);
  lua_pushvalue(L, -1);
  lua_rawget(L, -3);
  assert(lua_tonumber(L, -1) == 1.25);
  lua_pop(L, 2);
  for (i = 0; i < 3; i++) {
    if (i == 0) lua_pushnil(L);
    else if (i == 1) lua_pushnumber(L, NAN);
    else lua_newtable(L);
    assert(!lj_tab_storenum_rooted(L, L->top-2, L->top-1, 12.5));
    clean(L);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}

/* At this boundary the original stack cells may disappear and the stack may
** move. Only the two helper anchors authorize the store after collection. */
static void handoff(lua_State *L, TValue *tabroot, TValue *keyroot)
{
  lj_tab_test_set_rooted_store_hook(NULL);
  hook_calls++;
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == baseline_anchors + 2u);
  assert(gc2_smr_readers_acq(G(L)) == baseline_readers);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  assert(tvistab(tabroot) && tvisstr(keyroot));
  lua_pushnil(L); lua_replace(L, 1);
  lua_pushnil(L); lua_replace(L, 2);
  assert(lua_checkstack(L, 4096));
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(tvistab(tabroot) && tvisstr(keyroot));
  lj_tab_resize(L, tabV(tabroot), 128, 6);
  errno = 713;  /* The complete operation restores its caller's error state. */
  if (hook_throw) luaL_error(L, "rooted store rollback");
}

static void gc_handoff(lua_State *L, TValue *tabroot, TValue *keyroot,
		       TValue *valueroot)
{
  gc_hook_calls++;
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == baseline_anchors + 3u);
  assert(gc2_smr_readers_acq(G(L)) == baseline_readers);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  assert(tvistab(tabroot) && tvisgcv(keyroot) && tvisgcv(valueroot));
  lua_pushnil(L); lua_replace(L, 1);
  lua_pushnil(L); lua_replace(L, 2);
  lua_pushnil(L); lua_replace(L, 3);
  assert(lua_checkstack(L, 4096));
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(tvistab(tabroot) && tvisgcv(keyroot) && tvisgcv(valueroot));
  lj_tab_resize(L, tabV(tabroot), 128, 6);
  errno = 811;
  if (hook_throw) luaL_error(L, "rooted GC store rollback");
}

static int call_store(lua_State *L)
{
  int result;
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TSTRING);
  errno = 617;
  result = lj_tab_storenum_rooted(L, L->base, L->base+1, 29.5);
  assert(result && errno == 617);
  clean(L);
  lua_pushboolean(L, result);
  return 1;
}

static int call_gc_store(lua_State *L)
{
  int result;
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  luaL_checktype(L, 3, LUA_TTABLE);
  errno = 617;
  result = lj_tab_storegc_rooted(L, L->base, L->base+1, L->base+2);
  assert(result && errno == 617);
  clean(L);
  lua_pushboolean(L, result);
  return 1;
}

static void anchored_lifetime(lua_State *L)
{
  int mode;
  lua_pushcfunction(L, call_store);
  lua_setglobal(L, "rooted_store");
  for (mode = 0; mode < 2; mode++) {
    int status;
    hook_throw = mode;
    lj_tab_test_set_rooted_store_hook(handoff);
    status = luaL_dostring(L,
      "local weak = setmetatable({}, {__mode='v'})\n"
      "local function invoke()\n"
      "  local key = string.rep('rooted-key', 9000)\n"
      "  local t = {[key] = 1.25}; weak[1] = t\n"
      "  return rooted_store(t, key) -- Tail call drops these locals.\n"
      "end\n"
      "local ok, err = pcall(invoke)\n"
      "if ok then\n"
      "  assert(err and weak[1][next(weak[1])] == 29.5)\n"
      "else assert(tostring(err):find('rooted store rollback', 1, true)) end\n"
      "rooted_store_weak = weak\n");
    if (status) fprintf(stderr, "handoff mode %d: %s\n", mode, lua_tostring(L, -1));
    assert(status == 0);
    clean(L);
    /* Return past the Lua frame so a dead expression temporary cannot retain
    ** the weak witness. The helper has already popped both exact anchors. */
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_getglobal(L, "rooted_store_weak");
    lua_rawgeti(L, -1, 1);
    assert(lua_isnil(L, -1));
    lua_pop(L, 2);
    lua_pushnil(L); lua_setglobal(L, "rooted_store_weak");
  }
  assert(hook_calls == 2);
}

static void anchored_gc_lifetime(lua_State *L)
{
  lua_pushcfunction(L, call_gc_store);
  lua_setglobal(L, "rooted_gc_store");
  for (hook_throw = 0; hook_throw <= 1; hook_throw++) {
    int status;
    lj_tab_test_set_rooted_gc_store_hook(gc_handoff);
    status = luaL_dostring(L,
      "local weak = setmetatable({}, {__mode='v'})\n"
      "local function invoke()\n"
      "  local key = {}; weak[3] = key\n"
      "  local value = {}; weak[1] = value\n"
      "  local t = {[key] = false}; weak[2] = t\n"
      "  return rooted_gc_store(t, key, value)\n"
      "end\n"
      "local ok, err = pcall(invoke)\n"
      "if ok then\n"
      "  local t = weak[2]; assert(t and t[next(t)] == weak[1])\n"
      "else assert(tostring(err):find('rooted GC store rollback', 1, true)) end\n"
      "rooted_gc_store_weak = weak\n");
    if (status) fprintf(stderr, "GC handoff mode %d: %s\n",
                        hook_throw, lua_tostring(L, -1));
    assert(status == 0);
    clean(L);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_getglobal(L, "rooted_gc_store_weak");
    lua_rawgeti(L, -1, 1);
    assert(lua_isnil(L, -1));
    lua_pop(L, 1);
    lua_rawgeti(L, -1, 2);
    assert(lua_isnil(L, -1));
    lua_pop(L, 1);
    lua_rawgeti(L, -1, 3);
    assert(lua_isnil(L, -1));
    lua_pop(L, 2);
    lua_pushnil(L); lua_setglobal(L, "rooted_gc_store_weak");
  }
  lj_tab_test_set_rooted_gc_store_hook(NULL);
  assert(gc_hook_calls == 2);
}

static void reserve_failure(int second)
{
  lua_State *L = luaL_newstate();
  TGState *tg;
  TGRootAnchorBlock *block;
  uint32_t i, idx, top;
  TValue nilv;
  assert(L);
  luaL_openlibs(L);
  tg = L2TG(L);
  lua_newtable(L);
  lua_pushnumber(L, 1.25); lua_rawseti(L, -2, 1);
  lua_pushinteger(L, 1);
  setnilV(&nilv);
  /* Constructors may already have reserved a spare block. Fill exactly the
  ** existing capacity, leaving one slot for the second-reservation case. */
  top = TG_ROOT_ANCHOR_SLOTS;
  for (block = &tg->root_anchor;
       (block = lj_tg_root_anchor_next_acq(block)) != NULL; )
    top += TG_ROOT_ANCHOR_SLOTS;
  top -= second ? 1u : 0u;
  for (i = 0; i < top; i++)
    assert(lj_tg_root_anchor_push(L, tg, &nilv, &idx));
  lj_tg_root_test_fail_reserve_after(1);
  assert(!lj_tab_storenum_rooted(L, L->top-2, L->top-1, 99.5));
  lj_tg_root_test_fail_reserve_after(0);
  assert(lj_tg_root_anchor_top_acq(tg) == top);
  assert(gc2_smr_readers_acq(G(L)) == 0);
  lua_rawgeti(L, -2, 1);
  assert(lua_tonumber(L, -1) == 1.25);
  lj_tg_root_anchor_rollback(tg, 0);
  lua_close(L);
}

static void gc_reserve_failure(unsigned stage)
{
  lua_State *L = luaL_newstate();
  TGState *tg;
  TGRootAnchorBlock *block;
  uint32_t i, idx, top, capacity;
  TValue nilv;
  assert(L);
  luaL_openlibs(L);
  tg = L2TG(L);
  lua_newtable(L);
  lua_pushliteral(L, "key");
  lua_newtable(L);
  capacity = TG_ROOT_ANCHOR_SLOTS;
  for (block = &tg->root_anchor;
       (block = lj_tg_root_anchor_next_acq(block)) != NULL; )
    capacity += TG_ROOT_ANCHOR_SLOTS;
  top = capacity - (stage - 1u);
  setnilV(&nilv);
  for (i = 0; i < top; i++)
    assert(lj_tg_root_anchor_push(L, tg, &nilv, &idx));
  lj_tg_root_test_fail_reserve_after(1);
  assert(!lj_tab_storegc_rooted(L, L->top-3, L->top-2, L->top-1));
  lj_tg_root_test_fail_reserve_after(0);
  assert(lj_tg_root_anchor_top_acq(tg) == top);
  assert(gc2_smr_readers_acq(G(L)) == 0);
  lj_tg_root_anchor_rollback(tg, 0);
  lua_close(L);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF));
  baseline_anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  baseline_readers = gc2_smr_readers_acq(G(L));
  basics(L);
  refusals(L);
  anchored_lifetime(L);
  anchored_gc_lifetime(L);
  lua_close(L);
  reserve_failure(0);
  reserve_failure(1);
  gc_reserve_failure(1);
  gc_reserve_failure(2);
  gc_reserve_failure(3);
  puts("rooted stores: scalar and GC values, root handoff, relocation, GC, OOM and unwind OK");
  return 0;
}
