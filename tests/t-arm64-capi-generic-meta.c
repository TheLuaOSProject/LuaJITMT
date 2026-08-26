/*
** Ownerless actual-metamethod-call coverage for the generic Lua 5.1 C API.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_safepoint.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_thr.h"

#if !defined(LJ_API_ROOT_TEST_HELPERS)
#error "t-arm64-capi-generic-meta requires LJ_API_ROOT_TEST_HELPERS"
#endif

extern void lj_api_test_fail_field_key_alloc(uint32_t nth);

enum ApiMode {
  API_CONCAT,
  API_GETTABLE,
  API_GETFIELD,
  API_SETTABLE,
  API_SETFIELD
};

static lua_State *target;
static enum ApiMode mode;
static TGState *saved_hint;
static LJStateOwner saved_owner;
static ptrdiff_t saved_topofs;
static void *saved_cframe;
static uint32_t saved_roots;
static int force_key_oom;
static int busy_ref;

static void snapshot_target(lua_State *caller)
{
  TGState *tg = L2TG(caller);
  assert(target != NULL && lj_tg_load_cur_L(tg) == caller);
  saved_hint = target->tg_hint;
  saved_owner = lj_state_owner_word_acq(target);
  saved_topofs = savestack(target, target->top);
  saved_cframe = target->cframe;
  saved_roots = lj_tg_root_anchor_top_acq(tg);
  assert(lj_state_owner_tid(saved_owner) == 0);
}

static void assert_transport_restored(lua_State *caller)
{
  assert(lj_state_owner_word_acq(target) == saved_owner);
  assert(target->tg_hint == saved_hint);
  assert(target->cframe == saved_cframe);
  assert(lj_tg_load_cur_L(L2TG(caller)) == caller);
  assert(lj_tg_root_anchor_top_acq(L2TG(caller)) == saved_roots);
}

static int invoke_success(lua_State *L)
{
  snapshot_target(L);
  switch (mode) {
  case API_CONCAT:
    lua_concat(target, 2);
    assert(lua_gettop(target) == 1);
    assert(strcmp(lua_tostring(target, -1), "concat-ok") == 0);
    break;
  case API_GETTABLE:
    lua_gettable(target, -2);
    assert(lua_gettop(target) == 2);
    assert(strcmp(lua_tostring(target, -1), "get:key") == 0);
    break;
  case API_GETFIELD:
    lua_getfield(target, -1, "named");
    assert(lua_gettop(target) == 2);
    assert(strcmp(lua_tostring(target, -1), "field:named") == 0);
    break;
  case API_SETTABLE:
    lua_settable(target, -3);
    assert(lua_gettop(target) == 1);
    lua_getfield(target, -1, "seen");
    assert(strcmp(lua_tostring(target, -1), "key:value") == 0);
    lua_pop(target, 1);
    break;
  case API_SETFIELD:
    lua_setfield(target, -2, "named");
    assert(lua_gettop(target) == 1);
    lua_getfield(target, -1, "seen");
    assert(strcmp(lua_tostring(target, -1), "named:value") == 0);
    lua_pop(target, 1);
    break;
  }
  assert_transport_restored(L);
  return 0;
}

static int invoke_concat_negative(lua_State *L)
{
  ptrdiff_t topofs;
  snapshot_target(L);
  topofs = savestack(target, target->top);
  lua_concat(target, -1);
  assert(savestack(target, target->top) == topofs);
  assert(lua_tointeger(target, 1) == 17);
  assert(lua_tointeger(target, 2) == 23);
  assert_transport_restored(L);
  return 0;
}

static int invoke_error(lua_State *L)
{
  snapshot_target(L);
  if (force_key_oom)
    lj_api_test_fail_field_key_alloc(1);
  switch (mode) {
  case API_CONCAT: lua_concat(target, 2); break;
  case API_GETTABLE: lua_gettable(target, -2); break;
  case API_GETFIELD: lua_getfield(target, -1, "named"); break;
  case API_SETTABLE: lua_settable(target, -3); break;
  case API_SETFIELD: lua_setfield(target, -2, "named"); break;
  }
  assert(0 && "ownerless generic metamethod error returned");
  return 0;
}

static int stop_metamethod(lua_State *L)
{
  lj_safepoint_checkstop(L, LJ_GC2_HS_STOPREQ);
  assert(0 && "STOPREQ metamethod returned");
  return 0;
}

static void load_ownerless(lua_State *L, const char *chunk, int nres)
{
  target = lua_newthread(L);
  assert(luaL_loadstring(target, chunk) == LUA_OK);
  lua_call(target, 0, nres);
  assert(lua_gettop(target) == nres);
  assert(lj_state_owner_acq(target) == 0);
}

static void run_success(lua_State *L, enum ApiMode testmode,
			const char *chunk, int nres)
{
  lua_settop(L, 0);
  load_ownerless(L, chunk, nres);
  mode = testmode;
  lua_pushcfunction(L, invoke_success);
  assert(lua_pcall(L, 0, 0, 0) == LUA_OK);
  assert(lj_state_owner_acq(target) == 0);
  lua_settop(L, 0);
  target = NULL;
}

static void run_error(lua_State *L, enum ApiMode testmode,
		      const char *chunk, int nres, int expected_status,
		      int key_oom, const char *message)
{
  uint32_t roots = lj_tg_root_anchor_top_acq(L2TG(L));
  lua_settop(L, 0);
  load_ownerless(L, chunk, nres);
  mode = testmode;
  force_key_oom = key_oom;
  lua_pushcfunction(L, invoke_error);
  assert(lua_pcall(L, 0, 0, 0) == expected_status);
  assert(lua_tostring(L, -1) != NULL);
  assert(strstr(lua_tostring(L, -1), message) != NULL);
  assert(savestack(target, target->top) == saved_topofs);
  assert_transport_restored(L);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == roots);
  assert(lua_rawequal(target, 1, 1) == 1);
  assert(lj_tg_load_cur_L(L2TG(L)) == L);
  assert(lj_state_owner_word_acq(target) == saved_owner);
  lua_settop(L, 0);
  target = NULL;
  force_key_oom = 0;
}

static void test_success(lua_State *L)
{
  run_success(L, API_CONCAT,
    "local mt={__concat=function() collectgarbage(); return 'concat-ok' end}; "
    "return setmetatable({},mt),setmetatable({},mt)", 2);
  run_success(L, API_GETTABLE,
    "local mt={__index=function(_,k) collectgarbage(); return 'get:'..k end}; "
    "return setmetatable({},mt),'key'", 2);
  run_success(L, API_GETFIELD,
    "local mt={__index=function(_,k) collectgarbage(); return 'field:'..k end}; "
    "return setmetatable({},mt)", 1);
  run_success(L, API_SETTABLE,
    "local mt={__newindex=function(t,k,v) collectgarbage(); "
    "rawset(t,'seen',k..':'..v) end}; "
    "return setmetatable({},mt),'key','value'", 3);
  run_success(L, API_SETFIELD,
    "local mt={__newindex=function(t,k,v) collectgarbage(); "
    "rawset(t,'seen',k..':'..v) end}; "
    "return setmetatable({},mt),'value'", 2);

  lua_settop(L, 0);
  load_ownerless(L, "return 17, 23", 2);
  lua_pushcfunction(L, invoke_concat_negative);
  assert(lua_pcall(L, 0, 0, 0) == LUA_OK);
  assert(lj_state_owner_acq(target) == 0);
  lua_settop(L, 0);
  target = NULL;
}

static void test_errors(lua_State *L)
{
  run_error(L, API_CONCAT,
    "local mt={__concat=function() "
    "error('ownerless lua_concat metamethod error') end}; "
    "return setmetatable({},mt),setmetatable({},mt)", 2,
    LUA_ERRRUN, 0, "ownerless lua_concat metamethod error");
  run_error(L, API_GETTABLE,
    "local mt={__index=function() "
    "error('ownerless lua_gettable metamethod error') end}; "
    "return setmetatable({},mt),'key'", 2,
    LUA_ERRRUN, 0, "ownerless lua_gettable metamethod error");
  run_error(L, API_GETFIELD,
    "local mt={__index=function() "
    "error('ownerless lua_getfield metamethod error') end}; "
    "return setmetatable({},mt)", 1,
    LUA_ERRRUN, 0, "ownerless lua_getfield metamethod error");
  run_error(L, API_SETTABLE,
    "local mt={__newindex=function() "
    "error('ownerless lua_settable metamethod error') end}; "
    "return setmetatable({},mt),'key','value'", 3,
    LUA_ERRRUN, 0, "ownerless lua_settable metamethod error");
  run_error(L, API_SETFIELD,
    "local mt={__newindex=function() "
    "error('ownerless lua_setfield metamethod error') end}; "
    "return setmetatable({},mt),'value'", 2,
    LUA_ERRRUN, 0, "ownerless lua_setfield metamethod error");

  lua_pushcfunction(L, stop_metamethod);
  lua_setglobal(L, "ownerless_stop_metamethod");
  run_error(L, API_GETFIELD,
    "return setmetatable({}, {__index=ownerless_stop_metamethod})", 1,
    LUA_ERRRUN, 0, "thread interrupted: VM shutdown");

  run_error(L, API_GETFIELD, "return {}", 1,
    LUA_ERRMEM, 1, "not enough memory");
  run_error(L, API_SETFIELD, "return {}, 'value'", 2,
    LUA_ERRMEM, 1, "not enough memory");
}

static uint32_t foreign_tid(lua_State *L)
{
  uint32_t tid = lj_thr_current_id(G(L)) + 1000u;
  return tid == 0 || tid == LJ_THREAD_GCSCAN ? 123u : tid;
}

static int invoke_busy(lua_State *L)
{
  uint32_t tid = lj_thr_current_id(G(L));
  int nslot = mode == API_GETFIELD ? 1 :
              mode == API_SETFIELD || mode == API_CONCAT ||
              mode == API_GETTABLE ? 2 : 3;
  int i;
  target = lua_newthread(L);
  assert(lj_state_claim(target, tid));
  for (i = 0; i < nslot; i++)
    lua_pushnil(target);
  lj_state_release(target, tid);
  lua_pushvalue(L, -1);
  busy_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_pop(L, 1);
  lj_state_owner_rel(target, foreign_tid(L));
  saved_hint = target->tg_hint;
  saved_owner = lj_state_owner_word_acq(target);
  saved_topofs = savestack(target, target->top);
  saved_cframe = target->cframe;
  switch (mode) {
  case API_CONCAT: lua_concat(target, 2); break;
  case API_GETTABLE: lua_gettable(target, -2); break;
  case API_GETFIELD: lua_getfield(target, -1, "named"); break;
  case API_SETTABLE: lua_settable(target, -3); break;
  case API_SETFIELD: lua_setfield(target, -2, "named"); break;
  }
  assert(0 && "foreign-busy generic API returned");
  return 0;
}

static void test_busy(lua_State *L)
{
  enum ApiMode testmode;
  for (testmode = API_CONCAT; testmode <= API_SETFIELD; testmode++) {
    uint32_t roots = lj_tg_root_anchor_top_acq(L2TG(L));
    int top = lua_gettop(L);
    const char *msg;
    mode = testmode;
    busy_ref = LUA_NOREF;
    lua_pushcfunction(L, invoke_busy);
    assert(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
    msg = lua_tostring(L, -1);
    assert(msg != NULL && strstr(msg, "thread busy") != NULL);
    assert(lua_gettop(L) == top+1);  /* Only the protected error result. */
    assert(savestack(target, target->top) == saved_topofs);
    assert(lj_state_owner_word_acq(target) == saved_owner);
    assert(target->tg_hint == saved_hint);
    assert(target->cframe == saved_cframe);
    assert(lj_tg_root_anchor_top_acq(L2TG(L)) == roots);
    lj_state_owner_rel(target, 0);
    luaL_unref(L, LUA_REGISTRYINDEX, busy_ref);
    busy_ref = LUA_NOREF;
    target = NULL;
    lua_pop(L, 1);
    assert(lua_gettop(L) == top);
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  TGState *tg;
  uint32_t roots;
  assert(L != NULL);
  luaL_openlibs(L);
  tg = L2TG(L);
  roots = lj_tg_root_anchor_top_acq(tg);
  test_success(L);
  test_errors(L);
  test_busy(L);
  lua_settop(L, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lj_tg_root_anchor_top_acq(tg) == roots);
  lua_close(L);
  puts("arm64 C API generic meta calls: OK");
  return 0;
}
