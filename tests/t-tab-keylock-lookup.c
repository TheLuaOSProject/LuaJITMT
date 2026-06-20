/*
** Focused guard for M5 table KEYLOCK lookup filtering.
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

static void store_keylock(Node *n)
{
  TValue keylock;
  setkeylockV(&keylock);
  tv_rawstore_rel(&n->key, tv_rawload(&keylock));
}

static void store_strkey(lua_State *L, Node *n, GCstr *s)
{
  TValue key;
  setstrV(L, &key, s);
  copyTVrel(L, &n->key, &key);
}

static void exercise_tombstone_anchor_insert(lua_State *L)
{
  GCtab *t;
  GCstr *anchor, *displaced, *replacement;
  Node *node, *freetop0;
  MSize freecount0;
  uint32_t seq = 0;

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask == 7);

  anchor = tabfwd_find_sid_bucket(L, "tab-keylock-lookup", t->hmask, 0,
				  &seq);
  displaced = tabfwd_find_sid_bucket(L, "tab-keylock-lookup", t->hmask, 0,
				     &seq);
  replacement = tabfwd_find_sid_bucket(L, "tab-keylock-lookup", t->hmask, 0,
				       &seq);

  tabfwd_set_str_i32(L, t, anchor, 11);
  tabfwd_set_str_i32(L, t, displaced, 22);
  node = lj_tab_node_acq(t);
  assert(strV(&node[0].key) == anchor);
  assert(lj_tab_nextnode_acq(&node[0]) != NULL);
  lj_tab_storenil(L, &node[0].val);
  assert(tvisnil(lj_tab_getstr(t, anchor)));
  freetop0 = getfreetop(t, node);
  freecount0 = lj_tab_node_freecount_acq(node);
  assert(freecount0 > 0);

  tabfwd_set_str_i32(L, t, replacement, 33);
  assert(getfreetop(t, node) == freetop0);
  assert(lj_tab_node_freecount_acq(node) == freecount0 - 1u);
  assert(tvisstr(&node[0].key) && strV(&node[0].key) == anchor);
  assert(tvisnil(&node[0].val));
  assert(tvisnil(lj_tab_getstr(t, anchor)));
  tabfwd_assert_str_i32(t, displaced, 22);
  tabfwd_assert_str_i32(t, replacement, 33);
  assert(tabfwd_count_next_visible(t) == 2);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  GCstr *anchor, *displaced;
  Node *node;
  MSize freecount0;
  uint32_t seq = 0;

  assert(L != NULL);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask == 7);

  anchor = tabfwd_find_sid_bucket(L, "tab-keylock-lookup", t->hmask, 0,
				  &seq);
  displaced = tabfwd_find_sid_bucket(L, "tab-keylock-lookup", t->hmask, 0,
				     &seq);
  assert(anchor != displaced);

  tabfwd_set_str_i32(L, t, anchor, 11);
  tabfwd_set_str_i32(L, t, displaced, 22);
  node = lj_tab_node_acq(t);
  assert(strV(&node[0].key) == anchor);
  assert(lj_tab_nextnode_acq(&node[0]) != NULL);
  assert(lj_tab_node_freecount_acq(node) == (MSize)t->hmask + 1u - 2u);
  tabfwd_assert_str_i32(t, anchor, 11);
  tabfwd_assert_str_i32(t, displaced, 22);
  assert(tabfwd_count_next_visible(t) == 2);
  {
    TValue keyv;
    freecount0 = lj_tab_node_freecount_acq(node);
    setstrV(L, &keyv, anchor);
    assert(lj_tab_newkey(L, t, &keyv) == &node[0].val);
    setstrV(L, &keyv, displaced);
    assert((cTValue *)lj_tab_newkey(L, t, &keyv) ==
	   lj_tab_getstr(t, displaced));
    assert(lj_tab_node_freecount_acq(node) == freecount0);
    assert(tabfwd_count_next_visible(t) == 2);
  }

  store_keylock(&node[0]);
  assert(tviskeylock(&node[0].key));
  assert(lj_tab_getstr(t, anchor) == NULL);
  tabfwd_assert_str_i32(t, displaced, 22);
  assert(tabfwd_count_next_visible(t) == 1);

  store_strkey(L, &node[0], anchor);
  tabfwd_assert_str_i32(t, anchor, 11);
  tabfwd_assert_str_i32(t, displaced, 22);
  assert(tabfwd_count_next_visible(t) == 2);
  exercise_tombstone_anchor_insert(L);

  lua_close(L);
  printf("t-tab-keylock-lookup OK: KEYLOCK is filtered from table reads\n");
  return 0;
}
