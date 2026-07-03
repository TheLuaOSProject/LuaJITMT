/*
** Focused regression fixture for table.clear routing while mt_entering is
** nonzero.
*/

#include <assert.h>
#include <limits.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_tab.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-tab-clear-entering requires LJ_TAB_TEST_HELPERS"
#endif

static void fill_table(lua_State *L, GCtab *t)
{
  lj_tab_storeint(L, lj_tab_setint(L, t, 0), 11);
  lj_tab_storeint(L, lj_tab_setint(L, t, 1), 22);
}

static void assert_cleared(GCtab *t)
{
  cTValue *v0 = lj_tab_getint(t, 0);
  cTValue *v1 = lj_tab_getint(t, 1);
  assert(v0 == NULL || tvisnil(v0));
  assert(v1 == NULL || tvisnil(v1));
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  GCtab *t;

  assert(L != NULL);
  g = G(L);
  lua_createtable(L, 4, 0);
  t = tabV(L->top - 1);

  fill_table(L, t);
  lj_tab_test_reset_clear_shared_calls();
  lj_tab_clear(L, t);
  assert(lj_tab_test_clear_shared_calls() == 0);
  assert_cleared(t);

  fill_table(L, t);
  lj_tab_test_reset_clear_shared_calls();
  assert(mt_entering_add_rlx(g, 1) == 0);
  lj_tab_clear(L, t);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, INT_MAX);
  assert(lj_tab_test_clear_shared_calls() == 1);
  assert_cleared(t);

  lua_close(L);
  return 0;
}
