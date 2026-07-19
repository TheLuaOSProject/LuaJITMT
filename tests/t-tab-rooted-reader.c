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
#include "lj_state.h"
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

static uint32_t pseudo_index_calls;
static uint32_t pseudo_newindex_calls;

static int pseudo_index_mm(lua_State *L)
{
  assert(lua_gettop(L) == 2);
  assert(lua_istable(L, 1));
  assert(lua_isstring(L, 2));
  pseudo_index_calls++;
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  lua_pushliteral(L, "pseudo-meta-value");
  return 1;
}

static int pseudo_newindex_mm(lua_State *L)
{
  assert(lua_gettop(L) == 3);
  assert(lua_istable(L, 1));
  assert(lua_isstring(L, 2));
  assert(lua_isnumber(L, 3));
  pseudo_newindex_calls++;
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  return 0;
}

static void push_pseudo_meta(lua_State *L)
{
  lua_newtable(L);
  lua_pushliteral(L, "__index");
  lua_pushcfunction(L, pseudo_index_mm);
  lua_rawset(L, -3);
  lua_pushliteral(L, "__newindex");
  lua_pushcfunction(L, pseudo_newindex_mm);
  lua_rawset(L, -3);
}

static int check_environment_meta_c(lua_State *L)
{
  uint32_t roots0 = lj_tg_root_anchor_top_acq(L2TG(L));

  lua_getfield(L, LUA_ENVIRONINDEX, "field");
  assert(strcmp(lua_tostring(L, -1), "pseudo-meta-value") == 0);
  lua_pop(L, 1);
  lua_pushliteral(L, "field-table");
  lua_gettable(L, LUA_ENVIRONINDEX);
  assert(strcmp(lua_tostring(L, -1), "pseudo-meta-value") == 0);
  lua_pop(L, 1);

  lua_pushinteger(L, 41);
  lua_setfield(L, LUA_ENVIRONINDEX, "write-field");
  lua_pushliteral(L, "write-table");
  lua_pushinteger(L, 42);
  lua_settable(L, LUA_ENVIRONINDEX);

  lua_pushliteral(L, "raw-key");
  lua_pushliteral(L, "raw-value");
  lua_rawset(L, LUA_ENVIRONINDEX);
  lua_pushliteral(L, "raw-key");
  lua_rawget(L, LUA_ENVIRONINDEX);
  assert(strcmp(lua_tostring(L, -1), "raw-value") == 0);
  lua_pop(L, 1);
  lua_pushliteral(L, "raw-array-value");
  lua_rawseti(L, LUA_ENVIRONINDEX, 7);
  lua_rawgeti(L, LUA_ENVIRONINDEX, 7);
  assert(strcmp(lua_tostring(L, -1), "raw-array-value") == 0);
  lua_pop(L, 1);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == roots0);
  return 0;
}

static int pseudo_getloop_c(lua_State *L)
{
  lua_getfield(L, LUA_ENVIRONINDEX, "loop");
  return 1;  /* Unreachable. */
}

static int pseudo_setloop_c(lua_State *L)
{
  lua_pushinteger(L, 1);
  lua_setfield(L, LUA_ENVIRONINDEX, "loop");
  return 0;  /* Unreachable. */
}

static void check_pseudo_meta_and_caught_errors(lua_State *L)
{
  TGState *tg = L2TG(L);
  uint32_t roots0 = lj_tg_root_anchor_top_acq(tg);
  int env, getfn, setfn, i;

  /* Exercise L->env through both function-valued meta continuations. */
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  push_pseudo_meta(L);
  assert(lua_setmetatable(L, -2));
  lua_getfield(L, LUA_GLOBALSINDEX, "global-field");
  assert(strcmp(lua_tostring(L, -1), "pseudo-meta-value") == 0);
  lua_pop(L, 1);
  lua_pushliteral(L, "global-table");
  lua_gettable(L, LUA_GLOBALSINDEX);
  assert(strcmp(lua_tostring(L, -1), "pseudo-meta-value") == 0);
  lua_pop(L, 1);
  lua_pushinteger(L, 43);
  lua_setfield(L, LUA_GLOBALSINDEX, "global-write-field");
  lua_pushliteral(L, "global-write-table");
  lua_pushinteger(L, 44);
  lua_settable(L, LUA_GLOBALSINDEX);
  lua_pushnil(L);
  assert(lua_setmetatable(L, -2));
  lua_pop(L, 1);

  /* Exercise the current C closure's independently mutable environment edge. */
  lua_newtable(L);
  env = lua_gettop(L);
  push_pseudo_meta(L);
  assert(lua_setmetatable(L, env));
  lua_pushcfunction(L, check_environment_meta_c);
  lua_pushvalue(L, env);
  assert(lua_setfenv(L, -2));
  lua_call(L, 0, 0);
  lua_pop(L, 1);
  assert(pseudo_index_calls == 4u);
  assert(pseudo_newindex_calls == 4u);
  assert(lj_tg_root_anchor_top_acq(tg) == roots0);

  /* A table-valued self-chain forces deterministic __index/__newindex errors.
  ** Repeated protected catches must restore both natural stack temporaries and
  ** every anchor allocated by the rooted meta helpers. */
  lua_newtable(L);
  env = lua_gettop(L);
  lua_newtable(L);
  lua_pushliteral(L, "__index");
  lua_pushvalue(L, env);
  lua_rawset(L, -3);
  lua_pushliteral(L, "__newindex");
  lua_pushvalue(L, env);
  lua_rawset(L, -3);
  assert(lua_setmetatable(L, env));
  lua_pushcfunction(L, pseudo_getloop_c);
  lua_pushvalue(L, env);
  assert(lua_setfenv(L, -2));
  getfn = lua_gettop(L);
  lua_pushcfunction(L, pseudo_setloop_c);
  lua_pushvalue(L, env);
  assert(lua_setfenv(L, -2));
  setfn = lua_gettop(L);
  for (i = 0; i < 32; i++) {
    lua_pushvalue(L, getfn);
    assert(lua_pcall(L, 0, 1, 0) == LUA_ERRRUN);
    lua_pop(L, 1);
    assert(lj_tg_root_anchor_top_acq(tg) == roots0);
    lua_pushvalue(L, setfn);
    assert(lua_pcall(L, 0, 1, 0) == LUA_ERRRUN);
    lua_pop(L, 1);
    assert(lj_tg_root_anchor_top_acq(tg) == roots0);
  }
  lua_settop(L, 1);
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
  GCtab *globalenv = lj_state_env_acq(L);
  lj_gc2_test_stack_admission_retry_once(obj2gco(globalenv));
  lua_pushliteral(L, "global-hash-value");
  lua_setfield(L, LUA_GLOBALSINDEX, "rooted-global");
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
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
  lj_gc2_test_stack_admission_retry_once(obj2gco(tabV(L->top - 2)));
  lua_call(L, 0, 0);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
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
  lj_gc2_test_stack_admission_retry_once(obj2gco(tabV(L->base)));
  lua_rawget(L, 1);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "hash-value") == 0);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  lua_pop(L, 1);

  lua_pushvalue(L, 1);
  lua_rawget(L, -1);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "self-value") == 0);
  lua_pop(L, 1);

  lua_pushvalue(L, 1);
  lua_rawget(L, lua_gettop(L));
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "self-value") == 0);
  lua_pop(L, 1);

  /* lua_setfield resolves its index before popping the value. This extreme
  ** aliases the selected table with the value slot that is subsequently
  ** shuffled into |key|value|. The public API must retain the table itself. */
  lua_pushvalue(L, 1);
  lua_setfield(L, -1, "self-field");
  lua_getfield(L, 1, "self-field");
  assert(lua_rawequal(L, 1, -1));
  lua_pop(L, 1);

  /* The same value/table alias is valid through an absolute positive index.
  ** The key/value shuffle moves the selected old-top value up one slot. */
  lua_pushvalue(L, 1);
  lua_setfield(L, lua_gettop(L), "positive-self-field");
  lua_getfield(L, 1, "positive-self-field");
  assert(lua_rawequal(L, 1, -1));
  lua_pop(L, 1);

  lua_rawgeti(L, 1, 1);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "array-value") == 0);
  lua_pop(L, 1);

  lua_pushvalue(L, 1);
  lua_rawgeti(L, lua_gettop(L), 1);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "array-value") == 0);
  lua_pop(L, 2);

  /* Raw setters also accept the selected table as their value. The temporary
  ** receiver root must not perturb either relative or absolute top indices. */
  lua_pushliteral(L, "raw-self-field");
  lua_pushvalue(L, 1);
  lua_rawset(L, -1);
  lua_getfield(L, 1, "raw-self-field");
  assert(lua_rawequal(L, 1, -1));
  lua_pop(L, 1);

  lua_pushliteral(L, "raw-positive-self-field");
  lua_pushvalue(L, 1);
  lua_rawset(L, lua_gettop(L));
  lua_getfield(L, 1, "raw-positive-self-field");
  assert(lua_rawequal(L, 1, -1));
  lua_pop(L, 1);

  lua_pushvalue(L, 1);
  lua_rawseti(L, -1, 11);
  lua_rawgeti(L, 1, 11);
  assert(lua_rawequal(L, 1, -1));
  lua_pop(L, 1);

  lua_pushvalue(L, 1);
  lua_rawseti(L, lua_gettop(L), 12);
  lua_rawgeti(L, 1, 12);
  assert(lua_rawequal(L, 1, -1));
  lua_pop(L, 1);
}

static void check_rawset_stack_relocation(lua_State *L)
{
  TValue *oldstack;

  /* A synthetic keyed-CAS CHANGED retry grows the stack between attempts.
  ** This simultaneously exercises the CAS wrapper's key/value remapping and
  ** lua_rawset's receiver/key/value remapping before its barriers and pop. */
  lua_pushliteral(L, "raw-retry-key");
  lua_pushliteral(L, "raw-retry-value");
  oldstack = tvref(L->stack);
  lj_tab_test_keyed_cas_changed_stack_grow_once();
  lua_rawset(L, 1);
  assert(lj_tab_test_keyed_cas_changed_stack_grow_hits() == 1u);
  assert(tvref(L->stack) != oldstack);
  lua_getfield(L, 1, "raw-retry-key");
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "raw-retry-value") == 0);
  lua_pop(L, 1);

  /* Integer raw stores have a C-local key but retain a stack source and the
  ** temporary receiver root through the same retry boundary. */
  lua_pushvalue(L, 1);
  oldstack = tvref(L->stack);
  lj_tab_test_keyed_cas_changed_stack_grow_once();
  lua_rawseti(L, 1, 29);
  assert(lj_tab_test_keyed_cas_changed_stack_grow_hits() == 1u);
  assert(tvref(L->stack) != oldstack);
  lua_rawgeti(L, 1, 29);
  assert(lua_rawequal(L, 1, -1));
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
  check_rawset_stack_relocation(L);
  check_pseudo_index_public_api(L);
  check_pseudo_meta_and_caught_errors(L);
  check_transient_retries(L);
  check_non_table_root(L);
  lua_close(L);
  printf("t-tab-rooted-reader OK: rooted table snapshots preserve parent/key/result lifetimes\n");
  return 0;
}
