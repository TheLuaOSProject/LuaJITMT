/*
** Focused test for GC2 SSB-to-grey traversal.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_lib.h"
#if LJ_HASFFI
#include "lj_cdata.h"
#endif
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

static void worker_drain_all(global_State *g)
{
  uint32_t i;
  for (i = 0; i < 1024; i++) {
    if (lj_gc2_ssb_empty(g))
      return;
    assert(lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH) != 0);
  }
  assert(lj_gc2_ssb_empty(g));
}

static int weak_snapshot_has(global_State *g, GCtab *t);

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

typedef struct WorkerDrainRaceCtx {
  global_State *g;
  pthread_barrier_t barrier;
  uint32_t limit;
  uint32_t progress[2];
} WorkerDrainRaceCtx;

typedef struct WorkerDrainRaceThread {
  WorkerDrainRaceCtx *ctx;
  uint32_t idx;
} WorkerDrainRaceThread;

typedef struct WeakPeerWriteCtx {
  lua_State *L;
  pthread_barrier_t barrier;
  int status;
} WeakPeerWriteCtx;

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

static void *grey_worker_drain_race_thread(void *arg)
{
  WorkerDrainRaceThread *argt = (WorkerDrainRaceThread *)arg;
  WorkerDrainRaceCtx *ctx = argt->ctx;
  grey_wait(&ctx->barrier);
  ctx->progress[argt->idx] =
    lj_gc2_worker_drain_progress(ctx->g, ctx->limit);
  return NULL;
}

static void *weak_peer_write_thread(void *arg)
{
  WeakPeerWriteCtx *ctx = (WeakPeerWriteCtx *)arg;
  lua_State *L = ctx->L;
  grey_wait(&ctx->barrier);
  if (!luaMT_attach(L)) {
    ctx->status = 1;
    return NULL;
  }
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 4);
  if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
    lua_pop(L, 1);
    luaMT_detach(L);
    ctx->status = 2;
    return NULL;
  }
  luaMT_detach(L);
  ctx->status = 0;
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

  assert(ctx.drained == 4);
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

static void test_worker_drain_race(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child, *grandchild;
  WorkerDrainRaceCtx ctx;
  WorkerDrainRaceThread arg0, arg1;
  pthread_t worker0, worker1;
  uint32_t total;
  uint64_t worker_runs0, worker_grey0, worker_ssb0, idle0, busy0;

  lua_settop(L, 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_newtable(L);
  grandchild = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);
  lua_pushvalue(L, -2);
  lua_rawseti(L, -4, 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(!lj_gc2_ssb_empty(g));

  worker_runs0 = la_load64_acq(&g->gc2.worker_runs);
  worker_grey0 = la_load64_acq(&g->gc2.worker_grey_drained);
  worker_ssb0 = la_load64_acq(&g->gc2.worker_ssb_converted);
  idle0 = la_load64_acq(&g->gc2.worker_idle_declares);
  busy0 = la_load64_acq(&g->gc2.worker_busy_retries);

  ctx.g = g;
  ctx.limit = 8;
  ctx.progress[0] = ctx.progress[1] = 0;
  arg0.ctx = &ctx; arg0.idx = 0;
  arg1.ctx = &ctx; arg1.idx = 1;
  assert(pthread_barrier_init(&ctx.barrier, NULL, 2) == 0);
  assert(pthread_create(&worker0, NULL, grey_worker_drain_race_thread,
			&arg0) == 0);
  assert(pthread_create(&worker1, NULL, grey_worker_drain_race_thread,
			&arg1) == 0);
  assert(pthread_join(worker0, NULL) == 0);
  assert(pthread_join(worker1, NULL) == 0);
  assert(pthread_barrier_destroy(&ctx.barrier) == 0);

  total = ctx.progress[0] + ctx.progress[1];
  assert(total == 4u);
  assert(la_load32_acq(&g->gc2.worker_active) == 0);
  assert(la_load64_acq(&g->gc2.worker_runs) == worker_runs0 + 1u);
  assert(la_load64_acq(&g->gc2.worker_grey_drained) == worker_grey0 + 3u);
  assert(la_load64_acq(&g->gc2.worker_ssb_converted) == worker_ssb0 + 1u);
  assert(la_load64_acq(&g->gc2.worker_idle_declares) > idle0 ||
	 la_load64_acq(&g->gc2.worker_busy_retries) > busy0);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 1);
  assert(lj_gc2_ssb_empty(g));

  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}

static void test_worker_leaf_ssb(lua_State *L, global_State *g, TGState *tg)
{
  GCstr *s, *s2;
  uint64_t worker_runs0, worker_ssb0, worker_grey0;

  lua_pushliteral(L, "gc2 worker leaf ssb");
  s = strV(L->top - 1);
  lua_pushliteral(L, "gc2 worker leaf progress");
  s2 = strV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ssb_push(g, obj2gco(s)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(!lj_gc2_ssb_empty(g));

  worker_runs0 = la_load64_acq(&g->gc2.worker_runs);
  worker_ssb0 = la_load64_acq(&g->gc2.worker_ssb_converted);
  worker_grey0 = la_load64_acq(&g->gc2.worker_grey_drained);

  assert(lj_gc2_worker_drain(g, 1) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(s)) == 1);
  assert(lj_gc2_ssb_empty(g));
  assert(la_load64_acq(&g->gc2.worker_runs) == worker_runs0 + 1u);
  assert(la_load64_acq(&g->gc2.worker_ssb_converted) == worker_ssb0 + 1u);
  assert(la_load64_acq(&g->gc2.worker_grey_drained) == worker_grey0);

  assert(lj_gc2_ssb_push(g, obj2gco(s2)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(!lj_gc2_ssb_empty(g));
  worker_runs0 = la_load64_acq(&g->gc2.worker_runs);
  worker_ssb0 = la_load64_acq(&g->gc2.worker_ssb_converted);
  worker_grey0 = la_load64_acq(&g->gc2.worker_grey_drained);
  assert(lj_gc2_worker_drain_progress(g, 1) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(s2)) == 1);
  assert(lj_gc2_ssb_empty(g));
  assert(la_load64_acq(&g->gc2.worker_runs) == worker_runs0 + 1u);
  assert(la_load64_acq(&g->gc2.worker_ssb_converted) == worker_ssb0 + 1u);
  assert(la_load64_acq(&g->gc2.worker_grey_drained) == worker_grey0);

  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 2);
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
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
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

static void test_jit_table_store_helper_barrier(lua_State *L, global_State *g,
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
  /* M6: existing non-weak table stores trace through the helper bridge. */
  assert(find_trace(g) != NULL);
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
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}

static void test_jit_weak_table_store_helper_barrier(lua_State *L,
						     global_State *g,
						     TGState *tg)
{
  GCtab *weak, *key, *val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "return function(t, k, v, n)\n"
    "  for i = 1, n do\n"
    "    if i > 1 then t[k] = v end\n"
    "  end\n"
    "end\n") == LUA_OK);
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
  /* Warm the trace with an existing non-nil slot, then overwrite it in P_WEAK. */
  lua_newtable(L);
  val = tabV(L->top - 1);

  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 4);
  lua_pushinteger(L, 100);
  lua_call(L, 4, 0);
  /* M8: existing weak table stores trace through a P_WEAK-aware helper. */
  assert(find_trace(g) != NULL);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  weak_keys0 = la_load64_acq(&g->gc2.weak_keys_marked);
  weak_vals0 = la_load64_acq(&g->gc2.weak_values_marked);

  lj_gc2_legacy_weak_begin(g);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 5);
  lua_pushinteger(L, 20);
  lua_call(L, 4, 0);
  assert(find_trace(g) != NULL);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(la_load64_acq(&g->gc2.weak_keys_marked) == weak_keys0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_values_marked) == weak_vals0 + 1u);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 5);
}

static void test_jit_weak_array_store_helper_barrier(lua_State *L,
						     global_State *g,
						     TGState *tg)
{
  GCtab *weak, *oldval, *val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "return function(t, v, n)\n"
    "  for i = 1, n do\n"
    "    if i > 1 then t[1] = v end\n"
    "  end\n"
    "end\n") == LUA_OK);
  lua_createtable(L, 1, 0);
  weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  oldval = tabV(L->top - 1);
  lua_pushvalue(L, 3);
  lua_rawseti(L, 2, 1);
  lua_newtable(L);
  val = tabV(L->top - 1);

  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushinteger(L, 100);
  lua_call(L, 3, 0);
  /* M8: existing weak-value array stores trace through the array helper. */
  assert(find_trace(g) != NULL);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(oldval)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  weak_keys0 = la_load64_acq(&g->gc2.weak_keys_marked);
  weak_vals0 = la_load64_acq(&g->gc2.weak_values_marked);

  lj_gc2_legacy_weak_begin(g);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 4);
  lua_pushinteger(L, 20);
  lua_call(L, 3, 0);
  assert(find_trace(g) != NULL);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(la_load64_acq(&g->gc2.weak_keys_marked) == weak_keys0);
  assert(la_load64_acq(&g->gc2.weak_values_marked) == weak_vals0 + 1u);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 4);
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

static void test_jit_current_trace_root(lua_State *L, global_State *g,
					TGState *tg)
{
  jit_State *J = G2J(g);
  GCtrace saved;
  GCfunc *fn;
  GCproto *pt;
  UNUSED(tg);

  assert(luaL_dostring(L, "return function() return 42 end\n") == LUA_OK);
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(pt)) == 0);
  saved = J->cur;
  memset(&J->cur, 0, sizeof(J->cur));
  J->cur.traceno = 1;
  J->cur.nk = REF_TRUE;
  J->cur.nins = REF_TRUE;
  setgcref(J->cur.startpt, obj2gco(pt));
  lj_gc2_scan_roots(g, L);
  assert(lj_gc2_ismarked(g, obj2gco(pt)) == 1);
  J->cur = saved;
  lj_gc2_legacy_cycle_end(g);
}

static void test_jit_tg_executing_trace_root(lua_State *L, global_State *g,
					     TGState *tg)
{
  GCtrace *T = find_trace(g);
  uint32_t old_vmstate;
  uint64_t trace_roots0;
  assert(T != NULL);
  assert(T->traceno > 0);

  old_vmstate = la_load32_acq((uint32_t *)&tg->vmstate);
  trace_roots0 = la_load64_acq(&g->gc2.tg_trace_roots);
  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(T)) == 0);
  la_store32_rel((uint32_t *)&tg->vmstate, (uint32_t)T->traceno);
  lj_gc2_scan_roots(g, NULL);
  assert(lj_gc2_ismarked(g, obj2gco(T)) == 1);
  assert(la_load64_acq(&g->gc2.tg_trace_roots) == trace_roots0 + 1u);
  la_store32_rel((uint32_t *)&tg->vmstate, old_vmstate);
  lj_gc2_legacy_cycle_end(g);
  UNUSED(L);
}
#endif

#if LJ_HASPROFILE
static int gc2_profile_callback(lua_State *L)
{
  UNUSED(L);
  return 0;
}

static void test_jit_profile_registry_weak_barrier(void)
{
  TGState *oldtg = lj_thr_get_tg();
  lua_State *L2 = luaL_newstate();
  global_State *g2;
  TGState *tg2;
  GCtab *reg;
  GCfunc *cb;
  uint64_t weak_vals0;

  assert(L2 != NULL);
  lua_gc(L2, LUA_GCSTOP, 0);
  lua_pushcfunction(L2, luaopen_base);
  lua_call(L2, 0, 1);
  lua_pop(L2, 1);
  lua_pushcfunction(L2, luaopen_package);
  lua_call(L2, 0, 1);
  lua_pop(L2, 1);
  lua_pushcfunction(L2, luaopen_jit);
  lua_pushliteral(L2, LUA_JITLIBNAME);
  lua_call(L2, 1, 1);
  lua_pop(L2, 1);
  g2 = G(L2);
  tg2 = G2TG(g2);
  assert(tg2 != NULL);

  lua_pushcfunction(L2, gc2_profile_callback);
  cb = funcV(L2->top - 1);
  lua_getregistry(L2);
  reg = tabV(L2->top - 1);
  lua_newtable(L2);
  lua_pushliteral(L2, "__mode");
  lua_pushliteral(L2, "v");
  lua_settable(L2, -3);
  lua_setmetatable(L2, -2);

  lj_gc2_legacy_mark_begin(g2);
  assert(lj_gc2_markobj(g2, obj2gco(reg)) == 1);
  flush_and_drain(g2, tg2);
  assert(weak_snapshot_has(g2, reg));
  assert(lj_gc2_ismarked(g2, obj2gco(cb)) == 0);
  lua_pop(L2, 1);

  weak_vals0 = la_load64_acq(&g2->gc2.weak_values_marked);
  lj_gc2_legacy_weak_begin(g2);
  lua_getglobal(L2, "require");
  lua_pushliteral(L2, LUA_JITLIBNAME ".profile");
  lua_call(L2, 1, 1);
  lua_getfield(L2, -1, "start");
  lua_pushliteral(L2, "");
  lua_pushvalue(L2, 1);
  lua_call(L2, 2, 0);
  assert(lj_gc2_ismarked(g2, obj2gco(cb)) == 1);
  while (lj_gc2_weak_drain(g2, 1) != 0)
    ;
  assert(la_load64_acq(&g2->gc2.weak_values_marked) > weak_vals0);
  luaJIT_profile_stop(L2);
  lj_gc2_legacy_cycle_end(g2);
  lua_close(L2);
  lj_thr_set_tg(oldtg);
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

static int weak_snapshot_has(global_State *g, GCtab *t)
{
  uint32_t i, n = lj_gc2_weak_snapshot_count(g);
  for (i = 0; i < n; i++)
    if (lj_gc2_weak_snapshot_tab(g, i) == t)
      return 1;
  return 0;
}

static int weak_entry_is_nil(lua_State *L, GCtab *weak, GCtab *key)
{
  TValue k;
  settabV(L, &k, key);
  return tvisnil(lj_tab_get(L, weak, &k));
}

static void legacy_weak_link(global_State *g, GCtab *t, int weak)
{
  lj_obj_masksetgcflags(obj2gco(t), LJ_GC_WEAK, weak);
  setgcrefr(t->gclist, g->gc.weak);
  setgcref(g->gc.weak, obj2gco(t));
}

static void make_weak_table_batch(lua_State *L, MSize n)
{
  MSize i;
  GCtab *weak, *key, *val;
  assert(lua_checkstack(L, (int)(n * 3u + 8u)));
  for (i = 0; i < n; i++)
    make_weak_table(L, "v", &weak, &key, &val);
}

static void mark_weak_table_batch(lua_State *L, global_State *g, MSize n)
{
  MSize i;
  for (i = 0; i < n; i++) {
    TValue *tv;
    lua_pushvalue(L, (int)(i * 3u + 1u));
    tv = L->top - 1;
    assert(tvistab(tv));
    assert(lj_gc2_markobj(g, obj2gco(tabV(tv))) == 1);
    lua_pop(L, 1);
  }
}

static void test_weak_snapshot_growth(lua_State *L, global_State *g, TGState *tg)
{
  MSize cap0, cap1, n;
  uint64_t overflow0, overflow1;

  lua_settop(L, 0);
  if (g->gc2.weak_capacity == 0) {
    lj_gc2_legacy_mark_begin(g);
    lj_gc2_legacy_cycle_end(g);
  }
  cap0 = g->gc2.weak_capacity;
  assert(cap0 > 0);
  n = cap0 + 1u;
  assert(n > cap0);

  make_weak_table_batch(L, n);
  overflow0 = la_load64_acq(&g->gc2.weak_tables_overflow);
  lj_gc2_legacy_mark_begin(g);
  mark_weak_table_batch(L, g, n);
  flush_and_drain(g, tg);
  assert(g->gc2.weak_capacity == cap0);
  assert(la_load64_acq(&g->gc2.weak_count) == n);
  assert(lj_gc2_weak_snapshot_count(g) == cap0);
  assert(la_load64_acq(&g->gc2.weak_tables_overflow) == overflow0 + 1u);
  lj_gc2_legacy_cycle_end(g);
  lua_settop(L, 0);

  make_weak_table_batch(L, n);
  overflow1 = la_load64_acq(&g->gc2.weak_tables_overflow);
  lj_gc2_legacy_mark_begin(g);
  cap1 = g->gc2.weak_capacity;
  assert(cap1 > cap0);
  assert(cap1 >= n);
  mark_weak_table_batch(L, g, n);
  flush_and_drain(g, tg);
  assert(la_load64_acq(&g->gc2.weak_count) == n);
  assert(lj_gc2_weak_snapshot_count(g) == n);
  assert(la_load64_acq(&g->gc2.weak_tables_overflow) == overflow1);
  lj_gc2_legacy_cycle_end(g);
  lua_settop(L, 0);
}

static void test_weak_snapshot_ready_publication(lua_State *L, global_State *g)
{
  GCtab *t;
  uint64_t idx;

  lua_settop(L, 0);
  lua_newtable(L);
  t = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(g->gc2.weak_stack != NULL);
  assert(g->gc2.weak_ready != NULL);
  assert(lj_gc2_weak_snapshot_count(g) == 0);
  idx = la_add64_rlx(&g->gc2.weak_count, 1);
  assert(idx == 0);
  assert(lj_gc2_weak_snapshot_count(g) == 0);
  assert(lj_gc2_weak_snapshot_clear(g, 1) == 0);
  assert(la_load64_acq(&g->gc2.weak_clear_cursor) == 0);
  setgcref(g->gc2.weak_stack[0], obj2gco(t));
  la_store8_rel(&g->gc2.weak_ready[0], 1);
  assert(lj_gc2_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_weak_snapshot_clear(g, 1) == 1u);
  assert(la_load64_acq(&g->gc2.weak_clear_cursor) == 1u);
  assert(lj_gc2_weak_snapshot_tab(g, 0) == t);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);
}

static void test_weak_snapshot_legacy_coverage(lua_State *L, global_State *g,
					       TGState *tg)
{
  GCtab *weak, *key, *val;
  GCtab *absent, *akey, *aval;
  uint64_t idx, count;

  lua_settop(L, 0);
  make_weak_table(L, "v", &weak, &key, &val);
  make_weak_table(L, "v", &absent, &akey, &aval);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_weak_snapshot_count(g) == 1u);
  setgcrefnull(g->gc.weak);
  legacy_weak_link(g, weak, LJ_GC_WEAKVAL);
  assert(!lj_gc2_weak_snapshot_covers_legacy(g, gcref(g->gc.weak)));

  lj_gc2_legacy_weak_begin(g);
  assert(!lj_gc2_weak_snapshot_covers_legacy(g, gcref(g->gc.weak)));
  assert(lj_gc2_weak_drain(g, 1) == 1u);
  assert(lj_gc2_weak_snapshot_covers_legacy(g, gcref(g->gc.weak)));

  lj_obj_cleargcflags(obj2gco(weak), LJ_GC_WEAK);
  assert(!lj_gc2_weak_snapshot_covers_legacy(g, gcref(g->gc.weak)));
  lj_obj_masksetgcflags(obj2gco(weak), LJ_GC_WEAK, LJ_GC_WEAKVAL);
  assert(lj_gc2_weak_snapshot_covers_legacy(g, gcref(g->gc.weak)));
  lj_obj_masksetgcflags(obj2gco(weak), LJ_GC_WEAK, LJ_GC_WEAKKEY);
  assert(lj_gc2_weak_snapshot_covers_legacy(g, gcref(g->gc.weak)));
  lj_obj_masksetgcflags(obj2gco(weak), LJ_GC_WEAK, LJ_GC_WEAK);
  assert(lj_gc2_weak_snapshot_covers_legacy(g, gcref(g->gc.weak)));
  lj_obj_masksetgcflags(obj2gco(weak), LJ_GC_WEAK, LJ_GC_WEAKVAL);

  count = la_load64_acq(&g->gc2.weak_count);
  idx = la_add64_rlx(&g->gc2.weak_count, 1);
  assert(idx == count);
  assert(idx < (uint64_t)g->gc2.weak_capacity);
  assert(!lj_gc2_weak_snapshot_covers_legacy(g, gcref(g->gc.weak)));
  la_store64_rlx(&g->gc2.weak_count, count);
  assert(lj_gc2_weak_snapshot_covers_legacy(g, gcref(g->gc.weak)));

  la_store64_rlx(&g->gc2.weak_count, (uint64_t)g->gc2.weak_capacity + 1u);
  assert(!lj_gc2_weak_snapshot_covers_legacy(g, gcref(g->gc.weak)));
  la_store64_rlx(&g->gc2.weak_count, count);

  setgcrefnull(g->gc.weak);
  legacy_weak_link(g, absent, LJ_GC_WEAKVAL);
  assert(!lj_gc2_weak_snapshot_covers_legacy(g, gcref(g->gc.weak)));

  setgcrefnull(g->gc.weak);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 6);
}

static void test_weak_complete_bridge(lua_State *L, global_State *g,
				      TGState *tg)
{
  GCtab *weak, *key, *val;
  GCtab *missing, *mkey, *mval;
  uint64_t runs0, progress0, skipped0, fallbacks0, backfills0;
  uint64_t backfill_tables0, backfill_cleared0;
  uint64_t clear_tables0, clear_cleared0;

  lua_settop(L, 0);
  make_weak_table(L, "v", &weak, &key, &val);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_weak_snapshot_count(g) == 1u);
  setgcrefnull(g->gc.weak);
  legacy_weak_link(g, weak, LJ_GC_WEAKVAL);
  lj_gc2_legacy_weak_begin(g);
  runs0 = la_load64_acq(&g->gc2.weak_complete_runs);
  progress0 = la_load64_acq(&g->gc2.weak_complete_progress);
  skipped0 = la_load64_acq(&g->gc2.weak_legacy_skipped);
  fallbacks0 = la_load64_acq(&g->gc2.weak_legacy_fallbacks);
  clear_tables0 = la_load64_acq(&g->gc2.weak_clear_tables);
  clear_cleared0 = la_load64_acq(&g->gc2.weak_clear_cleared);
  assert(lj_gc2_weak_complete(g, gcref(g->gc.weak), 1) == 1);
  assert(weak_entry_is_nil(L, weak, key));
  assert(la_load64_acq(&g->gc2.weak_complete_runs) == runs0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_complete_progress) == progress0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_legacy_skipped) == skipped0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_legacy_fallbacks) == fallbacks0);
  assert(la_load64_acq(&g->gc2.weak_clear_tables) == clear_tables0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_clear_cleared) == clear_cleared0 + 1u);
  setgcrefnull(g->gc.weak);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);

  make_weak_table(L, "v", &weak, &key, &val);
  make_weak_table(L, "v", &missing, &mkey, &mval);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_weak_snapshot_count(g) == 1u);
  setgcrefnull(g->gc.weak);
  legacy_weak_link(g, missing, LJ_GC_WEAKVAL);
  legacy_weak_link(g, weak, LJ_GC_WEAKVAL);
  lj_gc2_legacy_weak_begin(g);
  runs0 = la_load64_acq(&g->gc2.weak_complete_runs);
  skipped0 = la_load64_acq(&g->gc2.weak_legacy_skipped);
  fallbacks0 = la_load64_acq(&g->gc2.weak_legacy_fallbacks);
  backfills0 = la_load64_acq(&g->gc2.weak_legacy_backfills);
  backfill_tables0 = la_load64_acq(&g->gc2.weak_legacy_backfill_tables);
  backfill_cleared0 = la_load64_acq(&g->gc2.weak_legacy_backfill_cleared);
  assert(lj_gc2_weak_complete(g, gcref(g->gc.weak), 1) == 1);
  assert(weak_entry_is_nil(L, weak, key));
  assert(weak_entry_is_nil(L, missing, mkey));
  assert(la_load64_acq(&g->gc2.weak_complete_runs) == runs0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_legacy_skipped) == skipped0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_legacy_fallbacks) == fallbacks0);
  assert(la_load64_acq(&g->gc2.weak_legacy_backfills) == backfills0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_legacy_backfill_tables) ==
	 backfill_tables0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_legacy_backfill_cleared) ==
	 backfill_cleared0 + 1u);
  setgcrefnull(g->gc.weak);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 6);

  UNUSED(val);
  UNUSED(mval);
}

static void test_weak_tables(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *weakv, *keyv, *valv;
  GCtab *weakk, *keyk, *valk;
  GCtab *weakkv, *keykv, *valkv;
  uint64_t seen0, weakkey0, weakval0, allweak0, queued0, overflow0;
  uint64_t scan_runs0, scan_tables0, scan_slots0, scan_clearable0;
  uint64_t clear_runs0, clear_tables0, clear_slots0, clear_cleared0;

  make_weak_table(L, "v", &weakv, &keyv, &valv);
  make_weak_table(L, "k", &weakk, &keyk, &valk);
  make_weak_table(L, "kv", &weakkv, &keykv, &valkv);
  seen0 = la_load64_acq(&g->gc2.weak_tables_seen);
  weakkey0 = la_load64_acq(&g->gc2.weak_tables_weakkey);
  weakval0 = la_load64_acq(&g->gc2.weak_tables_weakval);
  allweak0 = la_load64_acq(&g->gc2.weak_tables_allweak);
  queued0 = la_load64_acq(&g->gc2.weak_tables_queued);
  overflow0 = la_load64_acq(&g->gc2.weak_tables_overflow);
  scan_runs0 = la_load64_acq(&g->gc2.weak_scan_runs);
  scan_tables0 = la_load64_acq(&g->gc2.weak_scan_tables);
  scan_slots0 = la_load64_acq(&g->gc2.weak_scan_slots);
  scan_clearable0 = la_load64_acq(&g->gc2.weak_scan_clearable);
  clear_runs0 = la_load64_acq(&g->gc2.weak_clear_runs);
  clear_tables0 = la_load64_acq(&g->gc2.weak_clear_tables);
  clear_slots0 = la_load64_acq(&g->gc2.weak_clear_slots);
  clear_cleared0 = la_load64_acq(&g->gc2.weak_clear_cleared);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_weak_snapshot_count(g) == 0);
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
  assert(la_load64_acq(&g->gc2.weak_tables_seen) == seen0 + 3u);
  assert(la_load64_acq(&g->gc2.weak_tables_weakkey) == weakkey0 + 2u);
  assert(la_load64_acq(&g->gc2.weak_tables_weakval) == weakval0 + 2u);
  assert(la_load64_acq(&g->gc2.weak_tables_allweak) == allweak0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_tables_queued) == queued0 + 3u);
  assert(la_load64_acq(&g->gc2.weak_tables_overflow) == overflow0);
  assert(lj_gc2_weak_snapshot_count(g) == 3u);
  assert(weak_snapshot_has(g, weakv));
  assert(weak_snapshot_has(g, weakk));
  assert(weak_snapshot_has(g, weakkv));
  assert(lj_gc2_weak_snapshot_scan(g, 1) == 1u);
  assert(lj_gc2_weak_snapshot_scan(g, 1) == 1u);
  assert(lj_gc2_weak_snapshot_scan(g, 1) == 1u);
  assert(lj_gc2_weak_snapshot_scan(g, 1) == 0);
  assert(la_load64_acq(&g->gc2.weak_scan_runs) == scan_runs0 + 3u);
  assert(la_load64_acq(&g->gc2.weak_scan_tables) == scan_tables0 + 3u);
  assert(la_load64_acq(&g->gc2.weak_scan_slots) == scan_slots0 + 3u);
  assert(la_load64_acq(&g->gc2.weak_scan_clearable) ==
	 scan_clearable0 + 3u);
  assert(lj_gc2_weak_drain(g, 1) == 0);
  assert(la_load64_acq(&g->gc2.weak_clear_cursor) == 0);
  lj_gc2_legacy_weak_begin(g);
  assert(lj_gc2_weak_drain(g, 1) == 1u);
  assert(lj_gc2_weak_drain(g, 1) == 1u);
  assert(lj_gc2_weak_drain(g, 1) == 1u);
  assert(la_load64_acq(&g->gc2.weak_clear_runs) == clear_runs0 + 3u);
  assert(la_load64_acq(&g->gc2.weak_clear_tables) == clear_tables0 + 3u);
  assert(la_load64_acq(&g->gc2.weak_clear_slots) == clear_slots0 + 3u);
  assert(la_load64_acq(&g->gc2.weak_clear_cleared) == clear_cleared0 + 3u);
  assert(weak_entry_is_nil(L, weakv, keyv));
  assert(weak_entry_is_nil(L, weakk, keyk));
  assert(weak_entry_is_nil(L, weakkv, keykv));
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 9);
}

static void test_worker_weak_drain(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *weak, *key, *val;
  uint64_t worker_runs0, worker_weak0, clear_tables0, clear_cleared0, idle0;

  make_weak_table(L, "v", &weak, &key, &val);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  lj_gc2_legacy_weak_begin(g);
  worker_runs0 = la_load64_acq(&g->gc2.worker_runs);
  worker_weak0 = la_load64_acq(&g->gc2.worker_weak_drained);
  clear_tables0 = la_load64_acq(&g->gc2.weak_clear_tables);
  clear_cleared0 = la_load64_acq(&g->gc2.weak_clear_cleared);
  assert(lj_gc2_worker_drain(g, 1) == 1u);
  assert(la_load64_acq(&g->gc2.worker_runs) == worker_runs0 + 1u);
  assert(la_load64_acq(&g->gc2.worker_weak_drained) == worker_weak0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_clear_tables) == clear_tables0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_clear_cleared) == clear_cleared0 + 1u);
  assert(weak_entry_is_nil(L, weak, key));
  idle0 = la_load64_acq(&g->gc2.worker_idle_declares);
  assert(lj_gc2_worker_drain_progress(g, 1) == 0);
  assert(la_load64_acq(&g->gc2.worker_idle_declares) == idle0 + 1u);
  assert(la_load32_acq(&g->gc2.worker_active) == 0);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}

static void test_weak_clear_marks_string_slots(lua_State *L, global_State *g,
					       TGState *tg)
{
  GCtab *weak, *val;
  GCstr *keystr, *modestr;
  uint64_t clear_cleared0;

  lua_settop(L, 0);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_setmetatable(L, -2);
  lua_pushliteral(L, "__mode");
  lua_pushfstring(L, "kv gc2 weak mode %p", (void *)weak);
  modestr = strV(L->top - 1);
  lua_settable(L, -3);
  lua_newtable(L);
  val = tabV(L->top - 1);
  lua_pushfstring(L, "gc2 weak string key %p", (void *)weak);
  keystr = strV(L->top - 1);
  lua_pushvalue(L, 2);
  lua_settable(L, 1);
  clear_cleared0 = la_load64_acq(&g->gc2.weak_clear_cleared);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(keystr)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(modestr)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  assert(lj_gc2_weak_drain(g, 1) == 0);
  lj_gc2_legacy_weak_begin(g);
  assert(lj_gc2_weak_drain(g, 1) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(keystr)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(modestr)) == 1);
  assert(!iswhite(obj2gco(keystr)));
  assert(!iswhite(obj2gco(modestr)));
  assert(la_load64_acq(&g->gc2.weak_clear_cleared) == clear_cleared0 + 1u);
  setstrV(L, L->top, keystr);
  L->top++;
  lua_gettable(L, 1);
  assert(tvisnil(L->top - 1));
  lua_pop(L, 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 2);
}

static void test_weak_drain_uses_captured_mode(lua_State *L, global_State *g,
					       TGState *tg)
{
  GCtab *weak, *key, *val;

  lua_settop(L, 0);
  make_weak_table(L, "v", &weak, &key, &val);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  assert(lua_getmetatable(L, 1) == 1);
  lua_pushliteral(L, "__mode");
  lua_pushnil(L);
  lua_settable(L, -3);
  lua_pop(L, 1);

  lj_gc2_legacy_weak_begin(g);
  assert(lj_gc2_weak_drain(g, 1) == 1u);
  assert(weak_entry_is_nil(L, weak, key));
  assert(lj_gc2_weak_drain(g, 1) == 0);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}

static void test_weak_pre_clear_late_write_survives_drain(lua_State *L,
							  global_State *g,
							  TGState *tg)
{
  GCtab *weak, *key, *oldval, *late_val;
  uint8_t oldstate;

  lua_settop(L, 0);
  make_weak_table(L, "v", &weak, &key, &oldval);
  lua_newtable(L);
  late_val = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(oldval)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 0);

  lj_gc2_legacy_weak_begin(g);
  oldstate = g->gc.state;
  g->gc.state = GCSatomic;
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 4);
  lua_settable(L, 1);  /* P_WEAK late write before weak drain. */
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 1);
  assert(lj_gc2_weak_drain(g, 1) == 1u);
  g->gc.state = oldstate;

  lua_pushvalue(L, 2);
  lua_gettable(L, 1);
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == late_val);
  lua_pop(L, 1);

  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 4);
}

static void test_weak_post_clear_resurrection_write(lua_State *L,
						    global_State *g,
						    TGState *tg)
{
  GCtab *weak, *key, *val, *late_key, *late_val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  make_weak_table(L, "kv", &weak, &key, &val);
  lua_newtable(L);
  late_key = tabV(L->top - 1);
  lua_newtable(L);
  late_val = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(late_key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 0);

  assert(lua_getmetatable(L, 1) == 1);
  lua_pushliteral(L, "__mode");
  lua_pushnil(L);
  lua_settable(L, -3);
  lua_pop(L, 1);

  lj_gc2_legacy_weak_begin(g);
  assert(lj_gc2_weak_drain(g, 1) == 1u);
  assert(weak_entry_is_nil(L, weak, key));
  assert(lj_gc2_weak_drain(g, 1) == 0);
  weak_keys0 = la_load64_acq(&g->gc2.weak_keys_marked);
  weak_vals0 = la_load64_acq(&g->gc2.weak_values_marked);

  lua_pushvalue(L, 4);
  lua_pushvalue(L, 5);
  lua_settable(L, 1);
  assert(lj_gc2_ismarked(g, obj2gco(late_key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 1);
  assert(la_load64_acq(&g->gc2.weak_keys_marked) == weak_keys0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_values_marked) == weak_vals0 + 1u);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lua_pushvalue(L, 4);
  lua_gettable(L, 1);
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == late_val);
  lua_pop(L, 1);

  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 5);
}

static void test_vm_weak_post_clear_existing_key_write(lua_State *L,
						       global_State *g,
						       TGState *tg)
{
  GCtab *weak, *key, *oldval, *late_val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "return function(t, k, v) t[k] = v end\n") == LUA_OK);
  make_weak_table(L, "k", &weak, &key, &oldval);
  lua_newtable(L);
  late_val = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(oldval)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 0);

  lj_gc2_legacy_weak_begin(g);
  assert(lj_gc2_weak_drain(g, 1) == 1u);
  assert(weak_entry_is_nil(L, weak, key));
  assert(lj_gc2_weak_drain(g, 1) == 0);
  weak_keys0 = la_load64_acq(&g->gc2.weak_keys_marked);
  weak_vals0 = la_load64_acq(&g->gc2.weak_values_marked);

  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 5);
  lua_call(L, 3, 0);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 1);
  assert(la_load64_acq(&g->gc2.weak_keys_marked) == weak_keys0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_values_marked) == weak_vals0);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lua_pushvalue(L, 3);
  lua_gettable(L, 2);
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == late_val);
  lua_pop(L, 1);

  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 5);
}

static void test_capi_rawset_weak_write_barrier(lua_State *L, global_State *g,
						TGState *tg)
{
  GCtab *hash_weak, *hash_key, *hash_val, *array_weak, *array_val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  lua_newtable(L);
  hash_weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "kv");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  hash_key = tabV(L->top - 1);
  lua_newtable(L);
  hash_val = tabV(L->top - 1);

  lua_newtable(L);
  array_weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  array_val = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(hash_weak)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(array_weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(hash_weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(array_weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(hash_key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(hash_val)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(array_val)) == 0);

  lj_gc2_legacy_weak_begin(g);
  weak_keys0 = la_load64_acq(&g->gc2.weak_keys_marked);
  weak_vals0 = la_load64_acq(&g->gc2.weak_values_marked);

  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_rawset(L, 1);  /* C API raw hash weak write. */
  lua_pushvalue(L, 5);
  lua_rawseti(L, 4, 1);  /* C API raw array weak write. */

  assert(lj_gc2_ismarked(g, obj2gco(hash_key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(hash_val)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(array_val)) == 1);
  assert(la_load64_acq(&g->gc2.weak_keys_marked) == weak_keys0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_values_marked) == weak_vals0 + 2u);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 5);
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

static void test_vm_weak_key_write_barrier(lua_State *L, global_State *g,
					   TGState *tg)
{
  GCtab *weak, *key, *val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "return function(t, k, v) t[k] = v end\n") == LUA_OK);
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
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 4);
  lua_call(L, 3, 0);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(la_load64_acq(&g->gc2.weak_keys_marked) == weak_keys0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_values_marked) == weak_vals0);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 4);
}

static void test_peer_weak_key_write_barrier(lua_State *L, global_State *g,
					     TGState *tg)
{
  lua_State *peer;
  GCtab *weak, *key, *val;
  WeakPeerWriteCtx ctx;
  pthread_t thread;
  uint64_t weak_keys0, weak_vals0;
  int i;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "return function(t, k, v) t[k] = v end\n") == LUA_OK);
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
  peer = lua_newthread(L);
  assert(peer != NULL);
  assert(lua_checkstack(peer, 4));
  for (i = 1; i <= 4; i++) {
    lua_pushvalue(L, i);
    lua_xmove(L, peer, 1);
  }

  ctx.L = peer;
  ctx.status = -1;
  assert(pthread_barrier_init(&ctx.barrier, NULL, 2) == 0);
  assert(pthread_create(&thread, NULL, weak_peer_write_thread, &ctx) == 0);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  weak_keys0 = la_load64_acq(&g->gc2.weak_keys_marked);
  weak_vals0 = la_load64_acq(&g->gc2.weak_values_marked);

  lj_gc2_legacy_weak_begin(g);
  grey_wait(&ctx.barrier);  /* Peer TG performs the P_WEAK table write. */
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.status == 0);
  assert(pthread_barrier_destroy(&ctx.barrier) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(la_load64_acq(&g->gc2.weak_keys_marked) == weak_keys0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_values_marked) == weak_vals0);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_settop(peer, 0);
  lua_pop(L, 5);
}

static void test_vm_weak_value_hash_key_barrier(lua_State *L, global_State *g,
						TGState *tg)
{
  GCtab *weak, *key, *val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "return function(t, k, v) t[k] = v end\n") == LUA_OK);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
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
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 4);
  lua_call(L, 3, 0);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(la_load64_acq(&g->gc2.weak_keys_marked) == weak_keys0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_values_marked) == weak_vals0);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 4);
}

static void test_vm_weak_value_array_barrier(lua_State *L, global_State *g,
					     TGState *tg)
{
  GCtab *weak, *val;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "return function(t, v) t[1] = v end\n") == LUA_OK);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_pushboolean(L, 0);
  lua_rawseti(L, -2, 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  val = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  lj_gc2_legacy_weak_begin(g);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_call(L, 2, 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}

static void test_table_insert_weak_value_array_barrier(lua_State *L,
						       global_State *g,
						       TGState *tg)
{
  GCtab *weak, *val;
  uint64_t weak_vals0;

  lua_settop(L, 0);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_pushboolean(L, 0);
  lua_rawseti(L, -2, 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  val = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  lj_gc2_legacy_weak_begin(g);
  weak_vals0 = la_load64_acq(&g->gc2.weak_values_marked);
  lua_getglobal(L, "table");
  lua_getfield(L, -1, "insert");
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_call(L, 2, 0);  /* table.insert weak-value array write. */
  lua_pop(L, 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(la_load64_acq(&g->gc2.weak_values_marked) == weak_vals0 + 1u);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 2);
}

static void test_capi_weak_newindex_target_write_barrier(lua_State *L,
							 global_State *g,
							 TGState *tg)
{
  GCtab *weak, *key, *oldval, *proxy, *late_val, *field_val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  make_weak_table(L, "v", &weak, &key, &oldval);
  lua_newtable(L);
  proxy = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__newindex");
  lua_pushvalue(L, 1);
  lua_settable(L, -3);
  lua_setmetatable(L, 4);
  lua_newtable(L);
  late_val = tabV(L->top - 1);
  lua_newtable(L);
  field_val = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(proxy)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(oldval)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(field_val)) == 0);

  lj_gc2_legacy_weak_begin(g);
  assert(lj_gc2_weak_drain(g, 1) == 1u);
  assert(weak_entry_is_nil(L, weak, key));
  assert(lj_gc2_weak_drain(g, 1) == 0);
  weak_keys0 = la_load64_acq(&g->gc2.weak_keys_marked);
  weak_vals0 = la_load64_acq(&g->gc2.weak_values_marked);

  lua_pushvalue(L, 2);
  lua_pushvalue(L, 5);
  lua_settable(L, 4);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 1);
  assert(la_load64_acq(&g->gc2.weak_keys_marked) == weak_keys0);
  assert(la_load64_acq(&g->gc2.weak_values_marked) == weak_vals0 + 1u);

  lua_pushvalue(L, 6);
  lua_setfield(L, 4, "field");
  assert(lj_gc2_ismarked(g, obj2gco(field_val)) == 1);
  assert(la_load64_acq(&g->gc2.weak_values_marked) == weak_vals0 + 2u);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);

  lua_pushvalue(L, 2);
  lua_gettable(L, 1);
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == late_val);
  lua_pop(L, 1);
  lua_getfield(L, 1, "field");
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == field_val);
  lua_pop(L, 1);

  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 6);
}

static void test_tvalue_range_barrier(lua_State *L, global_State *g,
				      TGState *tg, GCtab *child1,
				      GCtab *child2)
{
  TValue vals[2];

  settabV(L, &vals[0], child1);
  settabV(L, &vals[1], child2);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(child1)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child2)) == 0);
  lj_gc2_barrier_tvn_g(g, vals, 2);
  assert(lj_gc2_ismarked(g, obj2gco(child1)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child2)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
}

static void test_vm_tsetm_range_barrier(lua_State *L, global_State *g,
					TGState *tg)
{
  enum { TSETM_BIG_N = 96 };
  GCtab *child1, *child2, *parent, *src, *last;
  int i;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "return function(a, b)\n"
    "  local function many() return a, b end\n"
    "  return { many() }\n"
    "end,\n"
    "function(src, n)\n"
    "  local function many() return unpack(src, 1, n) end\n"
    "  return { many() }\n"
    "end\n") == LUA_OK);
  lua_newtable(L);
  child1 = tabV(L->top - 1);
  lua_newtable(L);
  child2 = tabV(L->top - 1);

  test_tvalue_range_barrier(L, g, tg, child1, child2);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(child1)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child2)) == 0);

  lua_pushvalue(L, 1);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 4);
  lua_call(L, 2, 1);
  parent = tabV(L->top - 1);
  assert(lj_tab_asize_acq(parent) >= 2);
  assert(tabV(lj_tab_getint(parent, 1)) == child1);
  assert(tabV(lj_tab_getint(parent, 2)) == child2);
  assert(lj_gc2_ismarked(g, obj2gco(child1)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child2)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);

  lua_createtable(L, TSETM_BIG_N, 0);
  src = tabV(L->top - 1);
  last = NULL;
  for (i = 1; i <= TSETM_BIG_N; i++) {
    lua_newtable(L);
    if (i == TSETM_BIG_N)
      last = tabV(L->top - 1);
    lua_rawseti(L, -2, i);
  }

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(src)) == 0);
  assert(last != NULL);
  assert(lj_gc2_ismarked(g, obj2gco(last)) == 0);

  lua_pushvalue(L, 2);
  lua_pushvalue(L, 5);
  lua_pushinteger(L, TSETM_BIG_N);
  lua_call(L, 2, 1);
  parent = tabV(L->top - 1);
  assert(lj_tab_asize_acq(parent) >= TSETM_BIG_N);
  assert(tabV(lj_tab_getint(parent, TSETM_BIG_N)) == last);
  assert(lj_gc2_ismarked(g, obj2gco(last)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 6);
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

static void test_tg_thread_roots(lua_State *L, global_State *g, TGState *tg)
{
  TGState extra_tg;
  lua_State *thread_L, *cur_L;
  uint64_t thread_roots0, cur_roots0;

  thread_L = lua_newthread(L);
  assert(thread_L != NULL);
  cur_L = lua_newthread(L);
  assert(cur_L != NULL);

  lj_tg_init_thread(g, &extra_tg, thread_L, 1);
  extra_tg.tid = tg->tid + 6000u;
  if (extra_tg.tid == 0 || extra_tg.tid == LJ_THREAD_GCSCAN)
    extra_tg.tid = 6000u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  extra_tg.cur_L = cur_L;
  cur_L->tg_hint = &extra_tg;
  thread_L->thr_owner = extra_tg.tid;
  cur_L->thr_owner = extra_tg.tid;

  thread_roots0 = la_load64_acq(&g->gc2.tg_thread_roots);
  cur_roots0 = la_load64_acq(&g->gc2.tg_cur_roots);
  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(thread_L)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(cur_L)) == 0);
  lj_tg_attach(g, &extra_tg);
  assert(g->gc2.n_threads == 2);
  lj_gc2_scan_roots(g, NULL);
  assert(lj_gc2_ismarked(g, obj2gco(thread_L)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(cur_L)) == 1);
  assert(la_load64_acq(&g->gc2.tg_thread_roots) > thread_roots0);
  assert(la_load64_acq(&g->gc2.tg_cur_roots) == cur_roots0 + 1u);

  thread_L->thr_owner = 0;
  cur_L->thr_owner = 0;
  thread_L->tg_hint = tg;
  cur_L->tg_hint = tg;
  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, &extra_tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 2);
}

static void test_minor_root_scan(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *registry_tab, *stack_tab;
  uint32_t generational0 = la_load32_acq(&g->gc2.generational);
  uint32_t sweep_gate0 = la_load32_acq(&g->gc2.minor_sweep_enabled);
  uint32_t roots_gate0 = la_load32_acq(&g->gc2.minor_roots_enabled);
  UNUSED(tg);

  lua_newtable(L);
  registry_tab = tabV(L->top - 1);
  lua_setfield(L, LUA_REGISTRYINDEX, "gc2_minor_root_scan");
  lua_newtable(L);
  stack_tab = tabV(L->top - 1);

  la_store32_rel(&g->gc2.generational, 0);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 1);
  la_store32_rel(&g->gc2.minor_roots_enabled, 1);
  lj_gc2_legacy_mark_begin(g);
  assert(la_load32_acq(&g->gc2.cycle_roots_minor) == 0);
  lj_gc2_scan_minor_roots(g, L);
  assert(lj_gc2_ismarked(g, obj2gco(stack_tab)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(registry_tab)) == 0);
  lj_gc2_legacy_cycle_end(g);

  la_store32_rel(&g->gc2.generational, 1);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 1);
  la_store32_rel(&g->gc2.minor_roots_enabled, 1);
  lj_gc2_legacy_mark_begin(g);
  assert(la_load32_acq(&g->gc2.cycle_roots_minor) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(stack_tab)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(registry_tab)) == 0);
  assert(lj_gc2_handshake(g, LJ_GC2_HS_SCAN_ROOTS) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(stack_tab)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(registry_tab)) == 0);
  la_store32_rel(&g->gc2.generational, generational0);
  la_store32_rel(&g->gc2.minor_sweep_enabled, sweep_gate0);
  la_store32_rel(&g->gc2.minor_roots_enabled, roots_gate0);
  lj_gc2_legacy_cycle_end(g);

  lua_pushnil(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "gc2_minor_root_scan");
  lua_pop(L, 1);
}

static void test_thread(lua_State *L, global_State *g, TGState *tg)
{
  lua_State *th, *busy, *oldcur;
  GCtab *stack_tab, *busy_tab;
  uint64_t claims0, busy0, requeues0, owner_scans0;
  uint64_t needscan0, owner_needscans0;
  uint64_t dirty_misses0;
  uint32_t busy_owner = tg->tid + 5000u;
  if (busy_owner == 0 || busy_owner == LJ_THREAD_GCSCAN)
    busy_owner = 123u;

  th = lua_newthread(L);
  assert(th != NULL);
  lua_newtable(th);
  stack_tab = tabV(th->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(stack_tab)) == 0);
  assert(lj_state_claim(th, LJ_THREAD_GCSCAN) == 0);
  assert(th->thr_owner == 0);
  claims0 = la_load64_acq(&g->gc2.thread_scan_claims);
  assert(lj_gc2_markobj(g, obj2gco(th)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarkedmem(g, tvref(th->stack)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(stack_tab)) == 1);
  assert(la_load64_acq(&g->gc2.thread_scan_claims) == claims0 + 1u);
  assert(th->thr_owner == 0);
  lj_gc2_legacy_cycle_end(g);

  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  busy_tab = tabV(busy->top - 1);
  busy->thr_owner = busy_owner;
  busy0 = la_load64_acq(&g->gc2.thread_scan_busy);
  requeues0 = la_load64_acq(&g->gc2.thread_scan_requeues);
  claims0 = la_load64_acq(&g->gc2.thread_scan_claims);
  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(busy)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(lj_gc2_worker_drain(g, 2) == 2u);
  assert(la_load64_acq(&g->gc2.thread_scan_busy) == busy0 + 1u);
  assert(la_load64_acq(&g->gc2.thread_scan_requeues) == requeues0 + 1u);
  assert(busy->thr_owner == busy_owner);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);
  busy->thr_owner = 0;
  worker_drain_all(g);
  assert(la_load64_acq(&g->gc2.thread_scan_claims) == claims0 + 1u);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 1);
  assert(lj_gc2_ssb_empty(g));
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);

  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  busy_tab = tabV(busy->top - 1);
  busy->thr_owner = tg->tid;
  busy0 = la_load64_acq(&g->gc2.thread_scan_busy);
  requeues0 = la_load64_acq(&g->gc2.thread_scan_requeues);
  owner_scans0 = la_load64_acq(&g->gc2.thread_scan_owner_scans);
  needscan0 = la_load64_acq(&g->gc2.thread_scan_needscan);
  owner_needscans0 = la_load64_acq(&g->gc2.thread_scan_owner_needscans);
  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(busy)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(lj_gc2_worker_drain(g, 2) == 2u);
  assert(la_load64_acq(&g->gc2.thread_scan_busy) == busy0 + 1u);
  assert(la_load64_acq(&g->gc2.thread_scan_requeues) == requeues0 + 1u);
  assert(la_load64_acq(&g->gc2.thread_scan_needscan) == needscan0 + 1u);
  assert(lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);
  lj_gc2_scan_roots(g, L);
  assert(busy->scan_epoch == g->gc2.cycle);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 1);
  assert(la_load64_acq(&g->gc2.thread_scan_owner_needscans) ==
	 owner_needscans0 + 1u);
  assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  (void)lj_gc2_flush_ssb(g, tg);
  worker_drain_all(g);
  assert(la_load64_acq(&g->gc2.thread_scan_owner_scans) > owner_scans0);
  assert(lj_gc2_ssb_empty(g));
  busy->thr_owner = 0;
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);

  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  busy_tab = tabV(busy->top - 1);
  oldcur = tg->cur_L;
  busy->thr_owner = tg->tid;
  lj_tg_setcur_L(g, busy);
  busy0 = la_load64_acq(&g->gc2.thread_scan_busy);
  requeues0 = la_load64_acq(&g->gc2.thread_scan_requeues);
  owner_scans0 = la_load64_acq(&g->gc2.thread_scan_owner_scans);
  lj_gc2_legacy_mark_begin(g);
  assert(busy->scan_epoch != g->gc2.cycle);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(busy)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(lj_gc2_worker_drain(g, 2) == 2u);
  assert(la_load64_acq(&g->gc2.thread_scan_busy) == busy0 + 1u);
  assert(la_load64_acq(&g->gc2.thread_scan_requeues) == requeues0 + 1u);
  assert(la_load64_acq(&g->gc2.thread_scan_owner_scans) == owner_scans0);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);
  busy->thr_owner = 0;
  if (oldcur)
    lj_tg_setcur_L(g, oldcur);
  else
    lj_tg_clearcur_L(g);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);

  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  busy_tab = tabV(busy->top - 1);
  oldcur = tg->cur_L;
  busy->thr_owner = tg->tid;
  lj_tg_setcur_L(g, busy);
  busy0 = la_load64_acq(&g->gc2.thread_scan_busy);
  requeues0 = la_load64_acq(&g->gc2.thread_scan_requeues);
  owner_scans0 = la_load64_acq(&g->gc2.thread_scan_owner_scans);
  needscan0 = la_load64_acq(&g->gc2.thread_scan_needscan);
  dirty_misses0 = la_load64_acq(&g->gc2.thread_scan_dirty_misses);
  lj_gc2_legacy_mark_begin(g);
  lj_gc2_scan_roots(g, busy);
  assert(busy->scan_epoch == g->gc2.cycle);
  assert(busy->scan_dirty_epoch == la_load64_acq(&tg->stack_dirty_epoch));
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 1);
  la_add64_rlx(&tg->stack_dirty_epoch, 1);
  assert(lj_gc2_flush_ssb(g, tg) != 0);
  {
    int i;
    for (i = 0; i < 64 &&
	 la_load64_acq(&g->gc2.thread_scan_dirty_misses) == dirty_misses0; i++)
      (void)lj_gc2_worker_drain(g, 64);
  }
  assert(la_load64_acq(&g->gc2.thread_scan_busy) > busy0);
  assert(la_load64_acq(&g->gc2.thread_scan_requeues) > requeues0);
  assert(la_load64_acq(&g->gc2.thread_scan_owner_scans) == owner_scans0);
  assert(la_load64_acq(&g->gc2.thread_scan_needscan) > needscan0);
  assert(la_load64_acq(&g->gc2.thread_scan_dirty_misses) > dirty_misses0);
  assert(lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN);
  lj_gc2_scan_roots(g, busy);
  assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  assert(busy->scan_dirty_epoch == la_load64_acq(&tg->stack_dirty_epoch));
  (void)lj_gc2_flush_ssb(g, tg);
  worker_drain_all(g);
  assert(la_load64_acq(&g->gc2.thread_scan_owner_scans) > owner_scans0);
  assert(lj_gc2_ssb_empty(g));
  busy->thr_owner = 0;
  if (oldcur)
    lj_tg_setcur_L(g, oldcur);
  else
    lj_tg_clearcur_L(g);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);

  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  busy_tab = tabV(busy->top - 1);
  oldcur = tg->cur_L;
  busy->thr_owner = tg->tid;
  lj_tg_setcur_L(g, busy);
  busy0 = la_load64_acq(&g->gc2.thread_scan_busy);
  requeues0 = la_load64_acq(&g->gc2.thread_scan_requeues);
  owner_scans0 = la_load64_acq(&g->gc2.thread_scan_owner_scans);
  lj_gc2_legacy_mark_begin(g);
  lj_gc2_scan_roots(g, busy);
  assert(busy->scan_epoch == g->gc2.cycle);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) != 0);
  worker_drain_all(g);
  assert(la_load64_acq(&g->gc2.thread_scan_busy) > busy0);
  assert(la_load64_acq(&g->gc2.thread_scan_owner_scans) > owner_scans0);
  assert(la_load64_acq(&g->gc2.thread_scan_requeues) == requeues0);
  assert(lj_gc2_ssb_empty(g));
  busy->thr_owner = 0;
  if (oldcur)
    lj_tg_setcur_L(g, oldcur);
  else
    lj_tg_clearcur_L(g);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);
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

static int gc2_empty_finalizer(lua_State *L)
{
  UNUSED(L);
  return 0;
}

static int gc2_udata_finalized;
static int gc2_cdata_finalized;
#if LJ_HASFFI
static int gc2_cdata_order[3];
static int gc2_cdata_order_count;
static int gc2_cdata_bulk_finalized;
#endif

static int gc2_counting_finalizer(lua_State *L)
{
  UNUSED(L);
  gc2_udata_finalized++;
  return 0;
}

static int gc2_cdata_counting_finalizer(lua_State *L)
{
  UNUSED(L);
  gc2_cdata_finalized++;
  return 0;
}

static void test_lib_register_weak_value_barrier(void)
{
  static const uint8_t init[] = {
    0, 0, 1,
    LIBINIT_COPY, 2,
    (uint8_t)(LIBINIT_STRING|4), 's', 'l', 'o', 't',
    LIBINIT_SET,
    LIBINIT_END
  };
  TGState *oldtg = lj_thr_get_tg();
  lua_State *L2 = luaL_newstate();
  global_State *g2;
  TGState *tg2;
  GCtab *mod, *val;
  uint64_t weak_vals0;

  assert(L2 != NULL);
  lua_gc(L2, LUA_GCSTOP, 0);
  lua_pushcfunction(L2, luaopen_base);
  lua_call(L2, 0, 1);
  lua_pop(L2, 1);
  lua_pushcfunction(L2, luaopen_package);
  lua_call(L2, 0, 1);
  lua_pop(L2, 1);
  g2 = G(L2);
  tg2 = G2TG(g2);
  assert(tg2 != NULL);
  assert(luaL_dostring(L2,
    "package.loaded.m8lib = nil\n"
    "m8lib = setmetatable({}, { __mode = 'v' })\n"
    "package.loaded.m8lib = m8lib\n") == LUA_OK);
  lua_newtable(L2);
  val = tabV(L2->top - 1);
  lua_getglobal(L2, "m8lib");
  mod = tabV(L2->top - 1);

  lj_gc2_legacy_mark_begin(g2);
  assert(lj_gc2_markobj(g2, obj2gco(mod)) == 1);
  flush_and_drain(g2, tg2);
  assert(weak_snapshot_has(g2, mod));
  assert(lj_gc2_ismarked(g2, obj2gco(val)) == 0);
  lua_pop(L2, 1);

  weak_vals0 = la_load64_acq(&g2->gc2.weak_values_marked);
  lj_gc2_legacy_weak_begin(g2);
  lj_lib_register(L2, "m8lib", init, NULL);
  assert(tabV(L2->top - 1) == mod);
  assert(lj_gc2_ismarked(g2, obj2gco(val)) == 1);
  while (lj_gc2_weak_drain(g2, 1) != 0)
    ;
  lua_getfield(L2, -1, "slot");
  assert(tabV(L2->top - 1) == val);
  assert(la_load64_acq(&g2->gc2.weak_values_marked) == weak_vals0 + 1u);
  lj_gc2_legacy_cycle_end(g2);
  lua_close(L2);
  lj_thr_set_tg(oldtg);
}

#if LJ_HASFFI
static int gc2_cdata_order_finalizer(lua_State *L, int id)
{
  UNUSED(L);
  assert(gc2_cdata_order_count < 3);
  gc2_cdata_order[gc2_cdata_order_count++] = id;
  return 0;
}

static int gc2_cdata_order_finalizer_1(lua_State *L)
{
  return gc2_cdata_order_finalizer(L, 1);
}

static int gc2_cdata_order_finalizer_2(lua_State *L)
{
  return gc2_cdata_order_finalizer(L, 2);
}

static int gc2_cdata_order_finalizer_3(lua_State *L)
{
  return gc2_cdata_order_finalizer(L, 3);
}

static int gc2_cdata_bulk_finalizer(lua_State *L)
{
  UNUSED(L);
  gc2_cdata_bulk_finalized++;
  return 0;
}

static void test_ffi_loaded_weak_value_barrier(void)
{
  TGState *oldtg = lj_thr_get_tg();
  lua_State *L2 = luaL_newstate();
  global_State *g2;
  TGState *tg2;
  GCtab *loaded, *mod;
  assert(L2 != NULL);
  lua_gc(L2, LUA_GCSTOP, 0);
  lua_pushcfunction(L2, luaopen_base);
  lua_call(L2, 0, 1);
  lua_pop(L2, 1);
  lua_pushcfunction(L2, luaopen_package);
  lua_call(L2, 0, 1);
  lua_pop(L2, 1);
  g2 = G(L2);
  tg2 = G2TG(g2);
  assert(tg2 != NULL);
  assert(luaL_dostring(L2,
    "package.loaded.ffi = nil\n"
    "setmetatable(package.loaded, { __mode = 'v' })\n") == LUA_OK);
  lua_getglobal(L2, "package");
  lua_getfield(L2, -1, "loaded");
  loaded = tabV(L2->top - 1);

  lj_gc2_legacy_mark_begin(g2);
  assert(lj_gc2_markobj(g2, obj2gco(loaded)) == 1);
  flush_and_drain(g2, tg2);
  assert(lj_gc2_weak_snapshot_count(g2) >= 1u);
  lua_settop(L2, 0);

  lj_gc2_legacy_weak_begin(g2);
  lua_pushcfunction(L2, luaopen_ffi);
  lua_call(L2, 0, 1);
  mod = tabV(L2->top - 1);
  assert(lj_gc2_ismarked(g2, obj2gco(mod)) == 1);
  while (lj_gc2_weak_drain(g2, 1) != 0)
    ;
  lua_getglobal(L2, "package");
  lua_getfield(L2, -1, "loaded");
  lua_getfield(L2, -1, "ffi");
  assert(tabV(L2->top - 1) == mod);
  lj_gc2_legacy_cycle_end(g2);
  lua_close(L2);
  lj_thr_set_tg(oldtg);
}
#endif

static void test_finalizer_spawn_deferred_state(lua_State *L, global_State *g)
{
  uint64_t deferrals0 = la_load64_acq(&g->gc2.finalizer_spawn_deferrals);

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "local th = require('threading')\n"
    "ffi.cdef('typedef struct { int x; } gc2_spawn_defer_t;')\n"
    "gc2_spawn_started = th.channel(1)\n"
    "gc2_spawn_release = th.channel(1)\n"
    "gc2_spawn_worker = nil\n"
    "do\n"
    "  local cd = ffi.gc(ffi.new('gc2_spawn_defer_t'), function()\n"
    "    gc2_spawn_worker = th.spawn(function(started, release)\n"
    "      started:send('started')\n"
    "      local msg, ok = release:recv(10)\n"
    "      return ok == true and msg == 'release'\n"
    "    end, gc2_spawn_started, gc2_spawn_release)\n"
    "  end)\n"
    "end\n") == LUA_OK);

  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.state == GCSfinalize);
  assert(la_load32_acq(&g->mt_live) != 0);
  assert(la_load32_acq(&g->mt_gc_exclusive) == 0);
  assert(lj_gc_threshold_load(g) == LJ_MAX_MEM);
  assert(la_load64_acq(&g->gc2.finalizer_spawn_deferrals) > deferrals0);

  assert(luaL_dostring(L,
    "local msg, ok = gc2_spawn_started:recv(1)\n"
    "assert(ok == true and msg == 'started')\n"
    "assert(gc2_spawn_release:send('release', 1) == true)\n"
    "local joined, result = gc2_spawn_worker:join(10)\n"
    "assert(joined == true and result == true, tostring(result))\n"
    "gc2_spawn_worker = nil\n"
    "gc2_spawn_started = nil\n"
    "gc2_spawn_release = nil\n") == LUA_OK);
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(la_load32_acq(&g->mt_live) == 0);
  assert(g->gc.state == GCSpause);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
}

static void drive_udata_finalizers(lua_State *L)
{
  int i;
  for (i = 0; i < 4; i++)
    lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
}

static void push_udata_finalizer_mt(lua_State *L)
{
  lua_newtable(L);
  lua_pushcfunction(L, gc2_empty_finalizer);
  lua_setfield(L, -2, "__gc");
}

static int test_unlink_udata_object(global_State *g, GCobj *target)
{
  GCRef *p = lj_obj_gcwref(obj2gco(mainthread(g)));
  GCobj *o;
  while ((o = gcref(*p)) != NULL) {
    if (o == target) {
      setgcrefr(*p, *lj_obj_gcwref(o));
      return 1;
    }
    p = lj_obj_gcwref(o);
  }
  return 0;
}

static void test_finreg_userdata_telemetry(lua_State *L, global_State *g)
{
  uint64_t sets0 = la_load64_acq(&g->gc2.finreg_udata_sets);
  uint64_t clears0 = la_load64_acq(&g->gc2.finreg_udata_clears);
  uint64_t queued0 = la_load64_acq(&g->gc2.finreg_udata_queued);
  uint64_t registered0 = la_load64_acq(&g->gc2.finreg_udata_registered);
  uint64_t discovered0 = la_load64_acq(&g->gc2.finreg_udata_discovered);
  uint64_t forgets0 = la_load64_acq(&g->gc2.finreg_udata_forgets);

  lua_settop(L, 0);
  lua_newuserdata(L, 1);
  push_udata_finalizer_mt(L);
  lua_setmetatable(L, -2);
  assert(la_load64_acq(&g->gc2.finreg_udata_sets) == sets0 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_udata_registered) ==
	 registered0 + 1u);
  lua_pushnil(L);
  lua_setmetatable(L, -2);
  assert(la_load64_acq(&g->gc2.finreg_udata_clears) == clears0 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_udata_forgets) == forgets0 + 1u);
  push_udata_finalizer_mt(L);
  lua_setmetatable(L, -2);
  assert(la_load64_acq(&g->gc2.finreg_udata_sets) == sets0 + 2u);
  assert(la_load64_acq(&g->gc2.finreg_udata_registered) ==
	 registered0 + 2u);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(la_load64_acq(&g->gc2.finreg_udata_queued) == queued0 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_udata_clears) == clears0 + 2u);
  assert(la_load64_acq(&g->gc2.finreg_udata_discovered) ==
	 discovered0 + 1u);

  lua_settop(L, 0);
  lua_newuserdata(L, 1);
  push_udata_finalizer_mt(L);
  lua_setmetatable(L, -2);
  assert(la_load64_acq(&g->gc2.finreg_udata_sets) == sets0 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_udata_registered) ==
	 registered0 + 3u);
  lua_getmetatable(L, -1);
  lua_pushnil(L);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 2);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(la_load64_acq(&g->gc2.finreg_udata_queued) == queued0 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_udata_clears) == clears0 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_udata_discovered) ==
	 discovered0 + 1u);

  lua_settop(L, 0);
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_setmetatable(L, -2);
  assert(la_load64_acq(&g->gc2.finreg_udata_sets) == sets0 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_udata_registered) ==
	 registered0 + 4u);
  lua_getmetatable(L, -1);
  lua_pushcfunction(L, gc2_empty_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 2);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(la_load64_acq(&g->gc2.finreg_udata_sets) == sets0 + 4u);
  assert(la_load64_acq(&g->gc2.finreg_udata_queued) == queued0 + 2u);
  assert(la_load64_acq(&g->gc2.finreg_udata_clears) == clears0 + 4u);
  assert(la_load64_acq(&g->gc2.finreg_udata_registered) ==
	 registered0 + 4u);
  assert(la_load64_acq(&g->gc2.finreg_udata_discovered) ==
	 discovered0 + 2u);

  lua_settop(L, 0);
  lua_newuserdata(L, 1);
  push_udata_finalizer_mt(L);
  lua_setmetatable(L, -2);
  assert(la_load64_acq(&g->gc2.finreg_udata_sets) == sets0 + 5u);
  assert(la_load64_acq(&g->gc2.finreg_udata_registered) ==
	 registered0 + 5u);
  assert(test_unlink_udata_object(g, obj2gco(udataV(L->top - 1))));
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(la_load64_acq(&g->gc2.finreg_udata_queued) == queued0 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_udata_clears) == clears0 + 5u);
  assert(la_load64_acq(&g->gc2.finreg_udata_discovered) ==
	 discovered0 + 3u);
}

static void test_finreg_internal_userdata_telemetry(lua_State *L,
						    global_State *g)
{
  uint64_t sets0 = la_load64_acq(&g->gc2.finreg_udata_sets);
  uint64_t clears0 = la_load64_acq(&g->gc2.finreg_udata_clears);
  uint64_t queued0 = la_load64_acq(&g->gc2.finreg_udata_queued);
  uint64_t registered0 = la_load64_acq(&g->gc2.finreg_udata_registered);
  uint64_t discovered0 = la_load64_acq(&g->gc2.finreg_udata_discovered);
  lua_Integer registered, immediate, discoverable, lazy;

  lua_settop(L, 0);
  lua_pushcfunction(L, gc2_empty_finalizer);
  lua_setglobal(L, "gc2_internal_udata_finalizer");
  assert(luaL_dostring(L,
    "local registered, immediate, discoverable, lazy = 0, 0, 0, 0\n"
    "do\n"
    "  local f = assert(io.tmpfile())\n"
    "  registered = registered + 1\n"
    "  immediate = immediate + 1\n"
    "  discoverable = discoverable + 1\n"
    "end\n"
    "do\n"
    "  local ok, buffer = pcall(require, 'buffer')\n"
    "  if ok and buffer and buffer.new then\n"
    "    local b = buffer.new()\n"
    "    registered = registered + 1\n"
    "    immediate = immediate + 1\n"
    "    discoverable = discoverable + 1\n"
    "  end\n"
    "end\n"
    "do\n"
    "  local ok, threading = pcall(require, 'threading')\n"
    "  if ok and threading and threading.mutex and threading.channel then\n"
    "    local m = threading.mutex()\n"
    "    registered = registered + 1\n"
    "    debug.getmetatable(m).__gc = gc2_internal_udata_finalizer\n"
    "    discoverable = discoverable + 1\n"
    "    lazy = lazy + 1\n"
    "    local ch = threading.channel(1)\n"
    "    registered = registered + 1\n"
    "    immediate = immediate + 1\n"
    "    discoverable = discoverable + 1\n"
    "  end\n"
    "end\n"
#if LJ_HASFFI
    "do\n"
    "  local ok, ffi = pcall(require, 'ffi')\n"
    "  if ok and ffi and ffi.pin then\n"
    "    registered = registered + 1 -- ffi.C default namespace.\n"
    "    immediate = immediate + 1\n"
    "    local pin = ffi.pin({})\n"
    "    registered = registered + 1\n"
    "    immediate = immediate + 1\n"
    "    discoverable = discoverable + 1\n"
    "  end\n"
    "end\n"
#endif
    "return registered, immediate, discoverable, lazy\n") ==
    LUA_OK);
  registered = lua_tointeger(L, -4);
  immediate = lua_tointeger(L, -3);
  discoverable = lua_tointeger(L, -2);
  lazy = lua_tointeger(L, -1);
  lua_pop(L, 4);
  assert(registered >= discoverable);
  assert(discoverable >= 3);
  assert(la_load64_acq(&g->gc2.finreg_udata_sets) ==
	 sets0 + (uint64_t)immediate);
  assert(la_load64_acq(&g->gc2.finreg_udata_registered) ==
	 registered0 + (uint64_t)registered);

  drive_udata_finalizers(L);
  assert(la_load64_acq(&g->gc2.finreg_udata_sets) ==
	 sets0 + (uint64_t)(immediate + lazy));
  assert(la_load64_acq(&g->gc2.finreg_udata_queued) ==
	 queued0 + (uint64_t)discoverable);
  assert(la_load64_acq(&g->gc2.finreg_udata_clears) ==
	 clears0 + (uint64_t)discoverable);
  assert(la_load64_acq(&g->gc2.finreg_udata_discovered) ==
	 discovered0 + (uint64_t)discoverable);
  lua_pushnil(L);
  lua_setglobal(L, "gc2_internal_udata_finalizer");
}

static void test_finreg_userdata_inplace_finalizer_behavior(lua_State *L)
{
  int finalized0;

  lua_settop(L, 0);
  finalized0 = gc2_udata_finalized;
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_setmetatable(L, -2);
  lua_getmetatable(L, -1);
  lua_pushcfunction(L, gc2_counting_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 2);
  drive_udata_finalizers(L);
  assert(gc2_udata_finalized == finalized0 + 1);

  lua_settop(L, 0);
  finalized0 = gc2_udata_finalized;
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, gc2_counting_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_getmetatable(L, -1);
  lua_pushnil(L);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 2);
  drive_udata_finalizers(L);
  assert(gc2_udata_finalized == finalized0);
}

static void test_finreg_userdata_queue_mark(lua_State *L, global_State *g,
					    TGState *tg)
{
  GCtab *env, *mt;
  GCudata *ud;
  uint64_t queued0;

  lua_settop(L, 0);
  lua_newtable(L);
  env = tabV(L->top - 1);
  lua_newuserdata(L, 1);
  ud = udataV(L->top - 1);
  lua_pushvalue(L, 1);
  lua_setfenv(L, -2);
  push_udata_finalizer_mt(L);
  mt = tabV(L->top - 1);
  lua_setmetatable(L, -2);
  queued0 = la_load64_acq(&g->gc2.finreg_udata_queued);
  lua_settop(L, 0);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(ud)) == 0);
  lj_gc2_finreg_udata_queue(g, obj2gco(ud));
  assert(la_load64_acq(&g->gc2.finreg_udata_queued) == queued0 + 1u);
  assert(lj_gc2_ismarked(g, obj2gco(ud)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(env)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(mt)) == 1);
  lj_gc2_legacy_cycle_end(g);
  setudataV(L, L->top++, ud);
  lua_pushnil(L);
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
}

#if LJ_HASFFI
static int test_unlink_root_object(global_State *g, GCobj *target)
{
  GCRef *p = &g->gc.root;
  GCobj *o;
  while ((o = gcref(*p)) != NULL) {
    if (o == target) {
      setgcrefr(*p, *lj_obj_gcwref(o));
      return 1;
    }
    p = lj_obj_gcwref(o);
  }
  return 0;
}

static void test_finreg_cdata_telemetry(lua_State *L, global_State *g)
{
  uint64_t sets0 = la_load64_acq(&g->gc2.finreg_cdata_sets);
  uint64_t clears0 = la_load64_acq(&g->gc2.finreg_cdata_clears);
  uint64_t sets1, clears1, queued1, pweak1, finalizerq1, finalizerd1, mpscd1;
  uint64_t sweepqueued1, claimed1, dispatched1, rootfallback1;
  uint64_t orderq1, orderclaimed1, orderunlinked1, orderfallback1;
  uint64_t sets2, clears2, queued2, pweak2, finalizerq2, finalizerd2, mpscd2;
  uint64_t sweepqueued2, claimed2, dispatched2;
  uint64_t orderq2, orderclaimed2, orderfallback2, rootfallback2;
  uint64_t closerootfallback2, pendingorder2;
  uint64_t overflow2;
  int finalized0;
  const int bulk_n = 160;

  lua_settop(L, 0);
  rootfallback1 =
    la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks);
  closerootfallback2 =
    la_load64_acq(&g->gc2.finreg_cdata_close_root_fallbacks);
  assert(luaL_dostring(L,
    "require('ffi')\n"
    "collectgarbage('collect')\n"
    "collectgarbage('stop')\n") ==
    LUA_OK);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks) ==
	 rootfallback1);
  assert(!lj_gc_cdata_fin_pending(g));
  lj_gc_finalize_cdata(L);
  assert(la_load64_acq(&g->gc2.finreg_cdata_close_root_fallbacks) ==
	 closerootfallback2);

  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "local cd = ffi.gc(ffi.new('char[?]', 8), function() end)\n"
    "ffi.gc(cd, nil)\n"
    "return cd\n") == LUA_OK);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sets) == sets0 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_clears) == clears0 + 1u);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);

  sets1 = la_load64_acq(&g->gc2.finreg_cdata_sets);
  clears1 = la_load64_acq(&g->gc2.finreg_cdata_clears);
  queued1 = la_load64_acq(&g->gc2.finreg_cdata_queued);
  sweepqueued1 = la_load64_acq(&g->gc2.finreg_cdata_sweep_queued);
  pweak1 = la_load64_acq(&g->gc2.finreg_cdata_pweak_queued);
  finalizerq1 = la_load64_acq(&g->gc2.finalizer_queued);
  finalizerd1 = la_load64_acq(&g->gc2.finalizer_dequeued);
  mpscd1 = la_load64_acq(&g->gc2.finalizer_mpsc_drained);
  claimed1 = la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed);
  dispatched1 = la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched);
  rootfallback1 =
    la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks);
  finalized0 = gc2_cdata_finalized;
  lua_settop(L, 0);
  lua_pushcfunction(L, gc2_cdata_counting_finalizer);
  lua_setglobal(L, "gc2_cdata_counting_finalizer");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "do\n"
    "  local cd = ffi.gc(ffi.new('char[?]', 8), gc2_cdata_counting_finalizer)\n"
    "end\n") ==
    LUA_OK);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sets) == sets1 + 1u);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(la_load64_acq(&g->gc2.finreg_cdata_queued) == queued1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sweep_queued) ==
	 sweepqueued1);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_queued) == pweak1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed) == claimed1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks) ==
	 rootfallback1);
  assert(la_load64_acq(&g->gc2.finalizer_queued) == finalizerq1 + 1u);
  assert(la_load64_acq(&g->gc2.finalizer_dequeued) == finalizerd1 + 1u);
  assert(la_load64_acq(&g->gc2.finalizer_mpsc_drained) == mpscd1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched) ==
	 dispatched1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_clears) == clears1 + 1u);
  assert(gc2_cdata_finalized == finalized0 + 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(la_load64_acq(&g->gc2.finreg_cdata_queued) == queued1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sweep_queued) ==
	 sweepqueued1);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed) == claimed1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks) ==
	 rootfallback1);
  assert(la_load64_acq(&g->gc2.finalizer_queued) == finalizerq1 + 1u);
  assert(la_load64_acq(&g->gc2.finalizer_dequeued) == finalizerd1 + 1u);
  assert(la_load64_acq(&g->gc2.finalizer_mpsc_drained) == mpscd1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched) ==
	 dispatched1 + 1u);
  assert(gc2_cdata_finalized == finalized0 + 1);

  sets1 = la_load64_acq(&g->gc2.finreg_cdata_sets);
  clears1 = la_load64_acq(&g->gc2.finreg_cdata_clears);
  queued1 = la_load64_acq(&g->gc2.finreg_cdata_queued);
  sweepqueued1 = la_load64_acq(&g->gc2.finreg_cdata_sweep_queued);
  pweak1 = la_load64_acq(&g->gc2.finreg_cdata_pweak_queued);
  finalizerq1 = la_load64_acq(&g->gc2.finalizer_queued);
  finalizerd1 = la_load64_acq(&g->gc2.finalizer_dequeued);
  mpscd1 = la_load64_acq(&g->gc2.finalizer_mpsc_drained);
  claimed1 = la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed);
  dispatched1 = la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched);
  orderq1 = la_load64_acq(&g->gc2.finreg_cdata_order_queued);
  orderclaimed1 = la_load64_acq(&g->gc2.finreg_cdata_order_claimed);
  orderunlinked1 = la_load64_acq(&g->gc2.finreg_cdata_order_unlinked);
  orderfallback1 = la_load64_acq(&g->gc2.finreg_cdata_order_fallbacks);
  rootfallback1 =
    la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks);
  finalized0 = gc2_cdata_finalized;
  lua_settop(L, 0);
  lua_pushcfunction(L, gc2_cdata_counting_finalizer);
  lua_setglobal(L, "gc2_cdata_counting_finalizer");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_rootless_order_fin_t;')\n"
    "return ffi.gc(ffi.new('gc2_rootless_order_fin_t'), gc2_cdata_counting_finalizer)\n") ==
    LUA_OK);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sets) == sets1 + 1u);
  assert(test_unlink_root_object(g, obj2gco(cdataV(L->top - 1))));
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(la_load64_acq(&g->gc2.finreg_cdata_queued) == queued1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sweep_queued) ==
	 sweepqueued1);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_queued) == pweak1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed) == claimed1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_queued) == orderq1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_claimed) ==
	 orderclaimed1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_unlinked) ==
	 orderunlinked1);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_fallbacks) ==
	 orderfallback1);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks) ==
	 rootfallback1);
  assert(la_load64_acq(&g->gc2.finalizer_queued) == finalizerq1 + 1u);
  assert(la_load64_acq(&g->gc2.finalizer_dequeued) == finalizerd1 + 1u);
  assert(la_load64_acq(&g->gc2.finalizer_mpsc_drained) == mpscd1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched) ==
	 dispatched1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_clears) == clears1 + 1u);
  assert(gc2_cdata_finalized == finalized0 + 1);

  sets1 = la_load64_acq(&g->gc2.finreg_cdata_sets);
  clears1 = la_load64_acq(&g->gc2.finreg_cdata_clears);
  queued1 = la_load64_acq(&g->gc2.finreg_cdata_queued);
  finalizerq1 = la_load64_acq(&g->gc2.finalizer_queued);
  finalizerd1 = la_load64_acq(&g->gc2.finalizer_dequeued);
  mpscd1 = la_load64_acq(&g->gc2.finalizer_mpsc_drained);
  claimed1 = la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed);
  dispatched1 = la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched);
  finalized0 = gc2_cdata_finalized;
  lua_settop(L, 0);
  lua_pushcfunction(L, gc2_cdata_counting_finalizer);
  lua_setglobal(L, "gc2_cdata_counting_finalizer");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_preclaim_clear_fin_t;')\n"
    "return ffi.gc(ffi.new('gc2_preclaim_clear_fin_t'), gc2_cdata_counting_finalizer)\n") ==
    LUA_OK);
  {
    GCcdata *cd = cdataV(L->top - 1);
    GCobj *o = obj2gco(cd);
    TValue nilv;
    lua_getglobal(L, "gc2_cdata_counting_finalizer");
    assert(tvisfunc(L->top - 1));
    assert(lj_gc2_finreg_cdata_preclaim(L, g, o, L->top - 1));
    assert(test_unlink_root_object(g, o));
    markfinalized(o);
    lj_gc2_finreg_cdata_queue(g, o);
    lj_gc2_finalizer_enqueue(g, o);
    setnilV(&nilv);
    lj_cdata_setfin(L, cd, gcval(&nilv), itype(&nilv));
  }
  assert(la_load64_acq(&g->gc2.finreg_cdata_sets) == sets1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_clears) == clears1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_queued) == queued1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed) == claimed1 + 1u);
  assert(la_load64_acq(&g->gc2.finalizer_queued) == finalizerq1 + 1u);
  lj_gc_finalize_udata(L);
  assert(la_load64_acq(&g->gc2.finalizer_dequeued) == finalizerd1 + 1u);
  assert(la_load64_acq(&g->gc2.finalizer_mpsc_drained) == mpscd1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched) ==
	 dispatched1 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_clears) == clears1 + 1u);
  assert(gc2_cdata_finalized == finalized0);
  lua_settop(L, 0);

  sets2 = la_load64_acq(&g->gc2.finreg_cdata_sets);
  clears2 = la_load64_acq(&g->gc2.finreg_cdata_clears);
  queued2 = la_load64_acq(&g->gc2.finreg_cdata_queued);
  sweepqueued2 = la_load64_acq(&g->gc2.finreg_cdata_sweep_queued);
  pweak2 = la_load64_acq(&g->gc2.finreg_cdata_pweak_queued);
  finalizerq2 = la_load64_acq(&g->gc2.finalizer_queued);
  finalizerd2 = la_load64_acq(&g->gc2.finalizer_dequeued);
  mpscd2 = la_load64_acq(&g->gc2.finalizer_mpsc_drained);
  claimed2 = la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed);
  dispatched2 = la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched);
  orderq2 = la_load64_acq(&g->gc2.finreg_cdata_order_queued);
  orderclaimed2 = la_load64_acq(&g->gc2.finreg_cdata_order_claimed);
  orderfallback2 = la_load64_acq(&g->gc2.finreg_cdata_order_fallbacks);
  rootfallback2 =
    la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks);
  gc2_cdata_order_count = 0;
  lua_pushcfunction(L, gc2_cdata_order_finalizer_1);
  lua_setglobal(L, "gc2_cdata_order_finalizer_1");
  lua_pushcfunction(L, gc2_cdata_order_finalizer_2);
  lua_setglobal(L, "gc2_cdata_order_finalizer_2");
  lua_pushcfunction(L, gc2_cdata_order_finalizer_3);
  lua_setglobal(L, "gc2_cdata_order_finalizer_3");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_order_fin_t;')\n"
    "do\n"
    "  local a = ffi.gc(ffi.new('gc2_order_fin_t'), gc2_cdata_order_finalizer_1)\n"
    "  local b = ffi.gc(ffi.new('gc2_order_fin_t'), gc2_cdata_order_finalizer_2)\n"
    "  local c = ffi.gc(ffi.new('gc2_order_fin_t'), gc2_cdata_order_finalizer_3)\n"
    "end\n") ==
    LUA_OK);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sets) == sets2 + 3u);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(la_load64_acq(&g->gc2.finreg_cdata_queued) == queued2 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sweep_queued) ==
	 sweepqueued2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_queued) == pweak2 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed) == claimed2 + 3u);
  assert(la_load64_acq(&g->gc2.finalizer_queued) == finalizerq2 + 3u);
  assert(la_load64_acq(&g->gc2.finalizer_dequeued) == finalizerd2 + 3u);
  assert(la_load64_acq(&g->gc2.finalizer_mpsc_drained) == mpscd2 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched) ==
	 dispatched2 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_queued) == orderq2 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_claimed) ==
	 orderclaimed2 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_fallbacks) ==
	 orderfallback2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks) ==
	 rootfallback2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_clears) == clears2 + 3u);
  assert(gc2_cdata_order_count == 3);
  assert(gc2_cdata_order[0] == 3);
  assert(gc2_cdata_order[1] == 2);
  assert(gc2_cdata_order[2] == 1);

  sets2 = la_load64_acq(&g->gc2.finreg_cdata_sets);
  clears2 = la_load64_acq(&g->gc2.finreg_cdata_clears);
  queued2 = la_load64_acq(&g->gc2.finreg_cdata_queued);
  sweepqueued2 = la_load64_acq(&g->gc2.finreg_cdata_sweep_queued);
  pweak2 = la_load64_acq(&g->gc2.finreg_cdata_pweak_queued);
  finalizerq2 = la_load64_acq(&g->gc2.finalizer_queued);
  finalizerd2 = la_load64_acq(&g->gc2.finalizer_dequeued);
  mpscd2 = la_load64_acq(&g->gc2.finalizer_mpsc_drained);
  claimed2 = la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed);
  dispatched2 = la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched);
  orderq2 = la_load64_acq(&g->gc2.finreg_cdata_order_queued);
  orderclaimed2 = la_load64_acq(&g->gc2.finreg_cdata_order_claimed);
  orderfallback2 = la_load64_acq(&g->gc2.finreg_cdata_order_fallbacks);
  rootfallback2 =
    la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks);
  gc2_cdata_order_count = 0;
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_regorder_fin_t;')\n"
    "do\n"
    "  local a = ffi.new('gc2_regorder_fin_t')\n"
    "  local b = ffi.new('gc2_regorder_fin_t')\n"
    "  local c = ffi.new('gc2_regorder_fin_t')\n"
    "  ffi.gc(b, gc2_cdata_order_finalizer_2)\n"
    "  ffi.gc(a, gc2_cdata_order_finalizer_1)\n"
    "  ffi.gc(c, gc2_cdata_order_finalizer_3)\n"
    "end\n") ==
    LUA_OK);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sets) == sets2 + 3u);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(la_load64_acq(&g->gc2.finreg_cdata_queued) == queued2 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sweep_queued) ==
	 sweepqueued2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_queued) == pweak2 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed) == claimed2 + 3u);
  assert(la_load64_acq(&g->gc2.finalizer_queued) == finalizerq2 + 3u);
  assert(la_load64_acq(&g->gc2.finalizer_dequeued) == finalizerd2 + 3u);
  assert(la_load64_acq(&g->gc2.finalizer_mpsc_drained) == mpscd2 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched) ==
	 dispatched2 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_queued) == orderq2 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_claimed) ==
	 orderclaimed2 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_fallbacks) ==
	 orderfallback2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks) ==
	 rootfallback2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_clears) == clears2 + 3u);
  assert(gc2_cdata_order_count == 3);
  assert(gc2_cdata_order[0] == 3);
  assert(gc2_cdata_order[1] == 1);
  assert(gc2_cdata_order[2] == 2);

  sets2 = la_load64_acq(&g->gc2.finreg_cdata_sets);
  clears2 = la_load64_acq(&g->gc2.finreg_cdata_clears);
  queued2 = la_load64_acq(&g->gc2.finreg_cdata_queued);
  sweepqueued2 = la_load64_acq(&g->gc2.finreg_cdata_sweep_queued);
  pweak2 = la_load64_acq(&g->gc2.finreg_cdata_pweak_queued);
  finalizerq2 = la_load64_acq(&g->gc2.finalizer_queued);
  finalizerd2 = la_load64_acq(&g->gc2.finalizer_dequeued);
  mpscd2 = la_load64_acq(&g->gc2.finalizer_mpsc_drained);
  claimed2 = la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed);
  dispatched2 = la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched);
  orderq2 = la_load64_acq(&g->gc2.finreg_cdata_order_queued);
  orderclaimed2 = la_load64_acq(&g->gc2.finreg_cdata_order_claimed);
  orderfallback2 = la_load64_acq(&g->gc2.finreg_cdata_order_fallbacks);
  rootfallback2 =
    la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks);
  gc2_cdata_order_count = 0;
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_rereg_fin_t;')\n"
    "do\n"
    "  local a = ffi.gc(ffi.new('gc2_rereg_fin_t'), gc2_cdata_order_finalizer_1)\n"
    "  local b = ffi.gc(ffi.new('gc2_rereg_fin_t'), gc2_cdata_order_finalizer_2)\n"
    "  ffi.gc(a, gc2_cdata_order_finalizer_3)\n"
    "end\n") ==
    LUA_OK);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sets) == sets2 + 3u);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(la_load64_acq(&g->gc2.finreg_cdata_queued) == queued2 + 2u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sweep_queued) ==
	 sweepqueued2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_queued) == pweak2 + 2u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed) == claimed2 + 2u);
  assert(la_load64_acq(&g->gc2.finalizer_queued) == finalizerq2 + 2u);
  assert(la_load64_acq(&g->gc2.finalizer_dequeued) == finalizerd2 + 2u);
  assert(la_load64_acq(&g->gc2.finalizer_mpsc_drained) == mpscd2 + 2u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched) ==
	 dispatched2 + 2u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_queued) == orderq2 + 2u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_claimed) ==
	 orderclaimed2 + 2u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_fallbacks) ==
	 orderfallback2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks) ==
	 rootfallback2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_clears) == clears2 + 2u);
  assert(gc2_cdata_order_count == 2);
  assert(gc2_cdata_order[0] == 3);
  assert(gc2_cdata_order[1] == 2);

  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(la_load64_acq(&g->gc2.finreg_cdata_queued) == queued2 + 2u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sweep_queued) ==
	 sweepqueued2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed) == claimed2 + 2u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks) ==
	 rootfallback2);
  assert(la_load64_acq(&g->gc2.finalizer_queued) == finalizerq2 + 2u);
  assert(la_load64_acq(&g->gc2.finalizer_dequeued) == finalizerd2 + 2u);
  assert(la_load64_acq(&g->gc2.finalizer_mpsc_drained) == mpscd2 + 2u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched) ==
	 dispatched2 + 2u);
  assert(gc2_cdata_order_count == 2);

  sets2 = la_load64_acq(&g->gc2.finreg_cdata_sets);
  clears2 = la_load64_acq(&g->gc2.finreg_cdata_clears);
  sweepqueued2 = la_load64_acq(&g->gc2.finreg_cdata_sweep_queued);
  pweak2 = la_load64_acq(&g->gc2.finreg_cdata_pweak_queued);
  rootfallback2 =
    la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks);
  closerootfallback2 =
    la_load64_acq(&g->gc2.finreg_cdata_close_root_fallbacks);
  finalizerq2 = la_load64_acq(&g->gc2.finalizer_queued);
  finalizerd2 = la_load64_acq(&g->gc2.finalizer_dequeued);
  mpscd2 = la_load64_acq(&g->gc2.finalizer_mpsc_drained);
  orderq2 = la_load64_acq(&g->gc2.finreg_cdata_order_queued);
  orderfallback2 = la_load64_acq(&g->gc2.finreg_cdata_order_fallbacks);
  pendingorder2 = la_load64_acq(&g->gc2.finreg_cdata_pending_order_hits);
  gc2_cdata_order_count = 0;
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_close_order_fin_t;')\n"
    "gc2_close_keep = {}\n"
    "gc2_close_keep[1] = ffi.gc(ffi.new('gc2_close_order_fin_t'), gc2_cdata_order_finalizer_1)\n"
    "gc2_close_keep[2] = ffi.gc(ffi.new('gc2_close_order_fin_t'), gc2_cdata_order_finalizer_2)\n"
    "gc2_close_keep[3] = ffi.gc(ffi.new('gc2_close_order_fin_t'), gc2_cdata_order_finalizer_3)\n") ==
    LUA_OK);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sets) == sets2 + 3u);
  assert(lj_gc_cdata_fin_pending(g));
  assert(la_load64_acq(&g->gc2.finreg_cdata_pending_order_hits) ==
	 pendingorder2 + 1u);
  lj_gc_finalize_cdata(L);
  assert(lj_gc2_finalizer_queue_pending(g));
  assert(!lj_gc_cdata_fin_pending(g));
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_queued) == orderq2 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_fallbacks) ==
	 orderfallback2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sweep_queued) ==
	 sweepqueued2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_queued) == pweak2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks) ==
	 rootfallback2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_close_root_fallbacks) ==
	 closerootfallback2);
  assert(la_load64_acq(&g->gc2.finalizer_queued) == finalizerq2 + 3u);
  lj_gc_finalize_udata(L);
  assert(la_load64_acq(&g->gc2.finalizer_dequeued) == finalizerd2 + 3u);
  assert(la_load64_acq(&g->gc2.finalizer_mpsc_drained) == mpscd2 + 3u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_clears) == clears2 + 3u);
  assert(!lj_gc2_finalizer_queue_pending(g));
  assert(!lj_gc_cdata_fin_pending(g));
  assert(gc2_cdata_order_count == 3);
  assert(gc2_cdata_order[0] == 3);
  assert(gc2_cdata_order[1] == 2);
  assert(gc2_cdata_order[2] == 1);
  lua_pushnil(L);
  lua_setglobal(L, "gc2_close_keep");

  sets2 = la_load64_acq(&g->gc2.finreg_cdata_sets);
  clears2 = la_load64_acq(&g->gc2.finreg_cdata_clears);
  sweepqueued2 = la_load64_acq(&g->gc2.finreg_cdata_sweep_queued);
  pweak2 = la_load64_acq(&g->gc2.finreg_cdata_pweak_queued);
  rootfallback2 =
    la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks);
  closerootfallback2 =
    la_load64_acq(&g->gc2.finreg_cdata_close_root_fallbacks);
  finalizerq2 = la_load64_acq(&g->gc2.finalizer_queued);
  finalizerd2 = la_load64_acq(&g->gc2.finalizer_dequeued);
  mpscd2 = la_load64_acq(&g->gc2.finalizer_mpsc_drained);
  orderq2 = la_load64_acq(&g->gc2.finreg_cdata_order_queued);
  orderfallback2 = la_load64_acq(&g->gc2.finreg_cdata_order_fallbacks);
  pendingorder2 = la_load64_acq(&g->gc2.finreg_cdata_pending_order_hits);
  gc2_cdata_order_count = 0;
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_close_rootless_order_fin_t;')\n"
    "return ffi.gc(ffi.new('gc2_close_rootless_order_fin_t'), gc2_cdata_order_finalizer_1)\n") ==
    LUA_OK);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sets) == sets2 + 1u);
  assert(test_unlink_root_object(g, obj2gco(cdataV(L->top - 1))));
  assert(lj_gc_cdata_fin_pending(g));
  assert(la_load64_acq(&g->gc2.finreg_cdata_pending_order_hits) ==
	 pendingorder2 + 1u);
  lua_pop(L, 1);
  lj_gc_finalize_cdata(L);
  assert(lj_gc2_finalizer_queue_pending(g));
  assert(!lj_gc_cdata_fin_pending(g));
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_queued) == orderq2 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_fallbacks) ==
	 orderfallback2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sweep_queued) ==
	 sweepqueued2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_queued) == pweak2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks) ==
	 rootfallback2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_close_root_fallbacks) ==
	 closerootfallback2);
  assert(la_load64_acq(&g->gc2.finalizer_queued) == finalizerq2 + 1u);
  lj_gc_finalize_udata(L);
  assert(la_load64_acq(&g->gc2.finalizer_dequeued) == finalizerd2 + 1u);
  assert(la_load64_acq(&g->gc2.finalizer_mpsc_drained) == mpscd2 + 1u);
  assert(la_load64_acq(&g->gc2.finreg_cdata_clears) == clears2 + 1u);
  assert(!lj_gc2_finalizer_queue_pending(g));
  assert(!lj_gc_cdata_fin_pending(g));
  assert(gc2_cdata_order_count == 1);
  assert(gc2_cdata_order[0] == 1);

  sets2 = la_load64_acq(&g->gc2.finreg_cdata_sets);
  clears2 = la_load64_acq(&g->gc2.finreg_cdata_clears);
  queued2 = la_load64_acq(&g->gc2.finreg_cdata_queued);
  sweepqueued2 = la_load64_acq(&g->gc2.finreg_cdata_sweep_queued);
  pweak2 = la_load64_acq(&g->gc2.finreg_cdata_pweak_queued);
  finalizerq2 = la_load64_acq(&g->gc2.finalizer_queued);
  finalizerd2 = la_load64_acq(&g->gc2.finalizer_dequeued);
  mpscd2 = la_load64_acq(&g->gc2.finalizer_mpsc_drained);
  claimed2 = la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed);
  dispatched2 = la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched);
  orderq2 = la_load64_acq(&g->gc2.finreg_cdata_order_queued);
  orderclaimed2 = la_load64_acq(&g->gc2.finreg_cdata_order_claimed);
  orderfallback2 = la_load64_acq(&g->gc2.finreg_cdata_order_fallbacks);
  rootfallback2 =
    la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks);
  overflow2 = la_load64_acq(&g->gc2.finreg_cdata_preclaim_overflow);
  gc2_cdata_bulk_finalized = 0;
  lua_pushcfunction(L, gc2_cdata_bulk_finalizer);
  lua_setglobal(L, "gc2_cdata_bulk_finalizer");
  lua_pushinteger(L, bulk_n);
  lua_setglobal(L, "gc2_cdata_bulk_n");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_bulk_fin_t;')\n"
    "do\n"
    "  for i = 1, gc2_cdata_bulk_n do\n"
    "    ffi.gc(ffi.new('gc2_bulk_fin_t'), gc2_cdata_bulk_finalizer)\n"
    "  end\n"
    "end\n") ==
    LUA_OK);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sets) ==
	 sets2 + (uint64_t)bulk_n);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(la_load64_acq(&g->gc2.finreg_cdata_queued) ==
	 queued2 + (uint64_t)bulk_n);
  assert(la_load64_acq(&g->gc2.finreg_cdata_sweep_queued) ==
	 sweepqueued2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_queued) ==
	 pweak2 + (uint64_t)bulk_n);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_claimed) ==
	 claimed2 + (uint64_t)bulk_n);
  assert(la_load64_acq(&g->gc2.finreg_cdata_preclaim_overflow) ==
	 overflow2);
  assert(g->gc2.finreg_cdata_preclaim_capacity >= (MSize)bulk_n);
  assert(la_load64_acq(&g->gc2.finalizer_queued) ==
	 finalizerq2 + (uint64_t)bulk_n);
  assert(la_load64_acq(&g->gc2.finalizer_dequeued) ==
	 finalizerd2 + (uint64_t)bulk_n);
  assert(la_load64_acq(&g->gc2.finalizer_mpsc_drained) ==
	 mpscd2 + (uint64_t)bulk_n);
  assert(la_load64_acq(&g->gc2.finreg_cdata_preclaim_dispatched) ==
	 dispatched2 + (uint64_t)bulk_n);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_queued) ==
	 orderq2 + (uint64_t)bulk_n);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_claimed) ==
	 orderclaimed2 + (uint64_t)bulk_n);
  assert(la_load64_acq(&g->gc2.finreg_cdata_order_fallbacks) ==
	 orderfallback2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_pweak_root_fallbacks) ==
	 rootfallback2);
  assert(la_load64_acq(&g->gc2.finreg_cdata_clears) ==
	 clears2 + (uint64_t)bulk_n);
  assert(gc2_cdata_bulk_finalized == bulk_n);

  lua_pushnil(L);
  lua_setglobal(L, "gc2_cdata_order_finalizer_1");
  lua_pushnil(L);
  lua_setglobal(L, "gc2_cdata_order_finalizer_2");
  lua_pushnil(L);
  lua_setglobal(L, "gc2_cdata_order_finalizer_3");
  lua_pushnil(L);
  lua_setglobal(L, "gc2_cdata_bulk_finalizer");
  lua_pushnil(L);
  lua_setglobal(L, "gc2_cdata_bulk_n");
  lua_pushnil(L);
  lua_setglobal(L, "gc2_cdata_counting_finalizer");
}
#endif

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
#if LJ_HASJIT || LJ_HASFFI
  luaL_openlibs(L);
#endif
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);

  test_strong_table(L, g, tg);
  test_grey_deque_growth(L, g, tg);
  test_grey_deque_steal_race(L, g, tg);
  test_worker_drain(L, g, tg);
  test_worker_drain_race(L, g, tg);
  test_worker_leaf_ssb(L, g, tg);
  test_fixpoint_round(L, g, tg);
  test_c_value_barrier(L, g, tg);
  test_c_table_rescan_barrier(L, g, tg);
  test_vm_upvalue_barrier(L, g, tg);
  test_vm_table_barrier(L, g, tg);
  test_vm_meta_tset_barrier(L, g, tg);
#if LJ_HASJIT
  test_jit_table_store_helper_barrier(L, g, tg);
  test_jit_weak_table_store_helper_barrier(L, g, tg);
  test_jit_weak_array_store_helper_barrier(L, g, tg);
  test_jit_upvalue_barrier(L, g, tg);
  test_jit_current_trace_root(L, g, tg);
  test_jit_tg_executing_trace_root(L, g, tg);
#endif
#if LJ_HASPROFILE
  test_jit_profile_registry_weak_barrier();
#endif
  test_weak_tables(L, g, tg);
  test_weak_snapshot_growth(L, g, tg);
  test_worker_weak_drain(L, g, tg);
  test_weak_snapshot_ready_publication(L, g);
  test_weak_snapshot_legacy_coverage(L, g, tg);
  test_weak_complete_bridge(L, g, tg);
  test_weak_clear_marks_string_slots(L, g, tg);
  test_weak_drain_uses_captured_mode(L, g, tg);
  test_weak_pre_clear_late_write_survives_drain(L, g, tg);
  test_weak_post_clear_resurrection_write(L, g, tg);
  test_vm_weak_post_clear_existing_key_write(L, g, tg);
  test_capi_rawset_weak_write_barrier(L, g, tg);
  test_weak_key_write_barrier(L, g, tg);
  test_vm_weak_key_write_barrier(L, g, tg);
  test_peer_weak_key_write_barrier(L, g, tg);
  test_vm_weak_value_hash_key_barrier(L, g, tg);
  test_vm_weak_value_array_barrier(L, g, tg);
  test_table_insert_weak_value_array_barrier(L, g, tg);
  test_capi_weak_newindex_target_write_barrier(L, g, tg);
  test_vm_tsetm_range_barrier(L, g, tg);
  test_closure(L, g, tg);
  test_tg_thread_roots(L, g, tg);
  test_minor_root_scan(L, g, tg);
  test_thread(L, g, tg);
  test_userdata(L, g);
  test_finreg_userdata_queue_mark(L, g, tg);
  test_finreg_userdata_telemetry(L, g);
  test_finreg_internal_userdata_telemetry(L, g);
  test_finreg_userdata_inplace_finalizer_behavior(L);
  test_lib_register_weak_value_barrier();
#if LJ_HASFFI
  test_ffi_loaded_weak_value_barrier();
  test_finreg_cdata_telemetry(L, g);
  test_finalizer_spawn_deferred_state(L, g);
#endif
  test_leaf_ssb(L, g, tg);

  lua_close(L);
  printf("t-gc2-traverse OK: SSB grey traversal verified\n");
  return 0;
}
