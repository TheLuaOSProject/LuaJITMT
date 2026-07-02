/*
** Focused x64 guard for ipairs_aux over a forwarded table array slot.
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

static const char ipairs_forward_src[] =
  "if ipairs_forward_arm_release then ipairs_forward_arm_release() end\n"
  "local n, seen, v3 = 0, false, nil\n"
  "for i, v in ipairs(ipairs_forward_t) do\n"
  "  n = n + 1\n"
  "  if i == 3 then v3 = v end\n"
  "  if v == ipairs_forward_value then seen = true end\n"
  "end\n"
  "assert(v3 == ipairs_forward_value, type(v3))\n"
  "assert(n > 0 and seen, tostring(n)..':'..tostring(seen))\n";

typedef struct IPairsArrayReleaseCtx {
  GCtab *t;
  TValue *array;
  MSize asize;
  pthread_t thread;
  int armed;
} IPairsArrayReleaseCtx;

static IPairsArrayReleaseCtx *ipairs_release_ctx;

static void ipairs_sleep_ns(long ns)
{
  struct timespec ts;
  ts.tv_sec = ns / 1000000000L;
  ts.tv_nsec = ns % 1000000000L;
  while (nanosleep(&ts, &ts) != 0)
    ;
}

static void *ipairs_publish_array_after_delay(void *arg)
{
  IPairsArrayReleaseCtx *ctx = (IPairsArrayReleaseCtx *)arg;
  ipairs_sleep_ns(5000000L);
  lj_tab_array_rel(ctx->t, ctx->array);
  lj_tab_asize_rel(ctx->t, ctx->asize);
  return NULL;
}

static int ipairs_arm_release(lua_State *L)
{
  IPairsArrayReleaseCtx *ctx = ipairs_release_ctx;
  UNUSED(L);
  assert(ctx != NULL);
  assert(!ctx->armed);
  ctx->armed = 1;
  assert(pthread_create(&ctx->thread, NULL,
			ipairs_publish_array_after_delay, ctx) == 0);
  return 0;
}

static void ipairs_run_with_retiring_current_root(lua_State *L, GCtab *t,
						  TValue *oldarray,
						  TValue *newarray,
						  MSize oldasize,
						  MSize newasize,
						  int32_t target)
{
  IPairsArrayReleaseCtx ctx;
  int32_t want = target + 1000;
  lj_tab_storeint(L, &oldarray[target], target + 9000);
  lj_tab_storeint(L, &newarray[target], want);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  lua_pushinteger(L, want);
  lua_setglobal(L, "ipairs_forward_value");
  ctx.t = t;
  ctx.array = newarray;
  ctx.asize = newasize;
  ctx.armed = 0;
  ipairs_release_ctx = &ctx;
  lua_pushcfunction(L, ipairs_arm_release);
  lua_setglobal(L, "ipairs_forward_arm_release");
  tabfwd_load_lua(L, ipairs_forward_src);
  tabfwd_run_loaded(L);
  assert(ctx.armed);
  assert(pthread_join(ctx.thread, NULL) == 0);
  ipairs_release_ctx = NULL;
  lua_pushnil(L);
  lua_setglobal(L, "ipairs_forward_arm_release");
  assert(tabfwd_get_i32(t, target) == want);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  TValue *oldarray, *newarray;
  MSize oldasize, newasize, oldacap;
  int32_t target;
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
  assert(oldasize > 4);
  target = 3;
  for (i = 0; i < oldasize; i++)
    tabfwd_set_int(L, t, (int32_t)i, (int32_t)i + 1000);
  for (i = 0; i < oldasize; i++)
    lj_tab_storeint(L, &oldarray[i], (int32_t)i + 1000);
  assert(tabfwd_get_i32(t, 0) == 1000);
  assert(tabfwd_get_i32(t, 1) == 1001);
  assert(tabfwd_get_i32(t, target) == target + 1000);
  lua_setglobal(L, "ipairs_forward_t");
  lua_pushinteger(L, target + 1000);
  lua_setglobal(L, "ipairs_forward_value");
  tabfwd_load_lua(L, ipairs_forward_src);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  assert(tabfwd_get_i32(t, 0) == 1000);
  assert(tabfwd_get_i32(t, 1) == 1001);
  assert(tabfwd_get_i32(t, target) == target + 1000);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);

  tabfwd_store_forward(&oldarray[target]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  assert(tabfwd_get_i32(t, 0) == 1000);
  assert(tabfwd_get_i32(t, 1) == 1001);
  assert(tabfwd_get_i32(t, target) == target + 1000);
  {
    cTValue *tv = lj_tab_getint_hop(t, target);
    assert(tv != NULL);
    assert(tvisnumber(tv));
    assert((tvisint(tv) ? intV(tv) : (int32_t)numV(tv)) == target + 1000);
  }
  tabfwd_run_loaded(L);

  ipairs_run_with_retiring_current_root(L, t, oldarray, newarray, oldasize,
					newasize, target);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  lj_tab_asize_rel(t, 0);
  tabfwd_load_lua(L, ipairs_forward_src);
  tabfwd_run_loaded(L);
  lj_tab_asize_rel(t, newasize);

  lua_close(L);
  printf("t-x64-ipairs-forward OK: ipairs_aux resolves forwarded array slots\n");
  return 0;
}
