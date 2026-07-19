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
  check_transient_retries(L);
  check_non_table_root(L);
  lua_close(L);
  printf("t-tab-rooted-reader OK: rooted table snapshots preserve parent/key/result lifetimes\n");
  return 0;
}
