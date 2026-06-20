# FINREG P_WEAK root fallback counter removal

Date: 2026-06-20

## Context

Ordered FINREG discovery now owns the normal P_WEAK cdata finalizer path, and
the remaining live fallback telemetry is `finreg_cdata_order_fallbacks` for
ordered preclaim allocation/test-injection failures plus
`finreg_cdata_sweep_queued` for defensive late sweep/free discovery.

`finreg_cdata_pweak_root_fallbacks` no longer had a production increment. It
was only initialized, exported through `collectgarbage("stats")`, printed by
the benchmark harness, and asserted unchanged in traversal coverage.

## Change

- Removed `GC2State.finreg_cdata_pweak_root_fallbacks`.
- Removed init and `collectgarbage("stats")` export for the stale field.
- Removed benchmark reporting and stats-test requirements for the stale key.
- Removed traversal snapshots/assertions that only proved the stale counter
  stayed flat; retained coverage for ordered queueing, ordered fallback,
  preclaim overflow, sweep queueing, MPSC finalizer dispatch, and close-time
  ordered discovery.

## Verification

Passed:

- `tools/ci/lua_test.sh m8_weak`
- `tools/ci/lua_test.sh m9_gc_stats`
- `tools/ci/lua_test.sh m9_bench_smoke`
