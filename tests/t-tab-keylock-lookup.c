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

static GCstr *find_sid_bucket(lua_State *L, uint32_t mask, uint32_t bucket,
			      uint32_t *seq)
{
  for (;;) {
    char buf[64];
    GCstr *s;
    snprintf(buf, sizeof(buf), "tab-keylock-lookup-%u-%08x",
	     bucket, (*seq)++);
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

static int count_next(GCtab *t);

static void exercise_tombstone_anchor_insert(lua_State *L)
{
  GCtab *t;
  GCstr *anchor, *displaced, *replacement;
  Node *node;
  uint32_t seq = 0;

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask == 7);

  anchor = find_sid_bucket(L, t->hmask, 0, &seq);
  displaced = find_sid_bucket(L, t->hmask, 0, &seq);
  replacement = find_sid_bucket(L, t->hmask, 0, &seq);

  setstrint(L, t, anchor, 11);
  setstrint(L, t, displaced, 22);
  node = lj_tab_node_acq(t);
  assert(strV(&node[0].key) == anchor);
  assert(lj_tab_nextnode_acq(&node[0]) != NULL);
  lj_tab_storenil(L, &node[0].val);
  assert(tvisnil(lj_tab_getstr(t, anchor)));

  setstrint(L, t, replacement, 33);
  assert(tvisstr(&node[0].key) && strV(&node[0].key) == anchor);
  assert(tvisnil(&node[0].val));
  assert(tvisnil(lj_tab_getstr(t, anchor)));
  assert_tabnum(t, displaced, 22);
  assert_tabnum(t, replacement, 33);
  assert(count_next(t) == 2);
}

static int count_next(GCtab *t)
{
  TValue key, out[2];
  int count = 0;
  setnilV(&key);
  while (lj_tab_next(t, &key, out) == 1) {
    assert(!tviskeylock(&out[0]));
    assert(!tvisnil(&out[1]));
    key = out[0];
    count++;
  }
  return count;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  GCstr *anchor, *displaced;
  Node *node;
  uint32_t seq = 0;

  assert(L != NULL);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask == 7);

  anchor = find_sid_bucket(L, t->hmask, 0, &seq);
  displaced = find_sid_bucket(L, t->hmask, 0, &seq);
  assert(anchor != displaced);

  setstrint(L, t, anchor, 11);
  setstrint(L, t, displaced, 22);
  node = lj_tab_node_acq(t);
  assert(strV(&node[0].key) == anchor);
  assert(lj_tab_nextnode_acq(&node[0]) != NULL);
  assert_tabnum(t, anchor, 11);
  assert_tabnum(t, displaced, 22);
  assert(count_next(t) == 2);
  {
    TValue keyv;
    setstrV(L, &keyv, anchor);
    assert(lj_tab_newkey(L, t, &keyv) == &node[0].val);
    setstrV(L, &keyv, displaced);
    assert((cTValue *)lj_tab_newkey(L, t, &keyv) ==
	   lj_tab_getstr(t, displaced));
    assert(count_next(t) == 2);
  }

  store_keylock(&node[0]);
  assert(tviskeylock(&node[0].key));
  assert(lj_tab_getstr(t, anchor) == NULL);
  assert_tabnum(t, displaced, 22);
  assert(count_next(t) == 1);

  store_strkey(L, &node[0], anchor);
  assert_tabnum(t, anchor, 11);
  assert_tabnum(t, displaced, 22);
  assert(count_next(t) == 2);
  exercise_tombstone_anchor_insert(L);

  lua_close(L);
  printf("t-tab-keylock-lookup OK: KEYLOCK is filtered from table reads\n");
  return 0;
}
