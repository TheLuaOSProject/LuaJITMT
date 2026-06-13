/*
** Focused public C API coverage for M4 Lua threading.
*/

#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static void failf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

static void check(int cond, const char *msg)
{
  if (!cond)
    failf("%s", msg);
}

static void check_lua(lua_State *L, int status, const char *what)
{
  if (status != LUA_OK)
    failf("%s: %s", what, lua_tostring(L, -1));
}

static int is_string(lua_State *L, int idx, const char *want)
{
  const char *got = lua_tostring(L, idx);
  return got != NULL && strcmp(got, want) == 0;
}

static void sleep_ms(long ms)
{
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0) {
  }
}

static int c_sleep_done(lua_State *L)
{
  sleep_ms((long)luaL_checkinteger(L, 1));
  lua_pushliteral(L, "done");
  return 1;
}

typedef struct AttachCtx {
  lua_State *L;
  int status;
  int result;
} AttachCtx;

typedef struct AttachCloseCtx {
  lua_State *L;
  int attached;
  int detached;
  int status;
} AttachCloseCtx;

static void store_flag(int *p, int v)
{
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static int load_flag(const int *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void load_lua_function(lua_State *L, const char *chunk)
{
  check_lua(L, luaL_loadstring(L, chunk), "luaL_loadstring");
  check_lua(L, lua_pcall(L, 0, 1, 0), "creating Lua function");
  check(lua_isfunction(L, -1), "chunk did not return a function");
}

static void check_threading_not_required(lua_State *L)
{
  lua_getglobal(L, "package");
  check(lua_istable(L, -1), "package table missing");
  lua_getfield(L, -1, "loaded");
  check(lua_istable(L, -1), "package.loaded table missing");
  lua_getfield(L, -1, "threading");
  check(lua_isnil(L, -1), "threading was required before public C API test");
  lua_pop(L, 3);
}

static void require_threading(lua_State *L)
{
  lua_getglobal(L, "require");
  lua_pushliteral(L, "threading");
  check_lua(L, lua_pcall(L, 1, 1, 0), "require(\"threading\")");
  check(lua_istable(L, -1), "require(\"threading\") did not return a table");
  lua_pop(L, 1);
}

static void check_spawn_stack(lua_State *L, int base, lua_State *child,
			      const char *what)
{
  int top = lua_gettop(L);
  if (top == base)
    return;
  if (top == base + 1 && lua_isthread(L, -1) && lua_tothread(L, -1) == child)
    return;
  failf("%s: luaMT_spawn left unexpected values on stack", what);
}

static void test_spawn_join_before_require(lua_State *L)
{
  lua_State *child;
  int base = lua_gettop(L);
  int nres;

  check_threading_not_required(L);

  load_lua_function(L,
    "return function(a, b)\n"
    "  return a + b\n"
    "end");
  lua_pushinteger(L, 40);
  lua_pushinteger(L, 2);
  child = luaMT_spawn(L, 2);
  check(child != NULL, "luaMT_spawn returned NULL");
  check_spawn_stack(L, base, child, "spawn/join");

  nres = luaMT_join(L, child, -1.0);
  check(nres == 2, "luaMT_join success result count mismatch");
  check(lua_gettop(L) == base + nres ||
	lua_gettop(L) == base + nres + 1,
	"luaMT_join success stack height mismatch");
  check(lua_toboolean(L, -2) == 1, "luaMT_join did not return true");
  check(lua_tointeger(L, -1) == 42, "luaMT_join result mismatch");
  lua_settop(L, base);
}

static void test_timeout_then_success(lua_State *L)
{
  lua_State *child;
  int base = lua_gettop(L);
  int nres;

  lua_pushcfunction(L, c_sleep_done);
  lua_pushinteger(L, 100);
  child = luaMT_spawn(L, 1);
  check(child != NULL, "luaMT_spawn timeout worker returned NULL");
  check_spawn_stack(L, base, child, "timeout worker");

  nres = luaMT_join(L, child, 0.0);
  check(nres == 2, "luaMT_join timeout result count mismatch");
  check(lua_gettop(L) == base + nres ||
	lua_gettop(L) == base + nres + 1,
	"luaMT_join timeout stack height mismatch");
  check(lua_isnil(L, -2), "luaMT_join timeout first result was not nil");
  check(is_string(L, -1, "timeout"), "luaMT_join timeout reason mismatch");
  lua_pop(L, 2);

  nres = luaMT_join(L, child, -1.0);
  check(nres == 2, "luaMT_join later success result count mismatch");
  check(lua_gettop(L) == base + nres ||
	lua_gettop(L) == base + nres + 1,
	"luaMT_join later success stack height mismatch");
  check(lua_toboolean(L, -2) == 1, "luaMT_join later success was not true");
  check(is_string(L, -1, "done"), "luaMT_join later result mismatch");
  lua_settop(L, base);
}

static void test_error_object(lua_State *L)
{
  lua_State *child;
  int base = lua_gettop(L);
  int nres;

  load_lua_function(L,
    "return function()\n"
    "  error({ tag = 'capi-error' }, 0)\n"
    "end");
  child = luaMT_spawn(L, 0);
  check(child != NULL, "luaMT_spawn error worker returned NULL");
  check_spawn_stack(L, base, child, "error worker");

  nres = luaMT_join(L, child, -1.0);
  check(nres == 2, "luaMT_join error result count mismatch");
  check(lua_gettop(L) == base + nres ||
	lua_gettop(L) == base + nres + 1,
	"luaMT_join error stack height mismatch");
  check(lua_toboolean(L, -2) == 0, "luaMT_join error did not return false");
  check(lua_istable(L, -1), "luaMT_join error object was not preserved");
  lua_getfield(L, -1, "tag");
  check(is_string(L, -1, "capi-error"), "luaMT_join error object field mismatch");
  lua_settop(L, base);
}

static void *attach_worker(void *arg)
{
  AttachCtx *ctx = (AttachCtx *)arg;
  lua_State *L = ctx->L;
  if (!luaMT_attach(L)) {
    ctx->status = 1;
    return NULL;
  }
  if (luaL_loadstring(L, "return 17") != LUA_OK) {
    ctx->status = 2;
    luaMT_detach(L);
    return NULL;
  }
  if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
    ctx->status = 3;
    luaMT_detach(L);
    return NULL;
  }
  ctx->result = (int)lua_tointeger(L, -1);
  lua_pop(L, 1);
  if (luaL_loadstring(L,
	"local ok, err = pcall(function()\n"
	"  return require'threading'.current()\n"
	"end)\n"
	"return ok, tostring(err)\n") != LUA_OK) {
    ctx->status = 4;
    luaMT_detach(L);
    return NULL;
  }
  if (lua_pcall(L, 0, 2, 0) != LUA_OK) {
    ctx->status = 5;
    luaMT_detach(L);
    return NULL;
  }
  if (lua_toboolean(L, -2) ||
      strstr(lua_tostring(L, -1), "attached thread is not joinable") == NULL) {
    ctx->status = 6;
    luaMT_detach(L);
    return NULL;
  }
  lua_pop(L, 2);
  luaMT_detach(L);
  ctx->status = 0;
  return NULL;
}

static void test_attach_detach(lua_State *L)
{
  lua_State *child;
  pthread_t thread;
  AttachCtx ctx;
  int base = lua_gettop(L);

  child = lua_newthread(L);
  memset(&ctx, 0, sizeof(ctx));
  ctx.L = child;
  if (pthread_create(&thread, NULL, attach_worker, &ctx) != 0)
    failf("pthread_create failed");
  if (pthread_join(thread, NULL) != 0)
    failf("pthread_join failed");
  check(ctx.status == 0, "luaMT_attach worker failed");
  check(ctx.result == 17, "luaMT_attach worker result mismatch");
  lua_settop(L, base);
}

static void *attach_close_worker(void *arg)
{
  AttachCloseCtx *ctx = (AttachCloseCtx *)arg;
  if (!luaMT_attach(ctx->L)) {
    store_flag(&ctx->status, 1);
    return NULL;
  }
  store_flag(&ctx->attached, 1);
  sleep_ms(100);
  luaMT_detach(ctx->L);
  store_flag(&ctx->detached, 1);
  store_flag(&ctx->status, 0);
  return NULL;
}

static void test_close_waits_for_attach(void)
{
  lua_State *L = luaL_newstate();
  lua_State *child;
  pthread_t thread;
  AttachCloseCtx ctx;
  int waited = 0;

  check(L != NULL, "luaL_newstate attach-close failed");
  luaL_openlibs(L);
  child = lua_newthread(L);
  memset(&ctx, 0, sizeof(ctx));
  ctx.L = child;
  if (pthread_create(&thread, NULL, attach_close_worker, &ctx) != 0)
    failf("pthread_create attach-close failed");
  while (!load_flag(&ctx.attached)) {
    sleep_ms(1);
    if (++waited > 1000)
      failf("attached thread did not attach");
  }
  lua_close(L);
  check(load_flag(&ctx.detached), "lua_close returned before attached thread detached");
  if (pthread_join(thread, NULL) != 0)
    failf("pthread_join attach-close failed");
  check(load_flag(&ctx.status) == 0, "attach-close worker failed");
}

int main(void)
{
  lua_State *L = luaL_newstate();
  check(L != NULL, "luaL_newstate failed");
  luaL_openlibs(L);

  luaMT_fence();
  test_spawn_join_before_require(L);
  require_threading(L);
  test_timeout_then_success(L);
  test_error_object(L);
  test_attach_detach(L);
  luaMT_fence();

  lua_close(L);
  test_close_waits_for_attach();
  puts("t-threading-capi OK: public luaMT spawn/join/fence/attach verified");
  return 0;
}
