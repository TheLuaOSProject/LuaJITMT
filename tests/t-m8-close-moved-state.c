/*
** Close-time table-store coverage for a quiescent universe moved to an
** unbound OS thread.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_thr.h"

typedef struct CloseCtx {
  lua_State *L;
  pthread_t owner;
  int saw_null_before;
  int saw_null_after;
  int closed;
} CloseCtx;

static CloseCtx *close_ctx;
static int finalizer_calls;
static int finalizer_write_checks;

static void push_marked_table(lua_State *L, const char *marker)
{
  lua_newtable(L);
  lua_pushstring(L, marker);
  lua_setfield(L, -2, "marker");
}

static void check_marked_table(lua_State *L, int idx, const char *marker)
{
  const char *actual;
  assert(lua_istable(L, idx));
  lua_getfield(L, idx, "marker");
  actual = lua_tostring(L, -1);
  assert(actual != NULL && strcmp(actual, marker) == 0);
  lua_pop(L, 1);
  finalizer_write_checks++;
}

static int moved_close_finalizer(lua_State *L)
{
  int base = lua_gettop(L);
  int sink;

  assert(close_ctx != NULL);
  assert(pthread_equal(pthread_self(), close_ctx->owner));
  assert(lj_thr_get_tg() == NULL);
  assert(finalizer_calls++ == 0);

  lua_getglobal(L, "m8_moved_close_sink");
  assert(lua_istable(L, -1));
  sink = lua_gettop(L);

  /* Both destinations already exist. Exercise the guarded hash and array
  ** overwrite paths with fresh GC-valued payloads while lua_close owns the
  ** main state but this OS thread has no raw TG carrier. */
  push_marked_table(L, "hash-finalizer-value");
  lua_setfield(L, sink, "hash");
  push_marked_table(L, "array-finalizer-value");
  lua_rawseti(L, sink, 1);

  lua_getfield(L, sink, "hash");
  check_marked_table(L, -1, "hash-finalizer-value");
  lua_pop(L, 1);
  lua_rawgeti(L, sink, 1);
  check_marked_table(L, -1, "array-finalizer-value");
  lua_pop(L, 1);

  assert(lj_thr_get_tg() == NULL);
  lua_settop(L, base);
  return 0;
}

static void install_close_finalizer(lua_State *L)
{
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, moved_close_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  /* Keep the object reachable so this callback can only run in the terminal
  ** all-finalizers pass, not during an incidental setup collection. */
  lua_setglobal(L, "m8_moved_close_finalizer_owner");
}

static void *close_on_unbound_thread(void *arg)
{
  CloseCtx *ctx = (CloseCtx *)arg;
  ctx->owner = pthread_self();
  ctx->saw_null_before = lj_thr_get_tg() == NULL;
  assert(ctx->saw_null_before);
  close_ctx = ctx;
  lua_close(ctx->L);
  ctx->saw_null_after = lj_thr_get_tg() == NULL;
  ctx->closed = 1;
  return NULL;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  CloseCtx ctx;
  pthread_t thread;

  assert(L != NULL);
  luaL_openlibs(L);

  lua_newtable(L);
  push_marked_table(L, "hash-before-close");
  lua_setfield(L, -2, "hash");
  push_marked_table(L, "array-before-close");
  lua_rawseti(L, -2, 1);
  lua_setglobal(L, "m8_moved_close_sink");
  install_close_finalizer(L);

  memset(&ctx, 0, sizeof(ctx));
  ctx.L = L;

  /* The universe is quiescent at this handoff. Drop the creator's raw
  ** compatibility binding before the fresh pthread becomes its sole actor,
  ** so successful teardown cannot leave a stale creator-side TLS alias. */
  assert(lj_thr_get_tg() != NULL);
  lj_thr_set_tg(NULL);
  assert(lj_thr_get_tg() == NULL);

  assert(pthread_create(&thread, NULL, close_on_unbound_thread, &ctx) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.saw_null_before && ctx.saw_null_after && ctx.closed);
  assert(finalizer_calls == 1);
  assert(finalizer_write_checks == 2);

  printf("t-m8-close-moved-state OK: raw-NULL moved close preserved "
	 "GC-valued finalizer table stores\n");
  return 0;
}
