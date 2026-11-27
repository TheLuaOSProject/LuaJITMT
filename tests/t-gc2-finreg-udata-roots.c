/* GC2 userdata FINREG records are reclaimed while other actors are live. */

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"

static unsigned finalized;

static int finalizer(lua_State *L)
{
  assert(lua_isuserdata(L, 1));
  finalized++;
  return 0;
}

static GC2FinRegUDataNode *push_registered(lua_State *L)
{
  GC2FinRegUDataNode *node;
  GCobj *o;
  lua_newuserdata(L, 1);
  o = obj2gco(udataV(L->top - 1));
  lua_newtable(L);
  lua_pushcfunction(L, finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  node = gc2_finreg_udata_head_acq(G(L));
  assert(node && gc2_finreg_udata_obj_acq(node) == o);
  return node;
}

static int allocated(global_State *g, GC2FinRegUDataNode *node)
{
  /* This fixture has no concurrent arena reclaimer. The mapping need not
  ** survive, and a set block bit can represent a deferred physical free. */
  return lj_gc2_mem_registered(g, node) &&
    lj_arena_bm_get(lj_arena_of(node)->block, lj_arena_cellof(node));
}

static void clear_metatable(lua_State *L)
{
  lua_pushnil(L);
  lua_setmetatable(L, -2);
}

static void full_cycle(lua_State *L)
{
  lua_gc(L, LUA_GCRESTART, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
}

static void active_and_leased_retirement(lua_State *L)
{
  global_State *g = G(L);
  GC2FinRegUDataNode *node = push_registered(L);
  LJGC2Lease lease;
  int i;
  for (i = 0; i < 4; i++) {
    full_cycle(L);
    assert(gc2_finreg_udata_head_acq(g) == node && allocated(g, node));
    assert(finalized == 0);
  }

  assert(lj_gc2_small_lease_try(g, node, sizeof(*node), 0, &lease, NULL));
  clear_metatable(L);
  assert(gc2_finreg_udata_head_acq(g) == NULL);
  assert(gc2_finreg_udata_retired_acq(g) == NULL);
  assert(allocated(g, node));
  assert(gc2_finreg_udata_obj_acq(node) == NULL);
  assert(!gc2_finreg_udata_active_acq(node));
  lj_gc2_lease_release(&lease);
  for (i = 0; i < 4 && allocated(g, node); i++)
    full_cycle(L);
  assert(!allocated(g, node));
  lua_pop(L, 1);
}

typedef struct ForgetRace {
  TGState tg;
  GCobj *target;
} ForgetRace;

static void *forget_worker(void *ud)
{
  ForgetRace *race = (ForgetRace *)ud;
  TGState *tg = &race->tg;
  lj_thr_set_tg(tg);
  lj_native_enter(tg);
  lj_tg_attach(tg->gl, tg);
  lj_gc2_finreg_udata_forget(tg->gl, race->target);
  (void)lj_native_leave_tg(tg);
  lj_tg_detach(tg->gl, tg);
  return NULL;
}

static void paused_retirement(lua_State *L, uint32_t stage)
{
  global_State *g = G(L);
  ForgetRace race;
  pthread_t worker;
  GC2FinRegUDataNode *target = push_registered(L);
  GC2FinRegUDataNode *prev = push_registered(L);
  GC2FinRegUDataNode *later;
  uint64_t retired = gc2_finreg_udata_retired_nodes_acq(g);
  unsigned i;
  race.target = gc2_finreg_udata_obj_acq(target);
  lj_tg_init_thread(g, &race.tg, NULL, 0);
  lj_tg_tid_rel(&race.tg, lj_thr_newid());

  lj_gc2_test_finreg_udata_pause(target, stage);
  assert(pthread_create(&worker, NULL, forget_worker, &race) == 0);
  while (!lj_gc2_test_finreg_udata_paused())
    sched_yield();
  /* Before an interior splice, this also freezes and unlinks the worker's
  ** predecessor. The old CAS must fail even though its lease pins bytes. */
  clear_metatable(L);
  assert(gc2_finreg_udata_head_acq(g) == NULL);
  assert(gc2_finreg_udata_retired_acq(g) == NULL);
  assert(gc2_finreg_udata_retired_nodes_acq(g) == retired + 2);
  assert(allocated(g, target));
  assert(gc2_smr_readers_acq(g) == 0);

  /* The paused actor holds no phase token or detached batch. A new record
  ** goes through the ordinary public registration and retirement path. */
  later = push_registered(L);
  clear_metatable(L);
  lua_pop(L, 1);
  if (stage == LJ_GC2_FINREG_TEST_POPPED)
    assert(!allocated(g, later));
  assert(gc2_finreg_udata_retired_acq(g) == NULL);
  assert(gc2_finreg_udata_retired_nodes_acq(g) == retired + 3);

  lj_gc2_test_finreg_udata_release();
  assert(pthread_join(worker, NULL) == 0);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(lj_tg_fini_thread(g, &race.tg));
  (void)lj_gc2_worker_drain(g, 1);
  assert(gc2_finreg_udata_retired_acq(g) == NULL);
  assert(gc2_finreg_udata_retired_nodes_acq(g) == retired + 3);
  /* Drop two rooted objects whose records have already been removed. */
  lua_pop(L, 2);
  for (i = 0; i < 4 && (allocated(g, target) || allocated(g, prev)); i++)
    full_cycle(L);
  assert(!allocated(g, target) && !allocated(g, prev));
  assert(finalized == 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  uint32_t stage;
  assert(L != NULL);
  g = G(L);
  lua_gc(L, LUA_GCSTOP, 0);
  active_and_leased_retirement(L);
  for (stage = LJ_GC2_FINREG_TEST_FROZEN;
       stage <= LJ_GC2_FINREG_TEST_POPPED; stage++)
    paused_retirement(L, stage);

  (void)push_registered(L);
  lua_pop(L, 1);
  full_cycle(L);
  assert(finalized == 1);
  assert(gc2_finreg_udata_head_acq(g) == NULL);
  lua_close(L);
  assert(finalized == 1);
  puts("t-gc2-finreg-udata-roots OK: active, leased, and paused records verified");
  return 0;
}
