/*
** Public raw lookups and stores while a shared hash publisher is paused.
** The test hooks control scheduling without supplying keys or writing slots.
*/
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_tab.h"
#include "lj_thr.h"
#include "lib/test_sleep.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-tab-newkey-public requires LJ_TAB_TEST_HELPERS"
#endif

enum {
  TABLE = 1, KEY, COLLIDER, OTHER, EXISTING, OWNER_VALUE, PEER_VALUE
};

enum {
  RESERVE, KEY_CAS, BEFORE_LINK, AFTER_LINK, VALUE_CAS
};

typedef struct Insertion {
  lua_State *owner;
  GCtab *table;
  int stage;
  uint32_t paused, resume;
} Insertion;

static Insertion *insertion;

static void pause_owner(lua_State *L, GCtab *t, int stage)
{
  Insertion *ctx = insertion;
  if (L != ctx->owner || t != ctx->table || stage != ctx->stage)
    return;
  if (__atomic_exchange_n(&ctx->paused, 1, __ATOMIC_ACQ_REL))
    return;
  while (!__atomic_load_n(&ctx->resume, __ATOMIC_ACQUIRE))
    sched_yield();
}

static void after_reserve(lua_State *L, GCtab *t, Node *base)
{
  (void)base;
  pause_owner(L, t, RESERVE);
}

static void before_barrier(lua_State *L, GCtab *t, cTValue *keyslot)
{
  if (L == insertion->owner && t == insertion->table) {
    TValue key;
    lj_tv_load_acq(&key, keyslot);
    assert(tvistab(&key));
  }
  pause_owner(L, t, KEY_CAS);
}

static void after_key(lua_State *L, GCtab *t, Node *base, Node *anchor,
		      Node *claimed, uint32_t stage)
{
  (void)base;
  (void)anchor;
  (void)claimed;
  if (stage == LJ_TAB_NEWKEY_HOOK_COLLISION_NEXT)
    pause_owner(L, t, BEFORE_LINK);
  else if (stage == LJ_TAB_NEWKEY_HOOK_COLLISION_LINK)
    pause_owner(L, t, AFTER_LINK);
}

static void after_value(lua_State *L, GCtab *t, TValue *slot, cTValue *key,
			cTValue *value)
{
  (void)slot;
  if (L == insertion->owner && t == insertion->table)
    assert(tvistab(key) && tvistab(value));
  pause_owner(L, t, VALUE_CAS);
}

static void *store_from_owner(void *arg)
{
  lua_State *L = (lua_State *)arg;
  assert(lj_threading_attach(L));
  lua_rawset(L, 1);
  assert(lua_gettop(L) == 1);
  lj_threading_detach(L, 1);
  return NULL;
}

static void raw_get(lua_State *L, int key)
{
  lua_pushvalue(L, key);
  lua_rawget(L, TABLE);
}

static void raw_set(lua_State *L, int key, int value)
{
  lua_pushvalue(L, key);
  lua_pushvalue(L, value);
  lua_rawset(L, TABLE);
}

static void raw_set_int(lua_State *L, int key, int value)
{
  lua_pushvalue(L, key);
  lua_pushinteger(L, value);
  lua_rawset(L, TABLE);
}

static void check_int(lua_State *L, int key, int value)
{
  raw_get(L, key);
  assert(lua_type(L, -1) == LUA_TNUMBER && lua_tointeger(L, -1) == value);
  lua_pop(L, 1);
}

static void check_value(lua_State *L, int key, int value)
{
  raw_get(L, key);
  assert(value ? lua_rawequal(L, value, -1) : lua_isnil(L, -1));
  lua_pop(L, 1);
}

static void push_bucket_key(lua_State *L, Node *nodes, MSize mask, Node *anchor,
			    int slot, int same_bucket)
{
  int i;
  for (i = 0; i < 4096; i++) {
    lua_pushfstring(L, "paused-insert-%d-%d", slot, i);
    if ((hashstr_node(nodes, mask, strV(L->top-1)) == anchor) ==
	same_bucket)
      return;
    lua_pop(L, 1);
  }
  assert(0 && "could not allocate a key in the required bucket");
}

static void exercise(lua_State *L, int stage, int collision, int last_slot)
{
  Insertion ctx;
  pthread_t owner;
  Node *nodes, *anchor;
  MSize mask;
  uint32_t i;
  int keys, table_keys, table_values;
  int nslots = last_slot ? 2 : 16;

  lua_settop(L, 0);
  lua_createtable(L, 0, nslots);
  ctx.table = tabV(L->top-1);
  nodes = lj_tab_node_snapshot_acq(ctx.table, &mask);
  assert(mask == (MSize)nslots-1u);
  lua_newtable(L);  /* KEY */
  anchor = hashgcref_node(nodes, mask, (L->top-1)->gcr);
  push_bucket_key(L, nodes, mask, anchor, COLLIDER, 1);
  push_bucket_key(L, nodes, mask, anchor, OTHER, 0);
  push_bucket_key(L, nodes, mask, anchor, EXISTING, collision);
  lua_newtable(L);  /* OWNER_VALUE */
  lua_newtable(L);  /* PEER_VALUE */
  assert(lua_gettop(L) == PEER_VALUE);
  raw_set_int(L, EXISTING, 17);
  ctx.owner = lua_newthread(L);
  lua_pushvalue(L, TABLE);
  lua_pushvalue(L, KEY);
  lua_pushvalue(L, OWNER_VALUE);
  lua_xmove(L, ctx.owner, 3);
  ctx.stage = stage;
  ctx.paused = 0;
  ctx.resume = 0;
  insertion = &ctx;
  lj_tab_test_set_newkey_anchor_after_reserve_hook(after_reserve);
  lj_tab_test_set_newkey_chain_after_reserve_hook(after_reserve);
  lj_tab_test_set_newkey_before_barrier_hook(before_barrier);
  lj_tab_test_set_newkey_publish_hook(after_key);
  lj_tab_test_set_store_post_cas_hook(after_value);

  assert(pthread_create(&owner, NULL, store_from_owner, ctx.owner) == 0);
  for (i = 0; i < 50000u && !__atomic_load_n(&ctx.paused, __ATOMIC_ACQUIRE); i++)
    sleep_ns(100000L);
  assert(__atomic_load_n(&ctx.paused, __ATOMIC_ACQUIRE));
  if (last_slot)
    assert(lj_tab_node_freecount_acq(nodes) == 0);
  check_value(L, KEY, stage == VALUE_CAS ? OWNER_VALUE : 0);
  check_int(L, EXISTING, 17);
  raw_set(L, KEY, PEER_VALUE);
  check_value(L, KEY, PEER_VALUE);
  if (last_slot)
    assert(lj_tab_node_acq(ctx.table) != nodes);
  raw_set_int(L, EXISTING, 18);
  raw_set_int(L, COLLIDER, 19);
  raw_set_int(L, OTHER, 20);
  lua_pushvalue(L, KEY);
  lua_pushnil(L);
  lua_rawset(L, TABLE);
  check_value(L, KEY, 0);
  raw_set(L, KEY, PEER_VALUE);
  check_value(L, KEY, PEER_VALUE);
  check_int(L, EXISTING, 18);
  check_int(L, COLLIDER, 19);
  check_int(L, OTHER, 20);

  __atomic_store_n(&ctx.resume, 1, __ATOMIC_RELEASE);
  assert(pthread_join(owner, NULL) == 0);
  lj_tab_test_set_newkey_anchor_after_reserve_hook(NULL);
  lj_tab_test_set_newkey_chain_after_reserve_hook(NULL);
  lj_tab_test_set_newkey_before_barrier_hook(NULL);
  lj_tab_test_set_newkey_publish_hook(NULL);
  lj_tab_test_set_store_post_cas_hook(NULL);
  insertion = NULL;

  check_value(L, KEY, stage == VALUE_CAS ? PEER_VALUE : OWNER_VALUE);
  check_int(L, EXISTING, 18);
  check_int(L, COLLIDER, 19);
  check_int(L, OTHER, 20);

  /* The table is the only remaining root of the keys and final payload. */
  lua_settop(L, TABLE);
  lua_gc(L, LUA_GCCOLLECT, 0);
  keys = table_keys = table_values = 0;
  lua_pushnil(L);
  while (lua_next(L, TABLE)) {
    keys++;
    table_keys += lua_istable(L, -2);
    table_values += lua_istable(L, -1);
    lua_pop(L, 1);
  }
  assert(keys == 4 && table_keys == 1 && table_values == 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  exercise(L, RESERVE, 0, 1);
  exercise(L, RESERVE, 1, 1);
  exercise(L, KEY_CAS, 0, 0);
  exercise(L, KEY_CAS, 1, 0);
  exercise(L, BEFORE_LINK, 1, 0);
  exercise(L, AFTER_LINK, 1, 0);
  exercise(L, VALUE_CAS, 0, 0);
  exercise(L, VALUE_CAS, 1, 0);
  lua_close(L);
  puts("t-tab-newkey-public OK: rawget/rawset progress across paused hash insertion");
  return 0;
}
