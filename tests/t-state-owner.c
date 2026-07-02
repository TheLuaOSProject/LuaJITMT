/*
** Focused guard for M5 lua_State owner claims on foreign-state APIs.
*/

#include <assert.h>
#include <stdarg.h>
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

static int resume_return(lua_State *L);
static int c_upvalue_return(lua_State *L);
static lua_State *load_ownerless_results(lua_State *L, const char *src,
					 int nres);

static const char *push_vfstring(lua_State *L, const char *fmt, ...)
{
  const char *ret;
  va_list argp;
  va_start(argp, fmt);
  ret = lua_pushvfstring(L, fmt, argp);
  va_end(argp);
  return ret;
}

static void expect_thread_busy(lua_State *L, lua_CFunction fn,
			       const char *what)
{
  int status;
  const char *msg;
  lua_pushcfunction(L, fn);
  status = lua_pcall(L, 0, 0, 0);
  if (status != LUA_ERRRUN) {
    fprintf(stderr, "%s: expected LUA_ERRRUN, got %d\n", what, status);
    assert(status == LUA_ERRRUN);
  }
  msg = lua_tostring(L, -1);
  if (msg == NULL || strstr(msg, "thread busy") == NULL) {
    fprintf(stderr, "%s: unexpected error object type=%s message=%s\n",
	    what, lua_typename(L, lua_type(L, -1)), msg ? msg : "<null>");
    assert(msg != NULL);
    assert(strstr(msg, "thread busy") != NULL);
  }
  lua_pop(L, 1);
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
  int marker = 0;
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
  lua_copy(co, 3, 1);
  assert(lua_tointeger(co, 1) == 3);
  lua_pushvalue(co, 2);
  lua_replace(co, 3);
  assert(lua_gettop(co) == 3);
  assert(lua_tointeger(co, 2) == 1);
  assert(lua_tointeger(co, 3) == 1);
  lua_pushnil(co);
  assert(lua_gettop(co) == 4 && lua_isnil(co, 4));
  lua_pushnumber(co, 4.5);
  assert(lua_gettop(co) == 5 && lua_tonumber(co, 5) == 4.5);
  lua_pushinteger(co, 6);
  assert(lua_gettop(co) == 6 && lua_tointeger(co, 6) == 6);
  lua_pushboolean(co, 1);
  assert(lua_gettop(co) == 7 && lua_toboolean(co, 7));
  assert(lua_pushthread(co) == 0);
  assert(lua_gettop(co) == 8 && lua_tothread(co, 8) == co);
  lua_pushlstring(co, "hi\0x", 4);
  assert(lua_gettop(co) == 9);
  assert(lua_objlen(co, 9) == 4);
  assert(memcmp(lua_tolstring(co, 9, NULL), "hi\0x", 4) == 0);
  lua_pushstring(co, "ownerless");
  assert(lua_gettop(co) == 10);
  assert(strcmp(lua_tostring(co, 10), "ownerless") == 0);
  lua_pushstring(co, NULL);
  assert(lua_gettop(co) == 11 && lua_isnil(co, 11));
  assert(strcmp(push_vfstring(co, "vf:%s:%d", "ownerless", 21),
		"vf:ownerless:21") == 0);
  assert(lua_gettop(co) == 12);
  assert(strcmp(lua_tostring(co, 12), "vf:ownerless:21") == 0);
  assert(strcmp(lua_pushfstring(co, "f:%s:%d", "ownerless", 22),
		"f:ownerless:22") == 0);
  assert(lua_gettop(co) == 13);
  assert(strcmp(lua_tostring(co, 13), "f:ownerless:22") == 0);
  lua_pushlightuserdata(co, &marker);
  assert(lua_gettop(co) == 14 && lua_touserdata(co, 14) == &marker);
  lua_createtable(co, 2, 1);
  assert(lua_gettop(co) == 15 && lua_type(co, 15) == LUA_TTABLE);
  assert(lua_newuserdata(co, 4) != NULL);
  assert(lua_gettop(co) == 16 && lua_isuserdata(co, 16));
  assert(lua_newthread(co) == lua_tothread(co, 17));
  assert(lua_gettop(co) == 17 && lua_type(co, 17) == LUA_TTHREAD);
  lua_pushcclosure(co, resume_return, 0);
  assert(lua_gettop(co) == 18 && lua_iscfunction(co, 18));
  lua_call(co, 0, 1);
  assert(lua_gettop(co) == 18 && lua_tointeger(co, 18) == 91);
  lua_pushinteger(co, 70);
  lua_pushcclosure(co, c_upvalue_return, 1);
  assert(lua_gettop(co) == 19 && lua_iscfunction(co, 19));
  lua_call(co, 0, 1);
  assert(lua_gettop(co) == 19 && lua_tointeger(co, 19) == 70);
  lua_settop(co, 3);
  lua_settop(co, 5);
  assert(lua_gettop(co) == 5);
  assert(lua_isnil(co, 4) && lua_isnil(co, 5));
  lua_settop(co, -2);
  assert(lua_gettop(co) == 4);
  assert(lj_state_owner_acq(co) == 0);
  lua_pop(L, 1);
}

static void check_getter_api_unowned(lua_State *L)
{
  lua_State *co, *child;
  int ok = 0;
  int marker = 0;
  void *ud;
  lua_settop(L, 0);
  co = lua_newthread(L);
  assert(luaL_loadstring(co,
    "return 123, '45', true, {}, function() end") == 0);
  lua_call(co, 0, 5);
  lua_pushcfunction(L, resume_return);
  lua_xmove(L, co, 1);
  lua_pushlightuserdata(L, &marker);
  lua_xmove(L, co, 1);
  ud = lua_newuserdata(L, 4);
  lua_xmove(L, co, 1);
  child = lua_newthread(L);
  lua_xmove(L, co, 1);

  assert(lj_state_owner_acq(co) == 0);
  assert(lua_type(co, 1) == LUA_TNUMBER);
  assert(lua_iscfunction(co, 6));
  assert(lua_isnumber(co, 2));
  assert(lua_isstring(co, 1));
  assert(lua_isuserdata(co, 7) && lua_isuserdata(co, 8));
  assert(lua_rawequal(co, 1, 1));
  assert(lua_tonumberx(co, 2, &ok) == 45 && ok);
  assert(lua_tointegerx(co, 1, &ok) == 123 && ok);
  assert(lua_tonumber(co, 1) == 123);
  assert(lua_tointeger(co, 2) == 45);
  luaL_checkany(co, 1);
  assert(luaL_checknumber(co, 2) == 45);
  assert(luaL_optnumber(co, 10, 78) == 78);
  assert(luaL_checkinteger(co, 1) == 123);
  assert(luaL_optinteger(co, 10, 79) == 79);
  assert(lua_toboolean(co, 3));
  assert(lua_tocfunction(co, 6) == resume_return);
  assert(lua_touserdata(co, 7) == &marker);
  assert(lua_touserdata(co, 8) == ud);
  assert(lua_tothread(co, 9) == child);
  assert(lua_topointer(co, 4) != NULL);
  assert(lj_state_owner_acq(co) == 0);
  lua_settop(L, 0);
}

static void check_thread_env_unowned(lua_State *L)
{
  lua_State *co;
  GCtab *env, *udenv;
  void *ud;
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

  co = load_ownerless_results(L, "return function() end, {mark=74}", 2);
  assert(lua_setfenv(co, 1) == 1);
  assert(lua_gettop(co) == 1);
  lua_getfenv(co, 1);
  lua_getfield(co, -1, "mark");
  assert(lua_tointeger(co, -1) == 74);
  assert(lj_state_owner_acq(co) == 0);

  lua_settop(L, 0);
  co = lua_newthread(L);
  ud = lua_newuserdata(L, 4);
  lua_newtable(L);
  udenv = tabV(L->top-1);
  lua_pushinteger(L, 75);
  lua_setfield(L, -2, "mark");
  lua_xmove(L, co, 2);
  assert(lj_state_owner_acq(co) == 0);
  assert(lua_setfenv(co, 1) == 1);
  assert(lua_gettop(co) == 1);
  lua_getfenv(co, 1);
  assert(tabV(co->top-1) == udenv);
  lua_getfield(co, -1, "mark");
  assert(lua_tointeger(co, -1) == 75);
  assert(lua_touserdata(co, 1) == ud);
  assert(lj_state_owner_acq(co) == 0);
  lua_settop(L, 0);
}

static int resume_return(lua_State *L)
{
  lua_pushinteger(L, 91);
  return 1;
}

static int c_upvalue_return(lua_State *L)
{
  lua_pushvalue(L, lua_upvalueindex(1));
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

static void check_string_conversion_api_unowned(lua_State *L)
{
  lua_State *co;
  const char *s;
  size_t len;
  lua_settop(L, 0);

  co = load_ownerless_results(L, "return 123, 'abc', nil, {}, 456", 5);
  s = lua_tolstring(co, 1, &len);
  assert(s != NULL && strcmp(s, "123") == 0 && len == 3);
  assert(lua_type(co, 1) == LUA_TSTRING);
  s = luaL_checklstring(co, 2, &len);
  assert(s != NULL && strcmp(s, "abc") == 0 && len == 3);
  s = luaL_optlstring(co, 3, "fallback", &len);
  assert(s != NULL && strcmp(s, "fallback") == 0 && len == 8);
  s = luaL_checklstring(co, 5, &len);
  assert(s != NULL && strcmp(s, "456") == 0 && len == 3);
  s = lua_tolstring(co, 4, &len);
  assert(s == NULL && len == 0);
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L, "return 789, {1, 2, 3}, 'hello'", 3);
  assert(lua_objlen(co, 1) == 3);
  assert(lua_type(co, 1) == LUA_TSTRING);
  assert(lua_objlen(co, 2) == 3);
  assert(lua_objlen(co, 3) == 5);
  assert(lj_state_owner_acq(co) == 0);

  lua_settop(L, 0);
}

static void check_raw_object_api_unowned(lua_State *L)
{
  lua_State *co;
  lua_settop(L, 0);

  co = load_ownerless_results(L, "return {x=64}, 'x'", 2);
  lua_rawget(co, 1);
  assert(lua_tointeger(co, -1) == 64);
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L, "return {[4]=65}", 1);
  lua_rawgeti(co, 1, 4);
  assert(lua_tointeger(co, -1) == 65);
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L,
    "local mt={tag=66}; return setmetatable({}, mt)", 1);
  assert(lua_getmetatable(co, 1) == 1);
  lua_getfield(co, -1, "tag");
  assert(lua_tointeger(co, -1) == 66);
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L, "return function() end", 1);
  lua_getfenv(co, 1);
  assert(lua_istable(co, -1));
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L, "return {a=67}, nil", 2);
  assert(lua_next(co, 1) == 1);
  assert(lua_tointeger(co, -1) == 67);
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L, "return {}, 'raw', 71", 3);
  lua_rawset(co, 1);
  assert(lua_gettop(co) == 1);
  lua_getfield(co, 1, "raw");
  assert(lua_tointeger(co, -1) == 71);
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L, "return {}, 72", 2);
  lua_rawseti(co, 1, 5);
  assert(lua_gettop(co) == 1);
  lua_rawgeti(co, 1, 5);
  assert(lua_tointeger(co, -1) == 72);
  assert(lj_state_owner_acq(co) == 0);

  co = load_ownerless_results(L, "return {}, {tag=73}", 2);
  assert(lua_setmetatable(co, 1) == 1);
  assert(lua_gettop(co) == 1);
  assert(lua_getmetatable(co, 1) == 1);
  lua_getfield(co, -1, "tag");
  assert(lua_tointeger(co, -1) == 73);
  assert(lj_state_owner_acq(co) == 0);

  lua_settop(L, 0);
}

static void check_upvalue_api_unowned(lua_State *L)
{
  lua_State *co;
  const char *name;
  void *id;
  lua_settop(L, 0);

  co = load_ownerless_results(L,
    "local x=68; return function() return x end", 1);
  name = lua_getupvalue(co, 1, 1);
  assert(name != NULL && strcmp(name, "x") == 0);
  assert(lua_tointeger(co, -1) == 68);
  id = lua_upvalueid(co, 1, 1);
  assert(id != NULL);
  assert(lj_state_owner_acq(co) == 0);

  lua_settop(L, 0);
  co = load_ownerless_results(L,
    "local x=68; return function() return x end, 79", 2);
  name = lua_setupvalue(co, 1, 1);
  assert(name != NULL && strcmp(name, "x") == 0);
  assert(lua_gettop(co) == 1);
  lua_call(co, 0, 1);
  assert(lua_tointeger(co, -1) == 79);
  assert(lj_state_owner_acq(co) == 0);

  lua_settop(L, 0);
  co = lua_newthread(L);
  lua_pushinteger(L, 69);
  lua_pushcclosure(L, c_upvalue_return, 1);
  lua_xmove(L, co, 1);
  assert(lj_state_owner_acq(co) == 0);
  name = lua_getupvalue(co, 1, 1);
  assert(name != NULL && strcmp(name, "") == 0);
  assert(lua_tointeger(co, -1) == 69);
  id = lua_upvalueid(co, 1, 1);
  assert(id != NULL);
  assert(lj_state_owner_acq(co) == 0);

  lua_settop(L, 0);
  co = lua_newthread(L);
  lua_pushinteger(L, 69);
  lua_pushcclosure(L, c_upvalue_return, 1);
  lua_pushinteger(L, 80);
  lua_xmove(L, co, 2);
  assert(lj_state_owner_acq(co) == 0);
  name = lua_setupvalue(co, 1, 1);
  assert(name != NULL && strcmp(name, "") == 0);
  assert(lua_gettop(co) == 1);
  lua_call(co, 0, 1);
  assert(lua_tointeger(co, -1) == 80);
  assert(lj_state_owner_acq(co) == 0);

  lua_settop(L, 0);
  co = load_ownerless_results(L,
    "local function make(x) return function() return x end end\n"
    "return make(1), make(2)", 2);
  lua_upvaluejoin(co, 1, 1, 2, 1);
  lua_pushvalue(co, 1);
  lua_call(co, 0, 1);
  assert(lua_tointeger(co, -1) == 2);
  assert(lj_state_owner_acq(co) == 0);

  lua_settop(L, 0);
}

static void check_userdata_api_unowned(lua_State *L)
{
  lua_State *co;
  void *ud;
  lua_settop(L, 0);
  (void)luaL_newmetatable(L, "state_owner_udata");
  lua_pop(L, 1);
  co = lua_newthread(L);
  ud = lua_newuserdata(L, 4);
  luaL_getmetatable(L, "state_owner_udata");
  assert(lua_setmetatable(L, -2) == 1);
  lua_xmove(L, co, 1);
  assert(lj_state_owner_acq(co) == 0);
  assert(luaL_testudata(co, 1, "state_owner_udata") == ud);
  assert(luaL_checkudata(co, 1, "state_owner_udata") == ud);
  assert(luaL_testudata(co, 1, "state_owner_other_udata") == NULL);
  assert(lj_state_owner_acq(co) == 0);
  lua_settop(L, 0);
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

  co = load_ownerless_results(L,
    "return setmetatable({}, {__call=function() return 56 end})", 1);
  assert(luaL_getmetafield(co, 1, "__call") == 1);
  assert(lua_iscfunction(co, -1) == 0);
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

static int busy_lua_replace(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_replace(co, 1);
  return 0;
}

static int busy_lua_copy(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_copy(co, 1, 2);
  return 0;
}

static int busy_lua_pushnil(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_pushnil(co);
  return 0;
}

static int busy_lua_pushnumber(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_pushnumber(co, 3.5);
  return 0;
}

static int busy_lua_pushinteger(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_pushinteger(co, 3);
  return 0;
}

static int busy_lua_pushboolean(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_pushboolean(co, 1);
  return 0;
}

static int busy_lua_pushthread(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  (void)lua_pushthread(co);
  return 0;
}

static int busy_lua_pushlstring(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_pushlstring(co, "busy", 4);
  return 0;
}

static int busy_lua_pushstring(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_pushstring(co, "busy");
  return 0;
}

static int busy_lua_pushstring_null(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_pushstring(co, NULL);
  return 0;
}

static int busy_lua_pushfstring(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  (void)lua_pushfstring(co, "busy:%d", 1);
  return 0;
}

static int busy_lua_pushlightuserdata(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  int marker = 0;
  busy_stack_prepare(L, co);
  lua_pushlightuserdata(co, &marker);
  return 0;
}

static int busy_lua_createtable(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_createtable(co, 2, 1);
  return 0;
}

static int busy_lua_newuserdata(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  (void)lua_newuserdata(co, 4);
  return 0;
}

static int busy_lua_newthread_api(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  (void)lua_newthread(co);
  return 0;
}

static int busy_lua_pushcclosure_api(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  busy_stack_prepare(L, co);
  lua_pushcclosure(co, c_upvalue_return, 1);
  return 0;
}

static lua_State *busy_getter_prepare(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  int marker = 0;
  lua_pushinteger(L, 11);
  lua_xmove(L, co, 1);
  lua_pushliteral(L, "12");
  lua_xmove(L, co, 1);
  lua_pushcfunction(L, resume_return);
  lua_xmove(L, co, 1);
  lua_pushlightuserdata(L, &marker);
  lua_xmove(L, co, 1);
  lua_newthread(L);
  lua_xmove(L, co, 1);
  lj_state_owner_rel(co, foreign_tid(L));
  return co;
}

static int busy_lua_type(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)lua_type(co, 1);
  return 0;
}

static int busy_lua_isnumber(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)lua_isnumber(co, 1);
  return 0;
}

static int busy_lua_isstring(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)lua_isstring(co, 1);
  return 0;
}

static int busy_lua_isuserdata(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)lua_isuserdata(co, 4);
  return 0;
}

static int busy_lua_rawequal(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)lua_rawequal(co, 1, 1);
  return 0;
}

static int busy_lua_tonumber(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)lua_tonumber(co, 2);
  return 0;
}

static int busy_lua_tonumberx(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  int ok = 0;
  (void)lua_tonumberx(co, 2, &ok);
  return 0;
}

static int busy_lua_tointeger(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)lua_tointeger(co, 2);
  return 0;
}

static int busy_lua_tointegerx(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  int ok = 0;
  (void)lua_tointegerx(co, 2, &ok);
  return 0;
}

static int busy_luaL_checkany(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  luaL_checkany(co, 1);
  return 0;
}

static int busy_luaL_checknumber(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)luaL_checknumber(co, 2);
  return 0;
}

static int busy_luaL_optnumber(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)luaL_optnumber(co, 2, 0);
  return 0;
}

static int busy_luaL_checkinteger(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)luaL_checkinteger(co, 2);
  return 0;
}

static int busy_luaL_optinteger(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)luaL_optinteger(co, 2, 0);
  return 0;
}

static int busy_lua_toboolean(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)lua_toboolean(co, 1);
  return 0;
}

static int busy_lua_tocfunction(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)lua_tocfunction(co, 3);
  return 0;
}

static int busy_lua_touserdata(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)lua_touserdata(co, 4);
  return 0;
}

static int busy_lua_tothread(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)lua_tothread(co, 5);
  return 0;
}

static int busy_lua_topointer(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)lua_topointer(co, 1);
  return 0;
}

static int busy_lua_tolstring(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  size_t len = 0;
  (void)lua_tolstring(co, 1, &len);
  return 0;
}

static int busy_luaL_checklstring(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  size_t len = 0;
  (void)luaL_checklstring(co, 1, &len);
  return 0;
}

static int busy_luaL_optlstring(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  size_t len = 0;
  (void)luaL_optlstring(co, 1, "fallback", &len);
  return 0;
}

static int busy_lua_objlen(lua_State *L)
{
  lua_State *co = busy_getter_prepare(L);
  (void)lua_objlen(co, 1);
  return 0;
}

static lua_State *busy_table_prepare(lua_State *L, int push_key)
{
  lua_State *co = lua_newthread(L);
  lua_newtable(L);
  lua_pushliteral(L, "x");
  lua_pushinteger(L, 64);
  lua_settable(L, -3);
  lua_pushinteger(L, 65);
  lua_rawseti(L, -2, 4);
  if (push_key)
    lua_pushliteral(L, "x");
  else
    lua_pushnil(L);
  lua_xmove(L, co, 2);
  lj_state_owner_rel(co, foreign_tid(L));
  return co;
}

static lua_State *busy_metatable_prepare(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_newtable(L);
  lua_newtable(L);
  lua_setmetatable(L, -2);
  lua_xmove(L, co, 1);
  lj_state_owner_rel(co, foreign_tid(L));
  return co;
}

static lua_State *busy_function_prepare(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_pushcfunction(L, resume_return);
  lua_xmove(L, co, 1);
  lj_state_owner_rel(co, foreign_tid(L));
  return co;
}

static int busy_lua_rawget(lua_State *L)
{
  lua_State *co = busy_table_prepare(L, 1);
  lua_rawget(co, 1);
  return 0;
}

static int busy_lua_rawgeti(lua_State *L)
{
  lua_State *co = busy_table_prepare(L, 0);
  lua_rawgeti(co, 1, 4);
  return 0;
}

static int busy_lua_rawset(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_newtable(L);
  lua_pushliteral(L, "x");
  lua_pushinteger(L, 73);
  lua_xmove(L, co, 3);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_rawset(co, 1);
  return 0;
}

static int busy_lua_rawseti(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_newtable(L);
  lua_pushinteger(L, 74);
  lua_xmove(L, co, 2);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_rawseti(co, 1, 6);
  return 0;
}

static int busy_lua_setmetatable(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_newtable(L);
  lua_newtable(L);
  lua_xmove(L, co, 2);
  lj_state_owner_rel(co, foreign_tid(L));
  (void)lua_setmetatable(co, 1);
  return 0;
}

static int busy_lua_getmetatable(lua_State *L)
{
  lua_State *co = busy_metatable_prepare(L);
  (void)lua_getmetatable(co, 1);
  return 0;
}

static int busy_lua_getfenv(lua_State *L)
{
  lua_State *co = busy_function_prepare(L);
  lua_getfenv(co, 1);
  return 0;
}

static int busy_lua_next(lua_State *L)
{
  lua_State *co = busy_table_prepare(L, 0);
  (void)lua_next(co, 1);
  return 0;
}

static lua_State *busy_upvalue_prepare(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  assert(luaL_loadstring(L,
    "local x=68; return function() return x end") == 0);
  lua_call(L, 0, 1);
  lua_xmove(L, co, 1);
  lj_state_owner_rel(co, foreign_tid(L));
  return co;
}

static int busy_lua_getupvalue(lua_State *L)
{
  lua_State *co = busy_upvalue_prepare(L);
  (void)lua_getupvalue(co, 1, 1);
  return 0;
}

static int busy_lua_upvalueid(lua_State *L)
{
  lua_State *co = busy_upvalue_prepare(L);
  (void)lua_upvalueid(co, 1, 1);
  return 0;
}

static int busy_lua_setupvalue(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  assert(luaL_loadstring(L,
    "local x=68; return function() return x end") == 0);
  lua_call(L, 0, 1);
  lua_pushinteger(L, 81);
  lua_xmove(L, co, 2);
  lj_state_owner_rel(co, foreign_tid(L));
  (void)lua_setupvalue(co, 1, 1);
  return 0;
}

static int busy_lua_upvaluejoin(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  assert(luaL_loadstring(L,
    "local function make(x) return function() return x end end\n"
    "return make(1), make(2)") == 0);
  lua_call(L, 0, 2);
  lua_xmove(L, co, 2);
  lj_state_owner_rel(co, foreign_tid(L));
  lua_upvaluejoin(co, 1, 1, 2, 1);
  return 0;
}

static lua_State *busy_udata_prepare(lua_State *L)
{
  lua_State *co;
  (void)luaL_newmetatable(L, "state_owner_busy_udata");
  lua_pop(L, 1);
  co = lua_newthread(L);
  lua_newuserdata(L, 4);
  luaL_getmetatable(L, "state_owner_busy_udata");
  assert(lua_setmetatable(L, -2) == 1);
  lua_xmove(L, co, 1);
  lj_state_owner_rel(co, foreign_tid(L));
  return co;
}

static int busy_luaL_testudata(lua_State *L)
{
  lua_State *co = busy_udata_prepare(L);
  (void)luaL_testudata(co, 1, "state_owner_busy_udata");
  return 0;
}

static int busy_luaL_checkudata(lua_State *L)
{
  lua_State *co = busy_udata_prepare(L);
  (void)luaL_checkudata(co, 1, "state_owner_busy_udata");
  return 0;
}

static lua_State *busy_callmeta_prepare(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  assert(luaL_loadstring(L,
    "return setmetatable({}, {__call=function() return 55 end})") == 0);
  lua_call(L, 0, 1);
  lua_xmove(L, co, 1);
  lj_state_owner_rel(co, foreign_tid(L));
  return co;
}

static int busy_luaL_getmetafield(lua_State *L)
{
  lua_State *co = busy_callmeta_prepare(L);
  (void)luaL_getmetafield(co, 1, "__call");
  return 0;
}

static int busy_luaL_callmeta(lua_State *L)
{
  lua_State *co = busy_callmeta_prepare(L);
  (void)luaL_callmeta(co, 1, "__call");
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

static int busy_lua_setfenv_function(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_pushcfunction(L, resume_return);
  lua_newtable(L);
  lua_xmove(L, co, 2);
  lj_state_owner_rel(co, foreign_tid(L));
  (void)lua_setfenv(co, 1);
  return 0;
}

static int busy_lua_setfenv_udata(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  lua_newuserdata(L, 4);
  lua_newtable(L);
  lua_xmove(L, co, 2);
  lj_state_owner_rel(co, foreign_tid(L));
  (void)lua_setfenv(co, 1);
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
  check_getter_api_unowned(L);
  check_string_conversion_api_unowned(L);
  check_thread_env_unowned(L);
  check_call_entry_unowned(L);
  check_raw_object_api_unowned(L);
  check_upvalue_api_unowned(L);
  check_userdata_api_unowned(L);
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
  expect_thread_busy(L, busy_lua_replace, "busy lua_replace");
  expect_thread_busy(L, busy_lua_copy, "busy lua_copy");
  expect_thread_busy(L, busy_lua_pushnil, "busy lua_pushnil");
  expect_thread_busy(L, busy_lua_pushnumber, "busy lua_pushnumber");
  expect_thread_busy(L, busy_lua_pushinteger, "busy lua_pushinteger");
  expect_thread_busy(L, busy_lua_pushboolean, "busy lua_pushboolean");
  expect_thread_busy(L, busy_lua_pushthread, "busy lua_pushthread");
  expect_thread_busy(L, busy_lua_pushlstring, "busy lua_pushlstring");
  expect_thread_busy(L, busy_lua_pushstring, "busy lua_pushstring");
  expect_thread_busy(L, busy_lua_pushstring_null, "busy lua_pushstring NULL");
  expect_thread_busy(L, busy_lua_pushfstring, "busy lua_pushfstring");
  expect_thread_busy(L, busy_lua_pushlightuserdata,
		     "busy lua_pushlightuserdata");
  expect_thread_busy(L, busy_lua_createtable, "busy lua_createtable");
  expect_thread_busy(L, busy_lua_newuserdata, "busy lua_newuserdata");
  expect_thread_busy(L, busy_lua_newthread_api, "busy lua_newthread");
  expect_thread_busy(L, busy_lua_pushcclosure_api, "busy lua_pushcclosure");
  expect_thread_busy(L, busy_lua_type, "busy lua_type");
  expect_thread_busy(L, busy_lua_isnumber, "busy lua_isnumber");
  expect_thread_busy(L, busy_lua_isstring, "busy lua_isstring");
  expect_thread_busy(L, busy_lua_isuserdata, "busy lua_isuserdata");
  expect_thread_busy(L, busy_lua_rawequal, "busy lua_rawequal");
  expect_thread_busy(L, busy_lua_tonumber, "busy lua_tonumber");
  expect_thread_busy(L, busy_lua_tonumberx, "busy lua_tonumberx");
  expect_thread_busy(L, busy_lua_tointeger, "busy lua_tointeger");
  expect_thread_busy(L, busy_lua_tointegerx, "busy lua_tointegerx");
  expect_thread_busy(L, busy_luaL_checkany, "busy luaL_checkany");
  expect_thread_busy(L, busy_luaL_checknumber, "busy luaL_checknumber");
  expect_thread_busy(L, busy_luaL_optnumber, "busy luaL_optnumber");
  expect_thread_busy(L, busy_luaL_checkinteger, "busy luaL_checkinteger");
  expect_thread_busy(L, busy_luaL_optinteger, "busy luaL_optinteger");
  expect_thread_busy(L, busy_lua_toboolean, "busy lua_toboolean");
  expect_thread_busy(L, busy_lua_tocfunction, "busy lua_tocfunction");
  expect_thread_busy(L, busy_lua_touserdata, "busy lua_touserdata");
  expect_thread_busy(L, busy_lua_tothread, "busy lua_tothread");
  expect_thread_busy(L, busy_lua_topointer, "busy lua_topointer");
  expect_thread_busy(L, busy_lua_tolstring, "busy lua_tolstring");
  expect_thread_busy(L, busy_luaL_checklstring, "busy luaL_checklstring");
  expect_thread_busy(L, busy_luaL_optlstring, "busy luaL_optlstring");
  expect_thread_busy(L, busy_lua_objlen, "busy lua_objlen");
  expect_thread_busy(L, busy_lua_rawget, "busy lua_rawget");
  expect_thread_busy(L, busy_lua_rawgeti, "busy lua_rawgeti");
  expect_thread_busy(L, busy_lua_rawset, "busy lua_rawset");
  expect_thread_busy(L, busy_lua_rawseti, "busy lua_rawseti");
  expect_thread_busy(L, busy_lua_setmetatable, "busy lua_setmetatable");
  expect_thread_busy(L, busy_lua_getmetatable, "busy lua_getmetatable");
  expect_thread_busy(L, busy_lua_getfenv, "busy lua_getfenv");
  expect_thread_busy(L, busy_lua_next, "busy lua_next");
  expect_thread_busy(L, busy_lua_getupvalue, "busy lua_getupvalue");
  expect_thread_busy(L, busy_lua_upvalueid, "busy lua_upvalueid");
  expect_thread_busy(L, busy_lua_setupvalue, "busy lua_setupvalue");
  expect_thread_busy(L, busy_lua_upvaluejoin, "busy lua_upvaluejoin");
  expect_thread_busy(L, busy_luaL_testudata, "busy luaL_testudata");
  expect_thread_busy(L, busy_luaL_checkudata, "busy luaL_checkudata");
  expect_thread_busy(L, busy_luaL_getmetafield, "busy luaL_getmetafield");
  expect_thread_busy(L, busy_luaL_callmeta, "busy luaL_callmeta");
  expect_thread_busy(L, busy_getfenv_thread, "busy thread getfenv");
  expect_thread_busy(L, busy_setfenv_thread, "busy thread setfenv");
  expect_thread_busy(L, busy_lua_setfenv_function,
		     "busy lua_setfenv function");
  expect_thread_busy(L, busy_lua_setfenv_udata, "busy lua_setfenv udata");
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
