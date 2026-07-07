/*
** Focused tests for idempotent table resize hash-slot copying.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lib/test_sleep.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

typedef struct KeyPublishCtx {
  Node *node;
  TValue key;
} KeyPublishCtx;


static void *publish_key_after_delay(void *arg)
{
  KeyPublishCtx *ctx = (KeyPublishCtx *)arg;
  sleep_ns(5000000L);
  tv_rawstore_rel(&ctx->node->key, tv_rawload(&ctx->key));
  return NULL;
}

static GCtab *push_table(lua_State *L, int narr, int nrec)
{
  lua_createtable(L, narr, nrec);
  return tabV(L->top-1);
}

static GCstr *intern_key(lua_State *L, const char *key)
{
  return lj_str_new(L, key, strlen(key));
}

static int tv_i32(cTValue *tv)
{
  assert(tv != NULL);
  if (tvisint(tv))
    return intV(tv);
  assert(tvisnum(tv));
  return (int)numV(tv);
}

static void assert_str_value(GCtab *t, GCstr *key, int want)
{
  assert(tv_i32(lj_tab_getstr(t, key)) == want);
}

static Node *find_str_node(GCtab *t, GCstr *key, MSize *idxp)
{
  MSize hmask, i;
  Node *node = lj_tab_node_snapshot_acq(t, &hmask);
  assert(hmask > 0);
  for (i = 0; i <= hmask; i++) {
    TValue k;
    lj_tv_load_acq(&k, &node[i].key);
    if (tvisstr(&k) && strV(&k) == key) {
      *idxp = i;
      return &node[i];
    }
  }
  assert(0 && "missing source hash node");
  return NULL;
}

static void assert_slot_forwarded(Node *n)
{
  TValue val;
  lj_tv_load_acq(&val, &n->val);
  assert(tvisforward(&val));
}

static void assert_array_slot_forwarded(TValue *slot)
{
  TValue val;
  lj_tv_load_acq(&val, slot);
  assert(tvisforward(&val));
}

static void assert_array_slot_nil(TValue *slot)
{
  TValue val;
  lj_tv_load_acq(&val, slot);
  assert(tvisnil(&val));
}

static void assert_array_slot_value(TValue *slot, int want)
{
  TValue val;
  lj_tv_load_acq(&val, slot);
  assert(tv_i32(&val) == want);
}

static void test_put_if_absent_does_not_clobber(lua_State *L)
{
  GCtab *src, *dst;
  GCstr *key;
  MSize idx;

  lua_settop(L, 0);
  src = push_table(L, 0, 8);
  dst = push_table(L, 0, 8);
  key = intern_key(L, "resize-copy-no-clobber");
  lj_tab_storeint(L, lj_tab_setstr(L, src, key), 11);
  (void)find_str_node(src, key, &idx);

  assert(lj_tab_test_resize_copy_hash_slot(L, src, idx, dst, 0) == 1);
  assert_str_value(dst, key, 11);

  lj_tab_storeint(L, lj_tab_setstr(L, dst, key), 99);
  assert(lj_tab_test_resize_copy_hash_slot(L, src, idx, dst, 0) == 1);
  assert_str_value(dst, key, 99);
}

static void test_freeze_copy_is_idempotent(lua_State *L)
{
  GCtab *src, *dst;
  GCstr *key;
  Node *n;
  MSize idx;

  lua_settop(L, 0);
  src = push_table(L, 0, 8);
  dst = push_table(L, 0, 8);
  key = intern_key(L, "resize-copy-freeze-once");
  lj_tab_storeint(L, lj_tab_setstr(L, src, key), 22);
  n = find_str_node(src, key, &idx);

  assert(lj_tab_test_resize_copy_hash_slot(L, src, idx, dst, 1) == 1);
  assert_slot_forwarded(n);
  assert_str_value(dst, key, 22);

  assert(lj_tab_test_resize_copy_hash_slot(L, src, idx, dst, 1) == 0);
  assert_slot_forwarded(n);
  assert_str_value(dst, key, 22);
}

static void test_keylock_waits_for_published_key(lua_State *L)
{
  GCtab *src, *dst;
  GCstr *key;
  Node *n;
  MSize idx;
  KeyPublishCtx ctx;
  pthread_t thread;

  lua_settop(L, 0);
  src = push_table(L, 0, 8);
  dst = push_table(L, 0, 8);
  key = intern_key(L, "resize-copy-keylock-wait");
  lj_tab_storeint(L, lj_tab_setstr(L, src, key), 33);
  n = find_str_node(src, key, &idx);
  lj_tv_load_acq(&ctx.key, &n->key);
  ctx.node = n;

  setkeylockV(&n->key);
  lj_tab_test_reset_wait_no_l_calls();
  assert(pthread_create(&thread, NULL, publish_key_after_delay, &ctx) == 0);
  assert(lj_tab_test_resize_copy_hash_slot(L, src, idx, dst, 0) == 1);
  assert(pthread_join(thread, NULL) == 0);
  assert(lj_tab_test_wait_no_l_calls() > 0);
  assert_str_value(dst, key, 33);
}

static void test_array_copy_is_idempotent(lua_State *L)
{
  GCtab *src, *dst;
  TValue *oldarray;

  lua_settop(L, 0);
  src = push_table(L, 4, 0);
  dst = push_table(L, 4, 0);
  oldarray = lj_tab_array_acq(src);
  lj_tab_storeint(L, lj_tab_setint(L, src, 1), 44);

  assert(lj_tab_test_resize_copy_array_slot(L, src, 1, dst, 1) == 1);
  assert_array_slot_forwarded(&oldarray[1]);
  assert(tv_i32(lj_tab_getint(dst, 1)) == 44);

  assert(lj_tab_test_resize_copy_array_slot(L, src, 1, dst, 1) == 0);
  assert_array_slot_forwarded(&oldarray[1]);
  assert(tv_i32(lj_tab_getint(dst, 1)) == 44);
}

static void test_array_tail_rehash(lua_State *L)
{
  GCtab *src, *dst;
  TValue *oldarray;

  lua_settop(L, 0);
  src = push_table(L, 5, 0);
  dst = push_table(L, 2, 8);
  oldarray = lj_tab_array_acq(src);
  lj_tab_storeint(L, lj_tab_setint(L, src, 4), 55);

  assert(lj_tab_test_resize_copy_array_slot(L, src, 4, dst, 1) == 1);
  assert_array_slot_forwarded(&oldarray[4]);
  assert(tv_i32(lj_tab_getint(dst, 4)) == 55);
}

static void test_array_nil_freeze_modes(lua_State *L)
{
  GCtab *src, *dst;
  TValue *oldarray;

  lua_settop(L, 0);
  src = push_table(L, 4, 0);
  dst = push_table(L, 4, 0);
  oldarray = lj_tab_array_acq(src);
  assert_array_slot_nil(&oldarray[2]);

  assert(lj_tab_test_resize_copy_array_slot(L, src, 2, dst, 0) == 0);
  assert_array_slot_nil(&oldarray[2]);

  assert(lj_tab_test_resize_copy_array_slot(L, src, 2, dst, 1) == 0);
  assert_array_slot_forwarded(&oldarray[2]);
}

static void test_array_assist_same_index(lua_State *L)
{
  GCtab *src;
  TValue *oldarray, *newarray, *slot;
  MSize oldasize, newasize;

  lua_settop(L, 0);
  src = push_table(L, LJ_MAX_COLOSIZE + 16, 0);
  oldarray = lj_tab_array_acq(src);
  oldasize = lj_tab_asize_acq(src);
  lj_tab_storeint(L, lj_tab_setint(L, src, 1), 66);

  lj_tab_resize(L, src, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(src);
  newasize = lj_tab_asize_acq(src);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  assert(lj_tab_array_is_retiring(src, oldarray));

  lj_tab_array_rel(src, oldarray);
  lj_tab_asize_rel(src, oldasize);

  lj_tab_storeint(L, &oldarray[1], 66);
  lj_tab_storenilraw(&newarray[1]);
  lj_tab_test_reset_wait_no_l_calls();
  slot = lj_tab_test_resize_assist_array_slot(L, src, 1);
  assert(slot == &newarray[1]);
  assert_array_slot_forwarded(&oldarray[1]);
  assert(tv_i32(&newarray[1]) == 66);
  assert(lj_tab_test_wait_no_l_calls() == 0);

  lj_tab_storeint(L, &newarray[1], 99);
  slot = lj_tab_test_resize_assist_array_slot(L, src, 1);
  assert(slot == &newarray[1]);
  assert_array_slot_forwarded(&oldarray[1]);
  assert(tv_i32(&newarray[1]) == 99);
  assert(lj_tab_test_wait_no_l_calls() == 0);

  lj_tab_storenilraw(&oldarray[2]);
  lj_tab_storenilraw(&newarray[2]);
  slot = lj_tab_test_resize_assist_array_slot(L, src, 2);
  assert(slot == &newarray[2]);
  assert_array_slot_forwarded(&oldarray[2]);
  assert_array_slot_nil(&newarray[2]);
  assert(lj_tab_test_wait_no_l_calls() == 0);

  lj_tab_array_rel(src, newarray);
  lj_tab_asize_rel(src, newasize);
}

static void test_array_assist_refuses_tail_migration(lua_State *L)
{
  GCtab *src;
  TValue *oldarray, *newarray, *slot;
  MSize oldasize, newasize, target_asize;
  uint32_t tail;

  lua_settop(L, 0);
  src = push_table(L, LJ_MAX_COLOSIZE + 16, 0);
  oldarray = lj_tab_array_acq(src);
  oldasize = lj_tab_asize_acq(src);
  assert(oldasize > LJ_MAX_COLOSIZE + 8);
  target_asize = oldasize - 8u;
  tail = (uint32_t)(oldasize - 1u);
  lj_tab_storeint(L, lj_tab_setint(L, src, (int32_t)tail), 77);

  lj_tab_resize(L, src, (uint32_t)target_asize, 5);
  newarray = lj_tab_array_acq(src);
  newasize = lj_tab_asize_acq(src);
  assert(newarray != oldarray);
  assert(!lj_tab_array_is_colocated(src, newarray));
  assert(newasize == target_asize);
  assert(tail >= newasize);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  assert(lj_tab_array_is_retiring(src, oldarray));
  assert_array_slot_value(&oldarray[tail], 77);
  assert(tv_i32(lj_tab_getint(src, (int32_t)tail)) == 77);

  lj_tab_test_reset_wait_no_l_calls();
  slot = lj_tab_test_resize_assist_array_slot(L, src, tail);
  assert(slot == NULL);
  assert_array_slot_value(&oldarray[tail], 77);
  assert(tv_i32(lj_tab_getint(src, (int32_t)tail)) == 77);
  assert(lj_tab_test_wait_no_l_calls() == 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  test_put_if_absent_does_not_clobber(L);
  test_freeze_copy_is_idempotent(L);
  test_keylock_waits_for_published_key(L);
  test_array_copy_is_idempotent(L);
  test_array_tail_rehash(L);
  test_array_nil_freeze_modes(L);
  test_array_assist_same_index(L);
  test_array_assist_refuses_tail_migration(L);
  lua_close(L);
  printf("t-tab-resize-copy-helper OK: resize copy helpers are idempotent\n");
  return 0;
}
