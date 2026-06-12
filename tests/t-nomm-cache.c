/*
** Focused guard for M5 metatable negative-cache policy.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_meta.h"

static void assert_lua_ok(lua_State *L, int status, const char *what)
{
  if (status != LUA_OK) {
    fprintf(stderr, "%s: %s\n", what, lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static GCtab *new_table(lua_State *L)
{
  lua_newtable(L);
  return tabV(L->top-1);
}

static void set_non_mm_field(lua_State *L)
{
  lua_pushliteral(L, "ordinary");
  lua_pushinteger(L, 1);
  lua_rawset(L, -3);
}

static void check_lj_meta_cache_does_not_set_nomm(lua_State *L, GCtab *mt)
{
  mt->nomm = 0;
  assert(lj_meta_cache(mt, MM_index, mmname_str(G(L), MM_index)) == NULL);
  assert((mt->nomm & (uint8_t)(1u << MM_index)) == 0);
  assert(lj_meta_cache(mt, MM_len, mmname_str(G(L), MM_len)) == NULL);
  assert((mt->nomm & (uint8_t)(1u << MM_len)) == 0);
}

static void check_c_api_setmetatable_clears_nomm(lua_State *L)
{
  GCtab *mt;
  lua_settop(L, 0);
  (void)new_table(L);
  mt = new_table(L);
  assert(mt->nomm == (uint8_t)~0u);
  lua_pushvalue(L, -1);
  lua_setmetatable(L, 1);
  assert(mt->nomm == 0);
  check_lj_meta_cache_does_not_set_nomm(L, mt);
}

static void check_base_setmetatable_clears_nomm(lua_State *L)
{
  GCtab *mt;
  lua_settop(L, 0);
  lua_getglobal(L, "setmetatable");
  (void)new_table(L);
  mt = new_table(L);
  assert(mt->nomm == (uint8_t)~0u);
  lua_call(L, 2, 1);
  assert(mt->nomm == 0);
  lua_pop(L, 1);
}

static void check_missing_index_does_not_set_nomm(lua_State *L)
{
  GCtab *mt;
  lua_settop(L, 0);
  (void)new_table(L);
  mt = new_table(L);
  set_non_mm_field(L);
  assert(mt->nomm == 0);
  lua_pushvalue(L, -1);
  lua_setmetatable(L, 1);
  lua_getfield(L, 1, "missing");
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);
  assert((mt->nomm & (uint8_t)(1u << MM_index)) == 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  check_c_api_setmetatable_clears_nomm(L);
  check_base_setmetatable_clears_nomm(L);
  check_missing_index_does_not_set_nomm(L);
  assert_lua_ok(L, luaL_dostring(L, "local t = {}; return #t"), "len smoke");
  lua_close(L);
  printf("t-nomm-cache OK: runtime misses do not set nomm bits\n");
  return 0;
}
