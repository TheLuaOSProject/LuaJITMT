/*
** Focused test for x64 interpreter GC2 hard-threshold checks.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tg.h"

#include "lib/lua_fixture_helpers.h"


static void diagnose_phase(global_State *g, const char *name, unsigned line)
{
  TGState *tg=G2TG(g);
  fprintf(stderr,"snapshot %s line=%u phase=%u cycle=%u request=%u requests=%llu starts=%llu major=%llu force=%u threshold=%llu total=%llu debt=%llu hard=%llu checks=%llu assists=%llu sweep=%u/%u/%u mark_active=%u gen=%u minor=%u filtered=%llu pushed=%llu\n",
    name,line,gc2_phase_acq(g),gc2_cycle_acq(g),gc2_cycle_leader_acq(g),
    (unsigned long long)gc2_cycle_requests_acq(g),(unsigned long long)gc2_cycle_starts_acq(g),
    (unsigned long long)gc2_major_cycle_starts_acq(g),la_load32_acq(&g->gc2.force_major),
    (unsigned long long)lj_gc_threshold_load(g),(unsigned long long)lj_gc_total_load(g),
    (unsigned long long)la_load64_acq(&g->gc2.alloc_since_trigger),(unsigned long long)la_load64_acq(&g->gc2.hard_bytes),
    (unsigned long long)gc2_interp_hard_checks_acq(g),(unsigned long long)gc2_assist_runs_acq(g),
    gc2_sweep_bridge_ready_acq(g),gc2_sweep_root_done_acq(g),gc2_sweep_grace_needed_acq(g),
    tg?lj_tg_mark_active_acq(tg):0,gc2_generational_acq(g),gc2_minor_sweep_enabled_acq(g),
    (unsigned long long)gc2_remembered_filtered_acq(g),(unsigned long long)gc2_remembered_pushed_acq(g));
}

static void arm_gc2_hard_mark(global_State *g)
{
  diagnose_phase(g,"before-mark-begin",__LINE__);
  lj_gc2_mark_begin(g);
  diagnose_phase(g,"after-mark-begin",__LINE__);
  /* The explicit cycle-start request makes the ordinary threshold due. Reset
  ** it afterwards: these fixtures isolate the hard cadence from threshold GC. */
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  la_store64_rel(&g->gc2.hard_bytes, 1);
  la_store32_rel(&g->gc2.assist_shift, 0);
  la_store64_rel(&g->gc2.alloc_since_trigger, 2);
}

static void arm_gc2_normal_hard_mark(global_State *g)
{
  diagnose_phase(g,"before-mark-begin",__LINE__);
  lj_gc2_mark_begin(g);
  diagnose_phase(g,"after-mark-begin",__LINE__);
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  la_store64_rel(&g->gc2.hard_bytes, 2u * LJ_GC2_ACCT_FLUSH);
  la_store32_rel(&g->gc2.assist_shift, 0);
  la_store64_rel(&g->gc2.alloc_since_trigger,
		 2u * LJ_GC2_ACCT_FLUSH + 1u);
}

static void finish_gc2_mark(global_State *g)
{
  diagnose_phase(g,"before-cycle-to-idle",__LINE__);
  lj_gc2_cycle_to_idle(g);
  diagnose_phase(g,"after-cycle-to-idle",__LINE__);
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
  finish_gc2_mark(g);
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

  diagnose_phase(g,"before-pcall",__LINE__);
  ljt_lua_pcall(L, 0, 0, "lua_pcall");
  diagnose_phase(g,"after-pcall",__LINE__);

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
  finish_gc2_mark(g);
  lj_tg_local_total_xchg_acqrel(L2TG(L), 0);
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
  finish_gc2_mark(g);
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

  diagnose_phase(g,"before-pcall",__LINE__);
  ljt_lua_pcall(L, 0, 0, "lua_pcall");
  diagnose_phase(g,"after-pcall",__LINE__);

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
  finish_gc2_mark(g);
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
  test_normal_hard_tnew_batch_gate(L, g);

  ljt_lua_loadstring(L,
    "local x = {}\n"
    "assert(type(x) == 'table')\n");

  arm_gc2_hard_mark(g);
  interp_checks0 = gc2_interp_hard_checks_acq(g);
  assist_runs0 = gc2_assist_runs_acq(g);

  lj_gc_threshold_store(g, g->gc.total);
  diagnose_phase(g,"before-pcall",__LINE__);
  ljt_lua_pcall(L, 0, 0, "lua_pcall");
  diagnose_phase(g,"after-pcall",__LINE__);

  if (gc2_interp_hard_checks_acq(g) <= interp_checks0) {
    fputs("interpreted TNEW did not enter the GC2 hard check\n", stderr);
    assert(0);
  }
  if (gc2_assist_runs_acq(g) <= assist_runs0) {
    fputs("interpreted TNEW did not run the GC2 hard assist\n", stderr);
    assert(0);
  }

  finish_gc2_mark(g);

  ljt_lua_loadstring(L,
    "local x = {}\n"
    "assert(type(x) == 'table')\n");
  color_state0 = g->gc.state;
  arm_gc2_hard_mark(g);
  interp_checks0 = gc2_interp_hard_checks_acq(g);
  assist_runs0 = gc2_assist_runs_acq(g);

  diagnose_phase(g,"before-pcall",__LINE__);
  ljt_lua_pcall(L, 0, 0, "lua_pcall");
  diagnose_phase(g,"after-pcall",__LINE__);

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
  finish_gc2_mark(g);

  test_hard_only_fastfunc(L, g);
  test_hard_only_helper(L, g);
  test_hard_only_c_check(L, g);
  lua_close(L);
  puts("t-gc2-interp-hard-check OK: interpreter hard checks enter GC2 assist");
  return 0;
}
