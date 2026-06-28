/*
** Focused public C API coverage for M4 Lua threading.
*/

#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_tg.h"

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

static int return_99_ran;

static int c_return_99(lua_State *L)
{
  __atomic_store_n(&return_99_ran, 1, __ATOMIC_RELEASE);
  lua_pushinteger(L, 99);
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

typedef struct AttachExclusiveCloseCtx {
  lua_State *L;
  int started;
  int attached;
  int done;
} AttachExclusiveCloseCtx;

typedef struct JoinOwnerCtx {
  lua_State *child;
  int released;
} JoinOwnerCtx;

typedef struct StopreqPublishCtx {
  global_State *g;
  TGState *tg;
  int published;
} StopreqPublishCtx;

static lua_State *join_stopreq_child;

static void store_flag(int *p, int v)
{
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static int load_flag(const int *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void publish_stopreq(global_State *g, TGState *tg)
{
  uint64_t epoch;
  check(tg != NULL, "missing TG for STOPREQ publish");
  epoch = la_load64_rlx(&g->gc2.hs_epoch) + 1u;
  la_store32_rel(&g->gc2.hs_actions, LJ_GC2_HS_STOPREQ);
  la_store32_rel(&g->gc2.hs_pending, 1);
  la_store64_rel(&g->gc2.hs_epoch, epoch);
  lj_tg_reqmask_rel(tg, LJ_GC2_HS_STOPREQ);
  lj_tg_poll_rel(tg, 1);
}

static void *stopreq_publish_worker(void *arg)
{
  enum { WAIT_LIMIT = 1000 };
  StopreqPublishCtx *ctx = (StopreqPublishCtx *)arg;
  int i;
  for (i = 0; i < WAIT_LIMIT; i++) {
    if (lj_tg_in_native_acq(ctx->tg))
      break;
    sleep_ms(1);
  }
  check(lj_tg_in_native_acq(ctx->tg),
	"STOPREQ publisher did not observe native wait");
  publish_stopreq(ctx->g, ctx->tg);
  store_flag(&ctx->published, 1);
  return NULL;
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

static int join_stopreq_c(lua_State *L)
{
  check(join_stopreq_child != NULL, "missing STOPREQ join child");
  return luaMT_join(L, join_stopreq_child, -1.0);
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

  load_lua_function(L,
    "return function()\n"
    "  local t = {}\n"
    "  for i = 1, 64 do t[i] = 'root-' .. i end\n"
    "  return 'rooted', t, 12345\n"
    "end");
  child = luaMT_spawn(L, 0);
  check(child != NULL, "luaMT_spawn rooted worker returned NULL");
  check_spawn_stack(L, base, child, "rooted worker");
  lua_settop(L, base);
  sleep_ms(100);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);

  nres = luaMT_join(L, child, -1.0);
  check(nres == 4, "luaMT_join rooted result count mismatch");
  check(lua_gettop(L) == base + nres,
	"luaMT_join rooted stack height mismatch");
  check(lua_toboolean(L, -4) == 1, "luaMT_join rooted did not return true");
  check(is_string(L, -3, "rooted"), "luaMT_join rooted tag mismatch");
  check(lua_istable(L, -2), "luaMT_join rooted table result missing");
  lua_rawgeti(L, -2, 64);
  check(is_string(L, -1, "root-64"), "luaMT_join rooted table was not preserved");
  lua_pop(L, 1);
  check(lua_tointeger(L, -1) == 12345, "luaMT_join rooted number mismatch");
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

static void *join_owner_release_worker(void *arg)
{
  JoinOwnerCtx *ctx = (JoinOwnerCtx *)arg;
  sleep_ms(50);
  lj_state_owner_rel(ctx->child, 0);
  store_flag(&ctx->released, 1);
  return NULL;
}

static void test_join_waits_for_busy_done_owner(lua_State *L)
{
  enum { WAIT_LIMIT = 1000 };
  const uint32_t fake_owner = 0x60000000u;
  JoinOwnerCtx ctx;
  lua_State *child;
  pthread_t thread;
  int base = lua_gettop(L);
  int nres, i;

  store_flag(&return_99_ran, 0);
  lua_pushcfunction(L, c_return_99);
  child = luaMT_spawn(L, 0);
  check(child != NULL, "luaMT_spawn owner-wait worker returned NULL");
  check_spawn_stack(L, base, child, "owner-wait worker");

  for (i = 0; i < WAIT_LIMIT; i++) {
    if (load_flag(&return_99_ran) == 1 && lj_state_owner_acq(child) == 0)
      break;
    sleep_ms(1);
  }
  check(load_flag(&return_99_ran) == 1 && lj_state_owner_acq(child) == 0,
	"owner-wait worker did not run and release child state");

  lj_state_owner_rel(child, fake_owner);
  memset(&ctx, 0, sizeof(ctx));
  ctx.child = child;
  if (pthread_create(&thread, NULL, join_owner_release_worker, &ctx) != 0)
    failf("pthread_create owner release failed");

  nres = luaMT_join(L, child, -1.0);
  if (pthread_join(thread, NULL) != 0)
    failf("pthread_join owner release failed");
  check(load_flag(&ctx.released) == 1, "owner release worker did not run");
  check(nres == 2, "luaMT_join owner-wait result count mismatch");
  check(lua_gettop(L) == base + nres ||
	lua_gettop(L) == base + nres + 1,
	"luaMT_join owner-wait stack height mismatch");
  check(lua_toboolean(L, -2) == 1, "luaMT_join owner-wait did not return true");
  check(lua_tointeger(L, -1) == 99, "luaMT_join owner-wait result mismatch");
  lua_settop(L, base);
}

static void test_join_busy_done_owner_stopreq(lua_State *L)
{
  enum { WAIT_LIMIT = 1000 };
  const uint32_t fake_owner = 0x60000001u;
  StopreqPublishCtx ctx;
  lua_State *child;
  pthread_t thread;
  int base = lua_gettop(L);
  int status, nres, i;
  const char *err;

  store_flag(&return_99_ran, 0);
  lua_pushcfunction(L, c_return_99);
  child = luaMT_spawn(L, 0);
  check(child != NULL, "luaMT_spawn STOPREQ owner-wait worker returned NULL");
  check_spawn_stack(L, base, child, "STOPREQ owner-wait worker");

  for (i = 0; i < WAIT_LIMIT; i++) {
    if (load_flag(&return_99_ran) == 1 && lj_state_owner_acq(child) == 0)
      break;
    sleep_ms(1);
  }
  check(load_flag(&return_99_ran) == 1 && lj_state_owner_acq(child) == 0,
	"STOPREQ owner-wait worker did not run and release child state");

  lj_state_owner_rel(child, fake_owner);
  join_stopreq_child = child;
  memset(&ctx, 0, sizeof(ctx));
  ctx.g = G(L);
  ctx.tg = L2TG(L);
  check(ctx.tg != NULL, "missing TG for STOPREQ publisher");
  if (pthread_create(&thread, NULL, stopreq_publish_worker, &ctx) != 0)
    failf("pthread_create STOPREQ publisher failed");
  lua_pushcfunction(L, join_stopreq_c);
  status = lua_pcall(L, 0, LUA_MULTRET, 0);
  if (pthread_join(thread, NULL) != 0)
    failf("pthread_join STOPREQ publisher failed");
  check(load_flag(&ctx.published) == 1, "STOPREQ publisher did not run");
  check(status != LUA_OK, "luaMT_join ignored STOPREQ while child owner was busy");
  err = lua_tostring(L, -1);
  check(err && strstr(err, "thread interrupted: VM shutdown") != NULL,
	"luaMT_join STOPREQ error mismatch");
  lua_pop(L, 1);
  (void)lj_tg_flags_and_rlx(L2TG(L), (uint8_t)~TGF_STOPREQ);
  check(lj_state_owner_acq(child) == fake_owner,
	"STOPREQ join claimed or released busy child state");

  lj_state_owner_rel(child, 0);
  join_stopreq_child = NULL;
  nres = luaMT_join(L, child, -1.0);
  check(nres == 2, "luaMT_join STOPREQ recovery result count mismatch");
  check(lua_gettop(L) == base + nres ||
	lua_gettop(L) == base + nres + 1,
	"luaMT_join STOPREQ recovery stack height mismatch");
  check(lua_toboolean(L, -2) == 1, "luaMT_join STOPREQ recovery failed");
  check(lua_tointeger(L, -1) == 99,
	"luaMT_join STOPREQ recovery result mismatch");
  lua_settop(L, base);
}

static void test_mutex_lock_stopreq(lua_State *L)
{
  StopreqPublishCtx ctx;
  pthread_t thread;
  int base = lua_gettop(L);
  int status;
  const char *err;

  check_lua(L, luaL_loadstring(L,
    "local m = require('threading').mutex()\n"
    "m:lock()\n"
    "return function() return m:lock() end, m\n"),
    "creating mutex STOPREQ fixture chunk");
  check_lua(L, lua_pcall(L, 0, 2, 0), "creating mutex STOPREQ fixture");
  check(lua_isfunction(L, base + 1),
	"mutex STOPREQ fixture did not return lock function");
  check(lua_isuserdata(L, base + 2),
	"mutex STOPREQ fixture did not return mutex");

  memset(&ctx, 0, sizeof(ctx));
  ctx.g = G(L);
  ctx.tg = L2TG(L);
  check(ctx.tg != NULL, "missing TG for mutex STOPREQ publisher");
  if (pthread_create(&thread, NULL, stopreq_publish_worker, &ctx) != 0)
    failf("pthread_create mutex STOPREQ publisher failed");
  lua_pushvalue(L, base + 1);
  status = lua_pcall(L, 0, LUA_MULTRET, 0);
  if (pthread_join(thread, NULL) != 0)
    failf("pthread_join mutex STOPREQ publisher failed");
  check(load_flag(&ctx.published) == 1,
	"mutex STOPREQ publisher did not run");
  check(status != LUA_OK, "mutex lock ignored STOPREQ while blocked");
  err = lua_tostring(L, -1);
  check(err && strstr(err, "thread interrupted: VM shutdown") != NULL,
	"mutex lock STOPREQ error mismatch");
  lua_pop(L, 1);
  (void)lj_tg_flags_and_rlx(L2TG(L), (uint8_t)~TGF_STOPREQ);

  lua_getfield(L, base + 2, "unlock");
  lua_pushvalue(L, base + 2);
  check_lua(L, lua_pcall(L, 1, 0, 0), "unlocking mutex after STOPREQ");
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

static void *attach_exclusive_close_worker(void *arg)
{
  AttachExclusiveCloseCtx *ctx = (AttachExclusiveCloseCtx *)arg;
  store_flag(&ctx->started, 1);
  if (luaMT_attach(ctx->L)) {
    store_flag(&ctx->attached, 1);
    luaMT_detach(ctx->L);
  }
  store_flag(&ctx->done, 1);
  return NULL;
}

static void test_close_waits_for_entering_attach(void)
{
  lua_State *L = luaL_newstate();
  lua_State *child;
  global_State *g;
  pthread_t thread;
  AttachExclusiveCloseCtx ctx;
  int waited = 0;

  check(L != NULL, "luaL_newstate exclusive attach-close failed");
  luaL_openlibs(L);
  g = G(L);
  child = lua_newthread(L);
  mt_gc_exclusive_rel(g, 1);
  memset(&ctx, 0, sizeof(ctx));
  ctx.L = child;
  if (pthread_create(&thread, NULL, attach_exclusive_close_worker, &ctx) != 0)
    failf("pthread_create exclusive attach-close failed");
  while (mt_entering_acq(g) == 0) {
    check(!load_flag(&ctx.done), "exclusive attach returned before shutdown");
    sleep_ms(1);
    if (++waited > 5000)
      failf("exclusive attach did not reach entering state");
  }
  check(load_flag(&ctx.started) == 1, "exclusive attach worker did not start");
  check(mt_live_acq(g) == 0, "exclusive attach became live before shutdown");
  lua_close(L);
  if (pthread_join(thread, NULL) != 0)
    failf("pthread_join exclusive attach-close failed");
  check(load_flag(&ctx.done) == 1, "lua_close returned before entering attach finished");
  check(load_flag(&ctx.attached) == 0, "entering attach succeeded during shutdown");
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
  test_join_waits_for_busy_done_owner(L);
  test_join_busy_done_owner_stopreq(L);
  test_mutex_lock_stopreq(L);
  test_attach_detach(L);
  luaMT_fence();

  lua_close(L);
  test_close_waits_for_attach();
  test_close_waits_for_entering_attach();
  puts("t-threading-capi OK: public luaMT spawn/join/fence/attach/mutex verified");
  return 0;
}
