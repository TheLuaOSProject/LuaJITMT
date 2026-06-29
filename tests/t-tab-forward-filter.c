/*
** Focused guard for M5 table FORWARD value filtering.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

#include "lib/tab_forward_helpers.h"

typedef struct ForwardPublishCtx {
  GCtab *t;
  TValue *array;
  MSize asize;
  Node *node;
  MSize hmask;
  TValue *slot;
  TValue val;
  int publish_array;
} ForwardPublishCtx;

static void tabfwd_sleep_ns(long ns)
{
  struct timespec ts;
  ts.tv_sec = ns / 1000000000l;
  ts.tv_nsec = ns % 1000000000l;
  while (nanosleep(&ts, &ts) != 0)
    ;
}

static void *tabfwd_publish_successor(void *arg)
{
  ForwardPublishCtx *ctx = (ForwardPublishCtx *)arg;
  tabfwd_sleep_ns(5000000);
  tv_rawstore_rel(ctx->slot, tv_rawload(&ctx->val));
  if (ctx->publish_array) {
    lj_tab_array_rel(ctx->t, ctx->array);
    lj_tab_asize_rel(ctx->t, ctx->asize);
  } else {
    lj_tab_node_rel(ctx->t, ctx->node);
    lj_tab_hmask_rel(ctx->t, ctx->hmask);
  }
  return NULL;
}

static lua_Number tabfwd_table_maxn(lua_State *L, int idx)
{
  int top = lua_gettop(L);
  lua_Number n;
  if (idx < 0)
    idx = top + idx + 1;
  lua_getglobal(L, "table");
  lua_getfield(L, -1, "maxn");
  lua_pushvalue(L, idx);
  assert(lua_pcall(L, 1, 1, 0) == 0);
  n = lua_tonumber(L, -1);
  lua_settop(L, top);
  return n;
}

static void exercise_forward_waits_for_published_successor(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  MSize oldasize, newasize;
  ForwardPublishCtx ctx;
  pthread_t thread;
  GCstr *key;
  Node *oldnode, *newnode;
  MSize oldhmask, newhmask;
  TValue *newslot;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  lj_tab_storeint(L, lj_tab_setint(L, t, 5), 505);
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  tabfwd_assert_forward(&oldarray[5]);
  lj_tab_storenilraw(&newarray[5]);
  lj_tab_array_rel(t, oldarray);
  lj_tab_asize_rel(t, oldasize);

  ctx.t = t;
  ctx.array = newarray;
  ctx.asize = newasize;
  ctx.node = NULL;
  ctx.hmask = 0;
  ctx.slot = &newarray[5];
  setintV(&ctx.val, 505);
  ctx.publish_array = 1;
  assert(pthread_create(&thread, NULL, tabfwd_publish_successor, &ctx) == 0);
  tabfwd_assert_i32(lj_tab_getint(t, 5), 505);
  assert(pthread_join(thread, NULL) == 0);

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  key = tabfwd_newstr(L, "tab-forward-in-progress-hash");
  lj_tab_storeint(L, lj_tab_setstr(L, t, key), 606);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  lj_tab_resize(L, t, 0, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  tabfwd_assert_forward(tabfwd_find_str_slot(oldnode, oldhmask, key));
  newslot = tabfwd_find_str_slot(newnode, newhmask, key);
  assert(newslot != NULL);
  lj_tab_storenilraw(newslot);
  lj_tab_node_rel(t, oldnode);
  lj_tab_hmask_rel(t, oldhmask);

  ctx.t = t;
  ctx.array = NULL;
  ctx.asize = 0;
  ctx.node = newnode;
  ctx.hmask = newhmask;
  ctx.slot = newslot;
  setintV(&ctx.val, 606);
  ctx.publish_array = 0;
  assert(pthread_create(&thread, NULL, tabfwd_publish_successor, &ctx) == 0);
  tabfwd_assert_i32(lj_tab_getstr(t, key), 606);
  assert(pthread_join(thread, NULL) == 0);
}

static void exercise_forward_hops_after_later_publish(lua_State *L)
{
  GCtab *t;
  TValue *array_a, *array_b, *array_c;
  MSize asize_a, asize_b;
  GCstr *hashkey;
  TValue key, val;
  Node *node_a, *node_b, *node_c;
  MSize hmask_a, hmask_b;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  lj_tab_storeint(L, lj_tab_setint(L, t, 5), 505);
  array_a = lj_tab_array_acq(t);
  asize_a = lj_tab_asize_acq(t);
  lj_tab_resize(L, t, (uint32_t)asize_a + 8u, 0);
  array_b = lj_tab_array_acq(t);
  asize_b = lj_tab_asize_acq(t);
  lj_tab_resize(L, t, (uint32_t)asize_b + 8u, 0);
  array_c = lj_tab_array_acq(t);
  assert(array_a != array_b);
  assert(array_b != array_c);
  assert(lj_tab_array_nextgen_acq(array_a) == array_b);
  assert(lj_tab_array_nextgen_acq(array_b) == array_c);
  tabfwd_assert_forward(&array_a[5]);
  tabfwd_assert_forward(&array_b[5]);
  tabfwd_assert_i32(lj_tab_forwarded_array_slot(t, array_a, asize_a, 5,
						&val), 505);

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  hashkey = tabfwd_newstr(L, "tab-forward-later-publish-hash");
  setstrV(L, &key, hashkey);
  lj_tab_storeint(L, lj_tab_setstr(L, t, hashkey), 606);
  node_a = lj_tab_node_acq(t);
  hmask_a = lj_tab_node_hmask_acq(node_a);
  assert(hmask_a > 0);
  lj_tab_resize(L, t, 0, lj_fls(hmask_a) + 2u);
  node_b = lj_tab_node_acq(t);
  hmask_b = lj_tab_node_hmask_acq(node_b);
  lj_tab_resize(L, t, 0, lj_fls(hmask_b) + 2u);
  node_c = lj_tab_node_acq(t);
  assert(node_a != node_b);
  assert(node_b != node_c);
  assert(lj_tab_node_nextgen_acq(node_a) == node_b);
  assert(lj_tab_node_nextgen_acq(node_b) == node_c);
  tabfwd_assert_forward(tabfwd_find_str_slot(node_a, hmask_a, hashkey));
  tabfwd_assert_forward(tabfwd_find_str_slot(node_b, hmask_b, hashkey));
  tabfwd_assert_i32(lj_tab_forwarded_hash_slot(t, node_a, hmask_a, &key,
					       &val), 606);
}

static void exercise_array_forward_hop(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  MSize oldasize, newasize;
  int32_t tail;
  TValue val;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));

  lj_tab_storeint(L, lj_tab_setint(L, t, 1), 101);
  lj_tab_storeint(L, lj_tab_setint(L, t, 2), 202);
  lj_tab_storeint(L, lj_tab_setint(L, t, 3), 303);
  lj_tab_storeint(L, lj_tab_setint(L, t, 4), 404);
  lj_tab_storeint(L, lj_tab_setint(L, t, 5), 505);
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert(oldasize > 8);
  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  tabfwd_assert_i32(lj_tab_getint(t, 5), 505);
  tabfwd_assert_forward(&oldarray[5]);

  tabfwd_store_forward(&oldarray[5]);
  tabfwd_assert_i32(lj_tab_forwarded_array_slot(t, oldarray, oldasize, 5,
						&val), 505);
  assert(lj_tab_len(t) == 5);
  assert(tabfwd_table_maxn(L, -1) == 5);
#if LJ_HASJIT
  assert(lj_tab_len_hint(t, 5) == 5);
#endif
  assert(tabfwd_count_next_visible(t) == 5);
  lj_tab_storeint(L, lj_tab_setint(L, t, 5), 909);
  tabfwd_assert_forward(&oldarray[5]);
  tabfwd_assert_i32(&newarray[5], 909);
  tabfwd_assert_i32(lj_tab_getint(t, 5), 909);

  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);

  tail = (int32_t)newasize - 1;
  lj_tab_storeint(L, lj_tab_setint(L, t, tail), 606);
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  lj_tab_resize(L, t, 6, 4);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(newasize == 6);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  tabfwd_assert_i32(lj_tab_getint(t, tail), 606);
  tabfwd_assert_forward(&oldarray[tail]);

  tabfwd_store_forward(&oldarray[tail]);
  tabfwd_assert_i32(lj_tab_forwarded_array_slot(t, oldarray, oldasize,
						(MSize)tail, &val), 606);
  assert(tabfwd_table_maxn(L, -1) == (lua_Number)tail);
  lj_tab_storeint(L, lj_tab_setint(L, t, tail), 808);
  tabfwd_assert_forward(&oldarray[tail]);
  tabfwd_assert_i32(lj_tab_getint(t, tail), 808);

  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
}

static void exercise_hash_forward_hop(lua_State *L)
{
  GCtab *t;
  GCstr *hopstr = tabfwd_newstr(L, "tab-forward-hop-string");
  TValue lightkey;
  Node *oldnode, *newnode;
  MSize oldhmask, newhmask;
  TValue *oldstrslot, *oldnumslot, *oldkeyslot;
  TValue strkey;
  TValue val;

  lua_settop(L, 0);
  lua_createtable(L, 4, 8);
  t = tabV(L->top-1);
  setrawlightudV(&lightkey, (void *)(uintptr_t)0x12345);

  lj_tab_storeint(L, lj_tab_setstr(L, t, hopstr), 101);
  lj_tab_storeint(L, lj_tab_setint(L, t, 33), 202);
  lj_tab_storeint(L, lj_tab_set(L, t, &lightkey), 303);

  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldstrslot = tabfwd_find_str_slot(oldnode, oldhmask, hopstr);
  oldnumslot = tabfwd_find_num_slot(oldnode, oldhmask, 33);
  oldkeyslot = tabfwd_find_key_slot(oldnode, oldhmask, &lightkey);
  assert(oldstrslot != NULL);
  assert(oldnumslot != NULL);
  assert(oldkeyslot != NULL);
  setstrV(L, &strkey, hopstr);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  tabfwd_assert_i32(lj_tab_getstr(t, hopstr), 101);
  tabfwd_assert_i32(lj_tab_getint(t, 33), 202);
  tabfwd_assert_i32(lj_tab_get(L, t, &lightkey), 303);
  tabfwd_assert_forward(oldstrslot);
  tabfwd_assert_forward(oldnumslot);
  tabfwd_assert_forward(oldkeyslot);

  tabfwd_store_forward(oldstrslot);
  tabfwd_store_forward(oldnumslot);
  tabfwd_store_forward(oldkeyslot);

  tabfwd_assert_i32(lj_tab_forwarded_hash_slot(t, oldnode, oldhmask, &strkey,
					       &val), 101);
  {
    TValue numkey;
    setnumV(&numkey, 33);
    tabfwd_assert_i32(lj_tab_forwarded_hash_slot(t, oldnode, oldhmask,
						 &numkey, &val), 202);
  }
  tabfwd_assert_i32(lj_tab_forwarded_hash_slot(t, oldnode, oldhmask,
					       &lightkey, &val), 303);
  assert(tabfwd_table_maxn(L, -1) == 33);
  assert(tabfwd_count_next_visible(t) == 3);
  lj_tab_storeint(L, lj_tab_setstr(L, t, hopstr), 404);
  lj_tab_storeint(L, lj_tab_setint(L, t, 33), 505);
  lj_tab_storeint(L, lj_tab_set(L, t, &lightkey), 606);
  tabfwd_assert_forward(oldstrslot);
  tabfwd_assert_forward(oldnumslot);
  tabfwd_assert_forward(oldkeyslot);
  tabfwd_assert_i32(tabfwd_find_str_slot(newnode, newhmask, hopstr), 404);
  tabfwd_assert_i32(tabfwd_find_num_slot(newnode, newhmask, 33), 505);
  tabfwd_assert_i32(tabfwd_find_key_slot(newnode, newhmask, &lightkey), 606);
  tabfwd_assert_i32(lj_tab_getstr(t, hopstr), 404);
  tabfwd_assert_i32(lj_tab_getint(t, 33), 505);
  tabfwd_assert_i32(lj_tab_get(L, t, &lightkey), 606);

  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);
}

static void exercise_hash_to_array_forward_hop(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  Node *oldnode, *newnode;
  MSize oldasize, oldhmask;
  int32_t moveint;
  TValue *oldnumslot;
  TValue key, val;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 8, 8);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  moveint = (int32_t)oldasize + 5;

  lj_tab_storeint(L, lj_tab_setint(L, t, moveint), 707);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldnumslot = tabfwd_find_num_slot(oldnode, oldhmask, moveint);
  assert(oldnumslot != NULL);

  lj_tab_resize(L, t, (uint32_t)oldasize + 16u, lj_fls(oldhmask) + 1u);
  newarray = lj_tab_array_acq(t);
  newnode = lj_tab_node_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  tabfwd_assert_i32(lj_tab_getint(t, moveint), 707);
  tabfwd_assert_forward(oldnumslot);

  tabfwd_store_forward(oldnumslot);
  setnumV(&key, (lua_Number)moveint);
  tabfwd_assert_i32(lj_tab_forwarded_hash_slot(t, oldnode, oldhmask, &key,
					       &val), 707);
  assert(tabfwd_table_maxn(L, -1) == (lua_Number)moveint);
  lj_tab_storeint(L, lj_tab_setint(L, t, moveint), 909);
  tabfwd_assert_forward(oldnumslot);
  tabfwd_assert_i32(&newarray[moveint], 909);
  tabfwd_assert_i32(lj_tab_getint(t, moveint), 909);
  assert(tabfwd_count_next_visible(t) == 1);

  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  GCstr *visible, *hidden;
  TValue lightkey;
  TValue *slot;

  assert(L != NULL);
  luaL_openlibs(L);

  exercise_forward_waits_for_published_successor(L);
  exercise_forward_hops_after_later_publish(L);

  lua_createtable(L, 4, 4);
  t = tabV(L->top-1);

  lj_tab_storeint(L, lj_tab_setint(L, t, 1), 11);
  lj_tab_storeint(L, lj_tab_setint(L, t, 2), 22);
  slot = lj_tab_setint(L, t, 3);
  lj_tab_storeint(L, slot, 33);
  assert(lj_tab_len(t) == 3);
  tabfwd_store_forward(slot);
  assert(lj_tab_getint(t, 3) == NULL);
  assert(lj_tab_len(t) == 2);
  assert(tabfwd_table_maxn(L, -1) == 2);
#if LJ_HASJIT
  assert(lj_tab_len_hint(t, 2) == 2);
#endif
  lj_tab_storeint(L, lj_tab_setint(L, t, 3), 333);
  tabfwd_assert_forward(slot);
  tabfwd_assert_i32(lj_tab_getint(t, 3), 333);
  assert(lj_tab_len(t) == 3);
  assert(tabfwd_table_maxn(L, -1) == 3);

  visible = tabfwd_newstr(L, "tab-forward-filter-visible");
  hidden = tabfwd_newstr(L, "tab-forward-filter-hidden");
  lj_tab_storeint(L, lj_tab_setstr(L, t, visible), 44);
  slot = lj_tab_setstr(L, t, hidden);
  lj_tab_storeint(L, slot, 55);
  tabfwd_store_forward(slot);
  assert(lj_tab_getstr(t, hidden) == NULL);
  {
    TValue key;
    setstrV(L, &key, hidden);
    assert(tvisnil(lj_tab_get(L, t, &key)));
  }
  lj_tab_storeint(L, lj_tab_setstr(L, t, hidden), 66);
  tabfwd_assert_forward(slot);
  tabfwd_assert_i32(lj_tab_getstr(t, hidden), 66);

  setrawlightudV(&lightkey, (void *)(uintptr_t)0xfeed1234);
  slot = lj_tab_set(L, t, &lightkey);
  lj_tab_storeint(L, slot, 77);
  tabfwd_store_forward(slot);
  assert(tvisnil(lj_tab_get(L, t, &lightkey)));
  lj_tab_storeint(L, lj_tab_set(L, t, &lightkey), 88);
  tabfwd_assert_forward(slot);
  tabfwd_assert_i32(lj_tab_get(L, t, &lightkey), 88);

  assert(tabfwd_count_next_visible(t) == 6);
  assert(lj_tab_len(t) == 3);
  exercise_array_forward_hop(L);
  exercise_hash_forward_hop(L);
  exercise_hash_to_array_forward_hop(L);

  lua_close(L);
  printf("t-tab-forward-filter OK: FORWARD values stay internal to table scans\n");
  return 0;
}
