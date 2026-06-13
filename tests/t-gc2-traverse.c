/*
** Focused test for GC2 SSB-to-grey traversal.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_tg.h"
#if LJ_HASJIT
#include "lj_dispatch.h"
#include "lj_jit.h"
#endif

static void flush_and_drain(global_State *g, TGState *tg)
{
  (void)lj_gc2_flush_ssb(g, tg);
  (void)lj_gc2_drain_ssb(g);
  assert(lj_gc2_ssb_empty(g));
}

static void test_strong_table(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child;
  uint64_t grey_pushed0, grey_drained0;

  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "child");

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  grey_pushed0 = la_load64_acq(&g->gc2.grey_pushed);
  grey_drained0 = la_load64_acq(&g->gc2.grey_drained);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(parent->asize > 0);
  assert(parent->hmask > 0);
  assert(lj_gc2_ismarkedmem(g, tvref(parent->array)) == 1);
  assert(lj_gc2_ismarkedmem(g, noderef(parent->node)) == 1);
  assert(la_load64_acq(&g->gc2.grey_pushed) == grey_pushed0 + 2u);
  assert(la_load64_acq(&g->gc2.grey_drained) == grey_drained0 + 2u);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 2);
}

static void test_grey_deque_growth(lua_State *L, global_State *g, TGState *tg)
{
  enum { GC2_DEQUE_GROW_N = 300 };
  GCtab *parent, *child[GC2_DEQUE_GROW_N];
  uint64_t grey_pushed0, grey_drained0;
  int i;

  lua_createtable(L, GC2_DEQUE_GROW_N, 0);
  parent = tabV(L->top - 1);
  for (i = 0; i < GC2_DEQUE_GROW_N; i++) {
    lua_newtable(L);
    child[i] = tabV(L->top - 1);
    lua_rawseti(L, -2, i + 1);
  }

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child[0])) == 0);
  grey_pushed0 = la_load64_acq(&g->gc2.grey_pushed);
  grey_drained0 = la_load64_acq(&g->gc2.grey_drained);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  for (i = 0; i < GC2_DEQUE_GROW_N; i++)
    assert(lj_gc2_ismarked(g, obj2gco(child[i])) == 1);
  assert(la_load64_acq(&g->gc2.grey_pushed) ==
	 grey_pushed0 + GC2_DEQUE_GROW_N + 1u);
  assert(la_load64_acq(&g->gc2.grey_drained) ==
	 grey_drained0 + GC2_DEQUE_GROW_N + 1u);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);
}

typedef struct GreyRaceCtx {
  global_State *g;
  pthread_barrier_t barrier;
  GCobj *stolen;
} GreyRaceCtx;

typedef struct WorkerDrainCtx {
  global_State *g;
  uint32_t limit;
  uint32_t drained;
} WorkerDrainCtx;

static void grey_wait(pthread_barrier_t *barrier)
{
  int rc = pthread_barrier_wait(barrier);
  assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *grey_owner_thread(void *arg)
{
  GreyRaceCtx *ctx = (GreyRaceCtx *)arg;
  grey_wait(&ctx->barrier);
  (void)lj_gc2_drain_ssb(ctx->g);
  return NULL;
}

static void *grey_thief_thread(void *arg)
{
  GreyRaceCtx *ctx = (GreyRaceCtx *)arg;
  grey_wait(&ctx->barrier);
  ctx->stolen = lj_gc2_grey_steal(ctx->g);
  return NULL;
}

static void *grey_worker_drain_thread(void *arg)
{
  WorkerDrainCtx *ctx = (WorkerDrainCtx *)arg;
  ctx->drained = lj_gc2_worker_drain(ctx->g, ctx->limit);
  return NULL;
}

static void grey_publish_test_item(global_State *g, GCobj *o)
{
  uint64_t top = la_load64_acq(&g->gc2.grey_top);
  uint64_t bottom = la_load64_acq(&g->gc2.grey_bottom);
  MSize cap = g->gc2.grey_capacity;
  assert(top == bottom);
  assert(cap > 0);
  setgcref(g->gc2.grey_stack[(MSize)(bottom % cap)], o);
  la_fence_rel();  /* 05 section 5.6.3: publish slot before bottom. */
  la_store64_rel(&g->gc2.grey_bottom, bottom + 1);
}

static void test_grey_deque_steal_race(lua_State *L, global_State *g,
				       TGState *tg)
{
  enum { GC2_STEAL_RACE_N = 256 };
  int i;
  UNUSED(tg);

  lj_gc2_legacy_mark_begin(g);
  assert(g->gc2.grey_stack != NULL);
  assert(g->gc2.grey_capacity > 0);

  lua_pushliteral(L, "gc2 direct steal");
  grey_publish_test_item(g, obj2gco(strV(L->top - 1)));
  assert(lj_gc2_grey_steal(g) == obj2gco(strV(L->top - 1)));
  assert(lj_gc2_grey_steal(g) == NULL);
  assert(lj_gc2_ssb_empty(g));
  lua_pop(L, 1);

  lua_pushliteral(L, "gc2 owner pop");
  grey_publish_test_item(g, obj2gco(strV(L->top - 1)));
  assert(lj_gc2_drain_ssb(g) == 0);
  assert(lj_gc2_grey_steal(g) == NULL);
  assert(lj_gc2_ssb_empty(g));
  lua_pop(L, 1);

  for (i = 0; i < GC2_STEAL_RACE_N; i++) {
    GreyRaceCtx ctx;
    pthread_t owner, thief;
    uint64_t drained0, drained1;
    GCobj *o;
    int owner_won, thief_won;

    lua_pushfstring(L, "gc2 steal race %d", i);
    o = obj2gco(strV(L->top - 1));
    grey_publish_test_item(g, o);
    drained0 = la_load64_acq(&g->gc2.grey_drained);
    ctx.g = g;
    ctx.stolen = NULL;
    assert(pthread_barrier_init(&ctx.barrier, NULL, 2) == 0);
    assert(pthread_create(&owner, NULL, grey_owner_thread, &ctx) == 0);
    assert(pthread_create(&thief, NULL, grey_thief_thread, &ctx) == 0);
    assert(pthread_join(owner, NULL) == 0);
    assert(pthread_join(thief, NULL) == 0);
    assert(pthread_barrier_destroy(&ctx.barrier) == 0);

    drained1 = la_load64_acq(&g->gc2.grey_drained);
    owner_won = drained1 == drained0 + 1u;
    thief_won = ctx.stolen == o;
    assert(owner_won || thief_won);
    assert(!(owner_won && thief_won));
    assert(lj_gc2_ssb_empty(g));
    lua_pop(L, 1);
  }

  lj_gc2_legacy_cycle_end(g);
}

static void test_worker_drain(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child, *grandchild;
  WorkerDrainCtx ctx;
  pthread_t worker;
  uint64_t grey_drained0, worker_runs0, worker_grey0, worker_ssb0;

  lua_settop(L, 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_newtable(L);
  grandchild = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);  /* child[1] = grandchild. */
  lua_pushvalue(L, -2);
  lua_rawseti(L, -4, 1);  /* parent[1] = child. */

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(!lj_gc2_ssb_empty(g));

  grey_drained0 = la_load64_acq(&g->gc2.grey_drained);
  worker_runs0 = la_load64_acq(&g->gc2.worker_runs);
  worker_grey0 = la_load64_acq(&g->gc2.worker_grey_drained);
  worker_ssb0 = la_load64_acq(&g->gc2.worker_ssb_converted);

  ctx.g = g;
  ctx.limit = 8;
  ctx.drained = 0;
  assert(pthread_create(&worker, NULL, grey_worker_drain_thread, &ctx) == 0);
  assert(pthread_join(worker, NULL) == 0);

  assert(ctx.drained == 3);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 1);
  assert(lj_gc2_ssb_empty(g));
  assert(la_load64_acq(&g->gc2.grey_drained) == grey_drained0 + 3u);
  assert(la_load64_acq(&g->gc2.worker_runs) == worker_runs0 + 1u);
  assert(la_load64_acq(&g->gc2.worker_grey_drained) == worker_grey0 + 3u);
  assert(la_load64_acq(&g->gc2.worker_ssb_converted) == worker_ssb0 + 1u);

  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}

static void test_worker_leaf_ssb(lua_State *L, global_State *g, TGState *tg)
{
  GCstr *s;
  uint64_t worker_runs0, worker_ssb0, worker_grey0;

  lua_pushliteral(L, "gc2 worker leaf ssb");
  s = strV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ssb_push(g, obj2gco(s)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(!lj_gc2_ssb_empty(g));

  worker_runs0 = la_load64_acq(&g->gc2.worker_runs);
  worker_ssb0 = la_load64_acq(&g->gc2.worker_ssb_converted);
  worker_grey0 = la_load64_acq(&g->gc2.worker_grey_drained);

  assert(lj_gc2_worker_drain(g, 1) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(s)) == 1);
  assert(lj_gc2_ssb_empty(g));
  assert(la_load64_acq(&g->gc2.worker_runs) == worker_runs0 + 1u);
  assert(la_load64_acq(&g->gc2.worker_ssb_converted) == worker_ssb0 + 1u);
  assert(la_load64_acq(&g->gc2.worker_grey_drained) == worker_grey0);

  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);
}

static void test_fixpoint_round(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child, *grandchild;
  uint64_t rounds0, hits0, worker_runs0;
  UNUSED(tg);

  lua_settop(L, 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_newtable(L);
  grandchild = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);  /* child[1] = grandchild. */
  lua_pushvalue(L, -2);
  lua_rawseti(L, -4, 1);  /* parent[1] = child. */
  lua_pop(L, 2);  /* Keep only parent as a stack root. */

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);

  rounds0 = la_load64_acq(&g->gc2.fixpoint_rounds);
  hits0 = la_load64_acq(&g->gc2.fixpoint_hits);
  worker_runs0 = la_load64_acq(&g->gc2.worker_runs);

  assert(lj_gc2_fixpoint_round(g, L, 1) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  assert(!lj_gc2_ssb_empty(g));
  assert(la_load64_acq(&g->gc2.marks_this_round) > 0);
  assert(la_load64_acq(&g->gc2.fixpoint_rounds) == rounds0 + 1u);
  assert(la_load64_acq(&g->gc2.fixpoint_hits) == hits0);
  assert(la_load64_acq(&g->gc2.worker_runs) > worker_runs0);

  assert(lj_gc2_fixpoint_round(g, L, ~(uint32_t)0) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 1);
  assert(lj_gc2_ssb_empty(g));
  assert(la_load64_acq(&g->gc2.marks_this_round) > 0);
  assert(la_load64_acq(&g->gc2.fixpoint_rounds) == rounds0 + 2u);
  assert(la_load64_acq(&g->gc2.fixpoint_hits) == hits0);

  assert(lj_gc2_fixpoint_round(g, L, ~(uint32_t)0) == 1);
  assert(lj_gc2_ssb_empty(g));
  assert(la_load64_acq(&g->gc2.marks_this_round) == 0);
  assert(la_load64_acq(&g->gc2.fixpoint_rounds) == rounds0 + 3u);
  assert(la_load64_acq(&g->gc2.fixpoint_hits) == hits0 + 1u);

  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);
}

static void test_c_value_barrier(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child;

  lua_createtable(L, 1, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 2);
}

static void test_c_table_rescan_barrier(lua_State *L, global_State *g,
					TGState *tg)
{
  GCtab *parent, *child;

  lua_createtable(L, 1, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  assert(parent->asize > 0);
  settabV(L, arrayslot(parent, 0), child);
  lj_gc_anybarriert(L, parent);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 2);
}

static void test_vm_upvalue_barrier(lua_State *L, global_State *g, TGState *tg)
{
  GCfunc *fn;
  GCupval *uv;
  GCtab *old, *child;

  assert(luaL_dostring(L,
    "local x = {}\n"
    "return function(v) x = v end, x\n") == LUA_OK);
  fn = funcV(L->top - 2);
  old = tabV(L->top - 1);
  assert(isluafunc(fn));
  assert(fn->l.nupvalues == 1);
  uv = gco2uv(gcref(fn->l.uvptr[0]));
  assert(uv->closed);
  assert(uvval(uv) == &uv->tv);
  assert(tabV(uvval(uv)) == old);

  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(uv)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(uv)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(old)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  lua_pushvalue(L, -3);
  lua_pushvalue(L, -2);
  lua_call(L, 1, 0);
  assert(tabV(uvval(uv)) == child);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}

static void test_vm_table_barrier(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child;

  assert(luaL_dostring(L,
    "return function(t, v) t[1] = v end\n") == LUA_OK);
  lua_createtable(L, 1, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  lua_pushvalue(L, -3);
  lua_pushvalue(L, -3);
  lua_pushvalue(L, -3);
  lua_call(L, 2, 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}

static void test_vm_meta_tset_barrier(lua_State *L, global_State *g,
				      TGState *tg)
{
  GCtab *parent, *key, *child;

  assert(luaL_dostring(L,
    "return function(t, k, v) t[k] = v end\n") == LUA_OK);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  key = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  lua_pushvalue(L, -4);
  lua_pushvalue(L, -4);
  lua_pushvalue(L, -4);
  lua_pushvalue(L, -4);
  lua_call(L, 3, 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 4);
}

#if LJ_HASJIT
static GCtrace *find_trace(global_State *g)
{
  jit_State *J = G2J(g);
  MSize i;
  for (i = 1; i < J->sizetrace; i++) {
    GCtrace *T = traceref(J, i);
    if (T != NULL)
      return T;
  }
  return NULL;
}

static void test_jit_table_store_nyi_barrier(lua_State *L, global_State *g,
					     TGState *tg)
{
  GCtab *parent, *child;

  assert(luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "return function(t, v, n)\n"
    "  for i = 1, n do\n"
    "    if i > 1 then t[1] = v end\n"
    "  end\n"
    "end\n") == LUA_OK);
  lua_createtable(L, 1, 0);
  lua_newtable(L);
  lua_pushvalue(L, -3);
  lua_pushvalue(L, -3);
  lua_pushvalue(L, -3);
  lua_pushinteger(L, 100);
  lua_call(L, 3, 0);
  /* M5 keeps legacy table stores interpreted until traced AHdr/NHdr writes. */
  assert(find_trace(g) == NULL);
  lua_pop(L, 2);

  lua_createtable(L, 1, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  lua_pushvalue(L, -3);
  lua_pushvalue(L, -3);
  lua_pushvalue(L, -3);
  lua_pushinteger(L, 20);
  lua_call(L, 3, 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}

static void test_jit_upvalue_barrier(lua_State *L, global_State *g,
				     TGState *tg)
{
  GCfunc *fn;
  GCupval *uv;
  GCtab *old, *child;

  assert(luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local x = {}\n"
    "local function setuv(v, n)\n"
    "  for i = 1, n do\n"
    "    if i > 1 then x = v end\n"
    "  end\n"
    "end\n"
    "return setuv, x\n") == LUA_OK);
  fn = funcV(L->top - 2);
  old = tabV(L->top - 1);
  assert(isluafunc(fn));
  assert(fn->l.nupvalues == 1);
  uv = gco2uv(gcref(fn->l.uvptr[0]));
  assert(uv->closed);
  assert(tabV(uvval(uv)) == old);

  lua_newtable(L);
  lua_pushvalue(L, -3);
  lua_pushvalue(L, -2);
  lua_pushinteger(L, 100);
  lua_call(L, 2, 0);
  assert(find_trace(g) != NULL);
  old = tabV(uvval(uv));
  lua_pop(L, 1);

  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(uv)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(uv)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(old)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  lua_pushvalue(L, -3);
  lua_pushvalue(L, -2);
  lua_pushinteger(L, 20);
  lua_call(L, 2, 0);
  assert(tabV(uvval(uv)) == child);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}
#endif

static void make_weak_table(lua_State *L, const char *mode,
			    GCtab **weak, GCtab **key, GCtab **val)
{
  lua_newtable(L);
  *weak = tabV(L->top - 1);
  lua_newtable(L);
  *key = tabV(L->top - 1);
  lua_newtable(L);
  *val = tabV(L->top - 1);
  lua_pushvalue(L, -2);
  lua_pushvalue(L, -2);
  lua_settable(L, -5);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushstring(L, mode);
  lua_settable(L, -3);
  lua_setmetatable(L, -4);
}

static void test_weak_tables(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *weakv, *keyv, *valv;
  GCtab *weakk, *keyk, *valk;
  GCtab *weakkv, *keykv, *valkv;

  make_weak_table(L, "v", &weakv, &keyv, &valv);
  make_weak_table(L, "k", &weakk, &keyk, &valk);
  make_weak_table(L, "kv", &weakkv, &keykv, &valkv);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weakv)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(weakk)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(weakkv)) == 1);
  flush_and_drain(g, tg);

  assert(lj_gc2_ismarked(g, obj2gco(keyv)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(valv)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(keyk)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(valk)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(keykv)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(valkv)) == 0);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 9);
}

static void test_weak_key_write_barrier(lua_State *L, global_State *g,
					TGState *tg)
{
  GCtab *weak, *key, *val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "k");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  key = tabV(L->top - 1);
  lua_newtable(L);
  val = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  weak_keys0 = la_load64_acq(&g->gc2.weak_keys_marked);
  weak_vals0 = la_load64_acq(&g->gc2.weak_values_marked);

  lj_gc2_legacy_weak_begin(g);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_settable(L, 1);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(la_load64_acq(&g->gc2.weak_keys_marked) == weak_keys0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_values_marked) == weak_vals0 + 1u);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}

static void test_closure(lua_State *L, global_State *g, TGState *tg)
{
  GCfunc *fn;
  GCtab *up;

  assert(luaL_dostring(L,
    "local x = {}\n"
    "return function() return x end, x\n") == LUA_OK);
  fn = funcV(L->top - 2);
  up = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(up)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(fn)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(funcproto(fn))) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(up)) == 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 2);
}

static void test_thread(lua_State *L, global_State *g, TGState *tg)
{
  lua_State *th;
  GCtab *stack_tab;

  th = lua_newthread(L);
  assert(th != NULL);
  lua_newtable(th);
  stack_tab = tabV(th->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(stack_tab)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(th)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarkedmem(g, tvref(th->stack)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(stack_tab)) == 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);
}

static void test_userdata(lua_State *L, global_State *g)
{
  GCtab *env, *mt;
  GCudata *ud;

  lua_newtable(L);
  env = tabV(L->top - 1);
  lua_newuserdata(L, 1);
  ud = udataV(L->top - 1);
  lua_pushvalue(L, -2);
  lua_setfenv(L, -2);
  lua_newtable(L);
  mt = tabV(L->top - 1);
  lua_setmetatable(L, -2);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(env)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(mt)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(ud)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  assert(lj_gc2_drain_ssb(g) == 0);
  assert(lj_gc2_ssb_empty(g));
  assert(lj_gc2_ismarked(g, obj2gco(env)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(mt)) == 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 2);
}

static void test_leaf_ssb(lua_State *L, global_State *g, TGState *tg)
{
  GCstr *s;

  lua_pushliteral(L, "gc2 leaf ssb");
  s = strV(L->top - 1);
  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ssb_push(g, obj2gco(s)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(s)) == 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;

  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
#if LJ_HASJIT
  luaL_openlibs(L);
#endif
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);

  test_strong_table(L, g, tg);
  test_grey_deque_growth(L, g, tg);
  test_grey_deque_steal_race(L, g, tg);
  test_worker_drain(L, g, tg);
  test_worker_leaf_ssb(L, g, tg);
  test_fixpoint_round(L, g, tg);
  test_c_value_barrier(L, g, tg);
  test_c_table_rescan_barrier(L, g, tg);
  test_vm_upvalue_barrier(L, g, tg);
  test_vm_table_barrier(L, g, tg);
  test_vm_meta_tset_barrier(L, g, tg);
#if LJ_HASJIT
  test_jit_table_store_nyi_barrier(L, g, tg);
  test_jit_upvalue_barrier(L, g, tg);
#endif
  test_weak_tables(L, g, tg);
  test_weak_key_write_barrier(L, g, tg);
  test_closure(L, g, tg);
  test_thread(L, g, tg);
  test_userdata(L, g);
  test_leaf_ssb(L, g, tg);

  lua_close(L);
  printf("t-gc2-traverse OK: SSB grey traversal verified\n");
  return 0;
}
