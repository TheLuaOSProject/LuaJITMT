/*
** Focused regression fixture for table.insert structural ownership while
** mt_entering is nonzero.
*/

#include <assert.h>
#include <limits.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_tab.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-table-insert-entering requires LJ_TAB_TEST_HELPERS"
#endif

static void rawseti(lua_State *L, int idx, int32_t key, int32_t val)
{
  lua_pushinteger(L, val);
  lua_rawseti(L, idx, key);
}

static void call_insert_append(lua_State *L, int tabidx, int32_t val)
{
  lua_getglobal(L, "table");
  lua_getfield(L, -1, "insert");
  lua_remove(L, -2);
  lua_pushvalue(L, tabidx);
  lua_pushinteger(L, val);
  lua_call(L, 2, 0);
}

static void call_insert_pos(lua_State *L, int tabidx, int32_t pos, int32_t val)
{
  lua_getglobal(L, "table");
  lua_getfield(L, -1, "insert");
  lua_remove(L, -2);
  lua_pushvalue(L, tabidx);
  lua_pushinteger(L, pos);
  lua_pushinteger(L, val);
  lua_call(L, 3, 0);
}

static void assert_i(lua_State *L, int tabidx, int32_t key, int32_t expect)
{
  lua_rawgeti(L, tabidx, key);
  assert(lua_tointeger(L, -1) == expect);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);

  lua_createtable(L, 32, 0);
  rawseti(L, 1, 1, 11);
  rawseti(L, 1, 2, 22);
  rawseti(L, 1, 3, 33);

  lj_tab_test_reset_struct_enter_acquires();
  assert(mt_entering_add_rlx(g, 1) == 0);
  call_insert_append(L, 1, 44);
  call_insert_pos(L, 1, 2, 99);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, INT_MAX);

  assert(lj_tab_test_struct_enter_acquires() == 2);
  assert_i(L, 1, 1, 11);
  assert_i(L, 1, 2, 99);
  assert_i(L, 1, 3, 22);
  assert_i(L, 1, 4, 33);
  assert_i(L, 1, 5, 44);

  lua_close(L);
  return 0;
}
