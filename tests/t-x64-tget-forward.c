/*
** Focused x64 guard for TGET array fast paths over forwarded slots.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_tab.h"

#include "lib/tab_forward_helpers.h"

static const char tget_forward_src[] =
  "local t = tget_forward_t\n"
  "local k = tget_forward_key\n"
  "local want = tget_forward_value\n"
  "if tget_forward_arm_release then tget_forward_arm_release() end\n"
  "local function getv(a, key) return a[key] end\n"
  "assert(t[3] == want, type(t[3]))\n"
  "assert(t[k] == want, type(t[k]))\n"
  "assert(getv(t, k) == want, type(getv(t, k)))\n"
  "for i = k, k do assert(t[i] == want, type(t[i])) end\n";

typedef struct TGetArrayReleaseCtx {
  GCtab *t;
  TValue *array;
  MSize asize;
  pthread_t thread;
  int armed;
} TGetArrayReleaseCtx;

static TGetArrayReleaseCtx *tget_release_ctx;

static void tget_sleep_ns(long ns)
{
  struct timespec ts;
  ts.tv_sec = ns / 1000000000L;
  ts.tv_nsec = ns % 1000000000L;
  while (nanosleep(&ts, &ts) != 0)
    ;
}

static void *tget_publish_array_after_delay(void *arg)
{
  TGetArrayReleaseCtx *ctx = (TGetArrayReleaseCtx *)arg;
  tget_sleep_ns(5000000L);
  lj_tab_array_rel(ctx->t, ctx->array);
  lj_tab_asize_rel(ctx->t, ctx->asize);
  return NULL;
}

static int tget_arm_release(lua_State *L)
{
  TGetArrayReleaseCtx *ctx = tget_release_ctx;
  UNUSED(L);
  assert(ctx != NULL);
  assert(!ctx->armed);
  ctx->armed = 1;
  assert(pthread_create(&ctx->thread, NULL,
			tget_publish_array_after_delay, ctx) == 0);
  return 0;
}

static void tget_run_with_retiring_current_root(lua_State *L, GCtab *t,
						TValue *oldarray,
						TValue *newarray,
						MSize oldasize,
						MSize newasize,
						int32_t target)
{
  TGetArrayReleaseCtx ctx;
  int32_t stale = target + 9000;
  int32_t want = target + 3000;
  lj_tab_storeint(L, &oldarray[target], stale);
  lj_tab_storeint(L, &newarray[target], want);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  lua_pushinteger(L, want);
  lua_setglobal(L, "tget_forward_value");
  ctx.t = t;
  ctx.array = newarray;
  ctx.asize = newasize;
  ctx.armed = 0;
  tget_release_ctx = &ctx;
  lua_pushcfunction(L, tget_arm_release);
  lua_setglobal(L, "tget_forward_arm_release");
  tabfwd_load_lua(L, tget_forward_src);
  tabfwd_run_loaded(L);
  assert(ctx.armed);
  assert(pthread_join(ctx.thread, NULL) == 0);
  tget_release_ctx = NULL;
  lua_pushnil(L);
  lua_setglobal(L, "tget_forward_arm_release");
  assert(tabfwd_get_i32(t, target) == want);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  TValue *oldarray, *newarray;
  MSize oldasize, newasize, oldacap;
  int32_t target = 3;
  MSize i;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L, "jit.off()\n") == 0);

  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  assert((MSize)target < oldasize);
  for (i = 0; i < oldasize; i++) {
    int32_t v = (int32_t)i + 3000;
    tabfwd_set_int(L, t, (int32_t)i, v);
    lj_tab_storeint(L, &oldarray[i], v);
  }
  assert(tabfwd_get_i32(t, target) == target + 3000);
  lua_setglobal(L, "tget_forward_t");
  lua_pushinteger(L, target);
  lua_setglobal(L, "tget_forward_key");
  lua_pushinteger(L, target + 3000);
  lua_setglobal(L, "tget_forward_value");
  tabfwd_load_lua(L, tget_forward_src);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  assert(tabfwd_get_i32(t, target) == target + 3000);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);

  tabfwd_store_forward(&oldarray[target]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  assert(tabfwd_get_i32(t, target) == target + 3000);
  tabfwd_run_loaded(L);

  tget_run_with_retiring_current_root(L, t, oldarray, newarray, oldasize,
				      newasize, target);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  lj_tab_asize_rel(t, 0);
  tabfwd_load_lua(L, tget_forward_src);
  tabfwd_run_loaded(L);
  lj_tab_asize_rel(t, newasize);

  lua_close(L);
  printf("t-x64-tget-forward OK: TGET fast paths resolve forwarded array slots\n");
  return 0;
}
