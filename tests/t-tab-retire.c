/*
** Focused guard for M5 table hash-vector SMR retirement.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_tab.h"

static TabNodeRetire *find_retired(global_State *g, Node *node)
{
  TabNodeRetire *ret;
  for (ret = g->tab.retired_nodes; ret != NULL; ret = ret->next)
    if (ret->node == node)
      return ret;
  return NULL;
}

static void set_pair(lua_State *L, int i)
{
  char key[32];
  snprintf(key, sizeof(key), "tab-retire-%02d", i);
  lua_pushlstring(L, key, strlen(key));
  lua_pushinteger(L, i + 100);
  lua_rawset(L, -3);
}

static void check_pair(lua_State *L, int i)
{
  char key[32];
  snprintf(key, sizeof(key), "tab-retire-%02d", i);
  lua_pushlstring(L, key, strlen(key));
  lua_rawget(L, -2);
  assert(lua_tointeger(L, -1) == i + 100);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  GCtab *t;
  Node *oldnode;
  MSize oldhmask;
  uint64_t retire_epoch;
  TabNodeRetire *ret;
  int i;

  assert(L != NULL);
  g = G(L);
  assert(g->tab.retired_nodes == NULL);

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask > 0);
  for (i = 0; i < 4; i++)
    set_pair(L, i);

  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask == t->hmask);
  lj_tab_resize(L, t, t->asize, lj_fls(t->hmask) + 2u);
  assert(lj_tab_node_acq(t) != oldnode);
  assert(lj_tab_node_hmask_acq(oldnode) == oldhmask);
  ret = find_retired(g, oldnode);
  assert(ret != NULL);
  assert(ret->hmask == oldhmask);
  assert(ret->armed == 1);
  retire_epoch = ret->retire_epoch;
  assert(lj_tab_reclaim_retired(g, retire_epoch) == 0);
  assert(find_retired(g, oldnode) != NULL);
  assert(lj_tab_reclaim_retired(g, retire_epoch + 1u) == 1);
  assert(find_retired(g, oldnode) == NULL);
  for (i = 0; i < 4; i++)
    check_pair(L, i);

  oldnode = lj_tab_node_acq(t);
  lj_tab_resize(L, t, t->asize, lj_fls(t->hmask) + 2u);
  assert(find_retired(g, oldnode) != NULL);

  lua_close(L);
  printf("t-tab-retire OK: hash vectors retire by epoch and close cleanly\n");
  return 0;
}
