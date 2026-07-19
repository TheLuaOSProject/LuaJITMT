/*
** Focused coverage for rooted __index/__newindex table chains.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_meta.h"
#include "lj_tg.h"

#if defined(LJ_TG_ROOT_TEST_HELPERS)
static void check_partial_root_oom(lua_State *L)
{
  TGState *tg = L2TG(L);
  TValue nilv;
  uint32_t filler[TG_ROOT_ANCHOR_SLOTS];
  uint32_t base = lj_tg_root_anchor_top_acq(tg);
  uint32_t nfill = TG_ROOT_ANCHOR_SLOTS - 1u -
    (base % TG_ROOT_ANCHOR_SLOTS);
  uint32_t i;

  assert(luaL_loadstring(L,
    "local leaf = {}; local root = setmetatable({}, { __index = leaf }); "
    "return root.absent") == LUA_OK);
  setnilV(&nilv);
  for (i = 0; i < nfill; i++)
    assert(lj_tg_root_anchor_push(L, tg, &nilv, &filler[i]) != NULL);

  /* Root zero consumes the last embedded slot. Force the next reserve to fail
  ** and require explicit partial-init cleanup before the memory error unwinds. */
  lj_tg_root_test_fail_reserve_after(1);
  assert(lua_pcall(L, 0, 1, 0) == LUA_ERRMEM);
  assert(lj_tg_root_anchor_top_acq(tg) == base + nfill);
  lua_pop(L, 1);
  while (nfill != 0) {
    nfill--;
    lj_tg_root_anchor_pop(tg, filler[nfill]);
  }
  assert(lj_tg_root_anchor_top_acq(tg) == base);
}
#endif

static void check_c_api_result_root(lua_State *L)
{
  uint32_t roots0 = lj_tg_root_anchor_top_acq(L2TG(L));

  lua_newtable(L);
  lua_newtable(L);
  lua_pushliteral(L, "__index");
  lua_newtable(L);
  lua_pushliteral(L, "field");
  lua_pushliteral(L, "c-api-rooted");
  lua_rawset(L, -3);
  lua_rawset(L, -3);
  assert(lua_setmetatable(L, -2) == 1);

  /* A fresh host state has no VM cframe even though its initial vmstate is the
  ** interpreter value. Exercise the explicit-output ABI directly, including
  ** key/output aliasing, so it can never infer a bytecode destination. */
  assert(L->cframe == NULL);
  lua_pushliteral(L, "field");
  lua_pushnil(L);
  assert(lj_meta_tgettv_rooted(L, L->top-3, L->top-2, L->top-1) ==
	 L->top-1);
  assert(tvisstr(L->top-1));
  assert(strcmp(strVdata(L->top-1), "c-api-rooted") == 0);
  lua_pop(L, 2);

  lua_pushliteral(L, "field");
  assert(lj_meta_tgettv_rooted(L, L->top-2, L->top-1, L->top-1) ==
	 L->top-1);
  assert(tvisstr(L->top-1));
  assert(strcmp(strVdata(L->top-1), "c-api-rooted") == 0);
  lua_pop(L, 1);

  lua_getfield(L, -1, "field");
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "c-api-rooted") == 0);
  lua_pop(L, 1);
  lua_pushliteral(L, "field");
  lua_gettable(L, -2);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "c-api-rooted") == 0);
  lua_settop(L, 0);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == roots0);

  /* Host C API function-valued dispatch crosses the short frame-completion
  ** window where the helper has published receiver/key above L->top and the
  ** caller has not yet advanced it by two slots. Collect immediately inside
  ** both metamethods after that handoff. */
  assert(luaL_dostring(L,
    "return setmetatable({}, { __index = function(_, k) "
      "collectgarbage(); return 'host-call:' .. k end })") == LUA_OK);
  lua_getfield(L, -1, "field");
  assert(strcmp(lua_tostring(L, -1), "host-call:field") == 0);
  lua_settop(L, 0);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == roots0);

  assert(luaL_dostring(L,
    "local sink = {}; local proxy = setmetatable({}, { __newindex = "
      "function(_, k, v) collectgarbage(); sink[k] = v end }); "
      "return proxy, sink") == LUA_OK);
  lua_pushinteger(L, 73);
  lua_setfield(L, 1, "field");
  lua_getfield(L, 2, "field");
  assert(lua_tointeger(L, -1) == 73);
  lua_settop(L, 0);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == roots0);
}

static int c_api_getfield_error(lua_State *L)
{
  lua_pushinteger(L, 17);
  lua_getfield(L, -1, "missing");
  return 1;  /* Unreachable. */
}

static int c_api_setfield_error(lua_State *L)
{
  lua_pushinteger(L, 17);
  lua_pushinteger(L, 23);
  lua_setfield(L, -2, "missing");
  return 0;  /* Unreachable. */
}

static void check_caught_c_api_errors(lua_State *L)
{
  uint32_t roots0 = lj_tg_root_anchor_top_acq(L2TG(L));

  lua_pushcfunction(L, c_api_getfield_error);
  lua_setglobal(L, "c_api_getfield_error");
  lua_pushcfunction(L, c_api_setfield_error);
  lua_setglobal(L, "c_api_setfield_error");
  assert(luaL_dostring(L,
    "for _ = 1, 64 do\n"
    "  assert(not pcall(c_api_getfield_error))\n"
    "  assert(not pcall(c_api_setfield_error))\n"
    "end\n") == LUA_OK);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == roots0);
}

static void check_chain_semantics(lua_State *L)
{
  TGState *tg = L2TG(L);
  uint32_t roots0 = lj_tg_root_anchor_top_acq(tg);
  const char *script =
    "local read_leaf = { direct = 'rooted-index-leaf' }\n"
    "local read_mid = setmetatable({}, { __index = read_leaf })\n"
    "local read_root = setmetatable({}, { __index = read_mid })\n"
    "collectgarbage(); collectgarbage()\n"
    "assert(read_root.direct == 'rooted-index-leaf')\n"
    "local read_seen\n"
    "local read_call = setmetatable({}, { __index = function(self, key)\n"
    "  read_seen = self; collectgarbage(); return 'call:' .. key\n"
    "end })\n"
    "local read_call_root = setmetatable({}, { __index = read_call })\n"
    "assert(read_call_root.missing == 'call:missing')\n"
    "assert(read_seen == read_call)\n"
    "local write_leaf = {}\n"
    "local write_mid = setmetatable({}, { __newindex = write_leaf })\n"
    "local write_root = setmetatable({}, { __newindex = write_mid })\n"
    "write_root.direct = 'rooted-newindex-leaf'\n"
    "assert(rawget(write_leaf, 'direct') == 'rooted-newindex-leaf')\n"
    "assert(rawget(write_mid, 'direct') == nil)\n"
    "assert(rawget(write_root, 'direct') == nil)\n"
    "local write_seen, write_key, write_value\n"
    "local write_call = setmetatable({}, { __newindex = function(self, k, v)\n"
    "  write_seen, write_key, write_value = self, k, v; collectgarbage()\n"
    "end })\n"
    "local write_call_root = setmetatable({}, { __newindex = write_call })\n"
    "write_call_root.answer = 42\n"
    "assert(write_seen == write_call and write_key == 'answer' and "
      "write_value == 42)\n"
    "local ga, gb = {}, {}\n"
    "setmetatable(ga, { __index = gb }); setmetatable(gb, { __index = ga })\n"
    "local ok, err = pcall(function() return ga.absent end)\n"
    "assert(not ok and err:find('loop in gettable', 1, true))\n"
    "local sa, sb = {}, {}\n"
    "setmetatable(sa, { __newindex = sb })\n"
    "setmetatable(sb, { __newindex = sa })\n"
    "ok, err = pcall(function() sa.absent = true end)\n"
    "assert(not ok and err:find('loop in settable', 1, true))\n";

  assert(luaL_dostring(L, script) == LUA_OK);
  assert(lj_tg_root_anchor_top_acq(tg) == roots0);
}

static void check_repeated_semantic_errors(lua_State *L)
{
  static const char *const source[] = {
    "return (1).missing",
    "local x = 1; x.missing = 2",
    "local t = setmetatable({}, { __index = 17 }); return t.missing",
    "local t = setmetatable({}, { __newindex = 17 }); t.missing = 1",
    "local t = {}; t[nil] = 1",
    "local t = {}; t[0/0] = 1"
  };
  TGState *tg = L2TG(L);
  uint32_t roots0 = lj_tg_root_anchor_top_acq(tg);
  uint32_t i, j;

  for (i = 0; i < sizeof(source)/sizeof(source[0]); i++) {
    for (j = 0; j < 32u; j++) {
      int top = lua_gettop(L);
      assert(luaL_loadstring(L, source[i]) == LUA_OK);
      assert(lj_tg_root_anchor_top_acq(tg) == roots0);
      assert(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
      assert(lua_isstring(L, -1));
      /* Every deterministic catch must observe the exact pre-call anchor top,
      ** not rely on a later unrelated protected boundary to repair it. */
      assert(lj_tg_root_anchor_top_acq(tg) == roots0);
      lua_settop(L, top);
      assert(lj_tg_root_anchor_top_acq(tg) == roots0);
    }
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

#if defined(LJ_TG_ROOT_TEST_HELPERS)
  check_partial_root_oom(L);
#endif
  check_c_api_result_root(L);
  check_caught_c_api_errors(L);
  check_chain_semantics(L);
  check_repeated_semantic_errors(L);

#if LJ_HASFFI
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[typedef struct { int payload; } rooted_meta_chain_t;]]\n"
    "local index = { answer = 'ffi-index-rooted' }\n"
    "local ct = ffi.metatype('rooted_meta_chain_t', { __index = index })\n"
    "local value = ct()\n"
    "assert(value.answer == 'ffi-index-rooted')\n"
    "local ok, err = pcall(function() return value.missing_member end)\n"
    "assert(not ok and err:find('missing_member', 1, true))\n") == LUA_OK);
#endif

  lua_close(L);
  puts("t-meta-rooted-chain OK: rooted table chains, caller result roots, "
       "function carriers, loop errors and anchor cleanup verified");
  return 0;
}
