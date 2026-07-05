/*
** Focused instrumentation fixture for recursive call-unroll trace retention.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#ifndef LJ_TRACE_TEST_HELPERS
#error "t-jit-recursive-retention requires LJ_TRACE_TEST_HELPERS"
#endif

static uint32_t live_trace_count(jit_State *J, uint32_t *returnsp)
{
  TraceNo i, sizetrace = (TraceNo)trace_sizetrace_acq(J);
  uint32_t live = 0, returns = 0;
  for (i = 1; i < sizetrace; i++) {
    GCtrace *T = traceref(J, i);
    if (T && trace_traceno_acq(T) == i) {
      live++;
      if (trace_linktype_acq(T) == LJ_TRLINK_RETURN)
	returns++;
    }
  }
  *returnsp = returns;
  return live;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  jit_State *J = G2J(G(L));
  uint32_t live, returns;
  uint32_t aborts, linked, unlinks, return_unlinks;
  uint32_t findfree, reuses, grows;

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=56', 'hotexit=10')\n");

  lj_trace_test_reset_retention_stats();

  ljt_lua_dostring(L,
    "local function fib(n)\n"
    "  if n < 2 then return n end\n"
    "  return fib(n - 1) + fib(n - 2)\n"
    "end\n"
    "assert(fib(30) == 832040)\n"
    "assert(fib(30) == 832040)\n"
    "for _ = 1, 4 do assert(fib(24) == 46368) end\n"
    "for _ = 1, 8 do assert(fib(30) == 832040) end\n");

  aborts = lj_trace_test_call_unroll_aborts();
  linked = lj_trace_test_call_unroll_linked();
  unlinks = lj_trace_test_flush_unlink_calls();
  return_unlinks = lj_trace_test_flush_unlink_returns();
  findfree = lj_trace_test_findfree_calls();
  reuses = lj_trace_test_findfree_reuses();
  grows = lj_trace_test_findfree_grows();
  live = live_trace_count(J, &returns);

  assert(aborts > 0);
  assert(linked == aborts);
  assert(unlinks >= aborts);
  assert(return_unlinks > 0);
  assert(lj_trace_test_slot_release_clears() > 0);
  assert(findfree > 0);
  assert(reuses + grows == findfree);
  assert(lj_trace_test_last_unlinked() > 0);
  assert(lj_trace_test_last_findfree() > 0);
  assert(live > 0);
  assert(returns < return_unlinks);

  printf("t-jit-recursive-retention OK: aborts=%u unlinks=%u "
	 "return_unlinks=%u selflinks=%u slot_clears=%u live=%u "
	 "returns=%u findfree=%u reuse=%u grow=%u\n",
	 aborts, unlinks, return_unlinks, lj_trace_test_abort_selflinks(),
	 lj_trace_test_slot_release_clears(), live, returns, findfree, reuses,
	 grows);

  lua_close(L);
  return 0;
}
