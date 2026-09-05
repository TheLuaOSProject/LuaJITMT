/*
** Focused regression test for M5 table hash-node TValue snapshots.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

#include "lib/tab_forward_helpers.h"

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;

  assert(L != NULL);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);

  tabfwd_set_cstr_i32(L, t, "tab-slot-alpha", 11);
  tabfwd_set_cstr_i32(L, t, "tab-slot-beta", 22);
  tabfwd_set_cstr_i32(L, t, "tab-slot-gamma", 33);
  assert(tabfwd_count_next_visible(t) == 3);

  lj_tab_resize(L, t, t->asize, lj_fls(t->hmask) + 2u);
  tabfwd_assert_cstr_i32(L, t, "tab-slot-alpha", 11);
  tabfwd_assert_cstr_i32(L, t, "tab-slot-beta", 22);
  tabfwd_assert_cstr_i32(L, t, "tab-slot-gamma", 33);
  assert(tabfwd_count_next_visible(t) == 3);

  lua_close(L);
  printf("t-tab-slot-snapshot OK: hash-node TValue snapshots preserve table behavior\n");
  return 0;
}
