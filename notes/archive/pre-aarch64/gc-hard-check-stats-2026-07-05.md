# GC Hard-Check Stats

`threading.gcstats()` now exposes `jit_hard_checks` and `interp_hard_checks`,
the existing GC2 counters that record when trace-side or interpreter-side
allocation checks enter the hard-assist path.

The counters already backed C fixtures, but Lua probes could only see aggregate
`assist_runs`. Exposing both counters makes allocation-heavy benchmarks easier
to diagnose without adding one-off C instrumentation. The immediate motivation
was the closure-allocation probe where `collectgarbage("stop")` is fast but
normal GC remains dominated by legacy root-spine and pacing work.

This is telemetry only. It does not change GC scheduling, publication, or
collector semantics.

Initial closure-loop probe after adding the counters:

- `500000` iterations allocating one closure plus one closed upvalue ended in
  `phase=1` with about `1000000` root-spine objects.
- `assist_runs` rose by about `91`, while `jit_hard_checks` and
  `interp_hard_checks` stayed at `0`. The assists came from allocator-side
  accounting flushes, not the trace/interpreter GC check helpers.
- Raising `LJ_GC2_TRIGGER_MIN` from `8 * LJ_GC2_ACCT_FLUSH` to
  `32 * LJ_GC2_ACCT_FLUSH` did not materially improve closure throughput.
- Raising `LJ_GC2_TRACE_HARD_CHECK_BATCH` from
  `16 * LJ_GC2_ACCT_FLUSH` to `64 * LJ_GC2_ACCT_FLUSH` reduced assist count
  but made the closure benchmark slower in the sampled run.

The remaining closure gap is therefore more likely the legacy root-spine bridge
and active-cycle work shape than the raw number of hard-check helper entries.
