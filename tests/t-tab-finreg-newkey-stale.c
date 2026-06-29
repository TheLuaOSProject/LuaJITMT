/*
** FINREG new-key helpers must abandon old hash generations after resize.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_state.h"
#include "lj_str.h"
#include "lj_tab.h"

#include "lib/tab_forward_helpers.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-tab-finreg-newkey-stale requires LJ_TAB_TEST_HELPERS"
#endif

static int hook_hits;

static void resize_after_reserve(lua_State *L, GCtab *t, Node *nodebase)
{
  MSize oldhmask = lj_tab_node_hmask_acq(nodebase);
  hook_hits++;
  lj_tab_resize(L, t, t->asize, lj_fls((uint32_t)oldhmask) + 2u);
  assert(lj_tab_node_acq(t) != nodebase);
  assert(lj_tab_node_nextgen_acq(nodebase) == lj_tab_node_acq(t));
}

static int tv_absent(lua_State *L, cTValue *tv)
{
  return tv == NULL || tv == niltv(L) || tvisnil(tv);
}

static void root_str(lua_State *L, GCstr *s)
{
  setstrV(L, L->top, s);
  incr_top(L);
}

static void exercise_anchor_stale_after_reserve(lua_State *L)
{
  GCtab *t;
  GCstr *s;
  Node *oldnode;
  TValue key, claim, *slot = NULL;
  int rc;

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask == 7);
  oldnode = lj_tab_node_acq(t);
  s = tabfwd_newstr(L, "finreg-anchor-stale");
  root_str(L, s);
  setstrV(L, &key, s);
  setintV(&claim, 0x511);

  hook_hits = 0;
  lj_tab_test_set_newkey_anchor_after_reserve_hook(resize_after_reserve);
  rc = lj_tab_try_newkey_anchor(L, t, &key, &claim, &slot);
  lj_tab_test_set_newkey_anchor_after_reserve_hook(NULL);
  assert(hook_hits == 1);
  assert(rc == -1);
  assert(slot == NULL);
  assert(lj_tab_node_acq(t) != oldnode);
  assert(tv_absent(L, lj_tab_getstr(t, s)));

  rc = lj_tab_try_newkey_anchor(L, t, &key, &claim, &slot);
  assert(rc == 1);
  assert(slot == (TValue *)lj_tab_getstr(t, s));
  assert(tabfwd_tv_i32(slot) == 0x511);
  lj_tab_storenil(L, slot);
  lua_settop(L, 0);
}

static void exercise_chain_stale_after_reserve(lua_State *L)
{
  GCtab *t;
  GCstr *anchor, *collision;
  Node *oldnode;
  TValue key, claim, *slot = NULL;
  uint32_t seq = 0;
  int rc;

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask == 7);
  anchor = tabfwd_find_sid_bucket(L, "finreg-chain-stale", t->hmask, 0,
				  &seq);
  collision = tabfwd_find_sid_bucket(L, "finreg-chain-stale", t->hmask, 0,
				     &seq);
  assert(anchor != collision);
  root_str(L, anchor);
  root_str(L, collision);
  tabfwd_set_str_i32(L, t, anchor, 17);
  oldnode = lj_tab_node_acq(t);
  setstrV(L, &key, collision);
  setintV(&claim, 0x522);

  hook_hits = 0;
  lj_tab_test_set_newkey_chain_after_reserve_hook(resize_after_reserve);
  rc = lj_tab_try_newkey_chain(L, t, &key, &claim, &slot);
  lj_tab_test_set_newkey_chain_after_reserve_hook(NULL);
  assert(hook_hits == 1);
  assert(rc == -1);
  assert(slot == NULL);
  assert(lj_tab_node_acq(t) != oldnode);
  tabfwd_assert_str_i32(t, anchor, 17);
  assert(tv_absent(L, lj_tab_getstr(t, collision)));

  rc = lj_tab_try_newkey_chain(L, t, &key, &claim, &slot);
  assert(rc == 1);
  assert(slot == (TValue *)lj_tab_getstr(t, collision));
  assert(tabfwd_tv_i32(slot) == 0x522);
  lj_tab_storenil(L, slot);
  lua_settop(L, 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  exercise_anchor_stale_after_reserve(L);
  exercise_chain_stale_after_reserve(L);
  lua_close(L);
  printf("t-tab-finreg-newkey-stale OK: FINREG helpers abandon stale generations\n");
  return 0;
}
