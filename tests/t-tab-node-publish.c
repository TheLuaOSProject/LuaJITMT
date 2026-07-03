/*
** Focused regression test for M5 table hash-vector pointer publication.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_tab.h"

static void assert_clear_hash(Node *node, MSize hmask)
{
  MSize i;
  for (i = 0; i <= hmask; i++) {
    assert(lj_tab_nextnode_acq(&node[i]) == NULL);
    assert(tvisnil(&node[i].key));
    assert(tvisnil(&node[i].val));
  }
}

static void set_pair(lua_State *L, int i)
{
  char key[32];
  snprintf(key, sizeof(key), "tab-node-publish-%02d", i);
  lua_pushlstring(L, key, strlen(key));
  lua_pushinteger(L, i + 200);
  lua_rawset(L, -3);
}

static void check_pair(lua_State *L, int i)
{
  char key[32];
  snprintf(key, sizeof(key), "tab-node-publish-%02d", i);
  lua_pushlstring(L, key, strlen(key));
  lua_rawget(L, -2);
  assert(lua_tointeger(L, -1) == i + 200);
  lua_pop(L, 1);
}

static void check_gc_key(lua_State *L)
{
  int tabidx;
  lua_settop(L, 0);
  lua_createtable(L, 0, 1);  /* target table */
  tabidx = lua_gettop(L);
  lua_newtable(L);  /* collectable key */
  lua_pushvalue(L, -1);
  lua_pushliteral(L, "gc-key-value");
  lua_rawset(L, tabidx);
  lua_pushvalue(L, -1);
  lua_rawget(L, tabidx);
  assert(strcmp(lua_tostring(L, -1), "gc-key-value") == 0);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  Node *oldnode, *node;
  int i;

  assert(L != NULL);

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask == 7);
  node = lj_tab_node_acq(t);
  assert(node == noderef(t->node));
  assert(lj_tab_node_hmask_acq(node) == 7);
  assert_clear_hash(node, lj_tab_node_hmask_acq(node));

  for (i = 0; i < 6; i++)
    set_pair(L, i);
  oldnode = lj_tab_node_acq(t);
  assert(lj_tab_node_hmask_acq(oldnode) == t->hmask);
  lj_tab_resize(L, t, t->asize, lj_fls(t->hmask) + 2u);
  assert(lj_tab_node_acq(t) != oldnode);
  assert(lj_tab_node_hmask_acq(oldnode) == 7);
  assert(lj_tab_node_hmask_acq(lj_tab_node_acq(t)) == t->hmask);
  for (i = 0; i < 6; i++)
    check_pair(L, i);

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  oldnode = lj_tab_node_acq(t);
  assert(oldnode != &G(L)->nilnode);
  lj_tab_resize(L, t, t->asize, 0);
  assert(t->hmask == 0);
  assert(lj_tab_node_acq(t) == &G(L)->nilnode);
  assert(lj_tab_node_hmask_acq(lj_tab_node_acq(t)) == 0);

  check_gc_key(L);

  lua_close(L);
  printf("t-tab-node-publish OK: table node vectors publish with acquire/release helpers\n");
  return 0;
}
