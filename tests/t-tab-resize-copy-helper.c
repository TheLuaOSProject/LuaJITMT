/*
** Focused tests for idempotent table resize hash-slot copying.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

typedef struct KeyPublishCtx {
  Node *node;
  TValue key;
} KeyPublishCtx;

static void sleep_ns(long ns)
{
  struct timespec ts;
  ts.tv_sec = ns / 1000000000l;
  ts.tv_nsec = ns % 1000000000l;
  while (nanosleep(&ts, &ts) != 0)
    ;
}

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

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  test_put_if_absent_does_not_clobber(L);
  test_freeze_copy_is_idempotent(L);
  test_keylock_waits_for_published_key(L);
  lua_close(L);
  printf("t-tab-resize-copy-helper OK: hash copy helper is idempotent\n");
  return 0;
}
