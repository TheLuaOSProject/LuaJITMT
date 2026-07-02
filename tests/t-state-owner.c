/*
** Focused guard for M5 lua_State owner claims on foreign-state APIs.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

#include "lj_obj.h"
#include "lj_thr.h"

#include "lib/lua_fixture_helpers.h"

static void expect_thread_busy(lua_State *L, lua_CFunction fn,
			       const char *what)
{
  int status;
  lua_pushcfunction(L, fn);
  status = lua_pcall(L, 0, 0, 0);
  assert(status == LUA_ERRRUN);
  assert(lua_tostring(L, -1) != NULL);
  assert(strstr(lua_tostring(L, -1), "thread busy") != NULL);
  lua_pop(L, 1);
  (void)what;
}

static uint32_t foreign_tid(lua_State *L)
{
  uint32_t tid = lj_thr_current_id(G(L)) + 1000u;
  if (tid == 0 || tid == LJ_THREAD_GCSCAN)
    tid = 123u;
  return tid;
}

static void check_xmove_unowned_target(lua_State *L)
{
  lua_State *co;
  int top0;
  lua_settop(L, 0);
  co = lua_newthread(L);
  top0 = lua_gettop(L);
  assert(lj_state_owner_acq(co) == 0);
  lua_pushinteger(L, 42);
  lua_xmove(L, co, 1);
  assert(lj_state_owner_acq(co) == 0);
  assert(lua_tointeger(co, -1) == 42);
  assert(lua_gettop(L) == top0);
  lua_pop(co, 1);
  lua_pop(L, 1);
}

static void check_xmove_unowned_source(lua_State *L)
{
  uint32_t tid = lj_thr_current_id(G(L));
  lua_State *co;
  lua_settop(L, 0);
  co = lua_newthread(L);
  assert(lj_state_owner_acq(co) == 0);
  assert(lj_state_claim(co, tid));
  lua_pushinteger(co, 77);
  lj_state_release(co, tid);
  assert(lj_state_owner_acq(co) == 0);
  lua_xmove(co, L, 1);
  assert(lj_state_owner_acq(co) == 0);
  assert(lua_tointeger(L, -1) == 77);
  assert(lua_gettop(co) == 0);
  lua_pop(L, 2);
}

static void check_stack_api_unowned(lua_State *L)
{
  lua_State *co;
  lua_settop(L, 0);
  co = lua_newthread(L);
  assert(lua_checkstack(co, 8));
  assert(lj_state_owner_acq(co) == 0);
  assert(luaL_loadstring(co, "return 1, 2, 3") == 0);
  lua_call(co, 0, 3);
  assert(lj_state_owner_acq(co) == 0);
  assert(lua_gettop(co) == 3);
  lua_remove(co, 2);
  assert(lua_gettop(co) == 2);
  assert(lua_tointeger(co, 1) == 1 && lua_tointeger(co, 2) == 3);
  lua_pushvalue(co, 1);
  assert(lua_gettop(co) == 3 && lua_tointeger(co, 3) == 1);
  lua_insert(co, 2);
  assert(lua_tointeger(co, 1) == 1);
  assert(lua_tointeger(co, 2) == 1);
  assert(lua_tointeger(co, 3) == 3);
  lua_settop(co, 5);
  assert(lua_gettop(co) == 5);
  assert(lua_isnil(co, 4) && lua_isnil(co, 5));
  lua_settop(co, -2);
  assert(lua_gettop(co) == 4);
  assert(lj_state_owner_acq(co) == 0);
  lua_pop(L, 1);
}

static void check_thread_env_unowned(lua_State *L)
{
  lua_State *co;
  GCtab *env;
  lua_settop(L, 0);
  co = lua_newthread(L);
  assert(lj_state_owner_acq(co) == 0);
  lua_newtable(L);
  env = tabV(L->top-1);
  assert(lua_setfenv(L, -2) == 1);
  assert(lj_state_owner_acq(co) == 0);
  lua_getfenv(L, -1);
  assert(tabV(L->top-1) == env);
  assert(lj_state_owner_acq(co) == 0);
  lua_pop(L, 2);
}

static int resume_return(lua_State *L)
{
  lua_pushinteger(L, 91);
  return 1;
}

static void check_call_entry_unowned(lua_State *L)
{
  lua_State *co;
  lua_settop(L, 0);

  co = lua_newthread(L);
  assert(luaL_loadstring(L, "return 95") == 0);
  lua_xmove(L, co, 1);
  assert(lj_state_owner_acq(co) == 0);
  lua_call(co, 0, 1);
  assert(lua_tointeger(co, -1) == 95);
  assert(lj_state_owner_acq(co) == 0);
  lua_pop(co, 1);

  co = lua_newthread(L);
  assert(luaL_loadstring(L, "return 96") == 0);
  lua_xmove(L, co, 1);
  assert(lj_state_owner_acq(co) == 0);
  assert(lua_pcall(co, 0, 1, 0) == 0);
  assert(lua_tointeger(co, -1) == 96);
  assert(lj_state_owner_acq(co) == 0);
  lua_pop(co, 1);

  co = lua_newthread(L);
  assert(lj_state_owner_acq(co) == 0);
  assert(lua_cpcall(co, resume_return, NULL) == 0);
  assert(lj_state_owner_acq(co) == 0);
  lua_settop(L, 0);
}

static lua_State *load_ownerless_results(lua_State *L, const char *src,
					 int nres)
{
  lua_State *co = lua_newthread(L);
  assert(luaL_loadstring(co, src) == 0);
  lua_call(co, 0, nres);
  assert(lj_state_owner_acq(co) == 0);
  return co;
}

static void check_metamethod_api_unowned(lua_State *L)
{
  lua_State *co;
  lua_settop(L, 0);

  co = load_ownerless_results(L,
    "local mt={__eq=function(a,b)return true end};"
    "return setmetatable({},mt), setmetatable({},mt)", 2);
  assert(lua_equal(co, -1, -2) == 1);
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L,
    "local mt={__lt=function(a,b)return true end};"
    "return setmetatable({},mt), setmetatable({},mt)", 2);
  assert(lua_lessthan(co, -2, -1) == 1);
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L,
    "local mt={__concat=function(a,b)return 'joined' end};"
    "return setmetatable({},mt), setmetatable({},mt)", 2);
  lua_concat(co, 2);
  assert(strcmp(lua_tostring(co, -1), "joined") == 0);
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L,
    "return setmetatable({}, {__index=function(t,k)return 42 end}), 'x'", 2);
  lua_gettable(co, -2);
  assert(lua_tointeger(co, -1) == 42);
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L,
    "return setmetatable({}, {__index=function(t,k)return 43 end})", 1);
  lua_getfield(co, -1, "x");
  assert(lua_tointeger(co, -1) == 43);
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L,
    "return setmetatable({}, {__newindex=function(t,k,v) rawset(t,k,v+1) end}),"
    "'x', 41", 3);
  lua_settable(co, -3);
  lua_getfield(co, -1, "x");
  assert(lua_tointeger(co, -1) == 42);
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L,
    "return setmetatable({}, {__newindex=function(t,k,v) rawset(t,k,v+1) end}),"
    "41", 2);
  lua_setfield(co, -2, "x");
  lua_getfield(co, -1, "x");
  assert(lua_tointeger(co, -1) == 42);
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L,
    "return setmetatable({}, {__call=function() return 55 end})", 1);
  assert(luaL_callmeta(co, -1, "__call") == 1);
  assert(lua_tointeger(co, -1) == 55);
  assert(lj_state_owner_acq(co) == 0);

  lua_settop(L, 0);
}

static void check_resume_unowned(lua_State *L)
{
  uint32_t tid = lj_thr_current_id(G(L));
  lua_State *co;
  lua_settop(L, 0);
  co = lua_newthread(L);
  assert(lj_state_owner_acq(co) == 0);
  assert(lj_state_claim(co, tid));
  lua_pushcfunction(co, resume_return);
  lj_state_release(co, tid);
  assert(lj_state_owner_acq(co) == 0);
  assert(lua_resume(co, 0) == LUA_OK);
  assert(lj_state_owner_acq(co) == 0);
  assert(lj_state_claim(co, tid));
  assert(lua_tointeger(co, -1) == 91);
  lua_pop(co, 1);
  lj_state_release(co, tid);
  lua_pop(L, 1);
}

static void check_lua_load_unowned(lua_State *L)
{
  lua_State *co;
  lua_settop(L, 0);
  co = lua_newthread(L);
  assert(lj_state_owner_acq(co) == 0);
  assert(luaL_loadstring(co, "return 94") == 0);
  assert(lj_state_owner_acq(co) == 0);
  assert(lua_resume(co, 0) == LUA_OK);
  assert(lua_tointeger(co, -1) == 94);
  lua_pop(co, 1);
  lua_pop(L, 1);
}

static void check_lua_getinfo_unowned(lua_State *L)
{
  lua_State *co;
  lua_Debug ar;
  lua_settop(L, 0);
  co = lua_newthread(L);
  assert(luaL_loadstring(L, "local x = 1; coroutine.yield(); return x") == 0);
  lua_xmove(L, co, 1);
  assert(lua_resume(co, 0) == LUA_YIELD);
  assert(lj_state_owner_acq(co) == 0);
  memset(&ar, 0, sizeof(ar));
  assert(lua_getstack(co, 1, &ar) == 1);
  assert(lua_getinfo(co, "Slf", &ar) == 1);
  assert(lua_isfunction(co, -1));
  lua_pop(co, 1);
  assert(lj_state_owner_acq(co) == 0);
  assert(lua_getinfo(co, "L", &ar) == 1);
  assert(lua_istable(co, -1));
  lua_pop(co, 1);
  assert(lj_state_owner_acq(co) == 0);
  lua_settop(L, 0);
}

static void check_coroutine_resume_unowned(lua_State *L)
{
  uint32_t tid = lj_thr_current_id(G(L));
  lua_State *co;
  int coidx;
  lua_settop(L, 0);
  co = lua_newthread(L);
  coidx = lua_gettop(L);
  assert(lj_state_owner_acq(co) == 0);
  assert(lj_state_claim(co, tid));
  lua_pushcfunction(co, resume_return);
  lj_state_release(co, tid);
  assert(lj_state_owner_acq(co) == 0);
  lua_getglobal(L, "coroutine");
  lua_getfield(L, -1, "resume");
  lua_pushvalue(L, coidx);
  lua_call(L, 1, 2);
  assert(lua_toboolean(L, -2));
  assert(lua_tointeger(L, -1) == 91);
  assert(lj_state_owner_acq(co) == 0);
  lua_settop(L, 0);
}

static void check_coroutine_wrap_unowned(lua_State *L)
{
  lua_State *co;
  lua_settop(L, 0);
  lua_getglobal(L, "coroutine");
  lua_getfield(L, -1, "wrap");
  lua_pushcfunction(L, resume_return);
  lua_call(L, 1, 1);
  assert(lua_getupvalue(L, -1, 1) != NULL);
  co = lua_tothread(L, -1);
  assert(co != NULL);
  assert(lj_state_owner_acq(co) == 0);
  lua_pop(L, 1);
  lua_call(L, 0, 1);
  assert(lua_tointeger(L, -1) == 91);
  assert(lj_state_owner_acq(co) == 0);
  lua_settop(L, 0);
}

static int busy_xmove_target(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_pushinteger(L, 1);
  lua_xmove(L, co, 1);
  return 0;
}

static int busy_xmove_source(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  uint32_t tid = lj_thr_current_id(G(L));
  assert(lj_state_claim(co, tid));
  lua_pushinteger(co, 2);
  lj_state_release(co, tid);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_xmove(co, L, 1);
  return 0;
}

static int busy_lua_status(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  (void)lua_status(co);
  return 0;
}

static void busy_stack_prepare(lua_State *L, lua_State *co)
{
  uint32_t tid = lj_thr_current_id(G(L));
  assert(lj_state_claim(co, tid));
  lua_pushinteger(co, 1);
  lua_pushinteger(co, 2);
  lj_state_release(co, tid);
  lj_state_owner_rel(co, foreign_tid(L));
}

static int busy_lua_gettop(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  (void)lua_gettop(co);
  return 0;
}

static int busy_lua_checkstack(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  (void)lua_checkstack(co, 1);
  return 0;
}

static int busy_lua_settop(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_settop(co, 1);
  return 0;
}

static int busy_lua_remove(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_remove(co, 1);
  return 0;
}

static int busy_lua_insert(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_insert(co, 1);
  return 0;
}

static int busy_lua_pushvalue(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_pushvalue(co, 1);
  return 0;
}

static int busy_getfenv_thread(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_getfenv(L, -1);
  return 0;
}

static int busy_setfenv_thread(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_newtable(L);
  (void)lua_setfenv(L, -2);
  return 0;
}

static int busy_lua_resume(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  (void)lua_resume(co, 0);
  return 0;
}

static int busy_lua_call(lua_State *L)
{
  uint32_t tid = lj_thr_current_id(G(L));
  lua_State *co = lua_newthread(L);
  assert(lj_state_claim(co, tid));
  lua_pushcfunction(co, resume_return);
  lj_state_release(co, tid);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_call(co, 0, 0);
  return 0;
}

static int busy_lua_pcall(lua_State *L)
{
  uint32_t tid = lj_thr_current_id(G(L));
  lua_State *co = lua_newthread(L);
  assert(lj_state_claim(co, tid));
  lua_pushcfunction(co, resume_return);
  lj_state_release(co, tid);
  lj_state_owner_rel(co, foreign_tid(L));
  (void)lua_pcall(co, 0, 0, 0);
  return 0;
}

static int busy_lua_cpcall(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  (void)lua_cpcall(co, resume_return, NULL);
  return 0;
}

static int busy_lua_load(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  (void)luaL_loadstring(co, "return 1");
  return 0;
}

static int busy_coroutine_resume(lua_State *L)
{
  uint32_t tid = lj_thr_current_id(G(L));
  lua_State *co = lua_newthread(L);
  int coidx = lua_gettop(L);
  assert(lj_state_claim(co, tid));
  lua_pushcfunction(co, resume_return);
  lj_state_release(co, tid);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_getglobal(L, "coroutine");
  lua_getfield(L, -1, "resume");
  lua_pushvalue(L, coidx);
  lua_call(L, 1, LUA_MULTRET);
  return 0;
}

static int busy_coroutine_wrap(lua_State *L)
{
  lua_State *co;
  lua_getglobal(L, "coroutine");
  lua_getfield(L, -1, "wrap");
  lua_pushcfunction(L, resume_return);
  lua_call(L, 1, 1);
  assert(lua_getupvalue(L, -1, 1) != NULL);
  co = lua_tothread(L, -1);
  assert(co != NULL);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_pop(L, 1);
  lua_call(L, 0, LUA_MULTRET);
  return 0;
}

static int busy_lua_getstack(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_Debug ar;
  memset(&ar, 0, sizeof(ar));
  lj_state_owner_rel(co, foreign_tid(L));
  (void)lua_getstack(co, 0, &ar);
  return 0;
}

static int busy_lua_getinfo(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_Debug ar;
  memset(&ar, 0, sizeof(ar));
  lj_state_owner_rel(co, foreign_tid(L));
  (void)lua_getinfo(co, "S", &ar);
  return 0;
}

static int busy_lua_getlocal(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_Debug ar;
  memset(&ar, 0, sizeof(ar));
  lj_state_owner_rel(co, foreign_tid(L));
  (void)lua_getlocal(co, &ar, 1);
  return 0;
}

static int busy_lua_setlocal(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_Debug ar;
  memset(&ar, 0, sizeof(ar));
  lj_state_owner_rel(co, foreign_tid(L));
  (void)lua_setlocal(co, &ar, 1);
  return 0;
}

static int busy_debug_getinfo(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_getglobal(L, "debug");
  lua_getfield(L, -1, "getinfo");
  lua_pushvalue(L, -3);
  lua_pushinteger(L, 0);
  lua_call(L, 2, LUA_MULTRET);
  return 0;
}

static int busy_debug_getlocal(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_getglobal(L, "debug");
  lua_getfield(L, -1, "getlocal");
  lua_pushvalue(L, -3);
  lua_pushinteger(L, 0);
  lua_pushinteger(L, 1);
  lua_call(L, 3, LUA_MULTRET);
  return 0;
}

static int busy_debug_setlocal(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_getglobal(L, "debug");
  lua_getfield(L, -1, "setlocal");
  lua_pushvalue(L, -3);
  lua_pushinteger(L, 0);
  lua_pushinteger(L, 1);
  lua_pushinteger(L, 99);
  lua_call(L, 4, LUA_MULTRET);
  return 0;
}

static int busy_debug_traceback(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_getglobal(L, "debug");
  lua_getfield(L, -1, "traceback");
  lua_pushvalue(L, -3);
  lua_call(L, 1, LUA_MULTRET);
  return 0;
}

static int busy_luaL_traceback(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  luaL_traceback(L, co, "msg", 0);
  return 0;
}

#if LJ_HASPROFILE
static int busy_luaJIT_profile_dumpstack(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  size_t len = 0;
  lj_state_owner_rel(co, foreign_tid(L));
  (void)luaJIT_profile_dumpstack(co, "l", 1, &len);
  return 0;
}

static int busy_jit_profile_dumpstack(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_getglobal(L, "require");
  lua_pushliteral(L, "jit.profile");
  lua_call(L, 1, 1);
  lua_getfield(L, -1, "dumpstack");
  lua_pushvalue(L, -3);
  lua_pushliteral(L, "l");
  lua_pushinteger(L, 1);
  lua_call(L, 3, LUA_MULTRET);
  return 0;
}
#endif

static int busy_coroutine_status(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_getglobal(L, "coroutine");
  lua_getfield(L, -1, "status");
  lua_pushvalue(L, -3);
  lua_call(L, 1, 1);
  return 1;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  check_xmove_unowned_target(L);
  check_xmove_unowned_source(L);
  check_stack_api_unowned(L);
  check_thread_env_unowned(L);
  check_call_entry_unowned(L);
  check_metamethod_api_unowned(L);
  check_resume_unowned(L);
  check_lua_load_unowned(L);
  check_lua_getinfo_unowned(L);
  check_coroutine_resume_unowned(L);
  check_coroutine_wrap_unowned(L);
  ljt_lua_assert_ok(L, luaL_dostring(L,
    "local co = coroutine.create(function() return 92 end)\n"
    "local ok, v = coroutine.resume(co)\n"
    "assert(ok and v == 92)\n"
    "local f = coroutine.wrap(function() return 93 end)\n"
    "assert(f() == 93)\n"),
    "Lua-created coroutine resume/wrap smoke");
  ljt_lua_assert_ok(L, luaL_dostring(L,
    "local co = coroutine.create(function()\n"
    "  local x = 41\n"
    "  coroutine.yield('pause')\n"
    "  return x\n"
    "end)\n"
    "assert(coroutine.resume(co))\n"
    "local info = debug.getinfo(co, 1, 'flnSu')\n"
    "assert(type(info) == 'table' and type(info.func) == 'function')\n"
    "local name, value = debug.getlocal(co, 1, 1)\n"
    "assert(name == 'x' and value == 41)\n"
    "assert(debug.setlocal(co, 1, 1, 42) == 'x')\n"
    "local _, changed = debug.getlocal(co, 1, 1)\n"
    "assert(changed == 42)\n"
    "assert(type(debug.traceback(co, 'msg')) == 'string')\n"
    "local ok, result = coroutine.resume(co)\n"
    "assert(ok and result == 42)\n"),
    "debug coroutine smoke");
  expect_thread_busy(L, busy_xmove_target, "busy target xmove");
  expect_thread_busy(L, busy_xmove_source, "busy source xmove");
  expect_thread_busy(L, busy_lua_status, "busy lua_status");
  expect_thread_busy(L, busy_lua_gettop, "busy lua_gettop");
  expect_thread_busy(L, busy_lua_checkstack, "busy lua_checkstack");
  expect_thread_busy(L, busy_lua_settop, "busy lua_settop");
  expect_thread_busy(L, busy_lua_remove, "busy lua_remove");
  expect_thread_busy(L, busy_lua_insert, "busy lua_insert");
  expect_thread_busy(L, busy_lua_pushvalue, "busy lua_pushvalue");
  expect_thread_busy(L, busy_getfenv_thread, "busy thread getfenv");
  expect_thread_busy(L, busy_setfenv_thread, "busy thread setfenv");
  expect_thread_busy(L, busy_lua_call, "busy lua_call");
  expect_thread_busy(L, busy_lua_pcall, "busy lua_pcall");
  expect_thread_busy(L, busy_lua_cpcall, "busy lua_cpcall");
  expect_thread_busy(L, busy_lua_resume, "busy lua_resume");
  expect_thread_busy(L, busy_lua_load, "busy lua_load");
  expect_thread_busy(L, busy_coroutine_resume, "busy coroutine.resume");
  expect_thread_busy(L, busy_coroutine_wrap, "busy coroutine.wrap");
  expect_thread_busy(L, busy_lua_getstack, "busy lua_getstack");
  expect_thread_busy(L, busy_lua_getinfo, "busy lua_getinfo");
  expect_thread_busy(L, busy_lua_getlocal, "busy lua_getlocal");
  expect_thread_busy(L, busy_lua_setlocal, "busy lua_setlocal");
  expect_thread_busy(L, busy_debug_getinfo, "busy debug.getinfo");
  expect_thread_busy(L, busy_debug_getlocal, "busy debug.getlocal");
  expect_thread_busy(L, busy_debug_setlocal, "busy debug.setlocal");
  expect_thread_busy(L, busy_debug_traceback, "busy debug.traceback");
  expect_thread_busy(L, busy_luaL_traceback, "busy luaL_traceback");
#if LJ_HASPROFILE
  expect_thread_busy(L, busy_luaJIT_profile_dumpstack,
		     "busy luaJIT_profile_dumpstack");
  expect_thread_busy(L, busy_jit_profile_dumpstack,
		     "busy jit.profile.dumpstack");
#endif
  expect_thread_busy(L, busy_coroutine_status, "busy coroutine.status");
  ljt_lua_assert_ok(L, luaL_dostring(L,
    "local co = coroutine.create(function() coroutine.yield(1) end)\n"
    "assert(coroutine.status(co) == 'suspended')\n"),
    "coroutine.status smoke");
  lua_close(L);
  printf("t-state-owner OK: foreign lua_State owner guards verified\n");
  return 0;
}
