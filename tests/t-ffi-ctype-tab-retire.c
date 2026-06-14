/*
** Focused guard for M7 FFI ctype-table RCU retirement.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ctype.h"

static CTypeTab *find_retired(CTState *cts, CTypeTab *tabh)
{
  CTypeTab *ret;
  for (ret = cts->retiredtab; ret != NULL; ret = ret->retired_next)
    if (ret == tabh)
      return ret;
  return NULL;
}

static void dostring(lua_State *L, const char *src)
{
  if (luaL_dostring(L, src) != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "lua error: %s\n", err ? err : "(non-string)");
    assert(0);
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  CTState *cts;
  CTypeTab *oldh, *newh;
  CTypeTab *ret;
  uint64_t retire_epoch;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);

  dostring(L, "local ffi = require('ffi')");
  cts = ctype_ctsG(g);
  assert(cts != NULL);
  oldh = ctype_tabh_acq(cts);
  assert(oldh != NULL);
  assert(cts->retiredtab == NULL);

  dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.typeof('struct { int m7_ctype_tab_retire; }')\n");

  newh = ctype_tabh_acq(cts);
  assert(newh != NULL);
  assert(newh != oldh);
  ret = find_retired(cts, oldh);
  assert(ret != NULL);
  assert(ret->sizetab > 0);
  retire_epoch = ret->retire_epoch;
  assert(lj_ctype_reclaim_retired(g, retire_epoch) == 0);
  assert(find_retired(cts, oldh) != NULL);
  assert(lj_ctype_reclaim_retired(g, retire_epoch + 1u) == 1);
  assert(find_retired(cts, oldh) == NULL);
  assert(ctype_tabh_acq(cts) == newh);

  lua_close(L);
  printf("t-ffi-ctype-tab-retire OK: ctype table grows by RCU and retires by epoch\n");
  return 0;
}
