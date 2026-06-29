/*
** Active-thread lua_gc() GC2 assistance fixture.
*/

#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tg.h"

enum {
  ACTIVE_GRAPH_CHILDREN = LJ_GC2_WORKER_DRAIN_BATCH * 5
};

static void enter_synthetic_active_peer(global_State *g)
{
  GCSize threshold = lj_gc_threshold_load(g);
  assert(mt_live_acq(g) == 0);
  if (mt_live_add_rlx(g, 1) == 0) {
    lj_gc_mt_threshold_store(g, threshold);
    lj_gc_threshold_store(g, LJ_MAX_MEM);
  }
  assert(mt_live_acq(g) == 1);
}

static void leave_synthetic_active_peer(global_State *g)
{
  assert(mt_live_sub_acqrel(g, 1) == 1);
  lj_gc_threshold_store(g, lj_gc_mt_threshold_load(g));
  lj_gc2_finalizer_spawn_release(g);
  mt_live_futex_wake(g, INT_MAX);
  assert(mt_live_acq(g) == 0);
}

static void seed_table_graph(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent;
  int i;

  lua_settop(L, 0);
  lua_createtable(L, ACTIVE_GRAPH_CHILDREN, 0);
  parent = tabV(L->top - 1);
  for (i = 0; i < ACTIVE_GRAPH_CHILDREN; i++) {
    lua_newtable(L);
    lua_rawseti(L, -2, i + 1);
  }

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
}

static void drain_mark_work(global_State *g)
{
  int i;
  for (i = 0; i < 1000; i++)
    if (lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH) == 0)
      return;
  assert(0);
}

static void reset_gc2(lua_State *L, global_State *g)
{
  drain_mark_work(g);
  assert(lj_gc2_test_ssb_empty(g));
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

static void test_active_collect_completes_active_cycle(lua_State *L,
						       global_State *g,
						       TGState *tg)
{
  uint64_t runs0, grey0;

  seed_table_graph(L, g, tg);
  runs0 = gc2_worker_runs_acq(g);
  grey0 = gc2_worker_grey_drained_acq(g);

  enter_synthetic_active_peer(g);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  leave_synthetic_active_peer(g);

  assert(gc2_worker_runs_acq(g) > runs0);
  assert(gc2_worker_grey_drained_acq(g) >
	 grey0 + LJ_GC2_WORKER_DRAIN_BATCH);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(lj_gc2_test_ssb_empty(g));

  lua_settop(L, 0);
}

static void test_active_collect_completes_idle_cycle(lua_State *L,
						     global_State *g)
{
  uint64_t starts0 = gc2_cycle_starts_acq(g);

  lua_gc(L, LUA_GCRESTART, -1);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);

  enter_synthetic_active_peer(g);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  leave_synthetic_active_peer(g);

  assert(gc2_cycle_starts_acq(g) == starts0 + 1u);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);

  lua_settop(L, 0);
}

static void test_active_collect_restores_stopped_cycle(lua_State *L,
						       global_State *g)
{
  uint64_t starts0 = gc2_cycle_starts_acq(g);

  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);

  enter_synthetic_active_peer(g);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  leave_synthetic_active_peer(g);

  assert(gc2_cycle_starts_acq(g) == starts0 + 1u);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(lua_gc(L, LUA_GCISRUNNING, 0) == 0);

  lua_gc(L, LUA_GCRESTART, -1);
}

static void test_active_step_returns_false(lua_State *L, global_State *g,
					   TGState *tg)
{
  uint64_t runs0;

  seed_table_graph(L, g, tg);
  runs0 = gc2_worker_runs_acq(g);

  enter_synthetic_active_peer(g);
  assert(lua_gc(L, LUA_GCSTEP, 0) == 0);
  leave_synthetic_active_peer(g);

  assert(gc2_worker_runs_acq(g) == runs0 + 1u);
  assert(lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH) != 0);

  reset_gc2(L, g);
}

static void test_active_step_starts_stopped_cycle(lua_State *L,
						  global_State *g)
{
  uint64_t starts0 = gc2_cycle_starts_acq(g);

  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);

  enter_synthetic_active_peer(g);
  assert(lua_gc(L, LUA_GCSTEP, 0) == 0);
  leave_synthetic_active_peer(g);

  assert(gc2_cycle_starts_acq(g) == starts0 + 1u);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);

  reset_gc2(L, g);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;

  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);

  test_active_collect_completes_active_cycle(L, g, tg);
  test_active_collect_completes_idle_cycle(L, g);
  test_active_collect_restores_stopped_cycle(L, g);
  test_active_step_returns_false(L, g, tg);
  test_active_step_starts_stopped_cycle(L, g);

  lua_close(L);
  printf("t-gc-active-collect-assist OK: active lua_gc completes GC2 collect\n");
  return 0;
}
