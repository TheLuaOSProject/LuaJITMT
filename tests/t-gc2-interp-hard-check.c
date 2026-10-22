/*
** Focused test for interpreter GC2 hard-threshold checks.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tg.h"

#include "lib/lua_fixture_helpers.h"

static void assert_loaded_chunk_has_op(lua_State *L, BCOp op)
{
  GCfunc *fn;
  GCproto *pt;
  BCPos i;

  assert(tvisfunc(L->top - 1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  for (i = 0; i < pt->sizebc; i++) {
    if (bc_op(proto_bc(pt)[i]) == op)
      return;
  }
  fputs("loaded GC2 fixture lacks required bytecode\n", stderr);
  assert(0);
}

static void arm_gc2_hard_mark(global_State *g)
{
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  /* The explicit cycle-start request makes the ordinary threshold due. Reset
  ** it afterwards: these fixtures isolate the hard cadence from threshold GC. */
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  la_store64_rel(&g->gc2.hard_bytes, 1);
  la_store32_rel(&g->gc2.assist_shift, 0);
  la_store64_rel(&g->gc2.alloc_since_trigger, 2);
}

static void arm_gc2_normal_hard_mark(global_State *g)
{
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  la_store64_rel(&g->gc2.hard_bytes, 2u * LJ_GC2_ACCT_FLUSH);
  la_store32_rel(&g->gc2.assist_shift, 0);
  la_store64_rel(&g->gc2.alloc_since_trigger,
		 2u * LJ_GC2_ACCT_FLUSH + 1u);
}

static void finish_gc2_mark(lua_State *L, global_State *g)
{
  /* A threshold step may leave fair MARK-close intent pending. The preserving
  ** cycle_to_idle abort can then refuse; use the real completion driver and
  ** prove that the next hard-assist case starts from a completed cycle. */
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_mark_close_intent_acq(g) == 0);
  g->gc.state = GCSpause;
  lj_gc_threshold_store(g, g->gc.total + 4u * LJ_GC2_ACCT_FLUSH);
}

static void test_hard_only_helper(lua_State *L, global_State *g)
{
  uint64_t interp_checks0, assist_runs0;
  uint8_t color_state0;

  color_state0 = g->gc.state;
  arm_gc2_hard_mark(g);
  interp_checks0 = gc2_interp_hard_checks_acq(g);
  assist_runs0 = gc2_assist_runs_acq(g);

  lj_gc_step_fixtop(L);

  if (gc2_interp_hard_checks_acq(g) <= interp_checks0) {
    fputs("hard-only helper did not enter the GC2 hard check\n", stderr);
    assert(0);
  }
  if (gc2_assist_runs_acq(g) <= assist_runs0) {
    fputs("hard-only helper did not run the GC2 hard assist\n", stderr);
    assert(0);
  }
  if (g->gc.state != color_state0) {
    fprintf(stderr, "hard-only helper moved color GC state %u -> %u\n",
	    (unsigned)color_state0, (unsigned)g->gc.state);
    assert(0);
  }
  finish_gc2_mark(L, g);
}

static void test_normal_hard_tnew_batch_gate(lua_State *L, global_State *g)
{
  uint64_t interp_checks0, assist_runs0;
  uint32_t fixtop_calls0;

  ljt_lua_loadstring(L,
    "local x = {}\n"
    "assert(type(x) == 'table')\n");
  arm_gc2_normal_hard_mark(g);
  lj_tg_local_total_xchg_acqrel(L2TG(L), 0);
  lj_gc_test_reset_step_fixtop_calls();
  fixtop_calls0 = lj_gc_test_step_fixtop_calls();
  interp_checks0 = gc2_interp_hard_checks_acq(g);
  assist_runs0 = gc2_assist_runs_acq(g);

  ljt_lua_pcall(L, 0, 0, "lua_pcall");

  if (lj_gc_test_step_fixtop_calls() != fixtop_calls0) {
    fputs("normal hard-only TNEW entered the fixtop helper before local batch debt\n",
	  stderr);
    assert(0);
  }
  if (gc2_interp_hard_checks_acq(g) != interp_checks0) {
    fputs("normal hard-only TNEW ran an interpreter hard check before local batch debt\n",
	  stderr);
    assert(0);
  }
  if (gc2_assist_runs_acq(g) != assist_runs0) {
    fputs("normal hard-only TNEW assisted before local batch debt\n", stderr);
    assert(0);
  }
  finish_gc2_mark(L, g);
  lj_tg_local_total_xchg_acqrel(L2TG(L), 0);
}

static void test_normal_hard_tnew_batch_boundary(lua_State *L,
					 global_State *g)
{
  uint64_t interp_checks0, assist_runs0;
  uint32_t fixtop_calls0;

  ljt_lua_loadstring(L,
    "local x = {}\n"
    "assert(type(x) == 'table')\n");
  arm_gc2_normal_hard_mark(g);
  lj_tg_local_total_xchg_acqrel(L2TG(L), LJ_GC2_ACCT_FLUSH);
  lj_gc_test_reset_step_fixtop_calls();
  fixtop_calls0 = lj_gc_test_step_fixtop_calls();
  interp_checks0 = gc2_interp_hard_checks_acq(g);
  assist_runs0 = gc2_assist_runs_acq(g);

  ljt_lua_pcall(L, 0, 0, "lua_pcall");

  if (lj_gc_test_step_fixtop_calls() <= fixtop_calls0) {
    fputs("normal hard-only TNEW skipped the local batch boundary\n", stderr);
    assert(0);
  }
  if (gc2_interp_hard_checks_acq(g) <= interp_checks0) {
    fputs("normal hard-only TNEW skipped the boundary hard check\n", stderr);
    assert(0);
  }
  if (gc2_assist_runs_acq(g) <= assist_runs0) {
    fputs("normal hard-only TNEW skipped the boundary assist\n", stderr);
    assert(0);
  }
  finish_gc2_mark(L, g);
  lj_tg_local_total_xchg_acqrel(L2TG(L), 0);
}

static void test_hard_only_tdup(lua_State *L, global_State *g)
{
  uint64_t interp_checks0, assist_runs0;
  uint32_t fixtop_calls0;
  uint8_t color_state0;

  ljt_lua_loadstring(L,
    "local x = { marker = 17 }\n"
    "assert(x.marker == 17)\n");
  assert_loaded_chunk_has_op(L, BC_TDUP);
  color_state0 = g->gc.state;
  arm_gc2_hard_mark(g);
  lj_gc_test_reset_step_fixtop_calls();
  fixtop_calls0 = lj_gc_test_step_fixtop_calls();
  interp_checks0 = gc2_interp_hard_checks_acq(g);
  assist_runs0 = gc2_assist_runs_acq(g);

  ljt_lua_pcall(L, 0, 0, "lua_pcall");

  if (lj_gc_test_step_fixtop_calls() <= fixtop_calls0) {
    fputs("interpreted hard-only TDUP skipped the fixtop helper\n", stderr);
    assert(0);
  }
  if (gc2_interp_hard_checks_acq(g) <= interp_checks0) {
    fputs("interpreted hard-only TDUP did not enter the GC2 hard check\n",
	  stderr);
    assert(0);
  }
  if (gc2_assist_runs_acq(g) <= assist_runs0) {
    fputs("interpreted hard-only TDUP did not run the GC2 hard assist\n",
	  stderr);
    assert(0);
  }
  if (g->gc.state != color_state0) {
    fprintf(stderr, "hard-only TDUP moved color GC state %u -> %u\n",
	    (unsigned)color_state0, (unsigned)g->gc.state);
    assert(0);
  }
  finish_gc2_mark(L, g);
}

static void test_hard_only_c_check(lua_State *L, global_State *g)
{
  uint64_t interp_checks0, assist_runs0;
  uint8_t color_state0;

  color_state0 = g->gc.state;
  arm_gc2_hard_mark(g);
  interp_checks0 = gc2_interp_hard_checks_acq(g);
  assist_runs0 = gc2_assist_runs_acq(g);

  lj_gc_check(L);

  if (gc2_interp_hard_checks_acq(g) <= interp_checks0) {
    fputs("hard-only C lj_gc_check did not enter the GC2 hard check\n",
	  stderr);
    assert(0);
  }
  if (gc2_assist_runs_acq(g) <= assist_runs0) {
    fputs("hard-only C lj_gc_check did not run the GC2 hard assist\n",
	  stderr);
    assert(0);
  }
  if (g->gc.state != color_state0) {
    fprintf(stderr, "hard-only C lj_gc_check moved color GC state %u -> %u\n",
	    (unsigned)color_state0, (unsigned)g->gc.state);
    assert(0);
  }
  finish_gc2_mark(L, g);
}

static void test_hard_only_fastfunc(lua_State *L, global_State *g)
{
  uint64_t interp_checks0, assist_runs0;
  uint8_t color_state0;

  ljt_lua_loadstring(L,
    "local x = string.char(65)\n"
    "assert(x == 'A')\n");
  color_state0 = g->gc.state;
  arm_gc2_hard_mark(g);
  interp_checks0 = gc2_interp_hard_checks_acq(g);
  assist_runs0 = gc2_assist_runs_acq(g);

  ljt_lua_pcall(L, 0, 0, "lua_pcall");

  if (gc2_interp_hard_checks_acq(g) <= interp_checks0) {
    fputs("hard-only fast function did not enter the GC2 hard check\n",
	  stderr);
    assert(0);
  }
  if (gc2_assist_runs_acq(g) <= assist_runs0) {
    fputs("hard-only fast function did not run the GC2 hard assist\n",
	  stderr);
    assert(0);
  }
  if (g->gc.state != color_state0) {
    fprintf(stderr, "hard-only fast function moved color GC state %u -> %u\n",
	    (unsigned)color_state0, (unsigned)g->gc.state);
    assert(0);
  }
  finish_gc2_mark(L, g);
}

/* The request token is durable after allocation accounting consumes its
** local debt. The VM must still visit a GC-safe helper, unless public STOP
** or an owned finalizer pause suppresses automatic admission. */
static void test_pending_idle_request(lua_State *L, global_State *g)
{
  static const char *const chunks[] = {
    "return {}", "return {1, 2, 3}", "return string.char(41)"
  };
  static const uint8_t paused[] = {
    0, LJ_GC_AUTO_STOPPED, LJ_GC_AUTO_FINPAUSE
  };
  unsigned i, j;
  for (i = 0; i < sizeof(chunks)/sizeof(chunks[0]); i++) {
    for (j = 0; j < sizeof(paused)/sizeof(paused[0]); j++) {
      uint32_t cycle;
      ljt_lua_loadstring(L, chunks[i]);
      if (i < 2) assert_loaded_chunk_has_op(L, i ? BC_TDUP : BC_TNEW);
      finish_gc2_mark(L, g);
      assert(lj_gc2_request_cycle_pressure(g, L2TG(L)));
      assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
      assert(gc2_cycle_leader_acq(g) != 0);
      cycle = gc2_cycle_acq(g);
      lj_gc_threshold_store(g, LJ_MAX_MEM);
      la_store64_rel(&g->gc2.hard_bytes, ~(uint64_t)0);
      la_store64_rel(&g->gc2.alloc_since_trigger, 0);
      lj_tg_local_total_xchg_acqrel(L2TG(L), 0);
      /* Synthetic flag state isolates both independent suppression bits. */
      la_store8_rel(&g->gc.auto_flags, paused[j]);
      lj_gc_test_reset_step_fixtop_calls();
      ljt_lua_pcall(L, 0, 0, "pending IDLE request");
      if (paused[j]) {
        assert(gc2_cycle_acq(g) == cycle);
        assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
        assert(gc2_cycle_leader_acq(g) != 0);
        assert(lj_gc_test_step_fixtop_calls() == 0);
      } else {
        if (gc2_cycle_acq(g) == cycle) {
          fputs("pending IDLE request did not enter a GC-safe VM helper\n",
                stderr);
          assert(0);
        }
        if (i < 2) assert(lj_gc_test_step_fixtop_calls() != 0);
      }
      la_store8_rel(&g->gc.auto_flags, 0);
      finish_gc2_mark(L, g);
    }
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  uint64_t interp_checks0, assist_runs0;
  uint8_t color_state0;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  assert(g != NULL);
  assert(gc2_interp_hard_checks_acq(g) == 0);

  ljt_lua_dostring(L, "if jit then jit.off() end\n");
  test_pending_idle_request(L, g);
  test_normal_hard_tnew_batch_gate(L, g);
  test_normal_hard_tnew_batch_boundary(L, g);
  test_hard_only_tdup(L, g);

  ljt_lua_loadstring(L,
    "local x = {}\n"
    "assert(type(x) == 'table')\n");

  arm_gc2_hard_mark(g);
  interp_checks0 = gc2_interp_hard_checks_acq(g);
  assist_runs0 = gc2_assist_runs_acq(g);

  lj_gc_threshold_store(g, g->gc.total);
  ljt_lua_pcall(L, 0, 0, "lua_pcall");

  if (gc2_interp_hard_checks_acq(g) <= interp_checks0) {
    fputs("interpreted TNEW did not enter the GC2 hard check\n", stderr);
    assert(0);
  }
  if (gc2_assist_runs_acq(g) <= assist_runs0) {
    fputs("interpreted TNEW did not run the GC2 hard assist\n", stderr);
    assert(0);
  }

  finish_gc2_mark(L, g);

  ljt_lua_loadstring(L,
    "local x = {}\n"
    "assert(type(x) == 'table')\n");
  color_state0 = g->gc.state;
  arm_gc2_hard_mark(g);
  interp_checks0 = gc2_interp_hard_checks_acq(g);
  assist_runs0 = gc2_assist_runs_acq(g);

  ljt_lua_pcall(L, 0, 0, "lua_pcall");

  if (gc2_interp_hard_checks_acq(g) <= interp_checks0) {
    fputs("interpreted hard-only TNEW did not enter the GC2 hard check\n",
	  stderr);
    assert(0);
  }
  if (gc2_assist_runs_acq(g) <= assist_runs0) {
    fputs("interpreted hard-only TNEW did not run the GC2 hard assist\n",
	  stderr);
    assert(0);
  }
  if (g->gc.state != color_state0) {
    fprintf(stderr, "interpreted hard-only TNEW moved color GC state %u -> %u\n",
	    (unsigned)color_state0, (unsigned)g->gc.state);
    assert(0);
  }
  finish_gc2_mark(L, g);

  test_hard_only_fastfunc(L, g);
  test_hard_only_helper(L, g);
  test_hard_only_c_check(L, g);
  lua_close(L);
  puts("t-gc2-interp-hard-check OK: interpreter hard checks enter GC2 assist");
  return 0;
}
