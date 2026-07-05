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

Follow-up fix:

- Added `LJ_TRACE_TEST_HELPERS` telemetry plus
  `tests/t-jit-recursive-retention.c`. The first measurement showed the
  workload had `return_unlinks=55`, `abort_selflinks=0`, `slot_clears=0`, and
  55 live return traces after warmup. In other words, the call-unroll aborts
  were not using the root stitched-call self-link path that needs the old return
  trace to stay in its public slot.
- `check_call_unroll()` now keeps the unlinked return trace only for the exact
  `trace_abort()` branch that can install the stock self-link blacklist:
  root recording, non-return start bytecode, and a nonzero invoking trace. Other
  call-unroll aborts use `lj_trace_flush_unlink_retire_return()`, which unlinks
  the return trace and retires its slot through the normal trace SMR path.
- The focused fixture now reports `slot_clears=9`, `returns=0`, and no
  self-link use for the recursive workload. A `-jv aux/bench/bench.lua fib30`
  sample showed trace slot 3 reused for the up-recursion trace after two
  call-unroll aborts, instead of retaining a run of live return roots.
- Follow-up coverage tightened `tests/t-jit-recursive-retention.c` so the
  fixture now requires this workload to release every unlinked return slot,
  reuse at least one released trace slot, avoid the stock self-link branch, and
  finish warmup with zero live `LJ_TRLINK_RETURN` roots. Those are behavioral
  assertions over helper counters, not source-shape checks.
