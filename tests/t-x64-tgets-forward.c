/*
** Focused x64 guard for TGETS over a forwarded hash slot.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

#include "lib/tab_forward_helpers.h"

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  GCstr *key;
  Node *oldnode, *newnode;
  MSize oldhmask, newhmask;
  TValue *oldslot;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L, "jit.off()\n") == 0);

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  key = lj_str_new(L, "tgets_forward_field",
		   sizeof("tgets_forward_field") - 1u);
  lj_tab_storeint(L, lj_tab_setstr(L, t, key), 4242);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldslot = tabfwd_find_str_slot(oldnode, oldhmask, key);
  assert(oldslot != NULL);

  lua_setglobal(L, "tgets_forward_t");
  tabfwd_load_lua(L,
    "local t = tgets_forward_t\n"
    "assert(t.tgets_forward_field == 4242, type(t.tgets_forward_field))\n");

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  assert(lj_tab_getstr(t, key) != NULL);

  tabfwd_store_forward(oldslot);
  la_store32_rel(&lj_tab_node_hdrw(oldnode)->flags, 0);
  lj_tab_hmask_rel(t, oldhmask);
  lj_tab_node_rel(t, oldnode);
  tabfwd_run_loaded(L);

  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);

  lua_close(L);
  printf("t-x64-tgets-forward OK: TGETS resolves forwarded hash slots\n");
  return 0;
}
