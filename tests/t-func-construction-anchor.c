/*
** Focused regression test for Lua-function construction anchors and OOM
** cancellation. Built with LJ_FUNC_TEST_HELPERS enabled.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_frame.h"
#include "lj_func.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_vm.h"

#include "lib/lua_fixture_helpers.h"
#include "lib/thread_fixture_helpers.h"

enum {
  OPENUV_THREADS = 4,
  OPENUV_ITERS = 1200
};

typedef struct ConstructCtx {
  GCproto *pt;
  GCtab *env;
  GCfunc *fn;
  uint32_t anchoridx;
} ConstructCtx;

typedef struct InheritCtx {
  GCproto *pt;
  GCfuncL *parent;
  GCfunc *fn;
} InheritCtx;

typedef struct OpenUVThreadCtx {
  lua_State *L;
  GCproto *pt;
  GCfuncL *parent;
  ljt_barrier_t *start;
  BCReg maxslot;
  int status;
} OpenUVThreadCtx;

static GCproto *first_child_proto(GCproto *pt)
{
  MSize i, n = proto_sizekgc_acq(pt);
  for (i = 0; i < n; i++) {
    GCobj *o = proto_kgc(pt, -(int32_t)i - 1);
    if (o && o->gch.gct == ~LJ_TPROTO)
      return gco2pt(o);
  }
  return NULL;
}

static TValue *construct_empty_cp(lua_State *L, lua_CFunction dummy, void *ud)
{
  ConstructCtx *ctx = (ConstructCtx *)ud;
  UNUSED(dummy);
  cframe_errfunc(L->cframe) = -1;
  ctx->fn = lj_func_newL_empty(L, ctx->pt, ctx->env, ctx->anchoridx);
  return NULL;
}

static TValue *construct_inherit_cp(lua_State *L, lua_CFunction dummy,
				    void *ud)
{
  InheritCtx *ctx = (InheritCtx *)ud;
  UNUSED(dummy);
  cframe_errfunc(L->cframe) = -1;
  ctx->fn = lj_func_newL_gc_forjit(L, L->base, ctx->pt, ctx->parent);
  return NULL;
}

static void test_each_upvalue_oom(lua_State *L)
{
  TGState *tg = L2TG(L);
  GCfunc *outer;
  GCproto *child;
  uint32_t nth;

  ljt_lua_dostring(L,
    "return function()\n"
    "  local a, b, c = 1, 2, 3\n"
    "  return function() return a + b + c end\n"
    "end\n");
  outer = funcV(L->top-1);
  child = first_child_proto(funcproto(outer));
  assert(child != NULL && child->sizeuv == 3);

  for (nth = 1; nth <= child->sizeuv; nth++) {
    ConstructCtx ctx;
    TValue ptv;
    TValue *anchor;
    uint32_t anchor_base = lj_tg_root_anchor_top_acq(tg);
    uint64_t total;
    int stacktop = lua_gettop(L);
    int errcode;

    setprotoV(L, &ptv, child);
    anchor = lj_tg_root_anchor_push(L, tg, &ptv, &ctx.anchoridx);
    assert(anchor != NULL && ctx.anchoridx == anchor_base);
    lj_gc_pubroot(L, anchor);
    ctx.pt = child;
    ctx.env = lj_state_env_acq(L);
    ctx.fn = NULL;
    total = lj_gc_total_load(G(L));

    lj_func_test_fail_empty_uv_after(nth);
    errcode = lj_vm_cpcall(L, NULL, &ctx, construct_empty_cp);
    assert(errcode == LUA_ERRMEM);
    assert(ctx.fn == NULL);
    assert(lj_func_test_empty_uv_fail_remaining() == 0);
    assert(lj_tg_root_anchor_top_acq(tg) == anchor_base + 1u);
    anchor = lj_tg_root_anchor_slot_acq(tg, ctx.anchoridx);
    assert(anchor != NULL && tvisproto(anchor) && protoV(anchor) == child);
    assert(lj_gc_total_load(G(L)) == total);

    lj_tg_root_anchor_pop(tg, ctx.anchoridx);
    assert(lj_tg_root_anchor_top_acq(tg) == anchor_base);
    lua_settop(L, stacktop);
  }
}

static void test_open_upvalue_handoff_then_oom(lua_State *L)
{
  TGState *tg = L2TG(L);
  InheritCtx ctx;
  GCfunc *parent;
  GCproto *child;
  TValue fnv;
  TValue *anchor;
  uint32_t anchoridx;
  uint32_t oldflags2;
  uint32_t cycle0;
  uint64_t oldthreshold, oldhard, oldtrigger;
  MSize i;
  BCReg maxslot = 0;
  int errcode;

  lua_settop(L, 0);
  ljt_lua_dostring(L,
    "return function()\n"
    "  local a, b = 10, 20\n"
    "  return function() return a + b end\n"
    "end\n");
  parent = funcV(L->top-1);
  child = first_child_proto(funcproto(parent));
  assert(child != NULL && child->sizeuv == 2);
  for (i = 0; i < child->sizeuv; i++) {
    uint32_t v = proto_uv(child)[i];
    assert(v & PROTO_UV_LOCAL);
    if ((BCReg)(v & 0xffu) > maxslot)
      maxslot = (BCReg)(v & 0xffu);
  }

  setfuncV(L, &fnv, parent);
  anchor = lj_tg_root_anchor_push(L, tg, &fnv, &anchoridx);
  assert(anchor != NULL);
  lj_gc_pubroot(L, anchor);
  for (i = 0; i <= maxslot; i++) {
    setintV(L->base + i, (int32_t)i + 1);
    lj_state_stack_pubtv(L, L, L->base + i);
  }
  L->top = L->base + maxslot + 1;

  oldflags2 = child->flags2;
  child->flags2 &= ~PROTO2_CELLUV;  /* Exercise legacy open-upvalue path. */
  ctx.pt = child;
  ctx.parent = &parent->l;
  ctx.fn = NULL;
  oldthreshold = lj_gc_threshold_load(G(L));
  oldhard = lj_gc2_hard_load(G(L));
  oldtrigger = lj_gc2_trigger_load(G(L));
  cycle0 = gc2_cycle_acq(G(L));
  lj_gc_threshold_store(G(L), 0);
  lj_gc2_hard_store(G(L), 0);
  lj_gc2_trigger_store(G(L), 0);
  la_store64_rel(&tg->local_total,
		 LJ_GC2_ACCT_FLUSH - (uint64_t)sizeof(GCupval));
  lj_func_test_collect_after_finduv(1);
  lj_func_test_fail_finduv_after(2);
  errcode = lj_vm_cpcall(L, NULL, &ctx, construct_inherit_cp);
  assert(errcode == LUA_ERRMEM && ctx.fn == NULL);
  assert(lj_func_test_finduv_fail_remaining() == 0);
  assert(gc2_cycle_acq(G(L)) != cycle0 ||
	 gc2_phase_acq(G(L)) != LJ_GC2_IDLE);
  assert(lj_state_openupval_acq(L) != NULL);
  assert(lj_gc2_obj_valid(G(L), lj_state_openupval_acq(L)));

  child->flags2 = oldflags2;
  lj_gc_threshold_store(G(L), oldthreshold);
  lj_gc2_hard_store(G(L), oldhard);
  lj_gc2_trigger_store(G(L), oldtrigger);
  lj_func_closeuv(L, L->base);
  assert(lj_state_openupval_acq(L) == NULL);
  lj_tg_root_anchor_pop(tg, anchoridx);
  L->top = L->base;
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
}

static void *openuv_thread_main(void *ud)
{
  OpenUVThreadCtx *ctx = (OpenUVThreadCtx *)ud;
  lua_State *L = ctx->L;
  int iter;
  if (!lj_threading_attach(L)) {
    ctx->status = 1;
    (void)ljt_barrier_wait(ctx->start);
    return NULL;
  }
  if (!lua_checkstack(L, (int)ctx->maxslot + 4)) {
    ctx->status = 2;
    (void)ljt_barrier_wait(ctx->start);
    lj_threading_detach(L, 1);
    return NULL;
  }
  lua_settop(L, 0);
  {
    int br = ljt_barrier_wait(ctx->start);
    if (br != 0 && br != LJT_BARRIER_SERIAL_THREAD) {
      ctx->status = 3;
      lj_threading_detach(L, 1);
      return NULL;
    }
  }
  for (iter = 0; iter < OPENUV_ITERS; iter++) {
    MSize i;
    GCfunc *fn;
    TValue fnv;
    for (i = 0; i <= ctx->maxslot; i++) {
      setintV(L->base + i, iter + (int32_t)i);
      lj_state_stack_pubtv(L, L, L->base + i);
    }
    L->top = L->base + ctx->maxslot + 1;
    fn = lj_func_newL_gc_forjit(L, L->base, ctx->pt, ctx->parent);
    setfuncV(L, &fnv, fn);
    copyTVrel(L, L->top, &fnv);
    lj_state_stack_pubtv(L, L, L->top);
    L->top++;
    assert(fn != NULL && lj_funcL_nupvalues(&fn->l) == ctx->pt->sizeuv);
    lj_func_closeuv(L, L->base);
    assert(lj_state_openupval_acq(L) == NULL);
    L->top = L->base;
  }
  ctx->status = 0;
  lj_threading_detach(L, 1);
  return NULL;
}

static void test_concurrent_per_state_open_upvalues(lua_State *L)
{
  global_State *g = G(L);
  pthread_t thread[OPENUV_THREADS];
  OpenUVThreadCtx ctx[OPENUV_THREADS];
  int ref[OPENUV_THREADS];
  ljt_barrier_t start;
  GCfunc *parent;
  GCproto *child;
  uint32_t oldflags2;
  uint64_t oldthreshold, oldhard, oldtrigger;
  BCReg maxslot = 0;
  MSize i;

  lua_settop(L, 0);
  ljt_lua_dostring(L,
    "return function()\n"
    "  local a, b = 1, 2\n"
    "  return function() return a + b end\n"
    "end\n");
  parent = funcV(L->top-1);
  child = first_child_proto(funcproto(parent));
  assert(child != NULL && child->sizeuv == 2);
  assert(!proto_legacyuv(child));
  for (i = 0; i < child->sizeuv; i++) {
    uint32_t v = proto_uv(child)[i];
    assert(v & PROTO_UV_LOCAL);
    if ((BCReg)(v & 0xffu) > maxslot)
      maxslot = (BCReg)(v & 0xffu);
  }
  oldflags2 = child->flags2;
  child->flags2 &= ~PROTO2_CELLUV;  /* Force the legacy per-state open path. */
  oldthreshold = lj_gc_threshold_load(g);
  oldhard = lj_gc2_hard_load(g);
  oldtrigger = lj_gc2_trigger_load(g);
  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  assert(ljt_barrier_init(&start, OPENUV_THREADS) == 0);
  for (i = 0; i < OPENUV_THREADS; i++) {
    ctx[i].L = lua_newthread(L);
    ref[i] = luaL_ref(L, LUA_REGISTRYINDEX);
    ctx[i].pt = child;
    ctx[i].parent = &parent->l;
    ctx[i].start = &start;
    ctx[i].maxslot = maxslot;
    ctx[i].status = -1;
    assert(pthread_create(&thread[i], NULL, openuv_thread_main, &ctx[i]) == 0);
  }
  for (i = 0; i < OPENUV_THREADS; i++) {
    assert(pthread_join(thread[i], NULL) == 0);
    assert(ctx[i].status == 0);
    assert(lj_state_openupval_acq(ctx[i].L) == NULL);
    luaL_unref(L, LUA_REGISTRYINDEX, ref[i]);
  }
  assert(ljt_barrier_destroy(&start) == 0);
  child->flags2 = oldflags2;
  lj_gc_threshold_store(g, oldthreshold);
  lj_gc2_hard_store(g, oldhard);
  lj_gc2_trigger_store(g, oldtrigger);
  /* The retained compatibility sentinel is no longer runtime topology. */
  assert(lj_uv_prev_acq(&g->uvhead) == &g->uvhead);
  assert(lj_uv_next_acq(&g->uvhead) == &g->uvhead);
  lua_settop(L, 0);
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  test_each_upvalue_oom(L);
  test_open_upvalue_handoff_then_oom(L);
  test_concurrent_per_state_open_upvalues(L);
  lua_close(L);
  puts("t-func-construction-anchor OK");
  return 0;
}
