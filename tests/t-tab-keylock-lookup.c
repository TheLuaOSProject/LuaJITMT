/*
** Focused guard for M5 table KEYLOCK lookup filtering.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_state.h"
#include "lj_str.h"
#include "lj_tab.h"

#include "lib/tab_forward_helpers.h"

typedef struct KeylockReleaseCtx {
  Node *node;
  TValue key;
  int delay_ms;
} KeylockReleaseCtx;

static void sleep_ms(int ms)
{
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0)
    ;
}

static void *release_keylock_after_delay(void *arg)
{
  KeylockReleaseCtx *ctx = (KeylockReleaseCtx *)arg;
  sleep_ms(ctx->delay_ms);
  tv_rawstore_rel(&ctx->node->key, tv_rawload(&ctx->key));
  return NULL;
}

static void store_keylock(Node *n)
{
  TValue keylock;
  setkeylockV(&keylock);
  tv_rawstore_rel(&n->key, tv_rawload(&keylock));
}

static void store_strkey(lua_State *L, Node *n, GCstr *s)
{
  TValue key;
  setstrV(L, &key, s);
  copyTVrel(L, &n->key, &key);
}

static void assert_next_empty(GCtab *t)
{
  TValue key, out[2];
  setnilV(&key);
  assert(lj_tab_next(t, &key, out) == 0);
}

static void exercise_unpublished_nil_key_value(lua_State *L)
{
  GCtab *t;
  Node *node;

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask == 7);
  node = lj_tab_node_acq(t);

  lj_tab_storeint(L, &node[0].val, 1234);
  assert(tvisnil(&node[0].key));
  assert_next_empty(t);

  lj_tab_resize(L, t, 0, 0);
  assert(t->hmask == 0);
  assert_next_empty(t);
}

static void exercise_tombstone_anchor_insert(lua_State *L)
{
  GCtab *t;
  GCstr *anchor, *displaced, *replacement;
  Node *node, *freetop0;
  MSize freecount0;
  uint32_t seq = 0;

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask == 7);

  anchor = tabfwd_find_sid_bucket(L, "tab-keylock-lookup", t->hmask, 0,
				  &seq);
  displaced = tabfwd_find_sid_bucket(L, "tab-keylock-lookup", t->hmask, 0,
				     &seq);
  replacement = tabfwd_find_sid_bucket(L, "tab-keylock-lookup", t->hmask, 0,
				       &seq);

  tabfwd_set_str_i32(L, t, anchor, 11);
  tabfwd_set_str_i32(L, t, displaced, 22);
  node = lj_tab_node_acq(t);
  assert(strV(&node[0].key) == anchor);
  assert(lj_tab_nextnode_acq(&node[0]) != NULL);
  lj_tab_storenil(L, &node[0].val);
  assert(tvisnil(lj_tab_getstr(t, anchor)));
  freetop0 = getfreetop(t, node);
  freecount0 = lj_tab_node_freecount_acq(node);
  assert(freecount0 > 0);

  tabfwd_set_str_i32(L, t, replacement, 33);
  assert(getfreetop(t, node) == freetop0);
  assert(lj_tab_node_freecount_acq(node) == freecount0 - 1u);
  assert(tvisstr(&node[0].key) && strV(&node[0].key) == anchor);
  assert(tvisnil(&node[0].val));
  assert(tvisnil(lj_tab_getstr(t, anchor)));
  tabfwd_assert_str_i32(t, displaced, 22);
  tabfwd_assert_str_i32(t, replacement, 33);
  assert(tabfwd_count_next_visible(t) == 2);
}

static void exercise_resize_waits_for_keylock(lua_State *L)
{
  GCtab *t;
  GCstr *anchor;
  Node *oldnode, *newnode, *oldn, *newn;
  MSize oldhmask, newhmask;
  KeylockReleaseCtx ctx;
  pthread_t thread;
  uint32_t seq = 0;

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask == 7);

  anchor = tabfwd_find_sid_bucket(L, "tab-keylock-resize", t->hmask, 0,
				  &seq);
  tabfwd_set_str_i32(L, t, anchor, 44);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  oldn = tabfwd_find_str_node(oldnode, oldhmask, anchor);
  assert(oldn != NULL);
  tabfwd_assert_i32(&oldn->val, 44);

  setstrV(L, L->top, anchor);  /* Keep the hidden key alive during resize. */
  incr_top(L);
  setstrV(L, &ctx.key, anchor);
  ctx.node = oldn;
  ctx.delay_ms = 20;
  store_keylock(oldn);
  assert(tviskeylock(&oldn->key));

  assert(pthread_create(&thread, NULL, release_keylock_after_delay, &ctx) == 0);
  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  assert(pthread_join(thread, NULL) == 0);

  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  newn = tabfwd_find_str_node(newnode, newhmask, anchor);
  assert(newn != NULL);
  assert(strV(&oldn->key) == anchor);
  tabfwd_assert_forward(&oldn->val);
  tabfwd_assert_i32(&newn->val, 44);
  tabfwd_assert_str_i32(t, anchor, 44);

  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  GCstr *anchor, *displaced;
  Node *node;
  MSize freecount0;
  uint32_t seq = 0;

  assert(L != NULL);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask == 7);

  anchor = tabfwd_find_sid_bucket(L, "tab-keylock-lookup", t->hmask, 0,
				  &seq);
  displaced = tabfwd_find_sid_bucket(L, "tab-keylock-lookup", t->hmask, 0,
				     &seq);
  assert(anchor != displaced);

  tabfwd_set_str_i32(L, t, anchor, 11);
  tabfwd_set_str_i32(L, t, displaced, 22);
  node = lj_tab_node_acq(t);
  assert(strV(&node[0].key) == anchor);
  assert(lj_tab_nextnode_acq(&node[0]) != NULL);
  assert(lj_tab_node_freecount_acq(node) == (MSize)t->hmask + 1u - 2u);
  tabfwd_assert_str_i32(t, anchor, 11);
  tabfwd_assert_str_i32(t, displaced, 22);
  assert(tabfwd_count_next_visible(t) == 2);
  {
    TValue keyv;
    freecount0 = lj_tab_node_freecount_acq(node);
    setstrV(L, &keyv, anchor);
    assert(lj_tab_newkey(L, t, &keyv) == &node[0].val);
    setstrV(L, &keyv, displaced);
    assert((cTValue *)lj_tab_newkey(L, t, &keyv) ==
	   lj_tab_getstr(t, displaced));
    assert(lj_tab_node_freecount_acq(node) == freecount0);
    assert(tabfwd_count_next_visible(t) == 2);
  }

  store_keylock(&node[0]);
  assert(tviskeylock(&node[0].key));
  assert(lj_tab_getstr(t, anchor) == NULL);
  tabfwd_assert_str_i32(t, displaced, 22);
  assert(tabfwd_count_next_visible(t) == 1);

  store_strkey(L, &node[0], anchor);
  tabfwd_assert_str_i32(t, anchor, 11);
  tabfwd_assert_str_i32(t, displaced, 22);
  assert(tabfwd_count_next_visible(t) == 2);
  exercise_unpublished_nil_key_value(L);
  exercise_tombstone_anchor_insert(L);
  exercise_resize_waits_for_keylock(L);

  lua_close(L);
  printf("t-tab-keylock-lookup OK: unpublished keys are filtered from table reads\n");
  return 0;
}
