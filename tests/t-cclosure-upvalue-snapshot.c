/*
** Focused guard for C closure upvalue publication snapshots.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static void check_lua(lua_State *L, int status, const char *what)
{
  if (status != LUA_OK) {
    fprintf(stderr, "%s: %s\n", what, lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static int push_upvalue_c(lua_State *L)
{
  lua_pushvalue(L, lua_upvalueindex(1));
  return 1;
}

static int copy_upvalue_c(lua_State *L)
{
  lua_pushnil(L);
  lua_copy(L, lua_upvalueindex(1), -1);
  return 1;
}

static void push_payload_table(lua_State *L, const char *tag, int seq);

static int replace_upvalue_store_c(lua_State *L)
{
  push_payload_table(L, "replace-store", 81);
  lua_replace(L, lua_upvalueindex(1));
  lua_pushvalue(L, lua_upvalueindex(1));
  return 1;
}

static int copy_upvalue_store_c(lua_State *L)
{
  push_payload_table(L, "copy-store", 82);
  lua_copy(L, -1, lua_upvalueindex(1));
  lua_pop(L, 1);
  lua_pushvalue(L, lua_upvalueindex(1));
  return 1;
}

static int dummy_c(lua_State *L)
{
  (void)L;
  return 0;
}

static int meta_tostring_c(lua_State *L)
{
  (void)L;
  lua_pushliteral(L, "meta-ok");
  return 1;
}

static void assert_string(lua_State *L, int idx, const char *want)
{
  const char *got = lua_tostring(L, idx);
  assert(got != NULL && strcmp(got, want) == 0);
}

static void push_payload_table(lua_State *L, const char *tag, int seq)
{
  lua_newtable(L);
  lua_pushliteral(L, "tag");
  lua_pushstring(L, tag);
  lua_settable(L, -3);
  lua_pushliteral(L, "seq");
  lua_pushinteger(L, seq);
  lua_settable(L, -3);
  lua_pushinteger(L, 2);
  lua_pushliteral(L, "two");
  lua_settable(L, -3);
  lua_pushinteger(L, 7);
  lua_pushliteral(L, "seven");
  lua_settable(L, -3);
}

static void assert_payload_table(lua_State *L, int idx,
				 const char *tag, int seq)
{
  const char *got_tag;
  int got_seq;
  assert(lua_istable(L, idx));
  lua_getfield(L, idx, "tag");
  got_tag = lua_tostring(L, -1);
  assert(got_tag != NULL && strcmp(got_tag, tag) == 0);
  lua_pop(L, 1);
  lua_getfield(L, idx, "seq");
  got_seq = (int)lua_tointeger(L, -1);
  assert(got_seq == seq);
  lua_pop(L, 1);
}

static void exercise_upvalue_reader(lua_State *L, lua_CFunction fn,
				    const char *tag)
{
  int base = lua_gettop(L);
  const char *name;

  lua_pushnil(L);
  lua_pushcclosure(L, fn, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);

  push_payload_table(L, tag, 41);
  name = lua_setupvalue(L, -2, 1);
  assert(name != NULL);

  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);

  lua_pushvalue(L, -1);
  check_lua(L, lua_pcall(L, 0, 1, 0), tag);
  assert_payload_table(L, -1, tag, 41);
  lua_pop(L, 1);

  lua_settop(L, base);
}

static void assert_table_field_string(lua_State *L, int idx, const char *key,
				      const char *want)
{
  lua_getfield(L, idx, key);
  assert_string(L, -1, want);
  lua_pop(L, 1);
}

static void assert_table_field_int(lua_State *L, int idx, const char *key,
				   int want)
{
  int got;
  lua_getfield(L, idx, key);
  got = (int)lua_tointeger(L, -1);
  assert(got == want);
  lua_pop(L, 1);
}

static int number_reader_c(lua_State *L)
{
  int ok = 0;
  size_t len = 0;
  const char *s;
  int uv = lua_upvalueindex(1);

  assert(lua_type(L, uv) == LUA_TNUMBER);
  assert(lua_isnumber(L, uv));
  assert(lua_isstring(L, uv));
  assert(!lua_iscfunction(L, uv));
  assert(!lua_isnil(L, uv));
  assert(!lua_isnone(L, uv));
  assert(!lua_isnoneornil(L, uv));
  assert(strcmp(lua_typename(L, lua_type(L, uv)), "number") == 0);
  assert(strcmp(luaL_typename(L, uv), "number") == 0);
  luaL_checktype(L, uv, LUA_TNUMBER);
  luaL_checkany(L, uv);
  assert(lua_toboolean(L, uv));
  assert((int)lua_tonumber(L, uv) == 123);
  assert((int)lua_tonumberx(L, uv, &ok) == 123 && ok);
  assert((int)lua_tointeger(L, uv) == 123);
  assert((int)lua_tointegerx(L, uv, &ok) == 123 && ok);
  assert((int)luaL_checknumber(L, uv) == 123);
  assert((int)luaL_optnumber(L, uv, 0) == 123);
  assert((int)luaL_checkinteger(L, uv) == 123);
  assert((int)luaL_optinteger(L, uv, 0) == 123);

  s = lua_tolstring(L, uv, &len);
  assert(s != NULL && len == 3 && strcmp(s, "123") == 0);
  assert(lua_type(L, uv) == LUA_TSTRING);
  lua_pushvalue(L, uv);
  return 1;
}

static int checklstring_reader_c(lua_State *L)
{
  size_t len = 0;
  const char *s;
  int uv = lua_upvalueindex(1);

  s = luaL_checklstring(L, uv, &len);
  assert(s != NULL && len == 3 && strcmp(s, "456") == 0);
  assert(lua_type(L, uv) == LUA_TSTRING);
  s = luaL_optlstring(L, uv, "fallback", &len);
  assert(s != NULL && len == 3 && strcmp(s, "456") == 0);
  lua_pushvalue(L, uv);
  return 1;
}

static int objlen_reader_c(lua_State *L)
{
  int uv = lua_upvalueindex(1);
  assert(lua_objlen(L, uv) == 3);
  assert(lua_type(L, uv) == LUA_TSTRING);
  lua_pushvalue(L, uv);
  return 1;
}

static int compare_api_reader_c(lua_State *L)
{
  int low = lua_upvalueindex(1);
  int high = lua_upvalueindex(2);
  int str1 = lua_upvalueindex(3);
  int str2 = lua_upvalueindex(4);

  assert(lua_lessthan(L, low, high));
  assert(!lua_lessthan(L, high, low));
  assert(!lua_equal(L, low, high));
  assert(!lua_rawequal(L, low, high));
  assert(lua_equal(L, str1, str2));
  assert(lua_rawequal(L, str1, str2));

  lua_pushboolean(L, 1);
  return 1;
}

static int table_api_reader_c(lua_State *L)
{
  int uv = lua_upvalueindex(1);
  int saw_entry;

  lua_getfield(L, uv, "tag");
  assert_string(L, -1, "tableops");
  lua_pop(L, 1);

  lua_pushliteral(L, "seq");
  lua_gettable(L, uv);
  assert((int)lua_tointeger(L, -1) == 41);
  lua_pop(L, 1);

  lua_pushinteger(L, 7);
  lua_rawget(L, uv);
  assert_string(L, -1, "seven");
  lua_pop(L, 1);

  lua_rawgeti(L, uv, 2);
  assert_string(L, -1, "two");
  lua_pop(L, 1);

  lua_pushnil(L);
  saw_entry = lua_next(L, uv);
  assert(saw_entry != 0);
  lua_pop(L, 2);

  lua_pushliteral(L, "newfield");
  lua_pushinteger(L, 99);
  lua_settable(L, uv);

  lua_pushliteral(L, "setv");
  lua_setfield(L, uv, "setfield");

  lua_pushliteral(L, "rawfield");
  lua_pushliteral(L, "rawv");
  lua_rawset(L, uv);

  lua_pushliteral(L, "rawi");
  lua_rawseti(L, uv, 3);

  lua_pushvalue(L, uv);
  return 1;
}

static int ref_api_reader_c(lua_State *L)
{
  int uv = lua_upvalueindex(1);
  int ref;

  lua_pushliteral(L, "ref-value");
  ref = luaL_ref(L, uv);
  assert(ref >= 1);

  lua_rawgeti(L, uv, ref);
  assert_string(L, -1, "ref-value");
  lua_pop(L, 1);

  luaL_unref(L, uv, ref);
  lua_rawgeti(L, uv, ref);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);

  lua_pushboolean(L, 1);
  return 1;
}

static int metatable_api_reader_c(lua_State *L)
{
  int uv = lua_upvalueindex(1);
  assert(lua_getmetatable(L, uv) == 1);
  lua_getfield(L, -1, "mtag");
  assert_string(L, -1, "oldmt");
  lua_pop(L, 2);

  lua_newtable(L);
  lua_pushliteral(L, "mtag");
  lua_pushliteral(L, "newmt");
  lua_settable(L, -3);
  assert(lua_setmetatable(L, uv) == 1);

  assert(lua_getmetatable(L, uv) == 1);
  lua_getfield(L, -1, "mtag");
  assert_string(L, -1, "newmt");
  lua_pop(L, 2);

  lua_pushvalue(L, uv);
  return 1;
}

static int function_env_reader_c(lua_State *L)
{
  int uv = lua_upvalueindex(1);
  lua_CFunction fn;

  assert(lua_type(L, uv) == LUA_TFUNCTION);
  assert(lua_iscfunction(L, uv));
  fn = lua_tocfunction(L, uv);
  assert(fn == dummy_c);
  assert(lua_topointer(L, uv) != NULL);

  lua_getfenv(L, uv);
  assert(lua_istable(L, -1));
  lua_pop(L, 1);

  lua_newtable(L);
  lua_pushliteral(L, "envtag");
  lua_pushliteral(L, "newenv");
  lua_settable(L, -3);
  assert(lua_setfenv(L, uv) == 1);

  lua_getfenv(L, uv);
  lua_getfield(L, -1, "envtag");
  assert_string(L, -1, "newenv");
  lua_pop(L, 2);

  lua_pushvalue(L, uv);
  return 1;
}

static int carrier_c(lua_State *L)
{
  lua_pushvalue(L, lua_upvalueindex(1));
  return 1;
}

static int upvalue_api_reader_c(lua_State *L)
{
  int uv = lua_upvalueindex(1);
  const char *name;
  void *id;

  name = lua_getupvalue(L, uv, 1);
  assert(name != NULL);
  assert_payload_table(L, -1, "inner", 11);
  lua_pop(L, 1);

  id = lua_upvalueid(L, uv, 1);
  assert(id != NULL);

  push_payload_table(L, "replaced", 77);
  name = lua_setupvalue(L, uv, 1);
  assert(name != NULL);

  lua_pushvalue(L, uv);
  return 1;
}

static int udata_reader_c(lua_State *L)
{
  int uv = lua_upvalueindex(1);
  void *p = luaL_testudata(L, uv, "snap_udata");
  assert(p != NULL);
  assert(lua_isuserdata(L, uv));
  assert(luaL_checkudata(L, uv, "snap_udata") == p);
  luaL_checktype(L, uv, LUA_TUSERDATA);
  luaL_checkany(L, uv);
  assert(lua_touserdata(L, uv) == p);
  assert(lua_topointer(L, uv) != NULL);
  lua_pushboolean(L, 1);
  return 1;
}

static int callmeta_reader_c(lua_State *L)
{
  int uv = lua_upvalueindex(1);

  assert(luaL_getmetafield(L, uv, "__tostring") == 1);
  assert(lua_iscfunction(L, -1));
  assert(lua_tocfunction(L, -1) == meta_tostring_c);
  lua_pop(L, 1);

  assert(luaL_getmetafield(L, uv, "__missing") == 0);
  assert(luaL_callmeta(L, uv, "__tostring") == 1);
  return 1;
}

static int thread_reader_c(lua_State *L)
{
  int uv = lua_upvalueindex(1);
  assert(lua_tothread(L, uv) != NULL);
  assert(lua_topointer(L, uv) != NULL);
  lua_pushboolean(L, 1);
  return 1;
}

static void exercise_number_readers(lua_State *L)
{
  int base = lua_gettop(L);

  lua_pushinteger(L, 123);
  lua_pushcclosure(L, number_reader_c, 1);
  check_lua(L, lua_pcall(L, 0, 1, 0), "number reader");
  assert_string(L, -1, "123");
  lua_pop(L, 1);

  lua_pushinteger(L, 456);
  lua_pushcclosure(L, checklstring_reader_c, 1);
  check_lua(L, lua_pcall(L, 0, 1, 0), "luaL_checklstring reader");
  assert_string(L, -1, "456");
  lua_pop(L, 1);

  lua_pushinteger(L, 789);
  lua_pushcclosure(L, objlen_reader_c, 1);
  check_lua(L, lua_pcall(L, 0, 1, 0), "objlen reader");
  assert_string(L, -1, "789");
  lua_pop(L, 1);

  lua_settop(L, base);
}

static void exercise_compare_apis(lua_State *L)
{
  int base = lua_gettop(L);

  lua_pushinteger(L, 3);
  lua_pushinteger(L, 7);
  lua_pushliteral(L, "same");
  lua_pushliteral(L, "same");
  lua_pushcclosure(L, compare_api_reader_c, 4);
  check_lua(L, lua_pcall(L, 0, 1, 0), "compare api reader");
  assert(lua_toboolean(L, -1));
  lua_pop(L, 1);

  lua_settop(L, base);
}

static void exercise_table_apis(lua_State *L)
{
  int base = lua_gettop(L);

  push_payload_table(L, "tableops", 41);
  lua_pushcclosure(L, table_api_reader_c, 1);
  check_lua(L, lua_pcall(L, 0, 1, 0), "table api reader");
  assert_table_field_int(L, -1, "newfield", 99);
  assert_table_field_string(L, -1, "setfield", "setv");
  assert_table_field_string(L, -1, "rawfield", "rawv");
  lua_rawgeti(L, -1, 3);
  assert_string(L, -1, "rawi");
  lua_pop(L, 2);

  lua_settop(L, base);
}

static void exercise_ref_apis(lua_State *L)
{
  int base = lua_gettop(L);

  lua_newtable(L);
  lua_pushcclosure(L, ref_api_reader_c, 1);
  check_lua(L, lua_pcall(L, 0, 1, 0), "ref api reader");
  assert(lua_toboolean(L, -1));
  lua_pop(L, 1);

  lua_settop(L, base);
}

static void exercise_metatable_apis(lua_State *L)
{
  int base = lua_gettop(L);

  push_payload_table(L, "metatable", 12);
  lua_newtable(L);
  lua_pushliteral(L, "mtag");
  lua_pushliteral(L, "oldmt");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);

  lua_pushcclosure(L, metatable_api_reader_c, 1);
  check_lua(L, lua_pcall(L, 0, 1, 0), "metatable api reader");
  assert(lua_getmetatable(L, -1) == 1);
  lua_getfield(L, -1, "mtag");
  assert_string(L, -1, "newmt");
  lua_pop(L, 3);

  lua_settop(L, base);
}

static void exercise_function_env_apis(lua_State *L)
{
  int base = lua_gettop(L);

  lua_pushcfunction(L, dummy_c);
  lua_pushcclosure(L, function_env_reader_c, 1);
  check_lua(L, lua_pcall(L, 0, 1, 0), "function env reader");
  assert(lua_iscfunction(L, -1));
  lua_getfenv(L, -1);
  lua_getfield(L, -1, "envtag");
  assert_string(L, -1, "newenv");
  lua_pop(L, 3);

  lua_settop(L, base);
}

static void exercise_nested_upvalue_apis(lua_State *L)
{
  int base = lua_gettop(L);

  push_payload_table(L, "inner", 11);
  lua_pushcclosure(L, carrier_c, 1);
  lua_pushcclosure(L, upvalue_api_reader_c, 1);
  check_lua(L, lua_pcall(L, 0, 1, 0), "nested upvalue reader");
  check_lua(L, lua_pcall(L, 0, 1, 0), "nested upvalue carrier");
  assert_payload_table(L, -1, "replaced", 77);
  lua_pop(L, 1);

  lua_settop(L, base);
}

static void exercise_upvalue_store_apis(lua_State *L)
{
  int base = lua_gettop(L);

  lua_pushnil(L);
  lua_pushcclosure(L, replace_upvalue_store_c, 1);
  check_lua(L, lua_pcall(L, 0, 1, 0), "replace upvalue store");
  assert_payload_table(L, -1, "replace-store", 81);
  lua_pop(L, 1);

  lua_pushnil(L);
  lua_pushcclosure(L, copy_upvalue_store_c, 1);
  check_lua(L, lua_pcall(L, 0, 1, 0), "copy upvalue store");
  assert_payload_table(L, -1, "copy-store", 82);
  lua_pop(L, 1);

  lua_settop(L, base);
}

static void exercise_udata_api(lua_State *L)
{
  int base = lua_gettop(L);
  void *ud;

  if (luaL_newmetatable(L, "snap_udata"))
    lua_pop(L, 1);
  else
    lua_pop(L, 1);

  ud = lua_newuserdata(L, sizeof(int));
  assert(ud != NULL);
  luaL_getmetatable(L, "snap_udata");
  lua_setmetatable(L, -2);
  lua_pushcclosure(L, udata_reader_c, 1);
  check_lua(L, lua_pcall(L, 0, 1, 0), "userdata reader");
  assert(lua_toboolean(L, -1));
  lua_pop(L, 1);

  lua_settop(L, base);
}

static void exercise_callmeta_api(lua_State *L)
{
  int base = lua_gettop(L);

  push_payload_table(L, "callmeta", 13);
  lua_newtable(L);
  lua_pushliteral(L, "__tostring");
  lua_pushcfunction(L, meta_tostring_c);
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_pushcclosure(L, callmeta_reader_c, 1);
  check_lua(L, lua_pcall(L, 0, 1, 0), "callmeta reader");
  assert_string(L, -1, "meta-ok");
  lua_pop(L, 1);

  lua_settop(L, base);
}

static void exercise_thread_api(lua_State *L)
{
  int base = lua_gettop(L);
  lua_State *co = lua_newthread(L);
  assert(co != NULL);
  lua_pushcclosure(L, thread_reader_c, 1);
  check_lua(L, lua_pcall(L, 0, 1, 0), "thread reader");
  assert(lua_toboolean(L, -1));
  lua_pop(L, 1);

  lua_settop(L, base);
}

static void exercise_gmatch_position(lua_State *L)
{
  check_lua(L, luaL_dostring(L,
    "local it = ('aa'):gmatch('a')\n"
    "local name, pos = debug.getupvalue(it, 3)\n"
    "assert(name and pos == 0, tostring(pos))\n"
    "assert(it() == 'a')\n"
    "name, pos = debug.getupvalue(it, 3)\n"
    "assert(name and pos == 1, tostring(pos))\n"
    "assert(it() == 'a')\n"
    "name, pos = debug.getupvalue(it, 3)\n"
    "assert(name and pos == 2, tostring(pos))\n"
    "assert(it() == nil)\n"
    "it = ('abcabc'):gmatch('a')\n"
    "assert(debug.setupvalue(it, 1, 'zzaz'))\n"
    "assert(debug.setupvalue(it, 2, 'z'))\n"
    "assert(debug.setupvalue(it, 3, 0))\n"
    "assert(it() == 'z')\n"
    "name, pos = debug.getupvalue(it, 3)\n"
    "assert(name and pos == 1, tostring(pos))\n"
    "assert(debug.setupvalue(it, 2, 'a'))\n"
    "assert(debug.setupvalue(it, 3, 2))\n"
    "assert(it() == 'a')\n"
    "name, pos = debug.getupvalue(it, 3)\n"
    "assert(name and pos == 3, tostring(pos))\n"), "gmatch position upvalue");
}

static void exercise_io_lines_upvalue_mutation(lua_State *L)
{
  check_lua(L, luaL_dostring(L,
    "local path = os.tmpname()\n"
    "local f = assert(io.open(path, 'w'))\n"
    "f:write('line\\n')\n"
    "f:close()\n"
    "local it = assert(io.lines(path))\n"
    "assert(debug.setupvalue(it, 1, false))\n"
    "local ok, err = pcall(it)\n"
    "os.remove(path)\n"
    "assert(not ok, 'mutated io.lines upvalue unexpectedly succeeded')\n"
    "assert(tostring(err):find('attempt to use a closed file', 1, true), tostring(err))\n"),
    "io.lines upvalue mutation");

  check_lua(L, luaL_dostring(L,
    "local path = os.tmpname()\n"
    "local f = assert(io.open(path, 'w'))\n"
    "f:write('12\\n34\\n')\n"
    "f:close()\n"
    "local it = assert(io.lines(path, '*n'))\n"
    "local name, opt = debug.getupvalue(it, 2)\n"
    "assert(name and opt == '*n', tostring(opt))\n"
    "assert(debug.setupvalue(it, 2, '*l'))\n"
    "assert(it() == '12')\n"
    "assert(it() == '34')\n"
    "assert(it() == nil)\n"
    "os.remove(path)\n"),
    "io.lines read-option upvalue mutation");
}

static void exercise_coroutine_wrap_upvalue_mutation(lua_State *L)
{
  check_lua(L, luaL_dostring(L,
    "local wrap = coroutine.wrap(function()\n"
    "  coroutine.yield('original-yield')\n"
    "  return 'original-done'\n"
    "end)\n"
    "assert(wrap() == 'original-yield')\n"
    "local co = coroutine.create(function()\n"
    "  coroutine.yield('replacement-yield')\n"
    "  return 'replacement-done'\n"
    "end)\n"
    "local name, old = debug.getupvalue(wrap, 1)\n"
    "assert(name and type(old) == 'thread')\n"
    "assert(debug.setupvalue(wrap, 1, co))\n"
    "assert(wrap() == 'replacement-yield')\n"
    "assert(wrap() == 'replacement-done')\n"),
    "coroutine.wrap upvalue mutation");
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  exercise_upvalue_reader(L, push_upvalue_c, "pushvalue");
  exercise_upvalue_reader(L, copy_upvalue_c, "copy");
  exercise_number_readers(L);
  exercise_compare_apis(L);
  exercise_table_apis(L);
  exercise_ref_apis(L);
  exercise_metatable_apis(L);
  exercise_function_env_apis(L);
  exercise_nested_upvalue_apis(L);
  exercise_upvalue_store_apis(L);
  exercise_udata_api(L);
  exercise_callmeta_api(L);
  exercise_thread_api(L);
  exercise_gmatch_position(L);
  exercise_io_lines_upvalue_mutation(L);
  exercise_coroutine_wrap_upvalue_mutation(L);

  lua_close(L);
  printf("t-cclosure-upvalue-snapshot OK: C closure upvalue snapshots verified\n");
  return 0;
}
