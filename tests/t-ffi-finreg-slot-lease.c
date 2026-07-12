/*
** FINREG slot leases must reject a retired hash generation without ever
** claiming its FORWARD marker.
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
  CTypeFinLease current = CTYPE_FIN_LEASE_INIT;
  TValue key, old, stale;
  TValue *oldslot;
  Node *oldnode;
  MSize hmask;
  uint32_t hbits;
  uint32_t readers;
  int rc;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "local ffi=require('ffi'); "
    "return ffi.gc(ffi.new('int[1]'), function() end)") == LUA_OK);
  assert(tviscdata(L->top - 1));
  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  setcdataV(L, &key, cdataV(L->top - 1));

  readers = gc2_smr_readers_acq(G(L));
  assert(lj_ctype_fin_get(L, cts, &key, &held) == LJ_CTYPE_FIN_FOUND);
  assert(held.tab != NULL && held.slot != NULL && held.smr_held);
  assert(gc2_smr_readers_acq(G(L)) == readers + 1u);
  oldslot = held.slot;
  oldnode = lj_tab_node_acq(held.tab);
  hmask = lj_tab_node_hmask_acq(oldnode);
  assert(hmask > 0);
  assert(lj_tab_read_current_keyed(held.tab, oldslot, &key, &old) ==
	 LJ_TAB_STORE_CAS_OK);
  assert(!tvisnil(&old) && !lj_cdata_fin_isclaim(&old));
  copyTVrel(L, L->top, &old);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;

  hbits = lj_fls((uint32_t)hmask) + 2u;
  if (hbits > LJ_MAX_HBITS)
    hbits = LJ_MAX_HBITS;
  lj_tab_resize(L, held.tab, lj_tab_asize_acq(held.tab), hbits);
  assert(lj_tab_node_acq(held.tab) != oldnode);
  lj_tv_load_acq(&stale, oldslot);
  assert(tvisforward(&stale));

  rc = lj_cdata_fin_claim_held(&held, &key, &stale, 1);
  assert(rc == LJ_CTYPE_FIN_RETRY);
  lj_tv_load_acq(&stale, oldslot);
  assert(tvisforward(&stale));  /* FORWARD was not overwritten by FINCLAIM. */
  lj_ctype_fin_lease_release(&held);
  assert(gc2_smr_readers_acq(G(L)) == readers);
  lj_ctype_fin_lease_release(&held);  /* Cleanup is intentionally idempotent. */
  assert(gc2_smr_readers_acq(G(L)) == readers);

  assert(lj_ctype_fin_get(L, cts, &key, &current) == LJ_CTYPE_FIN_FOUND);
  assert(gc2_smr_readers_acq(G(L)) == readers + 1u);
  assert(current.slot != oldslot);
  assert(lj_cdata_fin_claim_held(&current, &key, &stale, 1) ==
	 LJ_CTYPE_FIN_FOUND);
  assert(lj_cdata_fin_store_claim_held(&current, &key, L->top - 1));
  lj_ctype_fin_lease_release(&current);
  assert(gc2_smr_readers_acq(G(L)) == readers);
  L->top--;

  lua_close(L);
  printf("t-ffi-finreg-slot-lease OK: retired vector rejected safely\n");
#else
  printf("t-ffi-finreg-slot-lease SKIP: FFI disabled\n");
#endif
  return 0;
}
