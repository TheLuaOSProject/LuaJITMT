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

static void check_lua_ok(lua_State *L, int status, const char *what)
{
  if (status != LUA_OK) {
    fprintf(stderr, "%s: %s\n", what, lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

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
  assert(co->thr_owner == 0);
  lua_pushinteger(L, 42);
  lua_xmove(L, co, 1);
  assert(co->thr_owner == 0);
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
  assert(co->thr_owner == 0);
  assert(lj_state_claim(co, tid));
  lua_pushinteger(co, 77);
  lj_state_release(co, tid);
  assert(co->thr_owner == 0);
  lua_xmove(co, L, 1);
  assert(co->thr_owner == 0);
  assert(lua_tointeger(L, -1) == 77);
  assert(lua_gettop(co) == 0);
  lua_pop(L, 2);
}

static void check_thread_env_unowned(lua_State *L)
{
  lua_State *co;
  GCtab *env;
  lua_settop(L, 0);
  co = lua_newthread(L);
  assert(co->thr_owner == 0);
  lua_newtable(L);
  env = tabV(L->top-1);
  assert(lua_setfenv(L, -2) == 1);
  assert(co->thr_owner == 0);
  lua_getfenv(L, -1);
  assert(tabV(L->top-1) == env);
  assert(co->thr_owner == 0);
  lua_pop(L, 2);
}

static int resume_return(lua_State *L)
{
  lua_pushinteger(L, 91);
  return 1;
}

static void check_resume_unowned(lua_State *L)
{
  uint32_t tid = lj_thr_current_id(G(L));
  lua_State *co;
  lua_settop(L, 0);
  co = lua_newthread(L);
  assert(co->thr_owner == 0);
  assert(lj_state_claim(co, tid));
  lua_pushcfunction(co, resume_return);
  lj_state_release(co, tid);
  assert(co->thr_owner == 0);
  assert(lua_resume(co, 0) == LUA_OK);
  assert(co->thr_owner == 0);
  assert(lj_state_claim(co, tid));
  assert(lua_tointeger(co, -1) == 91);
  lua_pop(co, 1);
  lj_state_release(co, tid);
  lua_pop(L, 1);
}

static void check_coroutine_resume_unowned(lua_State *L)
{
  uint32_t tid = lj_thr_current_id(G(L));
  lua_State *co;
  int coidx;
  lua_settop(L, 0);
  co = lua_newthread(L);
  coidx = lua_gettop(L);
  assert(co->thr_owner == 0);
  assert(lj_state_claim(co, tid));
  lua_pushcfunction(co, resume_return);
  lj_state_release(co, tid);
  assert(co->thr_owner == 0);
  lua_getglobal(L, "coroutine");
  lua_getfield(L, -1, "resume");
  lua_pushvalue(L, coidx);
  lua_call(L, 1, 2);
  assert(lua_toboolean(L, -2));
  assert(lua_tointeger(L, -1) == 91);
  assert(co->thr_owner == 0);
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
  assert(co->thr_owner == 0);
  lua_pop(L, 1);
  lua_call(L, 0, 1);
  assert(lua_tointeger(L, -1) == 91);
  assert(co->thr_owner == 0);
  lua_settop(L, 0);
}

static int busy_xmove_target(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  co->thr_owner = foreign_tid(L);
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
  co->thr_owner = foreign_tid(L);
  lua_xmove(co, L, 1);
  return 0;
}

static int busy_lua_status(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  co->thr_owner = foreign_tid(L);
  (void)lua_status(co);
  return 0;
}

static int busy_getfenv_thread(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  co->thr_owner = foreign_tid(L);
  lua_getfenv(L, -1);
  return 0;
}

static int busy_setfenv_thread(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  co->thr_owner = foreign_tid(L);
  lua_newtable(L);
  (void)lua_setfenv(L, -2);
  return 0;
}

static int busy_lua_resume(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  co->thr_owner = foreign_tid(L);
  (void)lua_resume(co, 0);
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
  co->thr_owner = foreign_tid(L);
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
  co->thr_owner = foreign_tid(L);
  lua_pop(L, 1);
  lua_call(L, 0, LUA_MULTRET);
  return 0;
}

static int busy_lua_getstack(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_Debug ar;
  memset(&ar, 0, sizeof(ar));
  co->thr_owner = foreign_tid(L);
  (void)lua_getstack(co, 0, &ar);
  return 0;
}

static int busy_lua_getinfo(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_Debug ar;
  memset(&ar, 0, sizeof(ar));
  co->thr_owner = foreign_tid(L);
  (void)lua_getinfo(co, "S", &ar);
  return 0;
}

static int busy_lua_getlocal(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_Debug ar;
  memset(&ar, 0, sizeof(ar));
  co->thr_owner = foreign_tid(L);
  (void)lua_getlocal(co, &ar, 1);
  return 0;
}

static int busy_lua_setlocal(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_Debug ar;
  memset(&ar, 0, sizeof(ar));
  co->thr_owner = foreign_tid(L);
  (void)lua_setlocal(co, &ar, 1);
  return 0;
}

static int busy_debug_getinfo(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  co->thr_owner = foreign_tid(L);
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
  co->thr_owner = foreign_tid(L);
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
  co->thr_owner = foreign_tid(L);
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
  co->thr_owner = foreign_tid(L);
  lua_getglobal(L, "debug");
  lua_getfield(L, -1, "traceback");
  lua_pushvalue(L, -3);
  lua_call(L, 1, LUA_MULTRET);
  return 0;
}

static int busy_luaL_traceback(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  co->thr_owner = foreign_tid(L);
  luaL_traceback(L, co, "msg", 0);
  return 0;
}

#if LJ_HASPROFILE
static int busy_luaJIT_profile_dumpstack(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  size_t len = 0;
  co->thr_owner = foreign_tid(L);
  (void)luaJIT_profile_dumpstack(co, "l", 1, &len);
  return 0;
}

static int busy_jit_profile_dumpstack(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  co->thr_owner = foreign_tid(L);
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
  co->thr_owner = foreign_tid(L);
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
  check_thread_env_unowned(L);
  check_resume_unowned(L);
  check_coroutine_resume_unowned(L);
  check_coroutine_wrap_unowned(L);
  check_lua_ok(L, luaL_dostring(L,
    "local co = coroutine.create(function() return 92 end)\n"
    "local ok, v = coroutine.resume(co)\n"
    "assert(ok and v == 92)\n"
    "local f = coroutine.wrap(function() return 93 end)\n"
    "assert(f() == 93)\n"),
    "Lua-created coroutine resume/wrap smoke");
  check_lua_ok(L, luaL_dostring(L,
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
  expect_thread_busy(L, busy_getfenv_thread, "busy thread getfenv");
  expect_thread_busy(L, busy_setfenv_thread, "busy thread setfenv");
  expect_thread_busy(L, busy_lua_resume, "busy lua_resume");
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
  check_lua_ok(L, luaL_dostring(L,
    "local co = coroutine.create(function() coroutine.yield(1) end)\n"
    "assert(coroutine.status(co) == 'suspended')\n"),
    "coroutine.status smoke");
  lua_close(L);
  printf("t-state-owner OK: foreign lua_State owner guards verified\n");
  return 0;
}
