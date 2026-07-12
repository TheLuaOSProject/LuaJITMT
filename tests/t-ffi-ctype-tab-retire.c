/*
** Focused regression test for M7 FFI ctype-table RCU retirement.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_gc.h"
#include "lj_gc2.h"

#include "lib/ctype_growth_fixture_helpers.h"
#include "lib/lua_fixture_helpers.h"

static CTypeTab *new_retired_tab(lua_State *L, uint64_t epoch)
{
  CTypeTab *tabh = lj_mem_newt(L, sizeof(*tabh), CTypeTab);
  memset(tabh, 0, sizeof(*tabh));
  ctype_tab_sizetab_rel(tabh, 1);
  ctype_tab_retire_epoch_rel(tabh, epoch);
  ctype_tab_retired_next_rel(tabh, NULL);
  return tabh;
}

static unsigned retired_count(global_State *g, CTState *cts)
{
  CTypeTab *ret;
  unsigned n = 0;
  for (ret = ctype_retiredtab_acq(cts); ret != NULL;
       ret = ctype_tab_retired_next_acq(ret)) {
    assert(lj_gc2_mem_registered_known(g, ret));
    assert(++n <= 2u);
  }
  return n;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g;
  CTState *cts;
  CTypeTab *oldh, *newh;
  CTypeTab *head, *tail, *expect;
  uint64_t retire_epoch;

  g = G(L);

  ljt_lua_dostring(L, "local ffi = require('ffi')");
  cts = ctype_ctsG(g);
  assert(cts != NULL);
  oldh = ctype_tabh_acq(cts);
  assert(oldh != NULL);
  assert(ctype_retiredtab_acq(cts) == NULL);

  ljt_ctype_force_table_growth(L, cts, "lj_m7_ctype_tab_retire");

  newh = ctype_tabh_acq(cts);
  assert(newh != NULL);
  assert(newh != oldh);

  /* Growth can cross ordinary handshake opportunities and reclaim its first
  ** old generation before this fixture regains control. Drain any remainder,
  ** then publish a deterministic two-record chain with the same CTypeTab
  ** allocation/layout contract as the real growth path. */
  retire_epoch = lj_gc2_retire_epoch(g);
  (void)lj_gc2_reclaim_retired(g, retire_epoch + 1u);
  assert(ctype_retiredtab_acq(cts) == NULL);
  head = new_retired_tab(L, retire_epoch);
  tail = new_retired_tab(L, retire_epoch);
  ctype_tab_retired_next_rel(head, tail);
  expect = NULL;
  assert(ctype_retiredtab_cas(cts, &expect, head));

  (void)lj_gc2_reclaim_retired(g, retire_epoch);
  assert(retired_count(g, cts) == 2u);
  assert(lj_gc2_reclaim_retired(g, retire_epoch + 1u) >= 2);
  assert(ctype_retiredtab_acq(cts) == NULL);
  assert(ctype_tabh_acq(cts) == newh);

  lua_close(L);
  printf("t-ffi-ctype-tab-retire OK: ctype growth and generic exact-owner chain retirement verified\n");
  return 0;
}
