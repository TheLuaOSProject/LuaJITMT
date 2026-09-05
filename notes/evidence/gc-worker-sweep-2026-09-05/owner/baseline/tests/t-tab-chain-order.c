/*
** Focused regression test for M5 stable table nodes and hash-chain publication.
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

static MSize chain_len(Node *n)
{
  MSize len = 0;
  for (; n != NULL; n = lj_tab_nextnode_acq(n))
    len++;
  return len;
}

static void exercise_chainlen_resize(lua_State *L)
{
  GCtab *t;
  GCstr *keys[9];
  Node *node;
  MSize oldhmask;
  uint32_t seq = 0;
  int i;

  lua_settop(L, 0);
  lua_createtable(L, 0, 32);
  t = tabV(L->top-1);
  assert(t->hmask == 31);

  for (i = 0; i < 9; i++)
    keys[i] = tabfwd_find_sid_bucket(L, "tab-chain-order", t->hmask, 0,
				     &seq);

  for (i = 0; i < 8; i++)
    tabfwd_set_str_i32(L, t, keys[i], 100 + i);

  node = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(node);
  assert(oldhmask == 31);
  assert(chain_len(&node[0]) == 8);

  tabfwd_set_str_i32(L, t, keys[8], 108);
  node = lj_tab_node_acq(t);
  assert(lj_tab_node_hmask_acq(node) > oldhmask);
  for (i = 0; i < 9; i++)
    tabfwd_assert_str_i32(t, keys[i], 100 + i);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  GCstr *anchor0, *displaced, *anchor_main;
  Node *node, *displaced_node;
  Node *mainnext;
  uint32_t seq = 0;

  assert(L != NULL);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask == 7);

  anchor0 = tabfwd_find_sid_bucket(L, "tab-chain-order", t->hmask, 0, &seq);
  displaced = tabfwd_find_sid_bucket(L, "tab-chain-order", t->hmask, 0,
				     &seq);
  assert(anchor0 != displaced);

  tabfwd_set_str_i32(L, t, anchor0, 11);
  tabfwd_set_str_i32(L, t, displaced, 22);
  node = noderef(t->node);
  assert(strV(&node[0].key) == anchor0);
  displaced_node = lj_tab_nextnode_acq(&node[0]);
  assert(displaced_node != NULL);
  assert(strV(&displaced_node->key) == displaced);

  anchor_main = tabfwd_find_sid_bucket(L, "tab-chain-order", t->hmask,
				       (uint32_t)(displaced_node - node),
				       &seq);
  assert(anchor_main != anchor0 && anchor_main != displaced);
  tabfwd_set_str_i32(L, t, anchor_main, 77);
  node = noderef(t->node);
  assert(lj_tab_nextnode_acq(&node[0]) == displaced_node);
  assert(strV(&displaced_node->key) == displaced);
  mainnext = lj_tab_nextnode_acq(displaced_node);
  assert(mainnext != NULL);
  assert(strV(&mainnext->key) == anchor_main);
  tabfwd_assert_str_i32(t, anchor0, 11);
  tabfwd_assert_str_i32(t, displaced, 22);
  tabfwd_assert_str_i32(t, anchor_main, 77);

  exercise_chainlen_resize(L);

  lua_close(L);
  printf("t-tab-chain-order OK: stable nodes and release-published links\n");
  return 0;
}
