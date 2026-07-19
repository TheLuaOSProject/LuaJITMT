/*
** Close-time table-store coverage for a quiescent universe whose creator
** pthread exited without a private carrier handoff.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_thr.h"
#include "lj_tg.h"

typedef struct CloseCtx {
  lua_State *L;
  pthread_t owner;
  uint32_t creator_actor;
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

static void *create_on_departing_thread(void *arg)
{
  CloseCtx *ctx = (CloseCtx *)arg;
  lua_State *L = luaL_newstate();

  assert(L != NULL);
  luaL_openlibs(L);

  lua_newtable(L);
  push_marked_table(L, "hash-before-close");
  lua_setfield(L, -2, "hash");
  push_marked_table(L, "array-before-close");
  lua_rawseti(L, -2, 1);
  lua_setglobal(L, "m8_moved_close_sink");
  install_close_finalizer(L);

  ctx->creator_actor = lj_thr_actor_current();
  assert(ctx->creator_actor != 0);
  assert(lj_tg_actor_acq(G(L)->main_tg) == ctx->creator_actor);
  ctx->L = L;

#if !LJ_TARGET_LINUX
  /* macOS and Windows deliberately fail closed for an exited actor until their
  ** carrier has a kernel-backed death witness. Preserve the portable moved
  ** close contract through the explicit quiescent handoff on those targets. */
  assert(lj_thr_tg_handoff_current(G(L)->main_tg));
#endif

#if LJ_TARGET_LINUX
  /* Intentionally return with both the raw compatibility binding and TG actor
  ** untouched. Teardown updates only the never-freed actor-record hint, while
  ** kernel task termination supplies final authority; this path invokes no
  ** private carrier handoff or raw-unbind operation. */
#endif
  return NULL;
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
  CloseCtx ctx;
  pthread_t creator, closer;

  memset(&ctx, 0, sizeof(ctx));
  assert(pthread_create(&creator, NULL, create_on_departing_thread, &ctx) == 0);
  assert(pthread_join(creator, NULL) == 0);
  assert(ctx.L != NULL && ctx.creator_actor != 0);

  /* Joining orders TLS teardown before close. Linux additionally supplies the
  ** kernel-confirmed task termination witness and deliberately leaves the TG
  ** tagged with the departed creator. Other targets consume actor zero from
  ** the explicit handoff above. */
#if LJ_TARGET_LINUX
  assert(lj_tg_actor_acq(G(ctx.L)->main_tg) == ctx.creator_actor);
#else
  assert(lj_tg_actor_acq(G(ctx.L)->main_tg) == 0);
#endif
  assert(lj_thr_get_tg() == NULL);

  assert(pthread_create(&closer, NULL, close_on_unbound_thread, &ctx) == 0);
  assert(pthread_join(closer, NULL) == 0);
  assert(ctx.saw_null_before && ctx.saw_null_after && ctx.closed);
  assert(finalizer_calls == 1);
  assert(finalizer_write_checks == 2);

  printf("t-m8-close-moved-state OK: moved close preserved GC-valued "
#if LJ_TARGET_LINUX
	 "finalizer table stores with creator-exit death proof\n");
#else
	 "finalizer table stores with explicit quiescent handoff\n");
#endif
  return 0;
}
