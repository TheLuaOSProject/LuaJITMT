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

static void reset_jit(lua_State *L)
{
  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=56', 'hotexit=10')\n");
  lj_trace_test_reset_retention_stats();
}

static void check_retention_stats(lua_State *L, const char *label,
				  uint32_t max_aborts, uint32_t max_returns)
{
  jit_State *J = G2J(G(L));
  uint32_t live, returns;
  uint32_t aborts, linked, unlinks, return_unlinks;
  uint32_t slot_release_calls, slot_release_clears;
  uint32_t findfree, reuses, grows;

  aborts = lj_trace_test_call_unroll_aborts();
  linked = lj_trace_test_call_unroll_linked();
  unlinks = lj_trace_test_flush_unlink_calls();
  return_unlinks = lj_trace_test_flush_unlink_returns();
  slot_release_calls = lj_trace_test_slot_release_calls();
  slot_release_clears = lj_trace_test_slot_release_clears();
  findfree = lj_trace_test_findfree_calls();
  reuses = lj_trace_test_findfree_reuses();
  grows = lj_trace_test_findfree_grows();
  live = live_trace_count(J, &returns);

  printf("t-jit-recursive-retention %s: aborts=%u unlinks=%u "
	 "return_unlinks=%u selflinks=%u slot_calls=%u slot_clears=%u live=%u "
	 "returns=%u findfree=%u reuse=%u grow=%u\n",
	 label, aborts, unlinks, return_unlinks,
	 lj_trace_test_abort_selflinks(), slot_release_calls,
	 slot_release_clears, live, returns, findfree, reuses, grows);
  fflush(stdout);

  assert(aborts > 0);
  assert(aborts <= max_aborts);
  assert(linked == aborts);
  assert(unlinks >= aborts);
  assert(return_unlinks > 0);
  /*
  ** Call-unroll return traces must keep their public slots long enough for the
  ** recursive recorder to move past transient RET roots. A later full GC may
  ** release those unlinked traces, but if no release has happened yet they
  ** should still appear as retained return traces.
  */
  assert(slot_release_clears <= slot_release_calls);
  if (slot_release_clears == 0)
    assert(returns >= return_unlinks);
  else
    assert(slot_release_clears <= return_unlinks);
  assert(lj_trace_test_abort_selflinks() == 0);
  assert(findfree > 0);
  assert(reuses > 0);
  assert(reuses + grows == findfree);
  assert(lj_trace_test_last_unlinked() > 0);
  assert(lj_trace_test_last_findfree() > 0);
  assert(live > 0);
  assert(returns <= max_returns);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();

  reset_jit(L);
  ljt_lua_dostring(L,
    "local function fib(n)\n"
    "  if n < 2 then return n end\n"
    "  return fib(n - 1) + fib(n - 2)\n"
    "end\n"
    "assert(fib(30) == 832040)\n"
    "assert(fib(30) == 832040)\n"
    "for _ = 1, 4 do assert(fib(24) == 46368) end\n"
    "for _ = 1, 8 do assert(fib(30) == 832040) end\n");
  check_retention_stats(L, "static-fib", 32, 32);

  reset_jit(L);
  ljt_lua_dostring(L,
    "local function runfib()\n"
    "  collectgarbage('collect')\n"
    "  local function fib(n)\n"
    "    if n < 2 then return n end\n"
    "    return fib(n - 1) + fib(n - 2)\n"
    "  end\n"
    "  return fib(30)\n"
    "end\n"
    "for _ = 1, 5 do assert(runfib() == 832040) end\n");
  /* This mirrors aux/bench/bench.lua fib30: a nested recursive closure is
  ** rebuilt around each measured run. The limit leaves room for hotcount
  ** variance, but catches the old unbounded return-trace retry loop.
  */
  check_retention_stats(L, "bench-fib", 32, 48);

  lua_close(L);
  return 0;
}
