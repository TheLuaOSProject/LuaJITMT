/*
** Focused guard for M5 stable table nodes and hash-chain publication.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

static GCstr *find_sid_bucket(lua_State *L, uint32_t mask, uint32_t bucket,
			      uint32_t *seq)
{
  for (;;) {
    char buf[64];
    GCstr *s;
    snprintf(buf, sizeof(buf), "tab-chain-order-%u-%08x", bucket, (*seq)++);
    s = lj_str_new(L, buf, strlen(buf));
    if (((uint32_t)s->sid & mask) == bucket)
      return s;
  }
}

static void setstrint(lua_State *L, GCtab *t, GCstr *s, int32_t v)
{
  TValue *slot = lj_tab_setstr(L, t, s);
  setintV(slot, v);
}

static void assert_tabnum(GCtab *t, GCstr *s, int32_t v)
{
  cTValue *tv = lj_tab_getstr(t, s);
  assert(tv != NULL);
  assert(tvisnumber(tv));
  assert((int32_t)numV(tv) == v);
}

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
    keys[i] = find_sid_bucket(L, t->hmask, 0, &seq);

  for (i = 0; i < 8; i++)
    setstrint(L, t, keys[i], 100 + i);

  node = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(node);
  assert(oldhmask == 31);
  assert(chain_len(&node[0]) == 8);

  setstrint(L, t, keys[8], 108);
  node = lj_tab_node_acq(t);
  assert(lj_tab_node_hmask_acq(node) > oldhmask);
  for (i = 0; i < 9; i++)
    assert_tabnum(t, keys[i], 100 + i);
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

  anchor0 = find_sid_bucket(L, t->hmask, 0, &seq);
  displaced = find_sid_bucket(L, t->hmask, 0, &seq);
  assert(anchor0 != displaced);

  setstrint(L, t, anchor0, 11);
  setstrint(L, t, displaced, 22);
  node = noderef(t->node);
  assert(strV(&node[0].key) == anchor0);
  displaced_node = lj_tab_nextnode_acq(&node[0]);
  assert(displaced_node != NULL);
  assert(strV(&displaced_node->key) == displaced);

  anchor_main = find_sid_bucket(L, t->hmask,
				(uint32_t)(displaced_node - node), &seq);
  assert(anchor_main != anchor0 && anchor_main != displaced);
  setstrint(L, t, anchor_main, 77);
  node = noderef(t->node);
  assert(lj_tab_nextnode_acq(&node[0]) == displaced_node);
  assert(strV(&displaced_node->key) == displaced);
  mainnext = lj_tab_nextnode_acq(displaced_node);
  assert(mainnext != NULL);
  assert(strV(&mainnext->key) == anchor_main);
  assert_tabnum(t, anchor0, 11);
  assert_tabnum(t, displaced, 22);
  assert_tabnum(t, anchor_main, 77);

  exercise_chainlen_resize(L);

  lua_close(L);
  printf("t-tab-chain-order OK: stable nodes and release-published links\n");
  return 0;
}
