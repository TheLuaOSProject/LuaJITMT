/*
** Focused regression test for JIT trace body/exittab SMR retirement.
*/

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_str.h"
#include "lj_tg.h"
#include "lj_trace.h"

#ifndef LJ_TRACE_TEST_HELPERS
#error "t-jit-trace-retire requires LJ_TRACE_TEST_HELPERS"
#endif

static void test_trace_preserve_candidates(global_State *g, GCproto *pt)
{
  GCobj *bad = (GCobj *)(uintptr_t)U64x(00004000,00000000);
  assert(lj_trace_test_preserve_body_candidate(g, obj2gco(pt)) == 1);
  assert(lj_trace_test_preserve_body_candidate(g, bad) == 0);
  assert(lj_trace_test_proto_pc_candidate(g, obj2gco(pt), proto_bc(pt)) == 1);
  assert(lj_trace_test_proto_pc_candidate(g, obj2gco(pt),
					  proto_bc(pt) + pt->sizebc) == 0);
  assert(lj_trace_test_proto_pc_candidate(g, bad, proto_bc(pt)) == 0);
}

static void test_trace_complete_payload_layout(GCtrace *T)
{
  char *p = (char *)T + ((sizeof(GCtrace)+7)&~7);
  p += (MSize)(T->nins - T->nk) * sizeof(IRIns);
  T->snap = (SnapShot *)p;
  p += (MSize)T->nsnap * sizeof(SnapShot);
  T->snapmap = (SnapEntry *)p;
}

static GCSize test_trace_allocation_size(const GCtrace *T)
{
  size_t sztr = (sizeof(GCtrace) + 7u) & ~(size_t)7u;
  return (GCSize)(sztr + (MSize)(T->nins - T->nk) * sizeof(IRIns) +
	(MSize)T->nsnap * sizeof(SnapShot) +
	(MSize)T->nsnapmap * sizeof(SnapEntry));
}

static void test_trace_publish_header(global_State *g, GCtrace *T)
{
  GCArena *a = lj_arena_of(T);
  uint32_t cell = lj_arena_cellof(T);
  newwhite(g, T);
  lj_gc_publishobj_header(g, obj2gco(T));
  if (lj_arena_ishuge(a)) {
    TGState *tg = G2TG(g);
    LJHugeInfo hi;
    assert(tg != NULL && lj_tg_flags_test_acq(tg, TGF_HUGETAB));
    assert(lj_arena_hugetab_lookup(&tg->huge, T, &hi) == 1);
    assert((hi.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) ==
	   (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY));
  } else {
    assert(lj_arena_ready_get(a, cell));
  }
}

static void test_trace_publish_root(global_State *g, GCtrace *T)
{
  test_trace_publish_header(g, T);
  lj_gc_linkobj_new(g, obj2gco(T));
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
}

static uint8_t test_gc_colors(GCobj *o)
{
  return (uint8_t)(lj_obj_gcflags(o) & LJ_GC_COLORS);
}

static void test_gc2_unmark_small(GCobj *o)
{
  GCArena *a = lj_arena_of(o);
  uint32_t cell = lj_arena_cellof(o);
  assert(!lj_arena_ishuge(a));
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  lj_arena_bm_clear(a->mark, cell);
}

static void test_gc2_unmark_mem(void *p)
{
  GCArena *a = lj_arena_of(p);
  uint32_t cell = lj_arena_cellof(p);
  assert(!lj_arena_ishuge(a));
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  lj_arena_bm_clear(a->mark, cell);
}

static void test_trace_stale_startins_candidates(global_State *g, GCproto *pt,
						 GCtrace *T)
{
  GCobj *bad = (GCobj *)(uintptr_t)U64x(00004000,00000000);
  assert(lj_trace_test_stale_startins_candidate(g, obj2gco(T)) == 1);
  assert(lj_trace_test_stale_startins_candidate(g, obj2gco(pt)) == 0);
  assert(lj_trace_test_stale_startins_candidate(g, bad) == 0);
}

static GCtrace *retired_find(jit_State *J, GCtrace *needle)
{
  GCtrace *T;
  for (T = trace_retired_head_acq(J);
       T != NULL;
       T = trace_retired_next_acq(T))
    if (T == needle)
      return T;
  return NULL;
}

static GCtrace *published_trace_find(jit_State *J)
{
  MSize i;
  for (i = 1; i < J->sizetrace; i++) {
    GCtrace *T = traceref(J, i);
    if (T != NULL && trace_traceno_acq(T) > 0)
      return T;
  }
  return NULL;
}

static void settle_automatic_cycle(global_State *g)
{
  uint32_t attempts;
  for (attempts = 0;
       gc2_phase_acq(g) != LJ_GC2_IDLE && attempts < 4096u;
       attempts++) {
    (void)lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH);
    lj_gc2_cycle_to_idle(g);
  }
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
}

static void test_listed_slot_teardown_no_republish(lua_State *L)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  GCtrace *T;
  int i;
  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "return function(x) return x + 1 end\n") == LUA_OK);
  for (i = 1; i <= 20; i++) {
    lua_pushvalue(L, -1);
    lua_pushinteger(L, i);
    lua_call(L, 1, 1);
    lua_pop(L, 1);
  }
  settle_automatic_cycle(g);
  T = published_trace_find(J);
  assert(T != NULL && trace_traceno_acq(T) != 0);
  lj_trace_test_reset_retire_publish_calls();
  assert(lj_trace_retire_gc_claim(g, T));
  assert(la_load64_acq(&T->retire_epoch) != 0);
  assert(lj_trace_test_retire_publish_calls() == 1u);
  /* The body is already on the retire list, but its public slot still needs
  ** the second half of token-owned teardown. This must not repeat first
  ** retirement (which would enter an SMR reader under sweep reclaim). */
  assert(lj_trace_free_gc(g, T));
  assert(lj_trace_test_retire_publish_calls() == 1u);
  assert(trace_traceno_acq(T) == 0);
  lua_settop(L, 0);
}

static uint32_t reclaim_trace_at(global_State *g, uint64_t epoch)
{
  jit_State *J = G2J(g);
  uint32_t n;
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(lj_gc2_test_idle_reclaim_enter(g));
  assert(lj_jit_token_try(J));
  n = lj_trace_reclaim_retired(g, epoch);
  lj_jit_token_release(J);
  lj_gc2_test_idle_reclaim_leave(g);
  return n;
}

static void test_huge_reader_reclaim_retry(lua_State *L, GCproto *pt)
{
  enum {
    TEST_HUGE_TRACE_NREF = LJ_HUGE_THRESHOLD / sizeof(IRIns) + 64u
  };
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = G2TG(g);
  GCtrace tmpl, *T;
  static IRIns hugeir[REF_BASE + TEST_HUGE_TRACE_NREF];
  LJHugeReader reader = { NULL, NULL, 0 };
  LJHugeInfo hi;
  GCSize size;
  uint64_t epoch, completed;

  assert(trace_retired_head_acq(J) == NULL);
  assert(tg != NULL && lj_tg_flags_test_acq(tg, TGF_HUGETAB));
  assert(REF_BASE + TEST_HUGE_TRACE_NREF <= 0xffffu);
  memset(&tmpl, 0, sizeof(tmpl));
  tmpl.nk = REF_BASE;
  tmpl.nins = REF_BASE + TEST_HUGE_TRACE_NREF;
  tmpl.ir = hugeir;
  T = lj_trace_alloc(L, &tmpl);
  test_trace_complete_payload_layout(T);
  setgcref(T->startpt, obj2gco(pt));
  test_trace_publish_root(g, T);
  size = test_trace_allocation_size(T);
  assert(size > LJ_HUGE_THRESHOLD);
  assert(lj_arena_ishuge(lj_arena_of(T)));
  assert(lj_arena_hugetab_lookup(&tg->huge, T, &hi) == 1);
  assert(hi.size == size);

  /* Finish any allocator-triggered cycle while T is still a normal root. The
  ** white-box reclaim below needs the real IDLE reclaimer capability. */
  settle_automatic_cycle(g);
  lj_trace_free(g, T);
  assert(retired_find(J, T) == T);
  epoch = la_load64_acq(&T->retire_epoch) - 1u;
  completed = epoch + LJ_FLUSH_EPOCHS;

  /* A counted body reader must make pre-destructor ownership a plain retry.
  ** Publishing DEFER_FREE here would make raw re-preservation reject T and the
  ** last reader terminalize it without trace type/side-body destruction. */
  assert(lj_arena_hugetab_reader_acquire(
	   &tg->huge, T, &reader, &hi) == LJ_ARENA_HUGE_READER_ACQUIRED);
  assert(hi.readers == 1u);
  J->trace_reclaim_epoch = 0;
  assert(reclaim_trace_at(g, completed) == 0);
  assert(retired_find(J, T) == T);
  assert(!lj_trace_body_destroyed_acq(T));
  assert(J->trace_reclaim_epoch != completed);
  assert(lj_arena_hugetab_lookup(&tg->huge, T, &hi) == 1);
  assert(hi.readers == 1u);
  assert((hi.flags & (LJ_HUGEF_DEFER_FREE|LJ_HUGEF_FREEING|
		      LJ_HUGEF_BUSY)) == 0);

  assert(lj_arena_hugetab_reader_release(&reader, &hi) ==
	 LJ_ARENA_HUGE_READER_RELEASED);
  assert((hi.flags & (LJ_HUGEF_DEFER_FREE|LJ_HUGEF_FREEING)) == 0);
  assert(reclaim_trace_at(g, completed) >= 1);
  assert(retired_find(J, T) == NULL);
  /* The white-box completed epoch does not advance hs_epoch. Do not suppress
  ** a later body retired at the still-current real epoch. */
  J->trace_reclaim_epoch = 0;
  /* T may be unmapped by the successful IDLE reclaim. */
}

static void test_unpublished_huge_abandon_smr_collision(lua_State *L)
{
  global_State *g = G(L);
  GCSize size = (GCSize)LJ_HUGE_THRESHOLD + LJ_CELL_SIZE;
  void *p = lj_mem_newgco_raw(L, size,
	LJ_AF_TRAVERSABLE|LJ_AF_ROOT_CONSTRUCT);
  LJGC2ActivationSnap before, after;

  assert(p != NULL && lj_arena_ishuge(lj_arena_of(p)));
  settle_automatic_cycle(g);
  assert(lj_gc2_test_idle_reclaim_enter(g));
  assert(gc2_smr_reclaiming_acq(g) != 0);
  assert(gc2_smr_readers_acq(g) == 0);
  before = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_mem_abandon_gco_unpublished(g, p) ==
	 LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(&before, &after));
  assert(gc2_smr_reclaiming_acq(g) != 0);
  assert(gc2_smr_readers_acq(g) == 0);
  lj_gc2_test_idle_reclaim_leave(g);
  /* Huge cancellation is one-shot. Free the now rootless exact allocation
  ** directly instead of repeating the constructor-abandon transition. */
  lj_mem_free(g, p, size);
}

static void test_unpublished_preclaim_smr_collision(lua_State *L, GCproto *pt)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  GCtrace tmpl, *T, *cancelT;
  static IRIns dummyir[REF_TRUE+1];
  MCode **exittab;
  GCArena *a;
  uint32_t cell;
  GCSize cancel_size;
  LJGC2ActivationSnap before, after;

  memset(&tmpl, 0, sizeof(tmpl));
  tmpl.nk = REF_BASE;
  tmpl.nins = REF_BASE;
  tmpl.nsnap = 1;
  tmpl.ir = dummyir;
  T = lj_trace_alloc(L, &tmpl);
  test_trace_complete_payload_layout(T);
  test_trace_publish_header(g, T);
  setgcref(T->startpt, obj2gco(pt));
  exittab = lj_mem_newvec(L, 1, MCode *);
  exittab[0] = NULL;
  T->exittab = exittab;
  assert(trace_traceno_acq(T) == 0);
  assert(trace_nextroot_acq(T) == 0);
  assert(la_load64_acq(&T->retire_epoch) == 0);
  assert(!trace_retired_link_listed_acq(T));

  /* Keep the semantic-retirement body in its original constructor state.
  ** This second compact trace exercises cancellation and is freed directly
  ** after the writer releases, so abandon remains strictly one-shot. */
  cancelT = lj_trace_alloc(L, &tmpl);
  test_trace_complete_payload_layout(cancelT);
  test_trace_publish_header(g, cancelT);
  cancel_size = test_trace_allocation_size(cancelT);

  settle_automatic_cycle(g);
  assert(lj_jit_token_try(J));
  assert(lj_gc2_test_idle_reclaim_enter(g));
  assert(gc2_smr_reclaiming_acq(g) != 0);
  assert(gc2_smr_readers_acq(g) == 0);
  test_gc2_unmark_mem(T);
  test_gc2_unmark_mem(exittab);
  test_gc2_unmark_small(obj2gco(pt));
  before = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(!lj_trace_test_preserve_unpublished_publish(g, T));
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(&before, &after));
  assert(gc2_smr_reclaiming_acq(g) != 0);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(!lj_arena_bm_get(lj_arena_of(T)->mark, lj_arena_cellof(T)));
  assert(!lj_arena_bm_get(lj_arena_of(exittab)->mark,
			  lj_arena_cellof(exittab)));
  assert(!lj_arena_bm_get(lj_arena_of(pt)->mark, lj_arena_cellof(pt)));
  assert(!lj_gc2_activation_reclaim_veto(g));
  a = lj_arena_of(cancelT);
  cell = lj_arena_cellof(cancelT);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_LINKING);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_CONSTRUCT);
  before = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_mem_abandon_gco_unpublished(g, cancelT) ==
	 LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(&before, &after));
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  lj_gc2_test_idle_reclaim_leave(g);
  lj_mem_free(g, cancelT, cancel_size);

  /* The exact runtime path retries its tactical pre-claim mark, claims the
  ** epoch, and then performs mandatory full preservation before token release. */
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  /* pt is still a Lua-stack root, so remove any mark a start/root handshake
  ** may have published. The only operation between this check and the later
  ** assertion is the unpublished trace's mandatory post-claim traversal. */
  test_gc2_unmark_mem(T);
  test_gc2_unmark_mem(exittab);
  test_gc2_unmark_small(obj2gco(pt));
  assert(!lj_arena_bm_get(lj_arena_of(T)->mark, lj_arena_cellof(T)));
  assert(!lj_arena_bm_get(lj_arena_of(exittab)->mark,
			  lj_arena_cellof(exittab)));
  assert(!lj_arena_bm_get(lj_arena_of(pt)->mark, lj_arena_cellof(pt)));
  lj_trace_free_unpublished(g, T);
  lj_jit_token_release(J);
  assert(retired_find(J, T) == T);
  assert(lj_gc2_ismarked(g, obj2gco(T)) > 0);
  assert(lj_gc2_ismarkedmem(g, exittab) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(pt)) > 0);
  lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  /* The transition may already drain the now-proven retired body, otherwise
  ** normal later grace/terminal teardown retains ownership. Do not dereference
  ** T after this point. */
}

static void test_gc_claim_rescues_runnable_inbound(lua_State *L)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TraceVec *tv;
  GCtrace tmpl, *target, *source;
  static IRIns dummyir[REF_TRUE+1];
  TraceNo targetno = 0, sourceno = 0, i;
  uint64_t epoch;
  uint8_t target_colors;

  /* Trace vectors are lazy. Compile one ordinary loop so the synthetic reverse
  ** edge below shares the exact published-vector path used at runtime. */
  assert(luaL_dostring(L,
    "jit.opt.start('hotloop=1'); local s=0; "
    "for j=1,80 do s=s+j end; assert(s==3240)") == 0);
  tv = tracevec_acq(J);
  assert(tv != NULL);
  for (i = 1; (MSize)i < tv->sizetrace; i++) {
    if (gcref_acq(tv->slot[i]) == NULL) {
      if (targetno == 0)
	targetno = i;
      else {
	sourceno = i;
	break;
      }
    }
  }
  assert(targetno != 0 && sourceno != 0);

  memset(&tmpl, 0, sizeof(tmpl));
  tmpl.nk = REF_BASE;
  tmpl.nins = REF_BASE;
  tmpl.ir = dummyir;
  target = lj_trace_alloc(L, &tmpl);
  source = lj_trace_alloc(L, &tmpl);
  test_trace_complete_payload_layout(target);
  test_trace_complete_payload_layout(source);
  test_trace_publish_root(g, target);
  test_trace_publish_root(g, source);
  target_colors = test_gc_colors(obj2gco(target));

  trace_traceno_rel(target, targetno);
  trace_link_rel(target, targetno);
  trace_traceno_rel(source, sourceno);
  trace_link_rel(source, targetno);
  setgcrefrel(tv->slot[targetno], obj2gco(target));
  setgcrefrel(tv->slot[sourceno], obj2gco(source));

  /* The target cannot be retired while runnable source mcode still has a
  ** terminal link to its public number. Admission rescues semantic liveness
  ** and leaves the exact slot/body untouched for the root pass to retry.
  */
  assert(lj_trace_retire_gc_claim(g, target) == 0);
  assert(la_load64_acq(&target->retire_epoch) == 0);
  assert(trace_traceno_acq(target) == targetno);
  assert(trace_link_acq(source) == targetno);
  assert(gcref_acq(tv->slot[targetno]) == obj2gco(target));
  assert(!trace_retired_link_listed_acq(target));
  assert(lj_gc2_ismarked(g, obj2gco(target)) > 0);
  assert(test_gc_colors(obj2gco(target)) == target_colors);

  /* Remove the synthetic graph and transfer the two already root-published
  ** bodies through the ordinary semantic retire claim. The unpublished abort
  ** API owns only a still-LINKING recorder construction.
  */
  setgcrefrel(tv->slot[targetno], NULL);
  setgcrefrel(tv->slot[sourceno], NULL);
  trace_traceno_rel(target, 0);
  trace_traceno_rel(source, 0);
  trace_link_rel(target, 0);
  trace_link_rel(source, 0);
  epoch = lj_gc2_retire_epoch(g);
  assert(lj_trace_retire_gc_claim(g, target));
  assert(lj_trace_retire_gc_claim(g, source));
  assert(reclaim_trace_at(g, epoch + LJ_FLUSH_EPOCHS) >= 2);
}

static void test_reclaim_requeue_is_raw_and_retryable(lua_State *L,
					       GCproto *pt)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TraceVec *tv = tracevec_acq(J);
  GCtrace tmpl, *target, *source;
  static IRIns dummyir[REF_TRUE+1];
  TraceNo targetno = 0, sourceno = 0, i;
  uint64_t epoch, completed;

  assert(tv != NULL);
  for (i = 1; (MSize)i < tv->sizetrace; i++) {
    if (gcref_acq(tv->slot[i]) == NULL) {
      if (targetno == 0)
	targetno = i;
      else {
	sourceno = i;
	break;
      }
    }
  }
  assert(targetno != 0 && sourceno != 0);

  memset(&tmpl, 0, sizeof(tmpl));
  tmpl.nk = REF_BASE;
  tmpl.nins = REF_BASE;
  tmpl.ir = dummyir;
  target = lj_trace_alloc(L, &tmpl);
  source = lj_trace_alloc(L, &tmpl);
  test_trace_complete_payload_layout(target);
  test_trace_complete_payload_layout(source);
  setgcref(target->startpt, obj2gco(pt));
  test_trace_publish_root(g, target);
  test_trace_publish_root(g, source);

  /* Publish a valid retired body with synthetic public-number metadata, then
  ** leave a runnable source trace naming that number. The ready reclaim path
  ** must requeue target without repeating semantic proto/KGC preservation. */
  assert(lj_trace_retire_gc_claim(g, target));
  trace_nextroot_rel(target, targetno);
  trace_traceno_rel(source, sourceno);
  trace_link_rel(source, targetno);
  setgcrefrel(tv->slot[sourceno], obj2gco(source));

  epoch = la_load64_acq(&target->retire_epoch) - 1u;
  completed = epoch + LJ_FLUSH_EPOCHS;
  test_gc2_unmark_small(obj2gco(target));
  test_gc2_unmark_small(obj2gco(pt));
  assert(lj_gc2_ismarked(g, obj2gco(target)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(pt)) == 0);
  J->trace_reclaim_epoch = 0;
  assert(reclaim_trace_at(g, completed) == 0);
  assert(retired_find(J, target) == target);
  assert(lj_gc2_ismarked(g, obj2gco(target)) > 0);
  assert(lj_gc2_ismarked(g, obj2gco(pt)) == 0);
  /* A ready-but-transiently-blocked list must remain retryable in this exact
  ** completed epoch. Removing the inbound link is enough to reclaim it. */
  assert(J->trace_reclaim_epoch != completed);
  trace_link_rel(source, 0);
  assert(reclaim_trace_at(g, completed) >= 1);
  assert(retired_find(J, target) == NULL);

  setgcrefrel(tv->slot[sourceno], NULL);
  trace_traceno_rel(source, 0);
  assert(lj_trace_retire_gc_claim(g, source));
  /* Synthetic future-epoch calls above do not advance g->gc2.hs_epoch. Reset
  ** only the test throttle so this newly retired current-epoch body can drain. */
  J->trace_reclaim_epoch = 0;
  assert(reclaim_trace_at(g, epoch + LJ_FLUSH_EPOCHS) >= 1);
}

static void test_pending_trace_arena_requests_full_grace(lua_State *L,
						  GCproto *pt)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  GCtrace tmpl, *T;
  static IRIns dummyir[REF_TRUE+1];
  GCArena *a;
  uint32_t cell, flags, cursor;
  uint64_t epoch;
  int done = 0;

  memset(&tmpl, 0, sizeof(tmpl));
  tmpl.nk = REF_BASE;
  tmpl.nins = REF_BASE;
  tmpl.ir = dummyir;
  T = lj_trace_alloc(L, &tmpl);
  test_trace_complete_payload_layout(T);
  setgcref(T->startpt, obj2gco(pt));
  test_trace_publish_header(g, T);
  assert(lj_jit_token_try(J));
  lj_trace_free_unpublished(g, T);
  lj_jit_token_release(J);
  epoch = la_load64_acq(&T->retire_epoch) - 1u;

  a = lj_arena_of(T);
  cell = lj_arena_cellof(T);
  assert(!lj_arena_ishuge(a));
  assert(lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_WHITE);
  flags = lj_arena_flags_acq(a);
  cursor = a->hdr.reclaim_cell;
  la_store32_rel(&a->hdr.flags, flags | LJ_AF_QUARANTINE);
  assert(lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_WHITE,
					 LJ_ARENA_SWEEP_LIVE));

  /* At both young completed epochs, the arena sees an intact retired trace and
  ** must request another handshake instead of letting quarantine finish. */
  J->trace_reclaim_epoch = 0;
  a->hdr.reclaim_cell = cell;
  gc2_sweep_grace_needed_rel(g, 0);
  assert(lj_gc_reclaim_gc2_arena(g, a, 1u, &done) != 0);
  assert(!done);
  assert(gc2_sweep_grace_needed_acq(g) != 0);
  assert(!lj_trace_body_destroyed_acq(T));
  assert(reclaim_trace_at(g, epoch) == 0);

  a->hdr.reclaim_cell = cell;
  gc2_sweep_grace_needed_rel(g, 0);
  assert(lj_gc_reclaim_gc2_arena(g, a, 1u, &done) != 0);
  assert(!done);
  assert(gc2_sweep_grace_needed_acq(g) != 0);
  assert(!lj_trace_body_destroyed_acq(T));
  assert(reclaim_trace_at(g, epoch + LJ_FLUSH_EPOCHS - 1u) == 0);

  /* Restore the synthetic quarantine sidecars before the retire owner performs
  ** the physical arena free at the first legally completed epoch. */
  assert(lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_LIVE,
					 LJ_ARENA_SWEEP_WHITE));
  la_store32_rel(&a->hdr.flags, flags);
  a->hdr.reclaim_cell = cursor;
  gc2_sweep_grace_needed_rel(g, 0);
  assert(reclaim_trace_at(g, epoch + LJ_FLUSH_EPOCHS) >= 1);
}

static void test_idle_reclaim_requires_proven_root_unlink(lua_State *L,
						   GCproto *pt)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  GCtrace tmpl, *T;
  static IRIns dummyir[REF_TRUE+1];
  GCstr *stale = lj_str_newlit(L, "trace-reclaim-string-sentinel");
  GCobj *saved;
  uint64_t epoch, completed;

  memset(&tmpl, 0, sizeof(tmpl));
  tmpl.nk = REF_BASE;
  tmpl.nins = REF_BASE;
  tmpl.ir = dummyir;
  T = lj_trace_alloc(L, &tmpl);
  test_trace_complete_payload_layout(T);
  setgcref(T->startpt, obj2gco(pt));
  test_trace_publish_root(g, T);
  lj_trace_free(g, T);
  assert(retired_find(J, T) == T);
  epoch = la_load64_acq(&T->retire_epoch) - 1u;
  completed = epoch + LJ_FLUSH_EPOCHS;

  /* Hide the otherwise valid ownership spine behind a real interned string.
  ** The first ready reclaim must sever the foreign incoming edge, but cannot
  ** free T until a later scan proves that the exact root was removed/absent. */
  saved = lj_gc_root_acq(g);
  lj_gc_root_rel(g, obj2gco(stale));
  lj_gcroot_repair_epoch_add(g);
  J->trace_reclaim_epoch = 0;
  assert(reclaim_trace_at(g, completed) == 0);
  assert(retired_find(J, T) == T);
  assert(!lj_trace_body_destroyed_acq(T));
  assert(lj_gc_root_acq(g) == NULL);

  lj_gc_root_rel(g, saved);
  lj_gcroot_repair_epoch_add(g);
  assert(reclaim_trace_at(g, completed) >= 1);
  assert(retired_find(J, T) == NULL);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  jit_State *J;
  GCproto *pt;
  GCtrace tmpl, *T;
  static IRIns dummyir[REF_TRUE+1];
  MCode **exittab;
  GCtrace *ret;
  uint64_t epoch, scoped_epoch;
  uint8_t trace_colors;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  J = G2J(g);
  assert(trace_retired_head_acq(J) == NULL);
  assert(luaL_loadstring(L, "return 1") == 0);
  pt = funcproto(funcV(L->top - 1));
  test_trace_preserve_candidates(g, pt);
  test_huge_reader_reclaim_retry(L, pt);
  test_gc_claim_rescues_runnable_inbound(L);
  test_reclaim_requeue_is_raw_and_retryable(L, pt);
  test_pending_trace_arena_requests_full_grace(L, pt);
  test_idle_reclaim_requires_proven_root_unlink(L, pt);

  memset(&tmpl, 0, sizeof(tmpl));
  tmpl.nk = REF_BASE;
  tmpl.nins = REF_BASE;
  tmpl.nsnap = 1;
  tmpl.nsnapmap = 0;
  tmpl.ir = dummyir;

  T = lj_trace_alloc(L, &tmpl);
  test_trace_complete_payload_layout(T);
  test_trace_publish_header(g, T);
  trace_colors = test_gc_colors(obj2gco(T));
  assert(gcref(T->startpt) == NULL);
  /* A scratch body has neither a public slot nor a retire-list node. A foreign
  ** recorder makes the nonwaiting GC claim defer without an epoch-only LP; its
  ** actual token owner then publishes the only authoritative discovery edge.
  */
  jit_token_rel(g, 0x7fffffffu);
  assert(lj_trace_retire_gc_claim(g, T) == 0);
  assert(la_load64_acq(&T->retire_epoch) == 0);
  jit_token_rel(g, 0);
  assert(lj_jit_token_try(J));
  lj_trace_free_unpublished(g, T);
  lj_jit_token_release(J);
  assert(retired_find(J, T) == T);
  lj_trace_markvecs(g, 0);  /* Compatibility argument cannot select color GC. */
  assert(lj_gc2_ismarked(g, obj2gco(T)) > 0);
  assert(test_gc_colors(obj2gco(T)) == trace_colors);
  assert(lj_trace_retire_gc_claim(g, T) == 1);
  epoch = la_load64_acq(&T->retire_epoch) - 1u;
  /* Earlier white-box cases pass a future completed epoch without advancing
  ** hs_epoch. This scratch body is retired afterward at the still-current
  ** epoch, so clear only the synthetic scan memo before aging it explicitly. */
  J->trace_reclaim_epoch = 0;
  assert(reclaim_trace_at(g, epoch + LJ_FLUSH_EPOCHS) >= 1);
  assert(retired_find(J, T) == NULL);

  T = lj_trace_alloc(L, &tmpl);
  test_trace_complete_payload_layout(T);
  test_trace_publish_root(g, T);
  test_trace_stale_startins_candidates(g, pt, T);
  setgcref(T->startpt, obj2gco(pt));
  exittab = lj_mem_newvec(L, 1, MCode *);
  exittab[0] = NULL;
  T->exittab = exittab;
  T->nsnap = 1;
  assert(retired_find(J, T) == NULL);

  lj_trace_free(g, T);
  ret = retired_find(J, T);
  assert(ret != NULL);
  assert(ret == T);
  assert(ret->exittab == exittab);

  epoch = ret->retire_epoch - 1u;
  assert(epoch == g->gc2.hs_epoch);
  J->trace_reclaim_epoch = 0;
  assert(reclaim_trace_at(g, epoch) == 0);
  assert(J->trace_reclaim_epoch == epoch);
  assert(reclaim_trace_at(g, epoch) == 0);
  assert(J->trace_reclaim_epoch == epoch);
  assert(retired_find(J, T) != NULL);
  assert(reclaim_trace_at(g, epoch + 1u) == 0);
  assert(J->trace_reclaim_epoch == epoch + 1u);
  assert(reclaim_trace_at(g, epoch + 1u) == 0);
  assert(J->trace_reclaim_epoch == epoch + 1u);
  assert(retired_find(J, T) != NULL);
  assert(reclaim_trace_at(g, epoch + LJ_FLUSH_EPOCHS) >= 1);
  assert(retired_find(J, T) == NULL);

  T = lj_trace_alloc(L, &tmpl);
  test_trace_complete_payload_layout(T);
  test_trace_publish_root(g, T);
  setgcref(T->startpt, obj2gco(pt));
  exittab = lj_mem_newvec(L, 1, MCode *);
  exittab[0] = NULL;
  T->exittab = exittab;
  T->nsnap = 1;
  scoped_epoch = la_load64_acq(&g->gc2.hs_epoch) + 1u;
  la_store64_rel(&T->retire_epoch, scoped_epoch + 1u);
  la_store64_rel(&g->gc2.hs_epoch, scoped_epoch + 1u);

  lj_trace_free(g, T);
  ret = retired_find(J, T);
  assert(ret != NULL);
  assert(ret == T);
  assert(ret->retire_epoch == scoped_epoch + 1u);
  assert(reclaim_trace_at(g, scoped_epoch + LJ_FLUSH_EPOCHS - 1u) == 0);
  assert(retired_find(J, T) != NULL);
  assert(reclaim_trace_at(g, scoped_epoch + LJ_FLUSH_EPOCHS) >= 1);
  assert(retired_find(J, T) == NULL);

  test_listed_slot_teardown_no_republish(L);
  test_unpublished_huge_abandon_smr_collision(L);
  test_unpublished_preclaim_smr_collision(L, pt);
  lua_close(L);
  printf("t-jit-trace-retire OK: bodies, exittabs, and inbound-link rescue retire correctly\n");
  return 0;
}
