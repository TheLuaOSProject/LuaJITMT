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

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  GCstr *anchor0, *displaced, *anchor7;
  Node *node;
  Node *main7next;
  uint32_t seq = 0;

  assert(L != NULL);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask == 7);

  anchor0 = find_sid_bucket(L, t->hmask, 0, &seq);
  displaced = find_sid_bucket(L, t->hmask, 0, &seq);
  anchor7 = find_sid_bucket(L, t->hmask, 7, &seq);
  assert(anchor0 != displaced && anchor0 != anchor7 && displaced != anchor7);

  setstrint(L, t, anchor0, 11);
  setstrint(L, t, displaced, 22);
  node = noderef(t->node);
  assert(strV(&node[0].key) == anchor0);
  assert(strV(&node[7].key) == displaced);

  setstrint(L, t, anchor7, 77);
  node = noderef(t->node);
  assert(lj_tab_nextnode_acq(&node[0]) == &node[7]);
  assert(strV(&node[7].key) == displaced);
  main7next = lj_tab_nextnode_acq(&node[7]);
  assert(main7next != NULL);
  assert(strV(&main7next->key) == anchor7);
  assert_tabnum(t, anchor0, 11);
  assert_tabnum(t, displaced, 22);
  assert_tabnum(t, anchor7, 77);

  lua_close(L);
  printf("t-tab-chain-order OK: stable nodes and release-published links\n");
  return 0;
}
