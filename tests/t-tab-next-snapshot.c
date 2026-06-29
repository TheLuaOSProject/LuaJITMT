/*
** Table next() must scan the generation that produced its cursor index.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

#include "lib/tab_forward_helpers.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-tab-next-snapshot requires LJ_TAB_TEST_HELPERS"
#endif

static lua_State *hookL;
static GCtab *hooktab;
static MSize hook_asize;
static uint32_t hook_hbits;
static uint32_t hook_expect_idx;
static uint32_t hook_resize_asize;
static int hook_seen;

static void resize_after_keyindex(GCtab *t, uint32_t idx)
{
  assert(t == hooktab);
  assert(idx == hook_expect_idx);
  lj_tab_test_set_next_after_keyindex_hook(NULL);
  hook_seen++;
  lj_tab_resize(hookL, t, hook_resize_asize, hook_hbits);
}

static void reset_hook(void)
{
  lj_tab_test_set_next_after_keyindex_hook(NULL);
  hookL = NULL;
  hooktab = NULL;
  hook_asize = 0;
  hook_hbits = 0;
  hook_expect_idx = 0;
  hook_resize_asize = 0;
  hook_seen = 0;
}

static void install_two_bucket_keys(lua_State *L, GCtab *t, GCstr **firstp,
				    GCstr **secondp, Node **firstnp,
				    Node **secondnp, MSize *hmaskp)
{
  uint32_t seq = 0;
  Node *node = lj_tab_node_acq(t);
  MSize hmask = lj_tab_node_hmask_acq(node);
  assert(hmask >= 1);
  *firstp = tabfwd_find_sid_bucket(L, "next-snapshot-first", (uint32_t)hmask,
				   0, &seq);
  *secondp = tabfwd_find_sid_bucket(L, "next-snapshot-second", (uint32_t)hmask,
				    1, &seq);
  tabfwd_set_str_i32(L, t, *firstp, 101);
  tabfwd_set_str_i32(L, t, *secondp, 202);
  node = lj_tab_node_acq(t);
  hmask = lj_tab_node_hmask_acq(node);
  *firstnp = tabfwd_find_str_node(node, hmask, *firstp);
  *secondnp = tabfwd_find_str_node(node, hmask, *secondp);
  assert(*firstnp == &node[0]);
  assert(*secondnp == &node[1]);
  *hmaskp = hmask;
}

static void assert_next_survives_array_resize(lua_State *L, uint32_t initial_asize,
					      uint32_t resize_asize)
{
  GCtab *t;
  GCstr *first, *second;
  Node *firstn, *secondn;
  MSize hmask;
  TValue *array;
  TValue key, out[2];
  int ok;

  lua_createtable(L, (int)initial_asize, 8);
  t = tabV(L->top-1);
  hook_asize = lj_tab_array_snapshot_acq(t, &array);
  install_two_bucket_keys(L, t, &first, &second, &firstn, &secondn, &hmask);
  hook_hbits = hmask > 0 ? lj_fls((uint32_t)hmask) + 1u : 0;
  hook_expect_idx = (uint32_t)hook_asize + (uint32_t)((firstn + 1) -
						      lj_tab_node_acq(t));
  assert(hook_expect_idx == (uint32_t)hook_asize + 1u);
  UNUSED(secondn);

  hookL = L;
  hooktab = t;
  hook_resize_asize = resize_asize;
  lj_tab_test_set_next_after_keyindex_hook(resize_after_keyindex);

  setstrV(L, &key, first);
  ok = lj_tab_next(t, &key, out);
  lj_tab_test_set_next_after_keyindex_hook(NULL);
  assert(hook_seen == 1);
  assert(ok == 1);
  assert(!tvistabinternal(&out[0]));
  assert(!tvistabinternal(&out[1]));
  assert(tvisstr(&out[0]) && strV(&out[0]) == second);
  tabfwd_assert_i32(&out[1], 202);
  tabfwd_assert_str_i32(t, second, 202);

  lua_pop(L, 1);
  reset_hook();
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  reset_hook();
  assert_next_survives_array_resize(L, 4, 48);
  assert_next_survives_array_resize(L, LJ_MAX_COLOSIZE + 16, 2);

  lua_close(L);
  printf("t-tab-next-snapshot OK: next() cursor scans stay generation-bound\n");
  return 0;
}
