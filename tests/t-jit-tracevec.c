/*
** Focused regression test for JIT trace-vector RCU growth and SMR retirement.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

static int retired_has(jit_State *J, TraceVec *needle)
{
  TraceVec *tv;
  for (tv = tracevec_retired_head_acq(J);
       tv != NULL;
       tv = tracevec_retired_next_acq(tv))
    if (tv == needle)
      return 1;
  return 0;
}

static uint32_t reclaim_trace_at(global_State *g, uint64_t epoch)
{
  jit_State *J = G2J(g);
  uint32_t expect = 0;
  uint32_t worker = 0;
  uint32_t n;
  int sweep = gc2_phase_acq(g) == LJ_GC2_SWEEP;
  if (sweep) {
    assert(gc2_sweep_bridge_ready_acq(g) != 0);
    assert(gc2_worker_active_cas(g, &worker, 1));
  } else {
    assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  }
  assert(gc2_smr_reclaiming_cas(g, &expect, 1));
  assert(lj_jit_token_try(J));
  n = lj_trace_reclaim_retired(g, epoch);
  lj_jit_token_release(J);
  gc2_smr_reclaiming_rel(g, 0);
  if (sweep)
    gc2_worker_active_rel(g, 0);
  return n;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g;
  jit_State *J;
  TraceVec *oldtv, *newtv;
  MSize live = 0;
  uint64_t epoch;

  g = G(L);
  J = G2J(g);
  /* Keep the retirement window under this fixture's control. An automatic
  ** cycle may otherwise complete a grace period between Lua trace growth and
  ** the following C assertion, legitimately reclaiming oldtv before the test
  ** gets a chance to inspect the retired list. */
  (void)lua_gc(L, LUA_GCSTOP, 0);
  assert(J->tracev == NULL);
  assert(tracevec_retired_head_acq(J) == NULL);

  ljt_lua_dostring(L,
    "jit.flush()\n"
    /* Leave room for the root plus ordinary interpreter/stitch side traces.
    ** maxtrace=2 can legitimately flush its whole cache while this diagnostic
    ** walks jit.util, making the vector-growth fixture timing-dependent. */
    "jit.opt.start('hotloop=1', 'hotexit=1', 'maxtrace=4')\n"
    "local function f1(n)\n"
    "  local s = 0 for i = 1, n do s = s + i end return s\n"
    "end\n"
    "for _ = 1, 20 do assert(f1(80) == 3240) end\n");

  oldtv = tracevec_acq(J);
  assert(oldtv != NULL);
  assert(oldtv->sizetrace == 5);
  for (live = 1; live < oldtv->sizetrace; live++)
    if (traceref_safe(J, (TraceNo)live) != NULL)
      break;
  assert(live < oldtv->sizetrace);
  assert(trace_sizetrace_acq(J) == oldtv->sizetrace);
  trace_sizetrace_rel(J, oldtv->sizetrace + 8u);
  assert(traceref(J, oldtv->sizetrace) == NULL);
  trace_sizetrace_rel(J, oldtv->sizetrace);
  assert(tracevec_retired_head_acq(J) == NULL);

  ljt_lua_dostring(L,
    "jit.opt.start('maxtrace=20')\n"
    "for k = 3, 12 do\n"
    "  local src = ('return function(n) local s=0; for i=1,n do ' ..\n"
    "    's=s+i+%d end return s end'):format(k)\n"
    "  local f = assert(loadstring(src))()\n"
    "  for _ = 1, 20 do f(80) end\n"
    "end\n");

  newtv = tracevec_acq(J);
  assert(newtv != NULL);
  assert(newtv != oldtv);
  assert(newtv->sizetrace == 21);
  assert(trace_sizetrace_acq(J) == newtv->sizetrace);
  assert(retired_has(J, oldtv));

  epoch = oldtv->retire_epoch;
  assert(epoch <= g->gc2.hs_epoch);
  assert(reclaim_trace_at(g, epoch) == 0);
  assert(retired_has(J, oldtv));
  assert(reclaim_trace_at(g, epoch + 1u) >= 1);
  assert(tracevec_retired_head_acq(J) == NULL);

  lua_close(L);
  printf("t-jit-tracevec OK: trace vector grows by RCU and retires by epoch\n");
  return 0;
}
