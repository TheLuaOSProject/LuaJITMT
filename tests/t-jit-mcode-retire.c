/*
** Focused regression test for JIT mcode SMR retirement after trace flush.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_state.h"
#include "lj_jit.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_mcode.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
typedef struct FailAllocCtx {
  lua_Alloc oldf;
  void *oldud;
  uint32_t grow_calls;
  int fail_grow;
} FailAllocCtx;

static void *fail_growing_alloc(void *ud, void *ptr, size_t osize,
				size_t nsize)
{
  FailAllocCtx *ctx = (FailAllocCtx *)ud;
  if (nsize > osize) {
    ctx->grow_calls++;
    if (ctx->fail_grow)
      return NULL;
  }
  return ctx->oldf(ctx->oldud, ptr, osize, nsize);
}
#endif

static MCodeRetire *retired_find(jit_State *J, MCode *needle)
{
  MCodeRetire *ret;
  for (ret = mcode_retired_head_acq(J);
       ret != NULL;
       ret = mcode_retired_next_acq(ret))
    if (ret->area == needle)
      return ret;
  return NULL;
}

static MCodeRetire *active_find(jit_State *J, MCode *needle)
{
  MCodeRetire *ret;
  for (ret = mcode_active_head_acq(J);
       ret != NULL;
       ret = mcode_retired_next_acq(ret))
    if (ret->area == needle)
      return ret;
  return NULL;
}

static GCtrace *trace_retired_find(jit_State *J, GCtrace *needle)
{
  GCtrace *T;
  for (T = trace_retired_head_acq(J);
       T != NULL;
       T = trace_retired_next_acq(T))
    if (T == needle)
      return T;
  return NULL;
}

static GCtrace *pin_trace_in_mcode_area(global_State *g, MCode *area,
					 size_t size)
{
  jit_State *J = G2J(g);
  uintptr_t lo = (uintptr_t)area;
  uintptr_t hi = lo + size;
  TraceNo i, sizetrace;
  GCtrace *pinned = NULL;
  /* The SMR reader is the independent lifetime proof required for the
  ** zero-to-one native-pin handoff. */
  lj_gc2_smr_read_enter(g);
  sizetrace = trace_sizetrace_acq(J);
  for (i = 1; i < sizetrace; i++) {
    GCtrace *T = traceref_safe(J, i);
    uintptr_t mcode;
    if (!T || !trace_runnable_acq(T, i))
      continue;
    mcode = (uintptr_t)trace_mcode_acq(T);
    if (mcode >= lo && mcode < hi && lj_trace_native_pin(T)) {
      pinned = T;
      break;
    }
  }
  lj_gc2_smr_read_leave(g);
  return pinned;
}

static int reclaim_gate_enter(global_State *g)
{
  uint32_t worker = 0;
  int sweep = gc2_phase_acq(g) == LJ_GC2_SWEEP;
  if (sweep) {
    assert(gc2_sweep_bridge_ready_acq(g) != 0);
    assert(gc2_worker_active_cas(g, &worker, 1));
    assert(lj_gc2_test_sweep_reclaim_scope_enter(g));
  } else {
    assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
    assert(lj_gc2_test_idle_reclaim_enter(g));
  }
  assert(gc2_smr_readers_acq(g) == 0);
  return sweep;
}

static void reclaim_gate_leave(global_State *g, int sweep)
{
  if (sweep) {
    lj_gc2_test_sweep_reclaim_scope_leave(g);
    gc2_worker_active_rel(g, 0);
  } else {
    lj_gc2_test_idle_reclaim_leave(g);
  }
}

static uint32_t reclaim_mcode_at(global_State *g, uint64_t epoch)
{
  jit_State *J = G2J(g);
  uint32_t n;
  int sweep = reclaim_gate_enter(g);
  assert(lj_jit_token_try(J));
  n = lj_mcode_reclaim_retired(g, epoch);
  lj_jit_token_release(J);
  reclaim_gate_leave(g, sweep);
  return n;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g;
  jit_State *J;
  MCode *oldmc;
  MCodeRetire *ret;
  GCtrace *pinned_trace;
  size_t oldmc_size, szall;
  uint64_t epoch;
#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
  FailAllocCtx alloc;
#endif

  g = G(L);
  J = G2J(g);
  assert(mcode_active_head_acq(J) == NULL);
  assert(mcode_retired_head_acq(J) == NULL);

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 40 do assert(f(120) == 7260) end\n");

  oldmc = J->mcarea;
  szall = J->szallmcarea;
  assert(oldmc != NULL);
  assert(szall != 0);
  ret = active_find(J, oldmc);
  assert(ret != NULL);
  assert(ret->retire_epoch == MCODE_RETIRE_EPOCH_ACTIVE);
  assert(mcode_retired_head_acq(J) == NULL);
  oldmc_size = ret->size;
  assert(oldmc_size != 0 && oldmc_size <= szall);
  pinned_trace = pin_trace_in_mcode_area(g, oldmc, oldmc_size);
  assert(pinned_trace != NULL);
  assert((uintptr_t)trace_mcode_acq(pinned_trace) >= (uintptr_t)oldmc);
  assert((uintptr_t)trace_mcode_acq(pinned_trace) <
	 (uintptr_t)oldmc + oldmc_size);
  assert(trace_native_pins_acq(pinned_trace) == 1u);

  /* Ordinary IDLE retire ownership is not evidence of a broken activation.
  ** Force the exact collision which used to pin NO_RECLAIM from the mcode
  ** allocation-side marker, then prove the independently rooted active list
  ** is retained by the next successful JIT root mark. */
  {
    GCArena *a = lj_arena_of(ret);
    uint32_t cell = lj_arena_cellof(ret);
    LJGC2ActivationSnap before, after;
    int sweep = reclaim_gate_enter(g);
    assert(!sweep);
    assert(gc2_smr_reclaiming_acq(g) != 0);
    assert(gc2_smr_readers_acq(g) == 0);
    assert(!lj_arena_ishuge(a));
    lj_arena_bm_clear(a->mark, cell);
    assert(!lj_arena_bm_get(a->mark, cell));
    before = lj_gc2_activation_snapshot(&g->gc2.activation);
    assert(lj_gc2_markmem_registered_publish_try(g, ret) == 0);
    after = lj_gc2_activation_snapshot(&g->gc2.activation);
    assert(lj_gc2_activation_equal(&before, &after));
    assert(gc2_smr_reclaiming_acq(g) != 0);
    assert(gc2_smr_readers_acq(g) == 0);
    assert(!lj_arena_bm_get(a->mark, cell));
    assert(!lj_gc2_activation_reclaim_veto(g));
    reclaim_gate_leave(g, sweep);
    assert(!lj_arena_bm_get(a->mark, cell));
    assert(lj_mcode_markretired(g, 1));
    assert(lj_gc2_ismarkedmem(g, ret) == 1);
  }

  /*
  ** A safepoint leader can flush while holding the global recorder token.
  ** Deny every allocator growth to prove the entire eventless flush, including
  ** machine-code retirement, is a no-throw ownership transfer.
  */
#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
  alloc.oldf = lua_getallocf(L, &alloc.oldud);
  alloc.grow_calls = 0;
  alloc.fail_grow = 1;
  lua_setallocf(L, fail_growing_alloc, &alloc);
  assert(lj_trace_flushall_gc(L) == 0);
  alloc.fail_grow = 0;
  lua_setallocf(L, alloc.oldf, alloc.oldud);
  assert(alloc.grow_calls == 0);
#else
  /* lua_setallocf is intentionally inert at this temporary GC2 boundary. */
  assert(lj_trace_flushall_gc(L) == 0);
#endif

  assert(mcode_active_head_acq(J) == NULL);
  assert(J->mcarea == NULL);
  assert(J->mctop == NULL);
  assert(J->mcbot == NULL);
  assert(J->szmcarea == 0);
  assert(J->szallmcarea == szall);
  ret = retired_find(J, oldmc);
  assert(ret != NULL);
  assert(trace_retired_find(J, pinned_trace) == pinned_trace);

  epoch = ret->retire_epoch;
  assert(epoch == g->gc2.hs_epoch);
  J->mcode_reclaim_epoch = 0;
  assert(reclaim_mcode_at(g, epoch) == 0);
  /* An entirely epoch-young list is stable for this completed generation.
  ** Record that scan and prove a duplicate call is eligible for the fast skip. */
  assert(J->mcode_reclaim_epoch == epoch);
  assert(reclaim_mcode_at(g, epoch) == 0);
  assert(J->mcode_reclaim_epoch == epoch);
  ret = retired_find(J, oldmc);
  assert(ret != NULL);
  assert(J->szallmcarea == szall);
  assert(reclaim_mcode_at(g, epoch + 1u) == 0);
  assert(J->mcode_reclaim_epoch == epoch + 1u);
  assert(reclaim_mcode_at(g, epoch + 1u) == 0);
  assert(J->mcode_reclaim_epoch == epoch + 1u);
  assert(retired_find(J, oldmc) != NULL);
  assert(reclaim_mcode_at(g, epoch + LJ_FLUSH_EPOCHS) == 0);
  /* The area is old enough now, but retired trace bodies still reference it.
  ** This transient ready-list block must not arm the same-epoch throttle: the
  ** trace drain below can remove the final reference without advancing grace. */
  assert(J->mcode_reclaim_epoch != epoch + LJ_FLUSH_EPOCHS);
  /*
  ** Retired trace bodies hold mcode pointers until their own SMR grace and
  ** the ownership root is detached (by the sweep bridge, or by IDLE reclaim).
  ** This preserves stale bytecode recovery: a patched loop or return can still
  ** need startins from the body. Exercise the applicable gated path itself.
  */
  {
    int sweep = reclaim_gate_enter(g);
    assert(lj_jit_token_try(J));
    (void)lj_trace_reclaim_retired(g, epoch + LJ_FLUSH_EPOCHS);
    assert(trace_retired_find(J, pinned_trace) == pinned_trace);
    assert(trace_native_pins_acq(pinned_trace) == 1u);
    assert(J->trace_reclaim_epoch == epoch + LJ_FLUSH_EPOCHS);
    assert(lj_mcode_reclaim_retired(g, epoch + LJ_FLUSH_EPOCHS) == 0);
    assert(J->mcode_reclaim_epoch == epoch + LJ_FLUSH_EPOCHS);
    assert(retired_find(J, oldmc) != NULL);
    lj_trace_native_unpin(g, pinned_trace);
    assert(trace_native_pins_acq(pinned_trace) == 0);
    assert(lj_trace_reclaim_retired(g, epoch + LJ_FLUSH_EPOCHS) >= 1);
    assert(lj_mcode_reclaim_retired(g, epoch + LJ_FLUSH_EPOCHS) >= 1);
    lj_jit_token_release(J);
    reclaim_gate_leave(g, sweep);
  }
  assert(mcode_retired_head_acq(J) == NULL);
  assert(J->szallmcarea == 0);

  lua_close(L);
  printf("t-jit-mcode-retire OK: no-throw mcode flush retires by epoch\n");
  return 0;
}
