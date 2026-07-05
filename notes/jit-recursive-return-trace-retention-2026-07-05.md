# JIT Recursive Return Trace Retention

2026-07-05

- Current `v2.1` no longer reproduces the old endless `TRACE 1`
  re-recording failure on `fib30`. Both `tests/stock/bench/recursive-fib.lua`
  and `aux/bench/bench.lua fib30` finish and publish an up-recursion trace.
- There is still a trace-shape/performance gap against stock: the fork records
  and retains more early `LJ_TRLINK_RETURN` roots before the up-recursion graph
  stabilizes. A typical direct `fib(30)` probe showed several live return roots
  before the up-recursion root, while stock tends to reuse earlier slots sooner.
- The relevant path is `check_call_unroll()` calling
  `lj_trace_flush_unlink()`. That unlink is intentional: replacing it with
  scoped retirement reopens the older correctness bug where `trace_abort()`
  cannot see the return trace and install the stock blacklist edge.
- A local experiment added an `unlink+retire return` helper and tried both:
  retiring only return-start aborts, and retiring every unlinked return trace.
  The focused gates still passed, but `-jv aux/bench/bench.lua fib30` continued
  allocating fresh return-trace numbers during the abort burst, and timing did
  not improve. The experiment was reverted.
- GDB confirmed the helper was reached with a root return trace and a
  single-thread/no-worker state, and that the immediate release path restored
  `J->freetrace`. The remaining gap is therefore not just "forgot to clear the
  slot"; it likely involves the surrounding abort/retry/penalty sequence and
  when subsequent recording starts relative to the slot release.

Useful commands:

- `LUA=luajit tools/ci/lua_test.sh m6_jit_recursive_call_unroll`
- `LUA=luajit tools/ci/lua_test.sh m6_jit_flush_hs`
- `env LUA_PATH="$PWD/src/?.lua;$PWD/src/?/init.lua;;" src/luajit -jv aux/bench/bench.lua fib30`

Next attempt:

- Instrument or expose a focused test-helper counter for call-unroll aborts,
  return-trace unlink, self-link blacklist, slot release, and immediate next
  `trace_findfree()` choice. Fix the trace-number reuse only after that sequence
  is observable in one C fixture.
