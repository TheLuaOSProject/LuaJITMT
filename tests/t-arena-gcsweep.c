/*
** Focused test for the runtime traversable arena sweep bridge.
*/

#ifndef LJ_GC2_TEST_HELPERS
#define LJ_GC2_TEST_HELPERS
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_tg.h"

static uint32_t ptr_state(void *p)
{
  GCArena *a = lj_arena_of(p);
  uint32_t cell = lj_arena_cellof(p);
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  return lj_arena_state(a, cell);
}

static int arena_list_contains(GCArena *a, GCArena *needle)
{
  while (a) {
    if (a == needle)
      return 1;
    a = lj_arena_next_acq(a);
  }
  return 0;
}

static int gc_root_list_contains(global_State *g, GCobj *needle)
{
  GCobj *o;
  uint32_t seen = 0;
  for (o = lj_gc_root_acq(g);
       o != NULL && seen++ < LJ_GC2_ROOT_SCAN_LIMIT;) {
    GCobj *next;
    if (o == needle)
      return 1;
    next = lj_obj_gcw_acq(o);
    if (next == o)
      break;
    o = next;
  }
  return 0;
}

/* Synthetic sweep fixtures bypass root/weak work intentionally. Keep their
** veto-only activation mirror coherent without pretending to close its gate. */
static LJGC2ActivationSnap test_publish_sweep_phase(global_State *g)
{
  LJGC2ActivationSnap idle, mark, weak, sweep;
  uint64_t epoch;
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  idle = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(idle.state == LJ_GC2_ACT_IDLE);
  assert(idle.gate == LJ_GC2_ROOT_GATE_OPEN);
  epoch = idle.mark_epoch == UINT64_MAX ? UINT64_MAX : idle.mark_epoch + 1u;
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &idle, epoch,
           LJ_GC2_ACT_MARK, &mark) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &mark, epoch,
           LJ_GC2_ACT_WEAK, &weak) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &weak, epoch,
           LJ_GC2_ACT_SWEEP_OPEN, &sweep) == LJ_GC2_TRANSITION_OK);
  gc2_phase_rel(g, LJ_GC2_SWEEP);
  /* These fixtures intentionally bypass the semantic root driver. Publish
  ** its prerequisite explicitly before any synthetic READY edge. */
  gc2_sweep_root_scanned_rel(g, 1);
  /* Synthetic fixtures intentionally skip string reclamation. Mirror the real
  ** WEAK->SWEEP initialization with a completed non-major string cycle so the
  ** paranoia close oracle sees a valid DONE state. */
  lj_str_gc2_sweep_begin(g, 0);
  assert(!lj_gc2_activation_reclaim_veto(g));
  return sweep;
}

static void arena_needsweep_move_head(TGAlloc *alloc, uint32_t kind,
				      GCArena *target)
{
  GCArena *head, *prev = NULL, *a;
  assert(alloc != NULL && kind < LJ_ARENA_NKINDS && target != NULL);
  head = alloc->needsweep[kind];
  if (head == target)
    return;
  for (a = head; a != NULL && a != target;) {
    GCArena *next = lj_arena_next_acq(a);
    assert(next != a);
    prev = a;
    a = next;
  }
  assert(a == target && prev != NULL);
  lj_arena_next_rel(prev, lj_arena_next_acq(target));
  lj_arena_next_rel(target, head);
  alloc->needsweep[kind] = target;
}

static int noop_finalizer(lua_State *L)
{
  (void)L;
  return 0;
}

static void test_preserve_abort_waits_for_restore_publisher(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtab *t;
  GCArena *a;
  LJGC2ActivationSnap sweep, idle;
  uint32_t cycle;
  int admission;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  t = lj_tab_new(L, 0, 0);
  assert(t != NULL);
  a = lj_arena_of(t);
  assert(!lj_arena_ishuge(a));
  assert((lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) != 0);

  cycle = ++g->gc2.cycle;
  sweep = test_publish_sweep_phase(g);
  assert(lj_arena_alloc_prepare_sweep_kind(
	&tg->alloc, LJ_ARENAK_TRAVERSABLE));
  tg->alloc.prepare_epoch = cycle;
  assert(arena_list_contains(
	tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE], a));

  /* Hold a legal counted terminal reader across the abort handshake. Its
  ** count defeats this owner's exact restore LP without blocking either side. */
  admission = lj_arena_rescue_enter(a);
  assert(admission == LJ_ARENA_RESCUE_FULL);
  lj_gc2_preserve_abort_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  idle = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(&sweep, &idle));
  assert(tg->alloc.prepare_epoch == cycle);
  assert(lj_gc2_sweep_needs_restore(g));

  lj_arena_rescue_leave(a);
  lj_gc2_preserve_abort_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  idle = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(idle.state == LJ_GC2_ACT_IDLE);
  assert(idle.gate == LJ_GC2_ROOT_GATE_OPEN);
  assert(idle.generation == sweep.generation + 1u);
  assert(idle.mark_epoch == sweep.mark_epoch);
  assert(!lj_gc2_activation_reclaim_veto(g));
  assert(tg->alloc.prepare_epoch == 0);
  assert(!lj_gc2_sweep_needs_restore(g));
  assert(arena_list_contains(
	tg->alloc.owned[LJ_ARENAK_TRAVERSABLE], a));
  lua_close(L);
}

static void test_prepare_collision_detaches_allocator(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCArena *first = NULL, *second = NULL, *fresh_a;
  void *fresh;
  uint32_t i;
  int admission;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  for (i = 0; i < 16u && !second; i++) {
    void *p = lj_arena_alloc(&tg->alloc, &tg->prng,
	LJ_HUGE_THRESHOLD, LJ_AF_TRAVERSABLE);
    GCArena *a;
    assert(p != NULL);
    a = lj_arena_of(p);
    if (!first)
      first = a;
    else if (a != first)
      second = a;
  }
  assert(first != NULL && second != NULL);

  /* A live OPEN publisher makes one arena's nonwaiting seal fail. Every old
  ** arena must nevertheless be detached from bump/bins/owned before ACK lets
  ** the mutator allocate again. */
  admission = lj_arena_rescue_enter(second);
  assert(admission == LJ_ARENA_RESCUE_FULL);
  assert(!lj_arena_alloc_prepare_sweep_kind(
	&tg->alloc, LJ_ARENAK_TRAVERSABLE));
  assert(!arena_list_contains(tg->alloc.owned[LJ_ARENAK_TRAVERSABLE],
	first));
  assert(!arena_list_contains(tg->alloc.owned[LJ_ARENAK_TRAVERSABLE],
	second));
  assert(arena_list_contains(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
	first));
  assert(arena_list_contains(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
	second));
  fresh = lj_arena_alloc(&tg->alloc, &tg->prng, 64u,
			 LJ_AF_TRAVERSABLE);
  assert(fresh != NULL);
  fresh_a = lj_arena_of(fresh);
  assert(fresh_a != first && fresh_a != second);

  lj_arena_rescue_leave(second);
  assert(lj_arena_alloc_prepare_sweep_kind(
	&tg->alloc, LJ_ARENAK_TRAVERSABLE));
  assert(arena_list_contains(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
	fresh_a));
  assert(lj_arena_alloc_restore_sweep_kind(
	&tg->alloc, LJ_ARENAK_TRAVERSABLE));
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(arena_list_contains(tg->alloc.owned[LJ_ARENAK_TRAVERSABLE],
	first));
  assert(arena_list_contains(tg->alloc.owned[LJ_ARENAK_TRAVERSABLE],
	second));
  assert(arena_list_contains(tg->alloc.owned[LJ_ARENAK_TRAVERSABLE],
	fresh_a));
  lua_close(L);
}

static void test_quarantine_late_live_after_eof(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtab *t;
  GCArena *a, *other_needsweep;
  LJGC2ActivationSnap sweep, idle;
  uint64_t hs_epoch;
  uint32_t cell, step, i;
  int done = 0;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  t = lj_tab_new(L, 0, 0);
  assert(t != NULL);
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  assert(gc_root_list_contains(g, obj2gco(t)));
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  assert(!lj_arena_ishuge(a));
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  assert((lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) != 0);

  lj_arena_alloc_prepare_sweep_kind(&tg->alloc,
				    LJ_ARENAK_TRAVERSABLE);
  assert(arena_list_contains(
	    tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE], a));
  arena_needsweep_move_head(&tg->alloc, LJ_ARENAK_TRAVERSABLE, a);
  /* Make every still-WHITE allocation in this isolated target arena an
  ** explicit retained/raw cell. The table remains linked until the deliberate
  ** late detach below, so no invalid header is manufactured for reclamation. */
  (void)lj_gc_sweep_gc2_arena_unmarked(g, a);
  assert(lj_arena_bm_get(a->mark, cell));
  assert(lj_arena_alloc_quarantine_one(&tg->alloc,
	    LJ_ARENAK_TRAVERSABLE, 1u) == a);
  /* Keep unrelated prepared arenas out of the focused owner-progress choice;
  ** restore this owner-local list before teardown. */
  other_needsweep = tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE];
  tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] = NULL;

  /* First complete a bounded pass with no actionable sidecar state. */
  while (!done) {
    step = lj_gc_reclaim_gc2_arena(g, a, 64u, &done);
    assert(step != 0 || done);
  }
  assert(a->hdr.reclaim_cell == LJ_ARENA_CELLS);
  assert(lj_arena_reclaim_deferred_acq(a) == 0);

  /* Model a sweep-time preservation/detach publication after the numeric
  ** cursor already passed this exact valid header. Before the fix, finish
  ** rejected LIVE forever while every subsequent reclaim started at EOF. */
  lj_gc_unlink_root_obj(g, obj2gco(t));
  assert(!gc_root_list_contains(g, obj2gco(t)));
  assert(lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_WHITE,
				  LJ_ARENA_SWEEP_LIVE));
  /* PREPSWEEP hands quarantine an exact CLOSED, publisher-free gate. */
  assert(lj_arena_remote_active_acq(a) == LJ_ARENA_REMOTE_CLOSED);
  g->gc2.cycle++;
  sweep = test_publish_sweep_phase(g);
  tg->alloc.prepare_epoch = g->gc2.cycle;
  gc2_sweep_bridge_ready_store_rlx(g, 0);
  gc2_sweep_root_done_rel(g, 1);
  assert(lj_gc2_handshake(g,
	LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_FLUSH_SSB) == 1);
  while (lj_gc2_test_ssb_drain(g) != 0)
    ;
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_thread_scan_needscan_pending_acq(g) == 0);
  /* This fixture manually publishes READY instead of driving the normal
  ** bridge helper. Retire the snapshot's completed mark round just as that
  ** helper does at its READY linearization point. */
  (void)gc2_marks_this_round_xchg_acqrel(g, 0);
  hs_epoch = gc2_hs_epoch_acq(g);
  assert(hs_epoch != 0);
  la_store64_rel(&a->hdr.retire_epoch, hs_epoch - 1u);
  gc2_sweep_grace_needed_rel(g, 0);
  lj_gc2_sweep_bridge_ready(g);
  assert(gc2_sweep_bridge_ready_acq(g));
  /* limit=1 makes the failed finish and exact rearm the sole reported unit.
  ** Without owner step accounting this call incorrectly declares no work. */
  assert(lj_gc2_test_sweep_owner_progress(g, tg, 1u) == 1u);
  /* CLOSED must precede the rejecting readiness scan and remain closed across
  ** retry; otherwise an ordinary producer can mutate after validation and
  ** still let the terminal close CAS succeed. */
  assert((lj_arena_remote_active_acq(a) & LJ_ARENA_REMOTE_CLOSED) != 0);
  assert(a->hdr.reclaim_cell == cell);  /* Exact actionable backedge. */

  assert(lj_gc2_test_sweep_owner_progress(g, tg, 1u) == 1u);
  assert(lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_WHITE);
  assert(gc_root_list_contains(g, obj2gco(t)));
  for (i = 0; i < 128u &&
	  tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE] == a; i++)
    assert(lj_gc2_test_sweep_owner_progress(g, tg, 1u) == 1u);
  assert(i < 128u);
  assert(lj_arena_alloc_reclaimed_head(&tg->alloc,
	    LJ_ARENAK_TRAVERSABLE) == a);
  while (lj_gc2_test_ssb_drain(g) != 0)
    ;
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_thread_scan_needscan_pending_acq(g) == 0);
  lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  idle = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(idle.state == LJ_GC2_ACT_IDLE);
  assert(idle.gate == LJ_GC2_ROOT_GATE_OPEN);
  assert(idle.generation == sweep.generation + 1u);
  assert(idle.mark_epoch == sweep.mark_epoch);
  assert(!lj_gc2_activation_reclaim_veto(g));

  /* Restore the other arenas prepared solely by this fixture. The completed
  ** target deliberately remains on the normal CLOSED reclaimed stack. */
  tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] = other_needsweep;
  lj_arena_alloc_restore_sweep_kind(&tg->alloc,
				    LJ_ARENAK_TRAVERSABLE);
  lua_close(L);
}

static void seed_traversable_needsweep(TGState *tg, uint32_t n)
{
  uint32_t i;
  for (i = 0; i < n; i++) {
    GCArena *a = lj_arena_map(&tg->prng, LJ_AF_TRAVERSABLE);
    assert(a != NULL);
    a->hdr.owner_tid = tg->alloc.owner_tid;
    a->hdr.flags |= LJ_AF_NEEDSWEEP;
    lj_arena_next_rel(a, tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE]);
    tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] = a;
  }
}

static void test_worker_owned_sweep_direct(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg, extra_tg;
  uint64_t worker_runs0, arenas0, idle0;
  uint32_t sweep_cycle, i;
  void *extra_plain, *extra_trav;
  GCArena *extra_plain_a, *extra_trav_a, *swept_a;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  lua_gc(L, LUA_GCCOLLECT, 0);

  lj_tg_init_thread(g, &extra_tg, NULL, 1);
  extra_tg.tid = tg->tid + 3000u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  lj_native_enter(&extra_tg);
  lj_tg_attach(g, &extra_tg);
  assert(g->gc2.n_threads == 2);

  extra_plain = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64, 0);
  extra_trav = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			      LJ_AF_TRAVERSABLE);
  assert(extra_plain != NULL);
  assert(extra_trav != NULL);
  extra_plain_a = lj_arena_of(extra_plain);
  extra_trav_a = lj_arena_of(extra_trav);

  g->gc2.cycle++;
  sweep_cycle = g->gc2.cycle;
  (void)test_publish_sweep_phase(g);
  gc2_sweep_bridge_ready_store_rlx(g, 0);
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_PLAIN);
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  lj_arena_alloc_restore_sweep_kind(&extra_tg.alloc, LJ_ARENAK_PLAIN);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_PLAIN],
			     extra_plain_a));
  assert(arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     extra_trav_a));
  swept_a = extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE];
  assert(swept_a != NULL);
  assert(lj_gc2_sweep_tg_ready(&extra_tg));
  assert(lj_gc2_sweep_needs_prepare(g));
  assert(lj_gc2_sweep_pending(g));

  worker_runs0 = gc2_worker_runs_acq(g);
  arenas0 = gc2_sweep_owner_arenas_acq(g);
  assert(lj_gc2_worker_drain(g, 1) == 0);
  assert(gc2_worker_runs_acq(g) == worker_runs0);
  assert(gc2_sweep_owner_arenas_acq(g) == arenas0);
  lj_gc2_sweep_bridge_ready(g);
  /* Quarantine classification, epoch grace and bitmap commit are separate
  ** bounded owner batches. Drive one unit at a time until this exact arena is
  ** complete instead of assuming the former in-place sweep fit in one call. */
  for (i = 0; i < 256u && lj_gc2_sweep_pending(g); i++)
    (void)lj_gc2_worker_drain(g, 1);
  assert(i != 0 && i < 256u);
  assert(gc2_worker_runs_acq(g) > worker_runs0);
  assert(gc2_sweep_owner_arenas_acq(g) == arenas0 + 1u);
  assert(gc2_worker_active_acq(g) == 0);
  assert(!arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			      swept_a));
  assert(arena_list_contains(lj_arena_alloc_reclaimed_head(
		&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE), swept_a));
  assert((extra_plain_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert((swept_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert((swept_a->hdr.flags & LJ_AF_RECLAIMED) != 0);
  assert(swept_a->hdr.sweep_epoch == sweep_cycle);
  assert(!lj_gc2_sweep_pending(g));
  idle0 = gc2_worker_idle_declares_acq(g);
  assert(lj_gc2_worker_drain(g, 1) == 0);
  assert(gc2_worker_idle_declares_acq(g) == idle0 + 1u);
  assert(gc2_worker_active_acq(g) == 0);

  lj_arena_alloc_restore_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  lj_gc2_cycle_to_idle(g);
  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, &extra_tg);
  lua_close(L);
}

static void test_minor_sweep_identity_direct(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg, extra_tg;
  uint64_t minor_arenas0;
  uint32_t sweep_cycle, i;
  void *dead, *live;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  lua_gc(L, LUA_GCCOLLECT, 0);

  lj_tg_init_thread(g, &extra_tg, NULL, 1);
  extra_tg.tid = tg->tid + 3500u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  lj_native_enter(&extra_tg);
  lj_tg_attach(g, &extra_tg);
  assert(g->gc2.n_threads == 2);

  extra_tg.alloc.alloc_black = 0;
  dead = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			LJ_AF_TRAVERSABLE);
  extra_tg.alloc.alloc_black = 1;
  live = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			LJ_AF_TRAVERSABLE);
  extra_tg.alloc.alloc_black = 0;
  assert(dead != NULL);
  assert(live != NULL);
  assert(lj_arena_of(dead) == lj_arena_of(live));
  assert(ptr_state(dead) == 2);
  assert(ptr_state(live) == 3);

  g->gc2.cycle++;
  sweep_cycle = g->gc2.cycle;
  (void)test_publish_sweep_phase(g);
  gc2_sweep_bridge_ready_store_rlx(g, 0);
  la_store32_rel(&g->gc2.cycle_minor_requested, 1);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 1);
  la_store32_rel(&g->gc2.cycle_sweep_minor, 1);
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE] != NULL);
  /* Opaque raw storage is intentionally retained unless its owner publishes
  ** physical destruction. Model that terminal publication explicitly; this
  ** fixture is testing minor bitmap identity, not GC-header classification. */
  assert(lj_arena_sweep_state_cas(lj_arena_of(dead),
				  lj_arena_cellof(dead),
				  LJ_ARENA_SWEEP_WHITE,
				  LJ_ARENA_SWEEP_FREEING));
  minor_arenas0 = gc2_minor_sweep_arenas_acq(g);
  assert(lj_gc2_test_sweep_owner_progress(g, &extra_tg, 1) == 0);
  assert(gc2_minor_sweep_arenas_acq(g) == minor_arenas0);
  lj_gc2_sweep_bridge_ready(g);
  for (i = 0; i < 256u &&
	      gc2_minor_sweep_arenas_acq(g) == minor_arenas0; i++)
    (void)lj_gc2_test_sweep_owner_progress(g, &extra_tg, 1);
  assert(i != 0 && i < 256u);
  assert(gc2_minor_sweep_arenas_acq(g) == minor_arenas0 + 1u);
  assert(ptr_state(dead) == 1);
  assert(ptr_state(live) == 3);
  assert(lj_arena_of(live)->hdr.sweep_epoch == sweep_cycle);

  lj_gc2_cycle_to_idle(g);
  la_store32_rel(&g->gc2.cycle_minor_requested, 0);
  la_store32_rel(&g->gc2.cycle_sweep_minor, 0);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 0);
  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, &extra_tg);
  lua_close(L);
}

static void test_boundary_lazy_sweep(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCRef empty;
  MSize oldstepmul;
  uint64_t arenas0, delta;
  uint32_t oldcycle, i;
  const uint32_t seeded = LJ_GC2_SWEEP_BATCH + 3u;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  lua_gc(L, LUA_GCCOLLECT, 0);

  oldcycle = g->gc2.cycle;
  g->gc2.cycle = oldcycle + 1u;
  (void)test_publish_sweep_phase(g);
  tg->alloc.sweep_epoch = g->gc2.cycle;  /* Simulate prepared boundary. */
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(tg->alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);

  seed_traversable_needsweep(tg, seeded);
  assert(lj_gc2_sweep_pending(g));

  setgcrefnull(empty);
  setmref(g->gc.sweep, &empty);
  g->gc.state = GCSsweep;
  oldstepmul = g->gc.stepmul;
  g->gc.stepmul = 1;
  arenas0 = gc2_sweep_owner_arenas_acq(g);

  (void)lj_gc_step(L);
  delta = gc2_sweep_owner_arenas_acq(g) - arenas0;
  assert(delta > 0);
  assert(delta <= LJ_GC2_SWEEP_BATCH);
  assert(g->gc.state == GCSsweep);
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] != NULL ||
	 tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE] != NULL);
  assert(tg->alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(lj_gc2_sweep_pending(g));

  for (i = 0; i < seeded + 4u && g->gc.state != GCSpause; i++)
    (void)lj_gc_step(L);
  assert(g->gc.state == GCSpause);
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(tg->alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(!lj_gc2_sweep_pending(g));
  setmref(g->gc.sweep, &g->gc.root);
  g->gc.stepmul = oldstepmul;
  lua_close(L);
}

static void test_boundary_lazy_sweep_extra_tg(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg, extra_tg;
  GCRef empty;
  MSize oldstepmul;
  uint64_t arenas0, delta;
  uint32_t oldcycle, sweep_cycle, i;
  void *extra_plain, *extra_trav;
  GCArena *extra_plain_a, *extra_trav_a;
  const uint32_t seeded = LJ_GC2_SWEEP_BATCH + 2u;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  lua_gc(L, LUA_GCCOLLECT, 0);

  lj_tg_init_thread(g, &extra_tg, NULL, 1);
  extra_tg.tid = tg->tid + 2000u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  lj_native_enter(&extra_tg);
  lj_tg_attach(g, &extra_tg);
  assert(g->gc2.n_threads == 2);

  extra_plain = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64, 0);
  extra_trav = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			      LJ_AF_TRAVERSABLE);
  assert(extra_plain != NULL);
  assert(extra_trav != NULL);
  extra_plain_a = lj_arena_of(extra_plain);
  extra_trav_a = lj_arena_of(extra_trav);
  assert(extra_plain_a->hdr.owner_tid == extra_tg.alloc.owner_tid);
  assert(extra_trav_a->hdr.owner_tid == extra_tg.alloc.owner_tid);

  oldcycle = g->gc2.cycle;
  g->gc2.cycle = oldcycle + 1u;
  sweep_cycle = g->gc2.cycle;
  (void)test_publish_sweep_phase(g);
  assert(lj_gc2_handshake(g, LJ_GC2_HS_RESET_ALLOC) == 2);
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_PLAIN],
			     extra_plain_a));
  assert(arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     extra_trav_a));
  assert((extra_plain_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert((extra_trav_a->hdr.flags & LJ_AF_NEEDSWEEP) != 0);
  seed_traversable_needsweep(&extra_tg, seeded);
  assert(!lj_gc2_sweep_needs_prepare(g));
  assert(lj_gc2_sweep_pending(g));

  setgcrefnull(empty);
  setmref(g->gc.sweep, &empty);
  g->gc.state = GCSsweep;
  oldstepmul = g->gc.stepmul;
  g->gc.stepmul = 1;
  arenas0 = gc2_sweep_owner_arenas_acq(g);

  (void)lj_gc_step(L);
  delta = gc2_sweep_owner_arenas_acq(g) - arenas0;
  assert(delta > 0);
  assert(delta <= LJ_GC2_SWEEP_BATCH);
  assert(g->gc.state == GCSsweep);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE] != NULL ||
	 extra_tg.alloc.quarantine[LJ_ARENAK_TRAVERSABLE] != NULL);
  assert(lj_gc2_sweep_pending(g));

  for (i = 0; i < seeded + 4u && g->gc.state != GCSpause; i++)
    (void)lj_gc_step(L);
  assert(g->gc.state == GCSpause);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(!lj_gc2_sweep_needs_prepare(g));
  assert(!lj_gc2_sweep_pending(g));
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_PLAIN],
			     extra_plain_a));
  assert(arena_list_contains(lj_arena_alloc_reclaimed_head(
		&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE), extra_trav_a));
  assert((extra_plain_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert((extra_trav_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert(extra_trav_a->hdr.sweep_epoch == sweep_cycle);
  assert(lj_tg_in_native_acq(&extra_tg) == 1);

  g->gc.stepmul = oldstepmul;
  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, &extra_tg);
  lua_close(L);
}

static void test_sweep_to_idle_worker_active(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCRef empty;
  uint64_t sweep_to_idle0;
  uint32_t i;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  lua_gc(L, LUA_GCCOLLECT, 0);

  g->gc2.cycle++;
  (void)test_publish_sweep_phase(g);
  tg->alloc.sweep_epoch = g->gc2.cycle;
  setgcrefnull(empty);
  setmref(g->gc.sweep, &empty);
  g->gc.state = GCSsweep;
  sweep_to_idle0 = gc2_sweep_to_idle_acq(g);

  gc2_worker_active_rel(g, 1);
  (void)lj_gc_step(L);
  assert(g->gc.state == GCSsweep);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_SWEEP);
  assert(gc2_sweep_to_idle_acq(g) == sweep_to_idle0);

  gc2_worker_active_rel(g, 0);
  /* Reclaimed arenas from the setup collection now correctly participate in
  ** this synthetic next sweep. Let the bounded owner finish those batches;
  ** the property under test is that worker_active prevented the transition,
  ** not that an otherwise-ready transition always fits in one GC step. */
  for (i = 0; i < 1024u && g->gc.state != GCSpause; i++)
    (void)lj_gc_step(L);
  assert(g->gc.state == GCSpause);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_IDLE);
  assert(gc2_sweep_to_idle_acq(g) == sweep_to_idle0 + 1u);

  setmref(g->gc.sweep, &g->gc.root);
  lua_close(L);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  TValue *tv;
  GCtab *keep, *arrtab, *deadtab, *deadcolo, *deadsplit;
  GCfunc *fn, *deadchunk, *deadfn, *livefn, *deadcf, *finchunk, *finfn;
  GCfunc *bcfn, *hugefn, *uvfn;
  GCupval *deaduv;
  GCproto *deadpt, *deadfnpt;
  GCproto *bcpt, *hugept, *finpt;
  GCSize before_tab, deadtab_size, deadarr_size, deadnode_size;
  GCSize before_colo, deadcolo_size;
  GCSize before_split, deadsplit_size, splitarr_size, splitnode_size;
  GCSize before_fn, deadfn_size, before_drop, deadpt_size, deadchunk_size;
  GCSize before_raw, before_cf, deadcf_size;
  GCSize before_bc, bcfn_size, bcpt_size, before_huge, hugefn_size;
  GCSize hugept_size;
  GCSize before_fin, finpt_size, finchunk_size, finfn_size;
  GCSize before_uv, uvfn_size, uv_size;
  uint64_t sweep_owner_runs0, sweep_owner_arenas0, sweep_owner_live0;
  uint64_t huge_live_bytes;
  uint32_t sweep_epoch0;
  void *raw, *deadarr, *deadnode, *splitarr, *splitnode;
  LJHugeInfo hugehi;
  GCArena *fna, *arra;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "keep = {}\n"
    "keep.f = function(x) return x + 1 end\n"
    "keep.dead = loadstring('return 42')\n"
    "keep.parent = loadstring('return function(x) return x + 7 end')\n"
    "keep.deadfn = keep.parent()\n"
    "keep.livefn = keep.parent()\n"
    "do\n"
    "  local x = 10\n"
    "  keep.uvdead = function() x = x + 1; return x end\n"
    "end\n"
    "keep.arr = {}\n"
    "for i = 1, 300 do keep.arr[i] = i end\n"
    "keep.deadtab = {}\n"
    "for i = 1, 300 do keep.deadtab[i] = i end\n"
    "for i = 1, 80 do keep.deadtab['k'..i] = i end\n"
    "keep.deadcolo = {10, 20, 30}\n"
    "keep.deadsplit = {1, 2, 3}\n"
    "for i = 4, 80 do keep.deadsplit[i] = i end\n"
    "collectgarbage('collect')\n") == LUA_OK);

  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);
  assert(tg->alloc.sweep_epoch != 0);

  lua_getglobal(L, "keep");
  tv = L->top - 1;
  assert(tvistab(tv));
  keep = tabV(tv);
  assert((lj_arena_of(keep)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(keep) == 2);
  assert(lj_arena_of(keep)->hdr.sweep_epoch == tg->alloc.sweep_epoch);

  lua_getfield(L, -1, "f");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  fna = lj_arena_of(fn);
  assert((fna->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(fn) == 2);
  assert(fna->hdr.sweep_epoch == tg->alloc.sweep_epoch);
  L->top--;

  lua_getfield(L, -1, "dead");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  assert(isluafunc(fn));
  deadchunk = fn;
  deadchunk_size = sizeLfunc((MSize)lj_funcL_nupvalues(&deadchunk->l));
  deadpt = funcproto(fn);
  deadpt_size = deadpt->sizept;
  assert((lj_obj_gcflags(obj2gco(deadpt)) &
	  (LJ_GC_FIXED|LJ_GC_SFIXED)) == 0);
  assert(ptr_state(deadchunk) == 2);
  assert(ptr_state(deadpt) == 2);
  L->top--;

  lua_getfield(L, -1, "deadfn");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  deadfn = funcV(tv);
  assert(isluafunc(deadfn));
  deadfn_size = sizeLfunc((MSize)lj_funcL_nupvalues(&deadfn->l));
  deadfnpt = funcproto(deadfn);
  assert(ptr_state(deadfn) == 2);
  assert(lj_arena_root_state_acq(lj_arena_of(deadfn),
    lj_arena_cellof(deadfn)) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(deadfn),
    lj_arena_cellof(deadfn)) == LJ_ARENA_DTOR_LFUNC0);
  assert(ptr_state(deadfnpt) == 2);
  L->top--;

  lua_getfield(L, -1, "livefn");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  livefn = funcV(tv);
  assert(isluafunc(livefn));
  assert(funcproto(livefn) == deadfnpt);
  assert(ptr_state(livefn) == 2);
  L->top--;

  lua_getfield(L, -1, "uvdead");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  uvfn = funcV(tv);
  assert(isluafunc(uvfn));
  assert(lj_funcL_nupvalues(&uvfn->l) == 1);
  uvfn_size = sizeLfunc((MSize)lj_funcL_nupvalues(&uvfn->l));
  deaduv = func_uv_acq(&uvfn->l, 0);
  uv_size = sizeof(GCupval);
  assert(deaduv->closed);
  assert(uvval(deaduv) == &deaduv->tv);
  assert((lj_arena_of(deaduv)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(uvfn) == 2);
  assert(ptr_state(deaduv) == 2);
  assert(lj_arena_root_state_acq(lj_arena_of(uvfn),
    lj_arena_cellof(uvfn)) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(uvfn),
    lj_arena_cellof(uvfn)) == LJ_ARENA_DTOR_LFUNC1);
  assert(lj_arena_root_state_acq(lj_arena_of(deaduv),
    lj_arena_cellof(deaduv)) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(deaduv),
    lj_arena_cellof(deaduv)) == LJ_ARENA_DTOR_CLOSED_UV);
  L->top--;

  lua_getfield(L, -1, "arr");
  tv = L->top - 1;
  assert(tvistab(tv));
  arrtab = tabV(tv);
  assert(arrtab->asize > 0);
  assert(arrtab->colo <= 0);
  arra = lj_arena_of(lj_tab_array_hdrw(lj_tab_array_acq(arrtab)));
  assert((arra->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
  assert(arra->hdr.sweep_epoch == 0);
  assert(ptr_state(lj_tab_array_hdrw(lj_tab_array_acq(arrtab))) == 3);
  L->top--;

  lua_getfield(L, -1, "deadtab");
  tv = L->top - 1;
  assert(tvistab(tv));
  deadtab = tabV(tv);
  assert(deadtab->asize > 0);
  assert(deadtab->hmask > 0);
  assert(deadtab->colo <= 0);
  deadtab_size = sizeof(GCtab);
  deadarr = lj_tab_array_hdrw(lj_tab_array_acq(deadtab));
  deadarr_size = lj_tab_array_bytes(deadtab->acap);
  deadnode = lj_tab_node_hdrw(lj_tab_node_acq(deadtab));
  deadnode_size = lj_tab_node_bytes(deadtab->hmask);
  assert((lj_arena_of(deadtab)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert((lj_arena_of(deadarr)->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
  assert((lj_arena_of(deadnode)->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
  assert(ptr_state(deadtab) == 2);
  assert(ptr_state(deadarr) == 3);
  assert(ptr_state(deadnode) == 3);
  L->top--;

  sweep_owner_runs0 = gc2_sweep_owner_runs_acq(g);
  sweep_owner_arenas0 = gc2_sweep_owner_arenas_acq(g);
  sweep_owner_live0 = gc2_sweep_owner_live_cells_acq(g);
  sweep_epoch0 = tg->alloc.sweep_epoch;
  before_tab = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadtab");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(gc2_sweep_owner_runs_acq(g) > sweep_owner_runs0);
  assert(gc2_sweep_owner_arenas_acq(g) > sweep_owner_arenas0);
  assert(gc2_sweep_owner_live_cells_acq(g) >= sweep_owner_live0);
  assert(tg->alloc.sweep_epoch > sweep_epoch0);
  assert(g->gc.total <=
	 before_tab - deadtab_size - deadarr_size - deadnode_size);
  assert((ptr_state(deadtab) & 2u) == 0);
  assert((ptr_state(deadarr) & 2u) == 0);
  assert((ptr_state(deadnode) & 2u) == 0);

  lua_getfield(L, -1, "deadcolo");
  tv = L->top - 1;
  assert(tvistab(tv));
  deadcolo = tabV(tv);
  assert(deadcolo->asize > 0);
  assert(deadcolo->hmask == 0);
  assert(deadcolo->colo > 0);
  deadcolo_size = sizetabcolo((uint32_t)deadcolo->colo & 0x7f);
  assert((lj_arena_of(deadcolo)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(deadcolo) == 2);
  L->top--;

  before_colo = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadcolo");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_colo - deadcolo_size);
  assert((ptr_state(deadcolo) & 2u) == 0);

  lua_getfield(L, -1, "deadsplit");
  tv = L->top - 1;
  assert(tvistab(tv));
  deadsplit = tabV(tv);
  assert(deadsplit->asize > 0);
  assert(deadsplit->colo < 0);
  assert(deadsplit->asize > lj_tab_colo_size(deadsplit));
  assert(lj_tab_colo_size(deadsplit) != 0);
  deadsplit_size = sizetabcolo(lj_tab_colo_size(deadsplit));
  splitarr = lj_tab_array_hdrw(lj_tab_array_acq(deadsplit));
  splitarr_size = lj_tab_array_bytes(deadsplit->acap);
  splitnode_size = deadsplit->hmask > 0 ?
		   lj_tab_node_bytes(deadsplit->hmask) : 0;
  splitnode = deadsplit->hmask > 0 ?
	      (void *)lj_tab_node_hdrw(lj_tab_node_acq(deadsplit)) : NULL;
  assert((lj_arena_of(deadsplit)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert((lj_arena_of(splitarr)->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
  assert(ptr_state(deadsplit) == 2);
  assert(ptr_state(splitarr) == 3);
  if (splitnode) {
    assert((lj_arena_of(splitnode)->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
    assert(ptr_state(splitnode) == 3);
  }
  L->top--;

  before_split = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadsplit");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <=
	 before_split - deadsplit_size - splitarr_size - splitnode_size);
  assert((ptr_state(deadsplit) & 2u) == 0);
  assert((ptr_state(splitarr) & 2u) == 0);
  if (splitnode)
    assert((ptr_state(splitnode) & 2u) == 0);

  /* Fixed/SFIXED retention overrides arena destructor identity. Rootless
  ** typed bodies have no ownership-spine entry which could otherwise preserve
  ** this flag, so exercise the post-grace validator directly through a real
  ** collection before allowing the ordinary destructor on the next cycle. */
  lj_obj_addgcflags_atomic(obj2gco(deadfn), LJ_GC_FIXED|LJ_GC_SFIXED);
  lua_pushnil(L);
  lua_setfield(L, -2, "deadfn");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert((ptr_state(deadfn) & 2u) != 0);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(deadfn),
    lj_arena_cellof(deadfn)) == LJ_ARENA_DTOR_LFUNC0);
  assert((lj_obj_gcflags(obj2gco(deadfn)) &
	  (LJ_GC_FIXED|LJ_GC_SFIXED)) == (LJ_GC_FIXED|LJ_GC_SFIXED));
  lj_obj_cleargcflags_atomic(obj2gco(deadfn), LJ_GC_FIXED|LJ_GC_SFIXED);

  before_fn = g->gc.total;
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_fn - deadfn_size);
  assert((ptr_state(deadfn) & 2u) == 0);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(deadfn),
    lj_arena_cellof(deadfn)) == LJ_ARENA_DTOR_NONE);
  assert(ptr_state(deadfnpt) == 2);
  assert(ptr_state(livefn) == 2);

  /* A cdata sidecar disagreement must likewise pin a rootless typed object
  ** without selecting either destructor family. The independently described
  ** closure can still be reclaimed in the first cycle; clearing the injected
  ** disagreement admits the closed-upvalue destructor in the next one. */
  lj_arena_bm_set(lj_arena_of(deaduv)->cdata, lj_arena_cellof(deaduv));
  before_uv = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "uvdead");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_uv - uvfn_size);
  assert((ptr_state(uvfn) & 2u) == 0);
  assert((ptr_state(deaduv) & 2u) != 0);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(uvfn),
    lj_arena_cellof(uvfn)) == LJ_ARENA_DTOR_NONE);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(deaduv),
    lj_arena_cellof(deaduv)) == LJ_ARENA_DTOR_CLOSED_UV);
  assert(lj_arena_cdata_get(lj_arena_of(deaduv),
    lj_arena_cellof(deaduv)) == 1u);
  lj_arena_bm_clear(lj_arena_of(deaduv)->cdata, lj_arena_cellof(deaduv));

  before_uv = g->gc.total;
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_uv - uv_size);
  assert((ptr_state(deaduv) & 2u) == 0);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(deaduv),
    lj_arena_cellof(deaduv)) == LJ_ARENA_DTOR_NONE);

  before_raw = g->gc.total;
  raw = lj_mem_newgco_raw(L, 64, LJ_AF_TRAVERSABLE);
  /* Raw allocator storage has malloc semantics and may come from a reclaimed
  ** arena, so establish the header value this deferral check expects. */
  memset(raw, 0, 64);
  assert(g->gc.total == before_raw + 64);
  assert(ptr_state(raw) == 2);
  assert(lj_gc2_markmem(g, raw) == 1);
  assert(ptr_state(raw) == 3);
  assert(lj_mem_freegco_defer(g, raw, 64) == 1);
  assert(g->gc.total == before_raw);
  assert(((GCobj *)raw)->gch.gct == 0);
  assert(ptr_state(raw) == 2);

  before_drop = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "dead");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_drop - deadpt_size - deadchunk_size);
  assert((ptr_state(deadchunk) & 2u) == 0);
  assert((ptr_state(deadpt) & 2u) == 0);
  assert(ptr_state(raw) == 1);

  lua_getfield(L, -1, "arr");
  lua_pushcclosure(L, noop_finalizer, 1);
  tv = L->top - 1;
  assert(tvisfunc(tv));
  deadcf = funcV(tv);
  assert(!isluafunc(deadcf));
  assert(lj_funcC_nupvalues(&deadcf->c) == 1);
  deadcf_size = sizeCfunc((MSize)lj_funcC_nupvalues(&deadcf->c));
  assert(tvistab(&deadcf->c.upvalue[0]));
  assert(tabV(&deadcf->c.upvalue[0]) == arrtab);
  assert((lj_arena_of(deadcf)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(deadcf) == 2);
  lua_setfield(L, -2, "deadcf");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(ptr_state(deadcf) == 2);

  before_cf = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadcf");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_cf - deadcf_size);
  assert((ptr_state(deadcf) & 2u) == 0);
  assert(ptr_state(arrtab) == 2);

  assert(luaL_dostring(L,
    "do\n"
    "  local f = assert(loadstring('return function(y) return y * 9 end'))()\n"
    "  keep.bcblob = string.dump(f)\n"
    "  keep.bcdead = assert(loadstring(keep.bcblob))\n"
    "end\n"
    "collectgarbage('collect')\n") == LUA_OK);
  lua_getfield(L, -1, "bcdead");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  bcfn = funcV(tv);
  assert(isluafunc(bcfn));
  bcfn_size = sizeLfunc((MSize)lj_funcL_nupvalues(&bcfn->l));
  bcpt = funcproto(bcfn);
  bcpt_size = bcpt->sizept;
  assert((lj_obj_gcflags(obj2gco(bcpt)) &
	  (LJ_GC_FIXED|LJ_GC_SFIXED)) == 0);
  assert((lj_arena_of(bcpt)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(bcfn) == 2);
  assert(ptr_state(bcpt) == 2);
  L->top--;

  before_bc = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "bcdead");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_bc - bcpt_size - bcfn_size);
  assert((ptr_state(bcfn) & 2u) == 0);
  assert((ptr_state(bcpt) & 2u) == 0);

  assert(luaL_dostring(L,
    "do\n"
    "  local t = {'return function() local x = 0\\n'}\n"
    "  for i = 1, 6000 do t[#t+1] = 'x = x + 1\\n' end\n"
    "  t[#t+1] = 'return x end'\n"
    "  keep.huge = assert(loadstring(table.concat(t)))()\n"
    "end\n"
    "collectgarbage('collect')\n") == LUA_OK);
  lua_getfield(L, -1, "huge");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  hugefn = funcV(tv);
  assert(isluafunc(hugefn));
  hugefn_size = sizeLfunc((MSize)lj_funcL_nupvalues(&hugefn->l));
  hugept = funcproto(hugefn);
  hugept_size = hugept->sizept;
  assert(hugept_size > LJ_HUGE_THRESHOLD);
  assert(lj_arena_ishuge(lj_arena_of(hugept)));
  assert(lj_arena_hugetab_lookup(&tg->huge, hugept, &hugehi) == 1);
  assert(hugehi.size == hugept_size);
  assert((hugehi.flags & LJ_HUGEF_TRAVERSABLE) != 0);
  /* A completed nongenerational major clears survivor marks for both small
  ** arenas and huge-table entries; liveness is republished next cycle. */
  assert((hugehi.flags & LJ_HUGEF_MARK) == 0);
  huge_live_bytes = la_load64_acq(&g->gc2.sweep_live_huge_bytes);
  assert(huge_live_bytes >= hugept_size);
  assert(la_load64_acq(&g->gc2.live_estimate) >= huge_live_bytes);
  assert(ptr_state(hugefn) == 2);
  L->top--;

  before_huge = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "huge");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_huge - hugept_size - hugefn_size);
  assert((ptr_state(hugefn) & 2u) == 0);
  assert(lj_arena_hugetab_lookup(&tg->huge, hugept, NULL) == 0);

  assert(luaL_dostring(L,
    "keep.deadfin = loadstring('return 43')\n"
    "keep.deadfinfn = keep.parent()\n") ==
	 LUA_OK);
  lua_getfield(L, -1, "deadfin");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  assert(isluafunc(fn));
  finchunk = fn;
  finchunk_size = sizeLfunc((MSize)lj_funcL_nupvalues(&finchunk->l));
  finpt = funcproto(fn);
  finpt_size = finpt->sizept;
  assert(ptr_state(finchunk) == 2);
  assert(ptr_state(finpt) == 2);
  L->top--;

  lua_getfield(L, -1, "deadfinfn");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  finfn = funcV(tv);
  assert(isluafunc(finfn));
  finfn_size = sizeLfunc((MSize)lj_funcL_nupvalues(&finfn->l));
  assert(funcproto(finfn) == deadfnpt);
  assert(ptr_state(finfn) == 2);
  L->top--;

  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, noop_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_setfield(L, -2, "ud");

  before_fin = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadfin");
  lua_pushnil(L);
  lua_setfield(L, -2, "deadfinfn");
  lua_pushnil(L);
  lua_setfield(L, -2, "ud");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_fin - finpt_size - finchunk_size - finfn_size);
  assert((ptr_state(finchunk) & 2u) == 0);
  assert((ptr_state(finfn) & 2u) == 0);
  assert((ptr_state(finpt) & 2u) == 0);

  lua_close(L);
  test_prepare_collision_detaches_allocator();
  test_preserve_abort_waits_for_restore_publisher();
  test_quarantine_late_live_after_eof();
  test_sweep_to_idle_worker_active();
  test_worker_owned_sweep_direct();
  test_minor_sweep_identity_direct();
  test_boundary_lazy_sweep();
  test_boundary_lazy_sweep_extra_tg();
  printf("t-arena-gcsweep OK: traversable runtime sweep bridge verified\n");
  return 0;
}
