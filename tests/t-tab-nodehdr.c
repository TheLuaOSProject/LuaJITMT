/*
** Guard that table hash masks are paired with their Node vector.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_tab.h"

static void set_pair(lua_State *L, int i)
{
  char key[32];
  snprintf(key, sizeof(key), "tab-nodehdr-%02d", i);
  lua_pushlstring(L, key, strlen(key));
  lua_pushinteger(L, i + 300);
  lua_rawset(L, -3);
}

static void check_pair(lua_State *L, int i)
{
  char key[32];
  snprintf(key, sizeof(key), "tab-nodehdr-%02d", i);
  lua_pushlstring(L, key, strlen(key));
  lua_rawget(L, -2);
  assert(lua_tointeger(L, -1) == i + 300);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  Node *nilnode, *oldnode, *newnode;
  MSize oldhmask, newhmask;
  int i;

  assert(L != NULL);
  nilnode = &G(L)->nilnode;
  assert(lj_tab_node_hmask_acq(nilnode) == 0);
  assert(lj_tab_node_hdr_flags_acq(nilnode) == 0);

  lua_createtable(L, 0, 2);
  t = tabV(L->top-1);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldnode != nilnode);
  assert(oldhmask == t->hmask);
  assert(lj_tab_node_hdr_flags_acq(oldnode) == 0);

  for (i = 0; i < 5; i++)
    set_pair(L, i);

  lj_tab_resize(L, t, t->asize, lj_fls(t->hmask) + 4u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  assert(lj_tab_node_hmask_acq(oldnode) == oldhmask);
  assert(lj_tab_node_hdr_flags_acq(oldnode) == TABNODE_FLAG_RETIRING);
  assert(newhmask == t->hmask);
  assert(newhmask > oldhmask);
  assert(lj_tab_node_hdr_flags_acq(newnode) == 0);

  for (i = 0; i < 5; i++)
    check_pair(L, i);

  lua_settop(L, 0);
  lua_createtable(L, 0, 2);
  t = tabV(L->top-1);
  assert(lj_tab_node_acq(t) != nilnode);
  oldnode = lj_tab_node_acq(t);
  assert(lj_tab_node_hdr_flags_acq(oldnode) == 0);
  lj_tab_resize(L, t, t->asize, 0);
  assert(lj_tab_node_acq(t) == nilnode);
  assert(lj_tab_node_hmask_acq(lj_tab_node_acq(t)) == 0);
  assert(lj_tab_node_hdr_flags_acq(oldnode) == TABNODE_FLAG_RETIRING);
  assert(lj_tab_node_hdr_flags_acq(nilnode) == 0);
  assert(t->hmask == 0);

  lua_close(L);
  printf("t-tab-nodehdr OK: hash masks stay paired with Node vectors\n");
  return 0;
}
