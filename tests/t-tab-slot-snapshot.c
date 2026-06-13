/*
** Focused guard for M5 table hash-node TValue snapshots.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

static void setstrint(lua_State *L, GCtab *t, const char *key, int32_t v)
{
  GCstr *s = lj_str_new(L, key, strlen(key));
  TValue *slot = lj_tab_setstr(L, t, s);
  setintV(slot, v);
}

static void checkstrint(lua_State *L, GCtab *t, const char *key, int32_t v)
{
  GCstr *s = lj_str_new(L, key, strlen(key));
  cTValue *slot = lj_tab_getstr(t, s);
  assert(slot != NULL);
  assert(tvisnumber(slot));
  assert((int32_t)numV(slot) == v);
}

static int count_next(GCtab *t)
{
  TValue key, out[2];
  int count = 0;
  setnilV(&key);
  while (lj_tab_next(t, &key, out) == 1) {
    key = out[0];
    assert(!tvisnil(&out[1]));
    count++;
  }
  return count;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;

  assert(L != NULL);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);

  setstrint(L, t, "tab-slot-alpha", 11);
  setstrint(L, t, "tab-slot-beta", 22);
  setstrint(L, t, "tab-slot-gamma", 33);
  assert(count_next(t) == 3);

  lj_tab_resize(L, t, t->asize, lj_fls(t->hmask) + 2u);
  checkstrint(L, t, "tab-slot-alpha", 11);
  checkstrint(L, t, "tab-slot-beta", 22);
  checkstrint(L, t, "tab-slot-gamma", 33);
  assert(count_next(t) == 3);

  lua_close(L);
  printf("t-tab-slot-snapshot OK: hash-node TValue snapshots preserve table behavior\n");
  return 0;
}
