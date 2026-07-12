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

static void test_trace_publish_header(global_State *g, GCtrace *T)
{
  GCArena *a = lj_arena_of(T);
  uint32_t cell = lj_arena_cellof(T);
  newwhite(g, T);
  lj_gc_publishobj_header(g, obj2gco(T));
  assert(!lj_arena_ishuge(a));
  assert(lj_arena_ready_get(a, cell));
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
  lua_close(L);
  printf("t-jit-trace-retire OK: bodies, exittabs, and inbound-link rescue retire correctly\n");
  return 0;
}
