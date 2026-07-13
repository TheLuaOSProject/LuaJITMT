/*
** Focused runtime regression for the veto-only typed GC2 activation mirror.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_thr.h"

static TabNodeRetire *find_retired_node(global_State *g, Node *node)
{
  TabNodeRetire *ret;
  for (ret = lj_tab_node_retired_head_acq(g);
       ret != NULL;
       ret = lj_tab_node_retired_next_acq(ret))
    if (lj_tab_node_retired_node_acq(ret) == node)
      return ret;
  return NULL;
}

static TabNodeRetire *make_reclaimable_retired_node(lua_State *L,
                                                     Node **oldnodep)
{
  global_State *g = G(L);
  GCtab *t;
  Node *oldnode;
  TabNodeRetire *ret;
  int i;
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  lua_createtable(L, 0, 8);
  t = tabV(L->top - 1);
  for (i = 1; i <= 6; i++) {
    lua_pushinteger(L, i);
    lua_pushinteger(L, i * 10);
    lua_rawset(L, -3);
  }
  oldnode = lj_tab_node_acq(t);
  assert(oldnode != NULL && t->hmask != 0);
  lj_tab_resize(L, t, 0, lj_fls(t->hmask) + 2u);
  assert(lj_tab_node_acq(t) != oldnode);
  ret = find_retired_node(g, oldnode);
  assert(ret != NULL);
  assert(lj_tab_node_retired_armed_acq(ret));
  assert(lj_tab_node_retired_epoch_acq(ret) != 0);
  *oldnodep = oldnode;
  return ret;
}

static void inject_sticky_mirror_mismatch(global_State *g)
{
  LJGC2ActivationSnap idle, ahead, pinned;
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  idle = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(idle.state == LJ_GC2_ACT_IDLE);
  assert(idle.gate == LJ_GC2_ROOT_GATE_OPEN);
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &idle,
           idle.mark_epoch == UINT64_MAX ? UINT64_MAX : idle.mark_epoch + 1u,
           LJ_GC2_ACT_MARK, &ahead) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_reclaim_veto(g));

  /* The real IDLE->MARK mirror expects typed IDLE. Its exact source mismatch
  ** must fail closed without preventing the legacy collector from advancing. */
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  pinned = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(pinned.state == LJ_GC2_ACT_NO_RECLAIM);
  assert(pinned.gate == LJ_GC2_ROOT_GATE_OPEN);
  lj_gc2_preserve_abort_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  pinned = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(pinned.state == LJ_GC2_ACT_NO_RECLAIM);
}

static void inject_explicit_no_reclaim(global_State *g)
{
  LJGC2ActivationSnap idle, pinned;
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  idle = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &idle,
           idle.mark_epoch, LJ_GC2_ACT_NO_RECLAIM,
           &pinned) == LJ_GC2_TRANSITION_OK);
  assert(pinned.state == LJ_GC2_ACT_NO_RECLAIM);
  assert(pinned.gate == LJ_GC2_ROOT_GATE_OPEN);
}

static LJGC2ActivationSnap activation_to_mark(global_State *g)
{
  LJGC2ActivationSnap idle, mark;
  uint64_t epoch;
  idle = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(idle.state == LJ_GC2_ACT_IDLE);
  assert(idle.gate == LJ_GC2_ROOT_GATE_OPEN);
  epoch = idle.mark_epoch == UINT64_MAX ? UINT64_MAX : idle.mark_epoch + 1u;
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &idle, epoch,
           LJ_GC2_ACT_MARK, &mark) == LJ_GC2_TRANSITION_OK);
  return mark;
}

static LJGC2ActivationSnap activation_mark_to_weak(
  global_State *g, const LJGC2ActivationSnap *mark)
{
  LJGC2ActivationSnap weak;
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, mark,
           mark->mark_epoch, LJ_GC2_ACT_WEAK, &weak) ==
         LJ_GC2_TRANSITION_OK);
  return weak;
}

static LJGC2ActivationSnap activation_weak_to_sweep(
  global_State *g, const LJGC2ActivationSnap *weak)
{
  LJGC2ActivationSnap sweep;
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, weak,
           weak->mark_epoch, LJ_GC2_ACT_SWEEP_OPEN, &sweep) ==
         LJ_GC2_TRANSITION_OK);
  return sweep;
}

static void test_phase_gate_does_not_overwrite_request(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  LJGC2ActivationSnap mark, after;
  uint32_t request, expect;

  assert(L != NULL);
  g = G(L);
  request = lj_tg_tid_acq(G2TG(g));
  assert(request != 0 && request != LJ_THREAD_GCSCAN);
  mark = activation_to_mark(g);
  gc2_phase_rel(g, LJ_GC2_MARK);
  expect = 0;
  assert(gc2_cycle_leader_cas(g, &expect, request));

  /* Legal MARK-start window: legacy MARK is visible before its request token
  ** is consumed. A closer must lose exact 0->GCSCAN and mutate nothing. */
  lj_gc2_preserve_abort_to_idle(g);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(&after, &mark));
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(gc2_cycle_leader_acq(g) == request);

  expect = request;
  assert(gc2_cycle_leader_cas(g, &expect, 0));
  lj_gc2_preserve_abort_to_idle(g);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_cycle_leader_acq(g) == 0);
  assert(after.state == LJ_GC2_ACT_IDLE);
  assert(after.gate == LJ_GC2_ROOT_GATE_OPEN);
  lua_close(L);
}

static void test_close_defers_active_worker(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  LJGC2ActivationSnap mark, weak, sweep, after;

  assert(L != NULL);
  g = G(L);
  mark = activation_to_mark(g);
  gc2_phase_rel(g, LJ_GC2_MARK);
  gc2_worker_active_rel(g, 1);

  /* Neither opportunistic preserve abort nor forced close may publish IDLE
  ** under an owner which can resume mutating mark/sweep state. */
  lj_gc2_preserve_abort_to_idle(g);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(&after, &mark));
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  lj_gc2_cycle_to_idle(g);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(&after, &mark));
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(gc2_worker_active_acq(g) == 1);

  gc2_worker_active_rel(g, 0);
  lj_gc2_cycle_to_idle(g);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_cycle_leader_acq(g) == 0);
  assert(after.state == LJ_GC2_ACT_IDLE);
  lua_close(L);

  /* The same exclusion protects the pre-bridge SWEEP boundary: a paused
  ** prepare/sweep owner must not be overtaken by restore and forced IDLE. */
  L = luaL_newstate();
  assert(L != NULL);
  g = G(L);
  mark = activation_to_mark(g);
  weak = activation_mark_to_weak(g, &mark);
  sweep = activation_weak_to_sweep(g, &weak);
  gc2_phase_rel(g, LJ_GC2_SWEEP);
  gc2_jit_phase_gate_rel(g, 0);
  gc2_worker_active_rel(g, 1);
  lj_gc2_preserve_abort_to_idle(g);
  lj_gc2_cycle_to_idle(g);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(&after, &sweep));
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  assert(gc2_worker_active_acq(g) == 1);
  gc2_worker_active_rel(g, 0);
  lj_gc2_cycle_to_idle(g);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(after.state == LJ_GC2_ACT_IDLE);
  lua_close(L);
}

static void test_preserve_abort_defensive_skews(void)
{
  lua_State *L;
  global_State *g;
  LJGC2ActivationSnap mark, weak, sweep, after;

  /* MARK->WEAK actor paused after legacy CAS and before its typed mirror. */
  L = luaL_newstate();
  assert(L != NULL);
  g = G(L);
  mark = activation_to_mark(g);
  gc2_phase_rel(g, LJ_GC2_WEAK);
  lj_gc2_preserve_abort_to_idle(g);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_cycle_leader_acq(g) == 0);
  assert(after.state == LJ_GC2_ACT_IDLE);
  assert(after.gate == LJ_GC2_ROOT_GATE_OPEN);
  assert(after.generation == mark.generation + 1u);
  lua_close(L);

  /* WEAK->SWEEP actor paused after typed staging and before legacy CAS. */
  L = luaL_newstate();
  assert(L != NULL);
  g = G(L);
  mark = activation_to_mark(g);
  weak = activation_mark_to_weak(g, &mark);
  sweep = activation_weak_to_sweep(g, &weak);
  gc2_phase_rel(g, LJ_GC2_WEAK);
  lj_gc2_preserve_abort_to_idle(g);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_cycle_leader_acq(g) == 0);
  assert(after.state == LJ_GC2_ACT_IDLE);
  assert(after.gate == LJ_GC2_ROOT_GATE_OPEN);
  assert(after.generation == sweep.generation + 1u);
  lua_close(L);
}

static void test_abort_missing_active_mirror_pins(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  LJGC2ActivationSnap after;

  assert(L != NULL);
  g = G(L);
  /* Exact close-gate ownership means no peer can already have reset a genuine
  ** WEAK source. Typed IDLE is therefore a missing mirror, not a benign skew. */
  gc2_phase_rel(g, LJ_GC2_WEAK);
  lj_gc2_preserve_abort_to_idle(g);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_cycle_leader_acq(g) == 0);
  assert(after.state == LJ_GC2_ACT_NO_RECLAIM);
  assert(after.gate == LJ_GC2_ROOT_GATE_OPEN);
  lua_close(L);
}

static void test_forward_abort_defer_and_orphan(void)
{
  lua_State *L;
  global_State *g;
  LJGC2ActivationSnap mark, after, reset;
  uint32_t expect;

  /* Reset is in flight: a delayed forward actor leaves the close owner's exact
  ** source untouched. */
  L = luaL_newstate();
  assert(L != NULL);
  g = G(L);
  mark = activation_to_mark(g);
  gc2_cycle_leader_rel(g, LJ_THREAD_GCSCAN);
  lj_gc2_test_activation_mirror_edge(g, LJ_GC2_MARK, LJ_GC2_WEAK);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(&after, &mark));
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &after,
           after.mark_epoch, LJ_GC2_ACT_IDLE, &reset) ==
         LJ_GC2_TRANSITION_OK);
  expect = LJ_THREAD_GCSCAN;
  assert(gc2_cycle_leader_cas(g, &expect, 0));
  lua_close(L);

  /* A completed abort is also benign even after the sentinel is released. */
  L = luaL_newstate();
  assert(L != NULL);
  g = G(L);
  lj_gc2_test_activation_mirror_edge(g, LJ_GC2_MARK, LJ_GC2_WEAK);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(after.state == LJ_GC2_ACT_IDLE);
  assert(after.gate == LJ_GC2_ROOT_GATE_OPEN);
  lua_close(L);

  /* Active typed authority beside unowned legacy IDLE is not an abort skew. */
  L = luaL_newstate();
  assert(L != NULL);
  g = G(L);
  (void)activation_to_mark(g);
  lj_gc2_test_activation_mirror_edge(g, LJ_GC2_MARK, LJ_GC2_WEAK);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(after.state == LJ_GC2_ACT_NO_RECLAIM);
  lua_close(L);

  /* Nor may a delayed MARK edge rewrite an unrelated non-IDLE legacy phase. */
  L = luaL_newstate();
  assert(L != NULL);
  g = G(L);
  (void)activation_to_mark(g);
  gc2_phase_rel(g, LJ_GC2_SWEEP);
  lj_gc2_test_activation_mirror_edge(g, LJ_GC2_MARK, LJ_GC2_WEAK);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(after.state == LJ_GC2_ACT_NO_RECLAIM);
  gc2_phase_rel(g, LJ_GC2_IDLE);
  lua_close(L);
}

static void run_mark_recheck_replacement(uint32_t replacement)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  LJGC2ActivationSnap after;
  uint32_t original, expect;

  assert(L != NULL);
  g = G(L);
  original = lj_tg_tid_acq(G2TG(g));
  assert(original != 0 && original != LJ_THREAD_GCSCAN);
  if (replacement == original)
    replacement = original == 17u ? 19u : 17u;
  gc2_cycle_leader_rel(g, original);
  lj_gc2_test_activation_mirror_edge(g, LJ_GC2_IDLE, LJ_GC2_MARK);
  gc2_cycle_leader_rel(g, replacement);  /* Deterministic admission loss. */
  assert(!lj_gc2_test_activation_mark_recheck(g, original));
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(after.state == LJ_GC2_ACT_IDLE);
  assert(after.gate == LJ_GC2_ROOT_GATE_OPEN);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_cycle_leader_acq(g) == replacement);
  expect = replacement;
  assert(gc2_cycle_leader_cas(g, &expect, 0));
  lua_close(L);
}

static void test_mark_stage_recheck(void)
{
  lua_State *L;
  global_State *g;
  LJGC2ActivationSnap mark, after, reset;
  uint32_t original, replacement, expect;

  run_mark_recheck_replacement(LJ_THREAD_GCSCAN);

  L = luaL_newstate();
  assert(L != NULL);
  original = lj_tg_tid_acq(G2TG(G(L)));
  lua_close(L);
  replacement = original ^ UINT32_C(0x40000000);
  if (replacement == 0 || replacement == LJ_THREAD_GCSCAN)
    replacement = 17u;
  run_mark_recheck_replacement(replacement);

  /* An unchanged exact request and staged word authorize legacy MARK publish. */
  L = luaL_newstate();
  assert(L != NULL);
  g = G(L);
  original = lj_tg_tid_acq(G2TG(g));
  gc2_cycle_leader_rel(g, original);
  lj_gc2_test_activation_mirror_edge(g, LJ_GC2_IDLE, LJ_GC2_MARK);
  mark = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_test_activation_mark_recheck(g, original));
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(&mark, &after));
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &mark,
           mark.mark_epoch, LJ_GC2_ACT_IDLE, &reset) ==
         LJ_GC2_TRANSITION_OK);
  expect = original;
  assert(gc2_cycle_leader_cas(g, &expect, 0));
  lua_close(L);
}

static void test_central_sweep_reclaim_activation_gate(void)
{
  lua_State *L;
  global_State *g;
  LJGC2ActivationSnap mark, weak, sweep, reset;

  /* Coherent SWEEP is admitted and owns the SMR gate. */
  L = luaL_newstate();
  assert(L != NULL);
  g = G(L);
  mark = activation_to_mark(g);
  weak = activation_mark_to_weak(g, &mark);
  sweep = activation_weak_to_sweep(g, &weak);
  gc2_phase_rel(g, LJ_GC2_SWEEP);
  gc2_jit_phase_gate_rel(g, 0);
  gc2_worker_active_rel(g, 1);
  assert(lj_gc2_test_sweep_reclaim_enter(g));
  assert(gc2_smr_reclaiming_acq(g) == 1);
  gc2_smr_reclaiming_rel(g, 0);
  gc2_worker_active_rel(g, 0);
  gc2_phase_rel(g, LJ_GC2_IDLE);
  gc2_jit_phase_gate_rel(g, 1);
  assert(lj_gc2_activation_try_abandon_sweep_open(&g->gc2.activation,
           &sweep, &reset) == LJ_GC2_TRANSITION_OK);
  lua_close(L);

  /* A legacy/typed mismatch is rejected before publishing smr_reclaiming. */
  L = luaL_newstate();
  assert(L != NULL);
  g = G(L);
  mark = activation_to_mark(g);
  gc2_phase_rel(g, LJ_GC2_SWEEP);
  gc2_jit_phase_gate_rel(g, 0);
  gc2_worker_active_rel(g, 1);
  assert(!lj_gc2_test_sweep_reclaim_enter(g));
  assert(gc2_smr_reclaiming_acq(g) == 0);
  gc2_worker_active_rel(g, 0);
  gc2_phase_rel(g, LJ_GC2_IDLE);
  gc2_jit_phase_gate_rel(g, 1);
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &mark,
           mark.mark_epoch, LJ_GC2_ACT_IDLE, &reset) ==
         LJ_GC2_TRANSITION_OK);
  lua_close(L);

  /* Sticky NO_RECLAIM is equally absorbing at the central entry point. */
  L = luaL_newstate();
  assert(L != NULL);
  g = G(L);
  inject_explicit_no_reclaim(g);
  gc2_phase_rel(g, LJ_GC2_SWEEP);
  gc2_jit_phase_gate_rel(g, 0);
  gc2_worker_active_rel(g, 1);
  assert(!lj_gc2_test_sweep_reclaim_enter(g));
  assert(gc2_smr_reclaiming_acq(g) == 0);
  gc2_worker_active_rel(g, 0);
  gc2_phase_rel(g, LJ_GC2_IDLE);
  gc2_jit_phase_gate_rel(g, 1);
  lua_close(L);
}

static void test_inflight_close_mirror_is_idempotent(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  LJGC2ActivationSnap idle, mark, weak, sweep, closed;
  uint64_t epoch;

  assert(L != NULL);
  g = G(L);
  idle = lj_gc2_activation_snapshot(&g->gc2.activation);
  epoch = idle.mark_epoch == UINT64_MAX ? UINT64_MAX : idle.mark_epoch + 1u;
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &idle, epoch,
           LJ_GC2_ACT_MARK, &mark) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &mark, epoch,
           LJ_GC2_ACT_WEAK, &weak) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &weak, epoch,
           LJ_GC2_ACT_SWEEP_OPEN, &sweep) == LJ_GC2_TRANSITION_OK);
  gc2_phase_rel(g, LJ_GC2_SWEEP);

  /* Model closer A paused immediately after its legacy xchg. Closer B sees an
  ** IDLE->IDLE no-op and must leave A's still-active typed source untouched. */
  assert(gc2_phase_xchg_acqrel(g, LJ_GC2_IDLE) == LJ_GC2_SWEEP);
  lj_gc2_test_activation_mirror_edge(g, LJ_GC2_IDLE, LJ_GC2_IDLE);
  closed = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(&closed, &sweep));
  assert(lj_gc2_activation_reclaim_veto(g));

  /* A completes its exact edge. */
  lj_gc2_test_activation_mirror_edge(g, LJ_GC2_SWEEP, LJ_GC2_IDLE);
  closed = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(closed.state == LJ_GC2_ACT_IDLE);
  assert(closed.gate == LJ_GC2_ROOT_GATE_OPEN);
  assert(closed.generation == sweep.generation + 1u);
  assert(!lj_gc2_activation_reclaim_veto(g));

  /* A close sentinel is never a new-cycle request, even though legacy phase is
  ** IDLE. It must survive until the close actor clears it. */
  gc2_cycle_leader_rel(g, LJ_THREAD_GCSCAN);
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_cycle_leader_acq(g) == LJ_THREAD_GCSCAN);
  assert(!lj_gc2_activation_reclaim_veto(g));
  gc2_cycle_leader_rel(g, 0);
  lua_close(L);
}

static void run_veto_case(int mismatch)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  TabNodeRetire *ret;
  Node *oldnode;
  GCArena *small;
  void *huge;
  LJHugeInfo hi, removed;
  size_t hugesize = LJ_HUGE_THRESHOLD + 4096u;
  uint64_t reclaim_epoch;
  uint32_t small_cursor;
  int done = -1, pending = 0;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert(lj_tg_flags_test_acq(tg, TGF_HUGETAB));

  ret = make_reclaimable_retired_node(L, &oldnode);
  reclaim_epoch = lj_tab_node_retired_epoch_acq(ret) +
                  LJ_TAB_RETIRE_EPOCHS;

  small = lj_arena_map(&tg->prng, LJ_AF_TRAVERSABLE|LJ_AF_QUARANTINE);
  assert(small != NULL);
  small->hdr.reclaim_cell = LJ_AFIRST_CELL;
  small_cursor = small->hdr.reclaim_cell;

  huge = lj_arena_huge_map(&tg->prng, hugesize, LJ_AF_TRAVERSABLE);
  assert(huge != NULL);
  assert(lj_arena_hugetab_insert(&tg->huge, huge, hugesize,
                                 LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_FREEING) == 1);
  assert(lj_arena_hugetab_lookup(&tg->huge, huge, &hi) == 1);
  assert((hi.flags & LJ_HUGEF_FREEING) != 0);

  if (mismatch)
    inject_sticky_mirror_mismatch(g);
  else
    inject_explicit_no_reclaim(g);
  assert(lj_gc2_activation_reclaim_veto(g));

  assert(lj_gc_reclaim_gc2_arena(g, small, 64u, &done) == 0);
  assert(done == 0);
  assert(small->hdr.reclaim_cell == small_cursor);

  assert(lj_gc_reclaim_gc2_huge(g, tg, huge, &hi, &pending) == 0);
  assert(pending == 1);
  assert(lj_arena_hugetab_lookup(&tg->huge, huge, NULL) == 1);

  assert(lj_gc2_reclaim_retired(g, reclaim_epoch) == 0);
  assert(find_retired_node(g, oldnode) == ret);

  assert(lj_arena_hugetab_delete(&tg->huge, huge, &removed) == 1);
  assert(removed.size == hugesize);
  lj_arena_huge_unmap(huge, removed.size);
  lj_arena_unmap(small);
  lua_close(L);  /* Terminal teardown owns the deliberately retained node. */
}

int main(void)
{
  test_phase_gate_does_not_overwrite_request();
  test_close_defers_active_worker();
  test_preserve_abort_defensive_skews();
  test_abort_missing_active_mirror_pins();
  test_forward_abort_defer_and_orphan();
  test_mark_stage_recheck();
  test_central_sweep_reclaim_activation_gate();
  test_inflight_close_mirror_is_idempotent();
  run_veto_case(1);
  run_veto_case(0);
  printf("t-gc2-activation-veto OK: exact phase gates, defensive skews, and reclaim vetoes\n");
  return 0;
}
