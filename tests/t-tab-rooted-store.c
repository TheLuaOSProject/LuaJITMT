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
static int gc_hook_lightud;
static int lightud_key_token, lightud_value_token;

static uint32_t nil_post_cas_calls, nil_found_calls, nil_newindex_calls;
static struct {
  GCtab *table;
  TValue *oldarray, *newarray;
  MSize newasize;
  int32_t key;
  TValue expected;
  GCtab *metatable;
  uint8_t expect_nil;
  uint8_t install_meta;
  uint8_t called;
} nil_stale_ctx;

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
  else if (mode == 4) lua_newtable(L);
  else lua_pushlightuserdata(L, &lightud_key_token);
}

static void basics(lua_State *L)
{
  int mode;
  for (mode = 0; mode < 6; mode++) {
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
  assert(tvistab(tabroot));
  if (gc_hook_lightud)
    assert(tvislightud(keyroot) && tvislightud(valueroot));
  else
    assert(tvisgcv(keyroot) && tvisgcv(valueroot));
  lua_pushnil(L); lua_replace(L, 1);
  lua_pushnil(L); lua_replace(L, 2);
  lua_pushnil(L); lua_replace(L, 3);
  assert(lua_checkstack(L, 4096));
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(tvistab(tabroot));
  if (gc_hook_lightud)
    assert(tvislightud(keyroot) && tvislightud(valueroot));
  else
    assert(tvisgcv(keyroot) && tvisgcv(valueroot));
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

static int call_new_store(lua_State *L)
{
  int result;
  errno = 617;
  result = lj_tab_storenew_rooted(L, L->base, L->base+1, L->base+2);
  assert(errno == 617);
  clean(L);
  lua_pushboolean(L, result);
  return 1;
}

static int call_lightud_store(lua_State *L)
{
  int result;
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TLIGHTUSERDATA);
  luaL_checktype(L, 3, LUA_TLIGHTUSERDATA);
  errno = 617;
  result = lj_tab_storegc_rooted(L, L->base, L->base+1, L->base+2);
  assert(result && errno == 617);
  clean(L);
  lua_pushboolean(L, result);
  return 1;
}

static int call_tagged_store(lua_State *L)
{
  int result;
  luaL_checktype(L, 1, LUA_TTABLE);
  result = lj_tab_storegc_rooted(L, L->base, L->base+1, L->base+2);
  clean(L);
  lua_pushboolean(L, result);
  return 1;
}

static int call_tagged_new_store(lua_State *L)
{
  int result = lj_tab_storenew_rooted(L, L->base, L->base+1, L->base+2);
  clean(L);
  lua_pushboolean(L, result);
  return 1;
}

static int call_raw_store(lua_State *L)
{
  int result;
  errno = 617;
  result = lj_tab_storeraw_rooted(L, L->base, L->base+1, L->base+2);
  assert(errno == 617);
  clean(L);
  lua_pushboolean(L, result);
  return 1;
}

static int push_lightud_pair(lua_State *L)
{
  lua_pushlightuserdata(L, &lightud_key_token);
  lua_pushlightuserdata(L, &lightud_value_token);
  return 2;
}

static void anchored_lifetime(lua_State *L)
{
  uint32_t calls_before = hook_calls;
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
  assert(hook_calls == calls_before + 2u);
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

static void rooted_nonnull_insertions(lua_State *L)
{
  uint32_t before = gc_hook_calls;
  lua_pushcfunction(L, call_new_store);
  lua_setglobal(L, "rooted_new_store");
  assert(luaL_dostring(L,
    "local t={}; assert(rooted_new_store(t,'insert',5)); assert(t.insert==5)\n"
    "local u=setmetatable({},{}); assert(rooted_new_store(u,'key',false)); assert(u.key==false)\n"
    "local calls=0; setmetatable(t,{__newindex=function(x,k,v) calls=calls+1; rawset(x,k,v) end})\n"
    "assert(rooted_new_store(t,'insert',6)); assert(t.insert==6 and calls==0)\n"
    "assert(not rooted_new_store(t,'missing',7)); assert(calls==0)\n"
    "local k='nil-node'; local u={}; rawset(u,k,1); rawset(u,k,nil)\n"
    "setmetatable(u,{__newindex=function(x,key,v) calls=calls+1; rawset(x,key,v) end})\n"
    "assert(not rooted_new_store(u,k,9)); assert(calls==0 and u[k]==nil)\n") == 0);
  lua_pushcfunction(L, call_tagged_new_store);
  lua_setglobal(L, "rooted_tagged_new_store");
  assert(luaL_dostring(L,
    "local t={}; assert(not rooted_tagged_new_store(t,nil,1))\n"
    "assert(not rooted_tagged_new_store(t,0/0,1))\n"
    "assert(not rooted_tagged_new_store(t,'nil-value',nil))\n") == 0);
  lua_pushcfunction(L, call_raw_store);
  lua_setglobal(L, "rooted_raw_store");
  lua_pushcfunction(L, call_raw_store);
  lua_setglobal(L, "rooted_tagged_raw_store");
  lua_pushcfunction(L, push_lightud_pair);
  lua_setglobal(L, "rooted_lightud_pair");
  assert(luaL_dostring(L,
    "local calls=0; local t=setmetatable({}, {__newindex=function() calls=calls+1; error('raw helper called __newindex') end})\n"
    "assert(rooted_raw_store(t,'new',7)); assert(rawget(t,'new')==7 and calls==0)\n"
    "local redirect={}; setmetatable(t,{__newindex=redirect})\n"
    "assert(rooted_raw_store(t,'redirect',12)); assert(rawget(t,'redirect')==12 and next(redirect)==nil)\n"
    "rawset(t,'present',1); assert(rooted_raw_store(t,'present',8)); assert(rawget(t,'present')==8 and calls==0)\n"
    "local k='nil-node'; rawset(t,k,1); rawset(t,k,nil); assert(rooted_raw_store(t,k,9)); assert(rawget(t,k)==9 and calls==0)\n"
    "local a={}; assert(rooted_raw_store(a,98273,11)); assert(rawget(a,98273)==11)\n"
    "local lk,lv=rooted_lightud_pair(); local u=setmetatable({}, {__newindex=function() error('raw lightud helper called __newindex') end})\n"
    "assert(rooted_tagged_raw_store(u,lk,lv)); assert(rawget(u,lk)==lv)\n"
    "assert(not rooted_tagged_raw_store(t,nil,1)); assert(not rooted_tagged_raw_store(t,0/0,1))\n"
    "assert(not rooted_tagged_raw_store(t,'nil-value',nil))\n") == 0);
  clean(L);

  lua_pushcfunction(L, call_new_store);
  lua_setglobal(L, "rooted_new_store");
  lua_pushcfunction(L, call_raw_store);
  lua_setglobal(L, "rooted_raw_store");
  {
    int raw, mode;
    for (raw = 0; raw <= 1; raw++) {
      lua_pushboolean(L, raw); lua_setglobal(L, "rooted_insert_raw_gc");
      for (mode = 0; mode <= 1; mode++) {
        int status;
        hook_throw = mode;
        lj_tab_test_set_rooted_gc_store_hook(gc_handoff);
        status = luaL_dostring(L,
      "local weak=setmetatable({}, {__mode='v'})\n"
      "local function invoke()\n"
      "  local k,v={},{}; local t={}; weak[1]=k; weak[2]=t; weak[3]=v\n"
      "  local set = rooted_insert_raw_gc and rooted_raw_store or rooted_new_store\n"
      "  if rooted_insert_raw_gc then setmetatable(t,{__newindex=function() error('raw newindex') end}) end\n"
      "  local ok,err=pcall(set,t,k,v)\n"
      "  if ok then assert(err and rawget(t,k)==v)\n"
      "  else assert(tostring(err):find('rooted GC store rollback',1,true)) end\n"
      "end\n"
      "invoke(); rooted_new_store_weak=weak\n");
        if (status) fprintf(stderr, "new-store GC raw=%d mode=%d: %s\n",
                            raw, mode, lua_tostring(L, -1));
        assert(status == 0);
        clean(L);
        lj_tab_test_set_rooted_gc_store_hook(NULL);
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_getglobal(L, "rooted_new_store_weak");
        lua_rawgeti(L, -1, 1); assert(lua_isnil(L, -1)); lua_pop(L, 1);
        lua_rawgeti(L, -1, 2); assert(lua_isnil(L, -1)); lua_pop(L, 1);
        lua_rawgeti(L, -1, 3); assert(lua_isnil(L, -1)); lua_pop(L, 2);
        lua_pushnil(L); lua_setglobal(L, "rooted_new_store_weak");
      }
    }
  }
  assert(gc_hook_calls == before + 4u);
}

static void new_store_reserve_failure(unsigned stage, int raw)
{
  lua_State *L = luaL_newstate();
  TGState *tg;
  TGRootAnchorBlock *block;
  uint32_t i, idx, top, capacity;
  TValue nilv;
  assert(L);
  luaL_openlibs(L);
  tg = L2TG(L);
  lua_newtable(L); lua_pushliteral(L, "reserve-key"); lua_pushinteger(L, 23);
  capacity = TG_ROOT_ANCHOR_SLOTS;
  for (block = &tg->root_anchor;
       (block = lj_tg_root_anchor_next_acq(block)) != NULL; )
    capacity += TG_ROOT_ANCHOR_SLOTS;
  top = capacity - (stage - 1u);
  setnilV(&nilv);
  for (i=0; i<top; i++) assert(lj_tg_root_anchor_push(L,tg,&nilv,&idx));
  lj_tg_root_test_fail_reserve_after(1);
  assert(!(raw ? lj_tab_storeraw_rooted(L,L->top-3,L->top-2,L->top-1) :
           lj_tab_storenew_rooted(L,L->top-3,L->top-2,L->top-1)));
  lj_tg_root_test_fail_reserve_after(0);
  assert(lj_tg_root_anchor_top_acq(tg)==top);
  assert(gc2_smr_readers_acq(G(L))==0);
  assert(lj_gc2_rootdesc_snapshot(&tg->root_desc,NULL)==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  lua_pushvalue(L,-2); lua_rawget(L,-4); assert(lua_isnil(L,-1)); lua_pop(L,1);
  lj_tg_root_anchor_rollback(tg,0);
  lua_close(L);
}

static void anchored_lightud_lifetime(lua_State *L)
{
  uint32_t before = gc_hook_calls;
  lua_pushcfunction(L, call_lightud_store);
  lua_setglobal(L, "rooted_lightud_store");
  lua_pushcfunction(L, call_tagged_store);
  lua_setglobal(L, "rooted_tagged_store");
  lua_pushcfunction(L, push_lightud_pair);
  lua_setglobal(L, "rooted_lightud_pair");
  /* Missing keys may reach the post-anchor hook before the existing-key
  ** protocol declines. Exercise ordinary refusals with the hook disabled. */
  assert(luaL_dostring(L,
    "local k,v=rooted_lightud_pair(); local t={[k]='prior'}\n"
    "assert(not rooted_tagged_store(t,nil,v))\n"
    "assert(not rooted_tagged_store(t,0/0,v))\n"
    "assert(not rooted_tagged_store(t,42,v))\n"
    "assert(not rooted_tagged_store(t,k,nil))\n"
    "assert(t[42]==nil and t[k]=='prior')\n") == 0);
  for (hook_throw = 0; hook_throw <= 1; hook_throw++) {
    int status;
    gc_hook_lightud = 1;
    lj_tab_test_set_rooted_gc_store_hook(gc_handoff);
    status = luaL_dostring(L,
      "local weak=setmetatable({}, {__mode='v'})\n"
      "local k,v=rooted_lightud_pair()\n"
      "local function invoke()\n"
      "  local t={[k]='prior'}; weak[1]=t\n"
      "  return rooted_lightud_store(t,k,v)\n"
      "end\n"
      "local ok,err=pcall(invoke)\n"
      "if ok then assert(err and weak[1][k]==v)\n"
      "else assert(tostring(err):find('rooted GC store rollback',1,true) and weak[1][k]=='prior') end\n"
      "rooted_lightud_store_weak=weak\n");
    if (status) fprintf(stderr, "lightuserdata handoff mode %d: %s\n",
                        hook_throw, lua_tostring(L, -1));
    assert(status == 0);
    clean(L);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_getglobal(L, "rooted_lightud_store_weak");
    lua_rawgeti(L, -1, 1);
    assert(lua_isnil(L, -1));
    lua_pop(L, 2);
    lua_pushnil(L); lua_setglobal(L, "rooted_lightud_store_weak");
  }
  gc_hook_lightud = 0;
  lj_tab_test_set_rooted_gc_store_hook(NULL);
  assert(gc_hook_calls == before + 2u);
}

static int call_nil_store(lua_State *L)
{
  int result;
  luaL_checktype(L, 1, LUA_TTABLE);
  errno = 617;
  result = lj_tab_storenil_rooted(L, L->base, L->base+1);
  assert(errno == 617);
  clean(L);
  lua_pushboolean(L, result);
  return 1;
}

static void nil_deletions(lua_State *L)
{
  int result;
  lua_newtable(L);
  lua_pushliteral(L, "delete"); lua_pushnumber(L, 7.5); lua_rawset(L, -3);
  lua_pushliteral(L, "delete");
  result = lj_tab_storenil_rooted(L, L->top-2, L->top-1);
  assert(result == 1); clean(L);
  lua_pushliteral(L, "delete"); lua_rawget(L, -3);
  assert(lua_isnil(L, -1)); lua_pop(L, 1);
  /* Repeated absent deletion is a successful no-op only with NULL metatable. */
  assert(lj_tab_storenil_rooted(L, L->top-2, L->top-1) == 1); clean(L);
  lua_pushliteral(L, "absent");
  assert(lj_tab_storenil_rooted(L, L->top-3, L->top-1) == 1); clean(L);
  lua_pop(L, 1);
  lua_pop(L, 2);
  /* Existing deletion is allowed with any metatable; absent assignment is not. */
  lua_newtable(L); lua_pushliteral(L, "delete"); lua_pushnumber(L, 9);
  lua_rawset(L, -3); lua_newtable(L); lua_setmetatable(L, -2);
  lua_pushliteral(L, "delete");
  assert(lj_tab_storenil_rooted(L, L->top-2, L->top-1) == 1); clean(L);
  /* Deletion retains the hash key's nil-valued node. It is absent for
  ** __newindex purposes even though the storage resolver finds the node. */
  assert(lj_tab_storenil_rooted(L, L->top-2, L->top-1) == 0); clean(L);
  lua_pushliteral(L, "absent");
  assert(lj_tab_storenil_rooted(L, L->top-3, L->top-1) == 0); clean(L);
  lua_pop(L, 3);
  lua_createtable(L, 8, 0);
  lua_newtable(L); lua_setmetatable(L, -2);
  lua_pushinteger(L, 3);
  assert(lj_tab_storenil_rooted(L, L->top-2, L->top-1) == 0); clean(L);
  lua_pop(L, 2);
  /* Invalid nil/NaN keys decline without retaining anchors or readers. */
  lua_newtable(L); lua_pushnil(L);
  assert(!lj_tab_storenil_rooted(L, L->top-2, L->top-1)); clean(L);
  lua_pop(L, 1);
  lua_newtable(L); lua_pushnumber(L, NAN);
  assert(!lj_tab_storenil_rooted(L, L->top-2, L->top-1)); clean(L);
  lua_pop(L, 2);
  {
    TValue forward;
    lua_newtable(L); setforwardV(&forward);
    assert(!lj_tab_storenil_rooted(L, L->top-1, &forward)); clean(L);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}

/* A one-shot post-CAS resize forces a committed old-generation store to
** become STALE before the helper returns. The helper must re-resolve rather
** than report failure or replay an insertion. */
static int nil_newindex(lua_State *L);

static void nil_resize_after_cas(lua_State *L, GCtab *t, TValue *dst,
				 cTValue *key, cTValue *value)
{
  assert(t == nil_stale_ctx.table && dst == &nil_stale_ctx.oldarray[nil_stale_ctx.key]);
  assert((tvisint(key) && intV(key) == nil_stale_ctx.key) ||
	 (tvisnum(key) && numV(key) == (lua_Number)nil_stale_ctx.key));
  if (nil_stale_ctx.expect_nil)
    assert(tvisnil(value));
  else
    assert(tv_rawload(value) == tv_rawload(&nil_stale_ctx.expected));
  /* A numeric-key nil CAS publishes no collectable edge, so it needs the
  ** keyed SMR interval and helper anchors, but no table-store descriptor. */
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  assert(gc2_smr_readers_acq(G(L)) == baseline_readers + 1u);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) ==
         baseline_anchors + (nil_stale_ctx.expect_nil ? 2u : 3u));
  nil_post_cas_calls++;
  assert(!nil_stale_ctx.called);
  nil_stale_ctx.called = 1;
  lj_tab_test_set_store_post_cas_hook(NULL);
  /* Publish the already prepared successor generation without waiting inside
  ** the helper's SMR interval. This is the same bounded handoff used by the
  ** existing post-CAS stale fixture. */
  setnilV(&nil_stale_ctx.newarray[nil_stale_ctx.key]);
  if (nil_stale_ctx.install_meta)
    lj_tab_metatable_rel(t, nil_stale_ctx.metatable);
  lj_tab_asize_rel(t, nil_stale_ctx.newasize);
  lj_tab_array_rel(t, nil_stale_ctx.newarray);
}

static int nil_newindex(lua_State *L)
{
  UNUSED(L);
  nil_newindex_calls++;
  return 0;
}

/* Deterministically interleave another raw delete after the helper's FOUND
** observation and before its CAS, then install __newindex. This exercises the
** overlapping-operation contract without relying on scheduler timing. */
static void nil_delete_after_found(lua_State *L, GCtab *t, TValue *dst,
				   cTValue *key, cTValue *value)
{
  TValue *slot;
  (void)dst; (void)value;
  nil_found_calls++;
  slot = lj_tab_set(L, t, key);
  setnilV(slot);
  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, nil_newindex);
  lua_setfield(L, -2, "__newindex");
  lua_setmetatable(L, -3);  /* table, key, metatable */
}

static void nil_resize_and_found_races(lua_State *L)
{
  TValue *oldarray;
  MSize oldasize, oldacap, newasize;
  int32_t k = 6;
  GCtab *t;
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  lua_pushinteger(L, k); lua_pushinteger(L, 41); lua_rawset(L, -3);
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  assert(lj_tab_array_separated(t) && (MSize)k < oldasize);
  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  nil_stale_ctx.table = t;
  nil_stale_ctx.oldarray = oldarray;
  nil_stale_ctx.newarray = lj_tab_array_acq(t);
  nil_stale_ctx.newasize = newasize = lj_tab_asize_acq(t);
  nil_stale_ctx.key = k;
  setnilV(&nil_stale_ctx.expected);
  nil_stale_ctx.expect_nil = 1;
  nil_stale_ctx.called = 0;
  assert(nil_stale_ctx.newarray != oldarray && newasize > oldasize);
  /* Re-expose the coherent old vector; the post-CAS hook publishes its
  ** prepared successor after the nil CAS has committed. */
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
                 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  lua_pushinteger(L, k);
  nil_post_cas_calls = 0;
  lj_tab_test_set_store_post_cas_hook(nil_resize_after_cas);
  assert(lj_tab_storenil_rooted(L, L->top-2, L->top-1) == 1);
  lj_tab_test_set_store_post_cas_hook(NULL);
  assert(nil_post_cas_calls == 1); clean(L);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  memset(&nil_stale_ctx, 0, sizeof(nil_stale_ctx));
  lua_pop(L, 1); /* original helper key */
  lua_pushinteger(L, k); lua_rawget(L, -2);
  assert(lua_isnil(L, -1)); lua_pop(L, 1);
  lua_pushinteger(L, 99); lua_rawseti(L, -2, k + 1);
  lua_rawgeti(L, -1, k + 1);
  assert(lua_tointeger(L, -1) == 99); lua_pop(L, 2);

  /* A non-nil absent-key store commits into the old array, then the hook
  ** publishes a successor that deliberately lacks that value and installs
  ** __newindex. The helper must retry the already-authorized raw assignment
  ** in the current generation without replaying the metamethod decision. */
  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, nil_newindex);
  lua_setfield(L, -2, "__newindex");
  nil_stale_ctx.metatable = tabV(L->top-1);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  lua_pushinteger(L, k); lua_pushinteger(L, 41); lua_rawset(L, -3);
  lua_pushinteger(L, k); lua_pushnil(L); lua_rawset(L, -3);
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  assert(lj_tab_array_separated(t) && (MSize)k < oldasize);
  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  nil_stale_ctx.table = t;
  nil_stale_ctx.oldarray = oldarray;
  nil_stale_ctx.newarray = lj_tab_array_acq(t);
  nil_stale_ctx.newasize = lj_tab_asize_acq(t);
  nil_stale_ctx.key = k;
  setnumV(&nil_stale_ctx.expected, 29.5);
  nil_stale_ctx.expect_nil = 0;
  nil_stale_ctx.install_meta = 1;
  nil_stale_ctx.called = 0;
  assert(nil_stale_ctx.newarray != oldarray);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
                 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  lua_pushinteger(L, k); lua_pushnumber(L, 29.5);
  nil_newindex_calls = 0;
  nil_post_cas_calls = 0;
  lj_tab_test_set_store_post_cas_hook(nil_resize_after_cas);
  assert(lj_tab_storenew_rooted(L, L->top-3, L->top-2, L->top-1) == 1);
  lj_tab_test_set_store_post_cas_hook(NULL);
  assert(nil_post_cas_calls == 1 && nil_stale_ctx.called);
  assert(nil_newindex_calls == 0);
  assert(tv_rawload(&nil_stale_ctx.newarray[k]) ==
         tv_rawload(&nil_stale_ctx.expected));
  clean(L);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  lua_pushinteger(L, k); lua_rawget(L, -4);
  assert(lua_tonumber(L, -1) == 29.5); lua_pop(L, 1);
  assert(lj_tab_metatable_acq(t) == nil_stale_ctx.metatable);
  memset(&nil_stale_ctx, 0, sizeof(nil_stale_ctx));
  lua_pop(L, 4);

  lua_createtable(L, 0, 2);
  lua_pushliteral(L, "race-delete"); lua_pushinteger(L, 17); lua_rawset(L, -3);
  lua_pushliteral(L, "race-delete");
  nil_found_calls = nil_newindex_calls = 0;
  lj_tab_test_set_nil_store_found_hook(nil_delete_after_found);
  assert(lj_tab_storenil_rooted(L, L->top-2, L->top-1) == 1);
  lj_tab_test_set_nil_store_found_hook(NULL);
  assert(nil_found_calls == 1 && nil_newindex_calls == 0); clean(L);
  lua_pushliteral(L, "race-delete"); lua_rawget(L, -3);
  assert(lua_isnil(L, -1)); lua_pop(L, 1);
  lua_pushinteger(L, 5); lua_pushinteger(L, 6); lua_settable(L, -4);
  assert(nil_newindex_calls == 1); /* the installed metamethod is live */
  lua_pop(L, 2);
}

/* Stable FORWARD markers are synthetic single-threaded equivalents of a
** completed handoff. They verify storage repair without relying on timing of
** an active resize owner, and the postcondition forbids key insertion. */
static void nil_stable_forward_repair(lua_State *L)
{
  TValue *array;
  MSize asize, hmask;
  GCtab *t;
  Node *node;
  lua_createtable(L, 128, 0);
  t = tabV(L->top-1);
  lua_pushinteger(L, 64); lua_pushinteger(L, 123); lua_rawset(L, -3);
  asize = lj_tab_array_snapshot_acq(t, &array);
  assert(asize > 64 && !lj_tab_array_is_colocated(t, array));
  setforwardV(&array[64]);
  lua_pushinteger(L, 64);
  assert(lj_tab_storenil_rooted(L, L->top-2, L->top-1) == 1);
  clean(L);
  lua_pushinteger(L, 64); lua_rawget(L, -3); assert(lua_isnil(L, -1));
  lua_pop(L, 1); lua_pop(L, 2);

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  lua_pushliteral(L, "forward-hash"); lua_pushinteger(L, 321); lua_rawset(L, -3);
  lua_pushliteral(L, "forward-hash");
  {
    Node *base = lj_tab_node_snapshot_acq(t, &hmask);
    node = hashstr_node(base, hmask, strV(L->top-1));
  }
  assert(hmask > 0 && node != NULL);
  setforwardV(&node->val);
  assert(lj_tab_storenil_rooted(L, L->top-2, L->top-1) == 1);
  clean(L);
  lua_pushliteral(L, "forward-hash"); lua_rawget(L, -3); assert(lua_isnil(L, -1));
  lua_pop(L, 3);
}

static void nil_handoff(lua_State *L)
{
  int mode;
  uint32_t calls_before = hook_calls;
  lua_pushcfunction(L, call_nil_store); lua_setglobal(L, "rooted_nil_store");
  for (mode = 0; mode < 2; mode++) {
    int status;
    hook_throw = mode;
    lj_tab_test_set_rooted_store_hook(handoff);
    status = luaL_dostring(L,
      "local t = { [string.rep('delete-key', 9000)] = 12 }\n"
      "local k = next(t)\n"
      "local ok, err = pcall(rooted_nil_store, t, k)\n"
      "if ok then assert(err and next(t) == nil)\n"
      "else assert(tostring(err):find('rooted store rollback',1,true)) end");
    if (status) fprintf(stderr, "nil handoff mode %d: %s\n",
                        mode, lua_tostring(L, -1));
    assert(status == 0);
    lj_tab_test_set_rooted_store_hook(NULL);
    clean(L);
  }
  assert(hook_calls == calls_before + 2u);
}

static void internal_lightud_refusals(lua_State *L)
{
  uint64_t invalid[] = { LJ_TFORWARD_BITS, LJ_TKEYLOCK_BITS,
    LJ_TAB_RESIZE_MARK_BITS(1, LJ_TAB_RESIZE_MARK_SRC),
    LJ_TAB_RESIZE_MARK_BITS(2, LJ_TAB_RESIZE_MARK_NIL_DONE) };
  unsigned i;
  for (i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
    TValue key;
    uint32_t waits;
    lua_newtable(L);
    lua_pushlightuserdata(L, &lightud_key_token);
    lua_pushvalue(L, -1);
    lua_pushliteral(L, "prior");
    lua_rawset(L, -4);
    lua_pushlightuserdata(L, &lightud_value_token);
    waits = lj_tab_test_store_wait_l_calls();
    lj_tv_load_acq(&key, L->top-2);
    tv_rawstore_rel(L->top-1, invalid[i]);
    assert(!lj_tab_storegc_rooted(L, L->top-3, L->top-2, L->top-1));
    assert(!lj_tab_storenew_rooted(L, L->top-3, L->top-2, L->top-1));
    assert(!lj_tab_storeraw_rooted(L, L->top-3, L->top-2, L->top-1));
    assert(tv_rawload_acq(L->top-1) == invalid[i]);
    clean(L);
    lua_pop(L, 1);
    lua_pushlightuserdata(L, &lightud_value_token);
    tv_rawstore_rel(L->top-2, invalid[i]);
    assert(!lj_tab_storegc_rooted(L, L->top-3, L->top-2, L->top-1));
    assert(!lj_tab_storenum_rooted(L, L->top-3, L->top-2, 1.25));
    assert(!lj_tab_storebool_rooted(L, L->top-3, L->top-2, 1));
    assert(!lj_tab_storenil_rooted(L, L->top-3, L->top-2));
    assert(!lj_tab_storenew_rooted(L, L->top-3, L->top-2, L->top-1));
    assert(!lj_tab_storeraw_rooted(L, L->top-3, L->top-2, L->top-1));
    assert(tv_rawload_acq(L->top-2) == invalid[i]);
    assert(lj_tab_test_store_wait_l_calls() == waits);
    clean(L);
    tv_rawstore_rel(L->top-2, tv_rawload(&key));
    lua_pop(L, 1);
    lua_pushvalue(L, -1);
    lua_rawget(L, -3);
    assert(strcmp(lua_tostring(L, -1), "prior") == 0);
    lua_pop(L, 3);
  }
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
  assert(!lj_tab_storenil_rooted(L, L->top-2, L->top-1));
  lj_tg_root_test_fail_reserve_after(0);
  assert(lj_tg_root_anchor_top_acq(tg) == top);
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
  nil_deletions(L);
  nil_resize_and_found_races(L);
  nil_stable_forward_repair(L);
  nil_handoff(L);
  refusals(L);
  anchored_lifetime(L);
  anchored_gc_lifetime(L);
  rooted_nonnull_insertions(L);
  anchored_lightud_lifetime(L);
  internal_lightud_refusals(L);
  lua_close(L);
  reserve_failure(0);
  reserve_failure(1);
  gc_reserve_failure(1);
  gc_reserve_failure(2);
  gc_reserve_failure(3);
  new_store_reserve_failure(1, 0);
  new_store_reserve_failure(2, 0);
  new_store_reserve_failure(3, 0);
  new_store_reserve_failure(4, 0);
  new_store_reserve_failure(1, 1);
  new_store_reserve_failure(2, 1);
  new_store_reserve_failure(3, 1);
  puts("rooted stores: scalar, GC/lightuserdata values and nil deletion, root handoff, relocation, GC, OOM and unwind OK");
  return 0;
}
