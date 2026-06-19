/*
** Focused guard for M5 table FORWARD value filtering.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

static void store_forward(TValue *slot)
{
  TValue forward;
  setforwardV(&forward);
  tv_rawstore_rel(slot, tv_rawload(&forward));
}

static int count_next(GCtab *t)
{
  TValue key, out[2];
  int count = 0;
  setnilV(&key);
  while (lj_tab_next(t, &key, out) == 1) {
    assert(!tvistabinternal(&out[0]));
    assert(!tvistabinternal(&out[1]));
    key = out[0];
    count++;
  }
  return count;
}

static GCstr *newstr(lua_State *L, const char *s)
{
  return lj_str_new(L, s, strlen(s));
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  GCstr *visible, *hidden;
  TValue *slot;

  assert(L != NULL);
  lua_createtable(L, 4, 4);
  t = tabV(L->top-1);

  lj_tab_storeint(L, lj_tab_setint(L, t, 1), 11);
  lj_tab_storeint(L, lj_tab_setint(L, t, 2), 22);
  slot = lj_tab_setint(L, t, 3);
  lj_tab_storeint(L, slot, 33);
  assert(lj_tab_len(t) == 3);
  store_forward(slot);
  assert(lj_tab_getint(t, 3) == NULL);
  assert(lj_tab_len(t) == 2);
#if LJ_HASJIT
  assert(lj_tab_len_hint(t, 2) == 2);
#endif

  visible = newstr(L, "tab-forward-filter-visible");
  hidden = newstr(L, "tab-forward-filter-hidden");
  lj_tab_storeint(L, lj_tab_setstr(L, t, visible), 44);
  slot = lj_tab_setstr(L, t, hidden);
  lj_tab_storeint(L, slot, 55);
  store_forward(slot);
  assert(lj_tab_getstr(t, hidden) == NULL);
  {
    TValue key;
    setstrV(L, &key, hidden);
    assert(tvisnil(lj_tab_get(L, t, &key)));
  }

  assert(count_next(t) == 3);
  assert(lj_tab_len(t) == 2);

  lua_close(L);
  printf("t-tab-forward-filter OK: FORWARD values stay internal to table scans\n");
  return 0;
}
