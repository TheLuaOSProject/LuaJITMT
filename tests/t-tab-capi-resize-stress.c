/*
** Concurrent public C API table setter stress over one shared table.
*/

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define WORKERS 4
#define ITERS 2500

typedef struct WorkerCtx {
  lua_State *L;
  int id;
  int status;
  volatile int *ready;
  volatile int *start;
} WorkerCtx;

static int load_flag(volatile int *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_flag(volatile int *p, int v)
{
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static void add_flag(volatile int *p, int v)
{
  (void)__atomic_add_fetch(p, v, __ATOMIC_ACQ_REL);
}

static void push_payload(lua_State *L, int id, int round)
{
  lua_newtable(L);
  lua_pushinteger(L, id);
  lua_setfield(L, -2, "owner");
  lua_pushinteger(L, round);
  lua_setfield(L, -2, "round");
}

static void *worker_main(void *arg)
{
  WorkerCtx *ctx = (WorkerCtx *)arg;
  lua_State *L = ctx->L;
  int id = ctx->id;
  int i;

  if (!luaMT_attach(L)) {
    ctx->status = 1;
    return NULL;
  }

  if (!lua_istable(L, 1) || !lua_checkstack(L, 16)) {
    ctx->status = 2;
    luaMT_detach(L);
    return NULL;
  }

  add_flag(ctx->ready, 1);
  while (!load_flag(ctx->start))
    sched_yield();

  for (i = 1; i <= ITERS; i++) {
    int array_key = id * 100000 + i;
    int table_key = 1000000 + id * 100000 + i;
    char rawkey[64];
    char field[64];

    lua_pushinteger(L, id * 1000000 + i);
    lua_rawseti(L, 1, array_key);
    if (i > 32 && (i & 7) == 0) {
      lua_pushnil(L);
      lua_rawseti(L, 1, array_key - 32);
    }

    lua_pushinteger(L, table_key);
    push_payload(L, id, i);
    lua_settable(L, 1);
    if (i > 64 && (i % 11) == 0) {
      lua_pushinteger(L, table_key - 48);
      lua_pushnil(L);
      lua_settable(L, 1);
    }

    snprintf(rawkey, sizeof(rawkey), "raw:%d:%d", id, i);
    lua_pushstring(L, rawkey);
    lua_pushinteger(L, id * 3000000 + i);
    lua_rawset(L, 1);
    if (i > 64 && (i % 13) == 0) {
      snprintf(rawkey, sizeof(rawkey), "raw:%d:%d", id, i - 52);
      lua_pushstring(L, rawkey);
      lua_pushnil(L);
      lua_rawset(L, 1);
    }

    snprintf(field, sizeof(field), "field:%d:%d", id, i);
    lua_pushinteger(L, id * 4000000 + i);
    lua_setfield(L, 1, field);
    if (i > 64 && (i % 17) == 0) {
      snprintf(field, sizeof(field), "field:%d:%d", id, i - 60);
      lua_pushnil(L);
      lua_setfield(L, 1, field);
    }

    if ((i & 255) == 0 && lua_gettop(L) != 1) {
      ctx->status = 3;
      luaMT_detach(L);
      return NULL;
    }
  }

  luaMT_detach(L);
  ctx->status = 0;
  return NULL;
}

static void push_weak_value_table(lua_State *L)
{
  lua_newtable(L);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
}

static void setup_sentinel(lua_State *L)
{
  /* Stack on entry: shared table. Leaves: shared, sentinel key, weak values. */
  lua_newtable(L);
  lua_pushliteral(L, "kind");
  lua_pushliteral(L, "sentinel-key");
  lua_settable(L, -3);

  push_weak_value_table(L);

  lua_newtable(L);
  lua_pushliteral(L, "kind");
  lua_pushliteral(L, "sentinel-value");
  lua_settable(L, -3);

  lua_pushvalue(L, -1);
  lua_rawseti(L, 3, 1);

  lua_pushvalue(L, 2);
  lua_pushvalue(L, -2);
  lua_settable(L, 1);
  lua_pop(L, 1);
}

static void verify_worker_results(lua_State *L)
{
  int id;
  for (id = 1; id <= WORKERS; id++) {
    int array_key = id * 100000 + ITERS;
    int table_key = 1000000 + id * 100000 + ITERS;
    lua_Integer got;
    char key[64];

    lua_rawgeti(L, 1, array_key);
    got = lua_tointeger(L, -1);
    if (got != id * 1000000 + ITERS) {
      fprintf(stderr, "array key %d for worker %d: type=%s got=%ld want=%d\n",
	      array_key, id, luaL_typename(L, -1), (long)got,
	      id * 1000000 + ITERS);
      assert(got == id * 1000000 + ITERS);
    }
    lua_pop(L, 1);

    lua_pushinteger(L, table_key);
    lua_gettable(L, 1);
    assert(lua_istable(L, -1));
    lua_getfield(L, -1, "owner");
    assert(lua_tointeger(L, -1) == id);
    lua_pop(L, 1);
    lua_getfield(L, -1, "round");
    assert(lua_tointeger(L, -1) == ITERS);
    lua_pop(L, 2);

    snprintf(key, sizeof(key), "raw:%d:%d", id, ITERS);
    lua_getfield(L, 1, key);
    assert(lua_tointeger(L, -1) == id * 3000000 + ITERS);
    lua_pop(L, 1);

    snprintf(key, sizeof(key), "field:%d:%d", id, ITERS);
    lua_getfield(L, 1, key);
    assert(lua_tointeger(L, -1) == id * 4000000 + ITERS);
    lua_pop(L, 1);
  }
}

static void verify_sentinel(lua_State *L)
{
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);

  lua_pushvalue(L, 2);
  lua_gettable(L, 1);
  assert(lua_istable(L, -1));
  lua_rawgeti(L, 3, 1);
  assert(lua_rawequal(L, -1, -2));
  lua_pop(L, 2);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  pthread_t threads[WORKERS];
  WorkerCtx ctx[WORKERS];
  volatile int ready = 0;
  volatile int start = 0;
  int i;

  assert(L != NULL);
  luaL_openlibs(L);
  lua_newtable(L);
  setup_sentinel(L);
  assert(lua_gettop(L) == 3);

  for (i = 0; i < WORKERS; i++) {
    lua_State *child = lua_newthread(L);
    lua_pushvalue(L, 1);
    lua_xmove(L, child, 1);
    memset(&ctx[i], 0, sizeof(ctx[i]));
    ctx[i].L = child;
    ctx[i].id = i + 1;
    ctx[i].ready = &ready;
    ctx[i].start = &start;
    assert(pthread_create(&threads[i], NULL, worker_main, &ctx[i]) == 0);
  }

  while (load_flag(&ready) != WORKERS)
    sched_yield();
  store_flag(&start, 1);

  for (i = 0; i < WORKERS; i++) {
    assert(pthread_join(threads[i], NULL) == 0);
    assert(ctx[i].status == 0);
  }

  verify_worker_results(L);
  verify_sentinel(L);

  lua_close(L);
  printf("t-tab-capi-resize-stress OK: public C setters survived concurrent resize\n");
  return 0;
}
