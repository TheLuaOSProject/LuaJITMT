/*
** A FINREG lease preserves its native entry across vector retirement.
** An old registration generation cannot erase a later callback.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_ctype.h"
#include "lj_cdata.h"

int main(void)
{
#if LJ_HASFFI
  lua_State *L = luaL_newstate();
  CTState *cts;
  CTypeFinLease held = CTYPE_FIN_LEASE_INIT;
  TValue key;
  Node *oldnode;
  la_u128 old, replacement, empty = { 0, 0 };
  uint32_t hbits, readers;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "local ffi=require('ffi'); "
    "return ffi.gc(ffi.new('int[1]'), function() end)") == LUA_OK);
  assert(tviscdata(L->top - 1));
  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  setcdataV(L, &key, cdataV(L->top - 1));

  readers = la_load32_acq(&cts->fin_native_readers);
  assert(lj_ctype_fin_get_native(cts, &key, &held) == LJ_CTYPE_FIN_FOUND);
  assert(held.tab != NULL && held.generation != NULL &&
         held.reader == LJ_CTYPE_FIN_READ_NATIVE);
  assert(la_load32_acq(&cts->fin_native_readers) == readers + 1u);
  old = finreg_load(held.generation);
  assert(finreg_state(old) == FINREG_ACTIVE);
  oldnode = lj_tab_node_acq(held.tab);
  hbits = lj_fls((uint32_t)lj_tab_node_hmask_acq(oldnode)) + 2u;
  lj_tab_resize(L, held.tab, lj_tab_asize_acq(held.tab), hbits);
  assert(lj_tab_node_acq(held.tab) != oldnode);

  lua_pushvalue(L, -1);
  lua_setglobal(L, "registered_cdata");
  assert(luaL_dostring(L, "local ffi=require('ffi'); "
                         "ffi.gc(registered_cdata, function() end)") == LUA_OK);
  replacement = finreg_load(held.generation);
  assert(finreg_state(replacement) == FINREG_ACTIVE);
  assert(finreg_id(replacement) != finreg_id(old));
  assert(!finreg_cas(held.generation, &old, empty));
  assert(old.hi == replacement.hi && old.lo == replacement.lo);

  lj_ctype_fin_lease_release(&held);
  assert(la_load32_acq(&cts->fin_native_readers) == readers);
  lj_ctype_fin_lease_release(&held);
  assert(la_load32_acq(&cts->fin_native_readers) == readers);

  lua_close(L);
  printf("t-ffi-finreg-slot-lease OK: stale registration rejected after vector retirement\n");
#else
  printf("t-ffi-finreg-slot-lease SKIP: FFI disabled\n");
#endif
  return 0;
}
