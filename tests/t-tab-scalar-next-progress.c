/* Public VM/direct-rooted progress while the real IDLE reclaimer is paused.
** The separate capi mode intentionally retains the known receiver-capture
** gap; its alarm is a recorded failure, never an accepted iterator result. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_bc.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_tab.h"
#include "lj_tg.h"

#ifndef LJ_GC2_TEST_HELPERS
#error "progress fixture requires GC2 pause helpers"
#endif

typedef struct PauseCtx { global_State *g; uint32_t done; } PauseCtx;
static void *reclaim_main(void *arg)
{
  PauseCtx *ctx = (PauseCtx *)arg;
  (void)lj_gc2_reclaim_retired(ctx->g, lj_gc2_retire_epoch(ctx->g) + 1u);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static int has_op(GCproto *pt, BCOp op)
{
  BCPos i;
  for (i = 0; i < pt->sizebc; i++)
    if (bc_op((BCIns)la_load32_acq((uint32_t *)&proto_bc(pt)[i])) == op)
      return 1;
  return 0;
}

static double score(cTValue *v)
{
  if (tvistrue(v)) return 101;
  if (tvisfalse(v)) return 103;
  assert(tvisnumber(v));
  return numberVnum(v);
}

static void populate(lua_State *L, const char *kind, unsigned *count, double *sum)
{
  int i;
  *count = 0; *sum = 0;
  if (strcmp(kind, "empty") == 0) {
    lua_newtable(L);
  } else if (strcmp(kind, "holes") == 0) {
    lua_createtable(L, 64, 0);
  } else if (strcmp(kind, "sparse") == 0) {
    lua_createtable(L, 40, 0);
    lua_pushnumber(L, 7); lua_rawseti(L, -2, 0);
    lua_pushboolean(L, 0); lua_rawseti(L, -2, 2);
    lua_pushboolean(L, 1); lua_rawseti(L, -2, 17);
    lua_pushnumber(L, -4); lua_rawseti(L, -2, 31);
    *count = 4; *sum = 207;
  } else if (strcmp(kind, "zero") == 0) {
    lua_createtable(L, 8, 0);
    lua_pushnumber(L, 2.5); lua_rawseti(L, -2, 0);
    lua_pushnumber(L, -0.0); lua_rawseti(L, -2, 1);
    *count = 2; *sum = 2.5;
  } else if (strcmp(kind, "bool") == 0) {
    lua_createtable(L, 5, 0);
    lua_pushboolean(L, 0); lua_rawseti(L, -2, 1);
    lua_pushboolean(L, 1); lua_rawseti(L, -2, 2);
    lua_pushboolean(L, 1); lua_rawseti(L, -2, 3);
    *count = 3; *sum = 305;
  } else {
    assert(strcmp(kind, "dense") == 0);
    lua_createtable(L, 5, 0);
    for (i = 1; i <= 5; i++) {
      lua_pushinteger(L, i * 11); lua_rawseti(L, -2, i);
    }
    *count = 5; *sum = 165;
  }
  assert(lj_tab_node_acq(tabV(L->top - 1)) == &G(L)->nilnode);
}

int main(int argc, char **argv)
{
  const char *mode, *kind;
  lua_State *L;
  PauseCtx ctx;
  pthread_t worker;
  struct timespec delay = { 0, 1000000 };
  uint32_t i, anchors;
  unsigned expected_count, count = 0;
  double expected_sum, sum = 0;
  int tableidx = 2;
#ifdef LJ_TAB_TEST_HELPERS
  uint32_t waits, no_l;
#endif
  assert(argc == 3);
  mode = argv[1]; kind = argv[2];
  assert(strcmp(mode, "next") == 0 || strcmp(mode, "itern") == 0 ||
         strcmp(mode, "rooted") == 0 || strcmp(mode, "cursor") == 0 ||
         strcmp(mode, "capi") == 0);
  L = luaL_newstate(); assert(L); luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF));
  assert(lua_checkstack(L, 128));
  if (strcmp(mode, "itern") == 0) {
    assert(luaL_loadstring(L,
      "local next=next; return function(t) local n,s=0,0; "
      "for k,v in next,t do n=n+1; "
      "if v==true then s=s+101 elseif v==false then s=s+103 else s=s+v end "
      "end; return n,s end") == 0);
  } else {
    assert(luaL_loadstring(L,
      "local next=next; return function(t) local n,s,k=0,0,nil; while true do "
      "local v; k,v=next(t,k); if k==nil then break end; n=n+1; "
      "if v==true then s=s+101 elseif v==false then s=s+103 else s=s+v end "
      "end; return n,s end") == 0);
  }
  assert(lua_pcall(L, 0, 1, 0) == 0 && lua_isfunction(L, 1));
  if (strcmp(mode, "itern") == 0) {
    GCproto *pt = funcproto(funcV(L->base));
    assert(has_op(pt, BC_ISNEXT) && has_op(pt, BC_ITERN));
  }
  populate(L, kind, &expected_count, &expected_sum);
  lua_pushnil(L); lua_pushnil(L); lua_pushnil(L);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(gc2_phase_acq(G(L)) == LJ_GC2_IDLE);
  (void)lua_gc(L, LUA_GCSTOP, 0);
  anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  ctx.g = G(L); ctx.done = 0;
  lj_gc2_test_idle_reclaim_pause_after_jit_quiescence();
  assert(pthread_create(&worker, NULL, reclaim_main, &ctx) == 0);
  for (i = 0; i < 5000 && !lj_gc2_test_idle_reclaim_paused(); i++) {
    assert(!la_load32_acq(&ctx.done)); nanosleep(&delay, NULL);
  }
  assert(lj_gc2_test_idle_reclaim_paused());
  assert(gc2_smr_reclaiming_acq(ctx.g) == LJ_GC2_SMR_META_EXCLUSIVE);
  assert(gc2_smr_readers_acq(ctx.g) == 0 && gc2_jit_phase_gate_acq(ctx.g) == 0);
#ifdef LJ_TAB_TEST_HELPERS
  waits = lj_tab_test_wait_l_calls(); no_l = lj_tab_test_wait_no_l_calls();
#endif
  printf("real reclaimer paused: mode=%s kind=%s\n", mode, kind); fflush(stdout);
  alarm(4);
  if (strcmp(mode, "next") == 0 || strcmp(mode, "itern") == 0) {
    lua_settop(L, 2);
    assert(lua_pcall(L, 1, 2, 0) == 0);
    count = (unsigned)lua_tointeger(L, -2); sum = lua_tonumber(L, -1);
  } else if (strcmp(mode, "rooted") == 0) {
    int status;
    while ((status = lj_tab_next_rooted(L, L->base + 1, L->base + 2,
                                       L->base + 2, L->base + 3, NULL)) > 0) {
      count++; sum += score(L->base + 3); assert(count <= expected_count);
    }
    assert(status == 0);
  } else if (strcmp(mode, "cursor") == 0) {
    int status;
    L->base[2].u32.lo = 0; L->base[2].u32.hi = LJ_KEYINDEX;
    while ((status = lj_tab_itern_rooted(L, L->base + 1, L->base + 2)) > 0) {
      count++; sum += score(L->base + 4); assert(count <= expected_count);
    }
    assert(status == 0);
  } else {
    lua_settop(L, 2); lua_pushnil(L);
    while (lua_next(L, tableidx)) {
      count++; sum += score(L->top - 1); lua_pop(L, 1);
      assert(count <= expected_count);
    }
  }
  alarm(0);
  assert(count == expected_count && sum == expected_sum);
  assert(lj_gc2_test_idle_reclaim_paused() && !la_load32_acq(&ctx.done));
  assert(gc2_smr_reclaiming_acq(ctx.g) == LJ_GC2_SMR_META_EXCLUSIVE);
  assert(gc2_smr_readers_acq(ctx.g) == 0 && gc2_jit_phase_gate_acq(ctx.g) == 0);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == anchors);
#ifdef LJ_TAB_TEST_HELPERS
  assert(lj_tab_test_wait_l_calls() == waits && lj_tab_test_wait_no_l_calls() == no_l);
#endif
  lj_gc2_test_idle_reclaim_release();
  assert(pthread_join(worker, NULL) == 0 && la_load32_acq(&ctx.done));
  assert(gc2_smr_reclaiming_acq(ctx.g) == LJ_GC2_SMR_OPEN);
  lua_close(L);
  printf("progress passed: mode=%s kind=%s count=%u sum=%.17g\n", mode, kind, count, sum);
  return 0;
}
