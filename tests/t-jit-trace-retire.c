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
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_jit.h"
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

static uint8_t test_gc_colors(GCobj *o)
{
  return (uint8_t)(lj_obj_gcflags(o) & LJ_GC_COLORS);
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

static uint32_t reclaim_trace_at(global_State *g, uint64_t epoch)
{
  jit_State *J = G2J(g);
  uint32_t expect = 0;
  uint32_t n;
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_smr_reclaiming_cas(g, &expect, 1));
  assert(lj_jit_token_try(J));
  n = lj_trace_reclaim_retired(g, epoch);
  lj_jit_token_release(J);
  gc2_smr_reclaiming_rel(g, 0);
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
  newwhite(g, target);
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

  /* Remove the synthetic graph and transfer the two scratch bodies to the
  ** ordinary unpublished-body retire path before continuing the fixture.
  */
  setgcrefrel(tv->slot[targetno], NULL);
  setgcrefrel(tv->slot[sourceno], NULL);
  trace_traceno_rel(target, 0);
  trace_traceno_rel(source, 0);
  trace_link_rel(target, 0);
  trace_link_rel(source, 0);
  epoch = lj_gc2_retire_epoch(g);
  assert(lj_jit_token_try(J));
  lj_trace_free_unpublished(g, target);
  lj_trace_free_unpublished(g, source);
  lj_jit_token_release(J);
  assert(reclaim_trace_at(g, epoch + LJ_FLUSH_EPOCHS) >= 2);
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

  memset(&tmpl, 0, sizeof(tmpl));
  tmpl.nk = REF_BASE;
  tmpl.nins = REF_BASE;
  tmpl.nsnap = 1;
  tmpl.nsnapmap = 0;
  tmpl.ir = dummyir;

  T = lj_trace_alloc(L, &tmpl);
  test_trace_complete_payload_layout(T);
  newwhite(g, T);
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
  assert(reclaim_trace_at(g, epoch + LJ_FLUSH_EPOCHS) >= 1);
  assert(retired_find(J, T) == NULL);

  T = lj_trace_alloc(L, &tmpl);
  test_trace_complete_payload_layout(T);
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
  assert(reclaim_trace_at(g, epoch) == 0);
  assert(retired_find(J, T) != NULL);
  assert(reclaim_trace_at(g, epoch + 1u) == 0);
  assert(retired_find(J, T) != NULL);
  assert(reclaim_trace_at(g, epoch + LJ_FLUSH_EPOCHS) >= 1);
  assert(retired_find(J, T) == NULL);

  T = lj_trace_alloc(L, &tmpl);
  test_trace_complete_payload_layout(T);
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

  lua_close(L);
  printf("t-jit-trace-retire OK: bodies, exittabs, and inbound-link rescue retire correctly\n");
  return 0;
}
