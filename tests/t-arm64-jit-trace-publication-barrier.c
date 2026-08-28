/*
** Bounded GC2 checkpoint for a trace whose body and trace slot are already
** published, but whose first runnable inbound edge is still closed.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_GC2_TEST_HELPERS)

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"

#if !LJ_HASJIT || !LJ_TARGET_OSX || !LJ_TARGET_ARM64 || \
    LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED != 1
#error "trace publication barrier needs admitted ARM64 roots and closed sides"
#endif

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "trace publication barrier setup failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static GCtrace *make_root_trace(lua_State *L)
{
  jit_State *J = G2J(G(L));
  GCtrace *T;
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=20','maxtrace=2'); "
    "function __gc2_trace_publication(n) "
    "  local i=0; local x=0; "
    "  while i<n do i=i+1; x=x+i end; return x "
    "end; "
    "for k=1,6 do assert(__gc2_trace_publication(20)==210) end");
  T = traceref_safe(J, 1);
  assert(T != NULL && trace_runnable_acq(T, 1));
  assert(trace_root_acq(T) == 0 && trace_link_acq(T) == 1);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);
  return T;
}

static int trace_marked(GCtrace *T)
{
  GCArena *a = lj_arena_of(T);
  uint32_t cell = lj_arena_cellof(T);
  assert(!lj_arena_ishuge(a));
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  return lj_arena_bm_get(a->mark, cell);
}

static void trace_unmark(GCtrace *T)
{
  GCArena *a = lj_arena_of(T);
  uint32_t cell = lj_arena_cellof(T);
  assert(!lj_arena_ishuge(a));
  lj_arena_bm_clear(a->mark, cell);
  assert(!trace_marked(T));
}

static void drain_all(global_State *g)
{
  uint32_t i;
  for (i = 0; i < 4096u && !lj_gc2_test_ssb_empty(g); i++)
    (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
}

static void expect_exact_active_entry(TGState *tg, GCRef *before, GCtrace *T)
{
  assert(before != NULL);
  assert(lj_tg_ssb_next_acq(tg) == before + 1);
  assert(gcref_acq(*before) == obj2gco(T));
}

static void test_exact_slot_veto(global_State *g, TGState *tg, GCtrace *T)
{
  TraceVec *tv = tracevec_acq(G2J(g));
  GCRef *before;
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_recovery_failed_acq(g) == 0);
  assert(tv != NULL && tv->sizetrace > 2u);
  assert(gcref_acq(tv->slot[2]) != obj2gco(T));
  before = lj_tg_ssb_next_acq(tg);
  assert(lj_gc_pubtrace_checkpoint_nodrain(g, 2, T) ==
	 LJ_GC2_TRACE_PUBLISH_VETOED);
  assert(lj_tg_ssb_next_acq(tg) == before);
  assert(gc2_recovery_items_acq(g) == 0);
  assert(gc2_recovery_failed_acq(g) == 1u);

  /* Test-only cleanup after observing the exact mismatch. */
  gc2_recovery_failed_rel(g, 0);
}

static void test_idle_to_mark(global_State *g, TGState *tg, GCtrace *T)
{
  GC2SSBNode *active, *held;
  GCRef *before;
  uint64_t drained_before, published_before;
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_recovery_failed_acq(g) == 0);
  held = lj_tg_ssb_free_pop(tg);
  assert(held != NULL && lj_tg_ssb_free_acq(tg) == NULL);
  active = lj_tg_ssb_active_acq(tg);
  before = lj_tg_ssb_next_acq(tg);
  assert(lj_gc_pubtrace_checkpoint_nodrain(g, 1, T) ==
	 LJ_GC2_TRACE_PUBLISH_QUEUED);
  expect_exact_active_entry(tg, before, T);
  published_before = gc2_ssb_items_published_acq(g);
  drained_before = gc2_ssb_items_drained_acq(g);

  /* MARK start preserves the IDLE SSB suffix instead of resetting it. The
  ** worker may consume it during the transition or in this explicit drain. */
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(lj_tg_ssb_active_acq(tg) == active);
  expect_exact_active_entry(tg, before, T);
  assert(gc2_ssb_items_published_acq(g) == published_before);
  assert(gc2_ssb_items_drained_acq(g) == drained_before);
  lj_tg_ssb_free_push(tg, held);
  drain_all(g);
  assert(trace_marked(T));
  lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
}

static void test_mark_active(global_State *g, TGState *tg, GCtrace *T)
{
  GCRef *before;
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  drain_all(g);
  trace_unmark(T);

  before = lj_tg_ssb_next_acq(tg);
  assert(lj_gc_pubtrace_checkpoint_nodrain(g, 1, T) ==
	 LJ_GC2_TRACE_PUBLISH_QUEUED);
  expect_exact_active_entry(tg, before, T);
  drain_all(g);
  assert(trace_marked(T));
  lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
}

static void test_full_ssb_veto(global_State *g, TGState *tg, GCtrace *T)
{
  GC2SSBNode *active, *held;
  GCRef *base, *end;
  uint32_t n = 0;

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(lj_gc2_test_ssb_empty(g));
  held = lj_tg_ssb_free_pop(tg);
  assert(held != NULL && lj_tg_ssb_free_acq(tg) == NULL);
  active = lj_tg_ssb_active_acq(tg);
  base = lj_tg_ssb_base_acq(tg);
  end = lj_tg_ssb_end_acq(tg);
  assert(active != NULL && base == active->slot);
  assert(end == base + TG_GC2_SSB_SLOTS);

  while (lj_tg_ssb_next_acq(tg) != end) {
    assert(n++ < TG_GC2_SSB_SLOTS);
    assert(lj_gc2_test_ssb_push(g, obj2gco(T)) == 1);
  }
  assert(lj_gc_pubtrace_checkpoint_nodrain(g, 1, T) ==
	 LJ_GC2_TRACE_PUBLISH_VETOED);
  assert(lj_tg_ssb_active_acq(tg) == active);
  assert(lj_tg_ssb_next_acq(tg) == end);
  assert(gc2_recovery_items_acq(g) == 0);
  assert(gc2_recovery_failed_acq(g) == 1u);

  /* The sticky recovery lane is itself a close veto. It does not need a
  ** synthetic queue identity, a drain, or the retrying activation repair. */
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  lj_gc2_mark_to_weak(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);

  /* Test-only cleanup after the veto has been observed. Production never
  ** clears this sticky lane. Clearing it makes the same direct phase attempt
  ** succeed, proving the first rejection was the recovery veto itself. */
  gc2_recovery_failed_rel(g, 0);
  lj_gc2_mark_to_weak(g);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  lj_tg_ssb_free_push(tg, held);
  drain_all(g);
  lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtrace *T;
  assert(L != NULL);
  luaL_openlibs(L);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL && tg->gl == g);
  T = make_root_trace(L);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);

  test_exact_slot_veto(g, tg, T);
  test_idle_to_mark(g, tg, T);
  test_mark_active(g, tg, T);
  test_full_ssb_veto(g, tg, T);

  run_lua(L, "jit.flush()");
  lua_close(L);
  printf("t-arm64-jit-trace-publication-barrier OK: queued IDLE-to-MARK "
	 "preservation, active-MARK traversal and bounded sticky veto\n");
  return 0;
}

#else

int main(void)
{
  printf("t-arm64-jit-trace-publication-barrier SKIP\n");
  return 0;
}

#endif
