/*
** Focused regression coverage for authoritative-root table readers.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_tg.h"

static void populate(lua_State *L)
{
  lua_createtable(L, 4, 4);
  lua_pushliteral(L, "array-value");
  lua_rawseti(L, 1, 1);
  lua_pushliteral(L, "named");
  lua_pushliteral(L, "hash-value");
  lua_rawset(L, 1);
  lua_pushvalue(L, 1);
  lua_pushliteral(L, "self-value");
  lua_rawset(L, 1);
}

static void check_integer_stack_result(lua_State *L)
{
  TValue *out;
  lua_pushnil(L);
  out = lj_tab_getinttv_rooted(L, L->base, 1, L->top - 1);
  assert(out == L->top - 1);
  assert(tvisstr(out));
  assert(strcmp(strVdata(out), "array-value") == 0);
  lua_pop(L, 1);

  lua_pushnil(L);
  out = lj_tab_getinttv_rooted(L, L->base, 777, L->top - 1);
  assert(out == L->top - 1 && tvisnil(out));
  lua_pop(L, 1);
}

static void check_generic_stack_key(lua_State *L)
{
  TValue *out;
  lua_pushliteral(L, "named");
  lua_pushnil(L);
  out = lj_tab_gettv_rooted(L, L->base, L->top - 2, L->top - 1);
  assert(out == L->top - 1);
  assert(tvisstr(out));
  assert(strcmp(strVdata(out), "hash-value") == 0);
  lua_pop(L, 2);
}

static void check_anchor_result(lua_State *L)
{
  TGState *tg = L2TG(L);
  TValue nilv;
  TValue *anchor;
  uint32_t idx;
  setnilV(&nilv);
  anchor = lj_tg_root_anchor_push(L, tg, &nilv, &idx);
  assert(lj_tab_getinttv_rooted(L, L->base, 1, anchor) == anchor);
  anchor = lj_tg_root_anchor_slot_acq(tg, idx);
  assert(tvisstr(anchor));
  assert(strcmp(strVdata(anchor), "array-value") == 0);
  lj_tg_root_anchor_pop(tg, idx);
}

static int check_environment_rawget_c(lua_State *L)
{
  lua_pushliteral(L, "rooted-env");
  lua_rawget(L, LUA_ENVIRONINDEX);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "env-hash-value") == 0);
  lua_pop(L, 1);

  lua_rawgeti(L, LUA_ENVIRONINDEX, 7);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "env-array-value") == 0);
  lua_pop(L, 1);
  return 0;
}

static void check_pseudo_index_public_api(lua_State *L)
{
  lua_pushliteral(L, "global-hash-value");
  lua_setfield(L, LUA_GLOBALSINDEX, "rooted-global");
  lua_pushliteral(L, "global-array-value");
  lua_rawseti(L, LUA_GLOBALSINDEX, 9);

  lua_pushliteral(L, "rooted-global");
  lua_rawget(L, LUA_GLOBALSINDEX);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "global-hash-value") == 0);
  lua_pop(L, 1);
  lua_rawgeti(L, LUA_GLOBALSINDEX, 9);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "global-array-value") == 0);
  lua_pop(L, 1);

  lua_newtable(L);
  lua_pushliteral(L, "env-hash-value");
  lua_setfield(L, -2, "rooted-env");
  lua_pushliteral(L, "env-array-value");
  lua_rawseti(L, -2, 7);
  lua_pushcfunction(L, check_environment_rawget_c);
  lua_pushvalue(L, -2);
  assert(lua_setfenv(L, -2));
  lua_call(L, 0, 0);
  lua_pop(L, 1);
}

static void check_alias_and_public_api(lua_State *L)
{
  TValue *slot;
  GCstr *key;

  /* Consume-key publication aliases key and result. A transient key admission
  ** must retry before overwriting the only key root. */
  lua_pushliteral(L, "named");
  key = strV(L->top - 1);
  lj_gc2_test_stack_admission_retry_once(obj2gco(key));
  slot = lj_tab_gettv_rooted(L, L->base, L->top - 1, L->top - 1);
  assert(slot == L->top - 1 && tvisstr(slot));
  assert(strcmp(strVdata(slot), "hash-value") == 0);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  lua_pop(L, 1);

  /* The valid lua_rawget(L, -1) extreme uses one table TValue as parent, key
  ** and result. Exact table/key leases retain both snapshots until terminal
  ** publication, while the base slot keeps the table semantically rooted. */
  lua_pushvalue(L, 1);
  slot = L->top - 1;
  assert(lj_tab_gettv_rooted(L, slot, slot, slot) == slot);
  assert(tvisstr(slot) && strcmp(strVdata(slot), "self-value") == 0);
  lua_pop(L, 1);

  lua_pushliteral(L, "named");
  lua_rawget(L, 1);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "hash-value") == 0);
  lua_pop(L, 1);

  lua_pushvalue(L, 1);
  lua_rawget(L, -1);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "self-value") == 0);
  lua_pop(L, 1);

  lua_rawgeti(L, 1, 1);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "array-value") == 0);
  lua_pop(L, 1);
}

static void check_transient_retries(lua_State *L)
{
  GCtab *t = tabV(L->base);
  GCstr *s;
  TValue *out;

  lj_gc2_test_stack_admission_retry_once(obj2gco(t));
  lua_pushnil(L);
  out = lj_tab_getinttv_rooted(L, L->base, 1, L->top - 1);
  assert(out == L->top - 1 && tvisstr(out));
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  s = strV(out);
  lua_pop(L, 1);

  lj_gc2_test_stack_admission_retry_once(obj2gco(s));
  lua_pushnil(L);
  out = lj_tab_getinttv_rooted(L, L->base, 1, L->top - 1);
  assert(out == L->top - 1 && tvisstr(out) && strV(out) == s);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  lua_pop(L, 1);

  lua_pushliteral(L, "named");
  s = strV(L->top - 1);
  lj_gc2_test_stack_admission_retry_once(obj2gco(s));
  lua_pushnil(L);
  out = lj_tab_gettv_rooted(L, L->base, L->top - 2, L->top - 1);
  assert(out == L->top - 1 && tvisstr(out));
  assert(strcmp(strVdata(out), "hash-value") == 0);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  lua_pop(L, 2);
}

static void check_non_table_root(lua_State *L)
{
  TValue key;
  TValue *out;
  setintV(&key, 1);
  lua_pushinteger(L, 42);
  lua_pushnil(L);
  out = lj_tab_gettv_rooted(L, L->top - 2, &key, L->top - 1);
  assert(out == L->top - 1 && tvisnil(out));
  lua_pop(L, 2);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  populate(L);
  assert(L->base == L->top - 1 && tvistab(L->base));
  check_integer_stack_result(L);
  check_generic_stack_key(L);
  check_anchor_result(L);
  check_alias_and_public_api(L);
  check_pseudo_index_public_api(L);
  check_transient_retries(L);
  check_non_table_root(L);
  lua_close(L);
  printf("t-tab-rooted-reader OK: rooted table snapshots preserve parent/key/result lifetimes\n");
  return 0;
}
