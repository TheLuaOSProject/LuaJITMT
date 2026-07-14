/*
** Tactical publication marks for FINREG/threading raw sidecars must lose
** benignly to an IDLE metadata writer. The independently owned raw body is
** then retained by the next mandatory list/userdata root scan.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_thr.h"
#include "lj_udata.h"

#if !defined(LJ_GC2_TEST_HELPERS)
#error "t-gc2-sidecar-publication requires LJ_GC2_TEST_HELPERS"
#endif

static void unmark_raw(void *p)
{
  GCArena *a = lj_arena_of(p);
  uint32_t cell = lj_arena_cellof(p);
  assert(!lj_arena_ishuge(a));
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  lj_arena_bm_clear(a->mark, cell);
  assert(!lj_arena_bm_get(a->mark, cell));
}

static int raw_marked(void *p)
{
  return lj_arena_bm_get(lj_arena_of(p)->mark, lj_arena_cellof(p));
}

static void assert_activation(global_State *g,
			      const LJGC2ActivationSnap *before)
{
  LJGC2ActivationSnap after =
    lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(before, &after));
  assert(gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_META_EXCLUSIVE);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(gc2_jit_phase_gate_acq(g) == 0);
  assert(lj_gc2_reclaim_context_held(g));
  assert(!lj_gc2_activation_reclaim_veto(g));
}

static void drain_mark_work(global_State *g)
{
  uint32_t i;
  (void)lj_gc2_flush_ssb(g, G2TG(g));
  for (i = 0; i < 4096u && !lj_gc2_test_ssb_empty(g); i++)
    (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  GCudata *ud;
  LJThread *th;
  TValue *roots;
  LJThreadLive *live, *retired;
  GC2FinRegUDataNode *finreg;
  LJGC2ActivationSnap before;

  assert(L != NULL);
  g = G(L);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);

  lua_newuserdata(L, sizeof(LJThread));
  ud = udataV(L->top - 1);
  th = (LJThread *)uddata(ud);
  memset(th, 0, sizeof(*th));
  lj_thread_udata_rel(th, ud);
  lj_udata_udtype_rel(ud, UDTYPE_THREAD);

  roots = lj_mem_newvec(L, 1, TValue);
  setnilV(&roots[0]);
  live = lj_mem_newt(L, sizeof(LJThreadLive), LJThreadLive);
  retired = lj_mem_newt(L, sizeof(LJThreadLive), LJThreadLive);
  finreg = lj_mem_newt(L, sizeof(GC2FinRegUDataNode),
		       GC2FinRegUDataNode);
  gc2_finreg_udata_obj_rel(finreg, obj2gco(ud));
  gc2_finreg_udata_retired_next_rel(finreg, NULL);
  gc2_finreg_udata_active_rel(finreg, 1);

  unmark_raw(roots);
  unmark_raw(live);
  unmark_raw(retired);
  unmark_raw(finreg);

  /* Model the exact ordinary IDLE retired-metadata writer collision. Local
  ** ownership covers each pre-CAS body; its published list/retirement protocol
  ** or userdata pointer covers it after the release publication. */
  assert(gc2_smr_readers_acq(g) == 0);
  assert(lj_gc2_test_idle_reclaim_enter(g));
  assert(gc2_jit_phase_gate_acq(g) == 0);
  assert(lj_gc2_reclaim_context_held(g));
  before = lj_gc2_activation_snapshot(&g->gc2.activation);

  lj_gc2_test_finreg_udata_node_publish(g, finreg);
  assert_activation(g, &before);
  assert(!raw_marked(finreg));

  lj_threading_test_start_roots_publish(L, ud, roots, 1);
  assert_activation(g, &before);
  assert(!raw_marked(roots));

  lj_threading_test_live_node_publish(L, ud, live);
  assert_activation(g, &before);
  assert(!raw_marked(live));

  lj_thread_live_next_rel(retired, NULL);
  lj_thread_live_retired_next_rel(retired, NULL);
  lj_thread_live_udata_ref_rel(retired, NULL);
  lj_threading_test_live_node_retire(g, retired);
  assert_activation(g, &before);
  assert(!raw_marked(retired));

  lj_gc2_test_idle_reclaim_leave(g);
  assert(gc2_smr_reclaiming_acq(g) == 0);
  assert(!lj_gc2_reclaim_context_held(g));

  /* The tactical misses requested retry but did not manufacture lifetime.
  ** The unchanged mandatory root paths must now mark all four published raw
  ** bodies: FINREG active list, thread userdata vector, and active/retired
  ** threading node lists. */
  lj_gc2_mark_begin(g);
  lj_gc2_test_scan_roots(g, L);
  drain_mark_work(g);
  assert(raw_marked(finreg));
  assert(raw_marked(roots));
  assert(raw_marked(live));
  assert(raw_marked(retired));
  lj_gc2_cycle_to_idle(g);

  lua_close(L);
  printf("t-gc2-sidecar-publication OK: tactical misses retained by mandatory scans\n");
  return 0;
}
