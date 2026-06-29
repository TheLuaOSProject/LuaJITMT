/*
** Colocated array resizes must freeze old inline slots before publication.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_tab.h"

#include "lib/tab_forward_helpers.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-tab-colocated-resize requires LJ_TAB_TEST_HELPERS"
#endif

static GCtab *hook_tab;
static TValue *hook_oldarray;
static MSize hook_oldasize;
static int hook_hits;
static int hook_rc_live;
static int hook_rc_nil;
static MSize hook_tail_live;
static MSize hook_tail_nil;

static void colocated_after_freeze(lua_State *L, GCtab *t, TValue *oldarray,
				   MSize oldasize)
{
  TValue key, src;
  assert(t == hook_tab);
  assert(oldarray == hook_oldarray);
  assert(oldasize == hook_oldasize);
  hook_hits++;

  setintV(&key, 1);
  setintV(&src, 2222);
  hook_rc_live = lj_tab_trystoretv_cas_keyed(L, t, &oldarray[1], &key, &src);

  setintV(&key, 2);
  setintV(&src, 3333);
  hook_rc_nil = lj_tab_trystoretv_cas_keyed(L, t, &oldarray[2], &key, &src);

  tabfwd_assert_forward(&oldarray[1]);
  tabfwd_assert_forward(&oldarray[2]);
}

static void colocated_tail_after_freeze(lua_State *L, GCtab *t,
					TValue *oldarray, MSize oldasize)
{
  TValue key, src;
  assert(t == hook_tab);
  assert(oldarray == hook_oldarray);
  assert(oldasize == hook_oldasize);
  hook_hits++;

  setintV(&key, (int32_t)hook_tail_live);
  setintV(&src, 5555);
  hook_rc_live = lj_tab_trystoretv_cas_keyed(L, t,
					     &oldarray[hook_tail_live],
					     &key, &src);

  setintV(&key, (int32_t)hook_tail_nil);
  setintV(&src, 6666);
  hook_rc_nil = lj_tab_trystoretv_cas_keyed(L, t,
					    &oldarray[hook_tail_nil],
					    &key, &src);

  tabfwd_assert_forward(&oldarray[hook_tail_live]);
  tabfwd_assert_forward(&oldarray[hook_tail_nil]);
}

static void exercise_colocated_split_freezes_all_slots(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray, key, src;
  TValue nilv;
  MSize oldasize;

  lua_settop(L, 0);
  lua_createtable(L, 3, 0);
  t = tabV(L->top-1);
  assert(t->colo > 0);
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert(oldasize >= 3);
  lj_tab_storeint(L, lj_tab_setint(L, t, 1), 1111);
  assert(tvisnil(&oldarray[2]));

  hook_tab = t;
  hook_oldarray = oldarray;
  hook_oldasize = oldasize;
  hook_hits = 0;
  hook_rc_live = -1;
  hook_rc_nil = -1;
  lj_tab_test_set_resize_colocated_after_freeze_hook(colocated_after_freeze);
  lj_tab_resize(L, t, LJ_MAX_COLOSIZE + 24, 0);
  lj_tab_test_set_resize_colocated_after_freeze_hook(NULL);

  assert(hook_hits == 1);
  assert(hook_rc_live == LJ_TAB_STORE_CAS_FORWARD);
  assert(hook_rc_nil == LJ_TAB_STORE_CAS_FORWARD);
  assert(t->colo < 0);
  newarray = lj_tab_array_acq(t);
  assert(newarray != oldarray);
  tabfwd_assert_forward(&oldarray[1]);
  tabfwd_assert_forward(&oldarray[2]);
  tabfwd_assert_i32(&newarray[1], 1111);
  lj_tv_load_acq(&nilv, &newarray[2]);
  assert(tvisnil(&nilv));

  setintV(&key, 2);
  setintV(&src, 4444);
  assert(lj_tab_trystoretv_cas_keyed(L, t, lj_tab_setint(L, t, 2),
				     &key, &src) == LJ_TAB_STORE_CAS_OK);
  tabfwd_assert_i32(&newarray[2], 4444);
  tabfwd_assert_i32(lj_tab_getint(t, 1), 1111);
  tabfwd_assert_i32(lj_tab_getint(t, 2), 4444);
}

static void exercise_colocated_shrink_freezes_tail(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, key, src;
  MSize oldasize, shrink_asize = 2;

  lua_settop(L, 0);
  lua_createtable(L, 5, 0);
  t = tabV(L->top-1);
  assert(t->colo > 0);
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert(oldasize > 4);
  lj_tab_storeint(L, lj_tab_setint(L, t, 1), 1111);
  lj_tab_storeint(L, lj_tab_setint(L, t, 3), 3333);
  assert(tvisnil(&oldarray[2]));

  hook_tab = t;
  hook_oldarray = oldarray;
  hook_oldasize = oldasize;
  hook_tail_live = 3;
  hook_tail_nil = 2;
  hook_hits = 0;
  hook_rc_live = -1;
  hook_rc_nil = -1;
  lj_tab_test_set_resize_colocated_after_freeze_hook(colocated_tail_after_freeze);
  lj_tab_resize(L, t, (uint32_t)shrink_asize, 0);
  lj_tab_test_set_resize_colocated_after_freeze_hook(NULL);

  assert(hook_hits == 1);
  assert(hook_rc_live == LJ_TAB_STORE_CAS_FORWARD);
  assert(hook_rc_nil == LJ_TAB_STORE_CAS_FORWARD);
  assert(lj_tab_asize_acq(t) == shrink_asize);
  assert(lj_tab_array_acq(t) == oldarray);
  tabfwd_assert_forward(&oldarray[3]);
  tabfwd_assert_forward(&oldarray[2]);
  tabfwd_assert_i32(lj_tab_getint(t, 1), 1111);
  tabfwd_assert_i32(lj_tab_getint(t, 3), 3333);
  assert(lj_tab_getint(t, 2) == NULL);

  setintV(&key, 2);
  setintV(&src, 7777);
  assert(lj_tab_trystoretv_cas_keyed(L, t, lj_tab_setint(L, t, 2),
				     &key, &src) == LJ_TAB_STORE_CAS_OK);
  tabfwd_assert_i32(lj_tab_getint(t, 2), 7777);
  tabfwd_assert_forward(&oldarray[2]);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  exercise_colocated_split_freezes_all_slots(L);
  exercise_colocated_shrink_freezes_tail(L);
  lua_close(L);
  printf("t-tab-colocated-resize OK: colocated resizes freeze stale slots\n");
  return 0;
}
