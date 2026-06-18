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

static void set_int_pair(lua_State *L, int key, int val)
{
  lua_pushinteger(L, key);
  lua_pushinteger(L, val);
  lua_rawset(L, -3);
}

static void check_int_value(lua_State *L, int key, int val)
{
  lua_rawgeti(L, -1, key);
  assert(lua_tointeger(L, -1) == val);
  lua_pop(L, 1);
}

static void check_int_array(lua_State *L, GCtab *t, int key, int val)
{
  assert((MSize)key < lj_tab_asize_acq(t));
  assert(!lj_tv_isnil_acq(&lj_tab_array_acq(t)[key]));
  check_int_value(L, key, val);
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
  set_int_pair(L, 3, 777);

  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask == t->hmask);
  assert(lj_tab_node_hdr_flags_acq(oldnode) == 0);
  lj_tab_resize(L, t, 8, lj_fls(t->hmask) + 2u);
  assert(lj_tab_node_acq(t) != oldnode);
  assert(lj_tab_node_hmask_acq(oldnode) == oldhmask);
  assert(lj_tab_node_hdr_flags_acq(oldnode) == TABNODE_FLAG_RETIRING);
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
  check_int_array(L, t, 3, 777);
  set_int_pair(L, 6, 888);
  check_int_array(L, t, 6, 888);

  oldnode = lj_tab_node_acq(t);
  assert(lj_tab_node_hdr_flags_acq(oldnode) == 0);
  lj_tab_resize(L, t, 2, 1);
  assert(lj_tab_asize_acq(t) == 2);
  assert(lj_tab_node_acq(t) != oldnode);
  assert(lj_tab_node_hdr_flags_acq(oldnode) == TABNODE_FLAG_RETIRING);
  assert(find_retired(g, oldnode) != NULL);
  for (i = 0; i < 4; i++)
    check_pair(L, i);
  check_int_value(L, 3, 777);
  check_int_value(L, 6, 888);

  lua_close(L);
  printf("t-tab-retire OK: hash vectors rebuild, route and retire cleanly\n");
  return 0;
}
