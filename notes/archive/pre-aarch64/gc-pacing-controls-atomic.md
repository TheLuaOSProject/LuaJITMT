# Atomic legacy GC pacing controls

The legacy `GCState.pause` and `GCState.stepmul` controls are shared through
the public `lua_gc()` API while GC step/pacing code reads them. They now use
`lj_gc_pause_*()` and `lj_gc_stepmul_*()` helpers in `lj_gc.h`.

- `LUA_GCSETPAUSE` and `LUA_GCSETSTEPMUL` use atomic exchange helpers so the
  API still returns the previous value while avoiding a data race on the shared
  control word.
- Restart-threshold calculation, incremental step-limit calculation, GC2 init,
  and state initialization all use the same helper boundary.
- The existing `m5_gc2_pacing_atomic` coverage now covers these legacy controls
  as well as the GC2 pacing counters. Comments document why the shared control
  words use helper access outside local initialization.

Verification:

- `make -C src -j$(nproc)`
- `tools/ci/lua_test.sh m5_gc2_pacing_atomic`
- `tools/ci/lua_test.sh m5_stock_api_surface`
- `tools/ci/lua_test.sh run_stock_tests -- --quiet`

Follow-up status:

- `tools/ci/lua_test.sh m2_arena_gcsweep` now passes.
- `tools/ci/lua_test.sh m6_jit_alloc_account` now passes. The fixture now keeps
  raw allocation-accounting checks below the synthetic cycle trigger, treats
  remembered-filtered counters as monotonic telemetry, and sets the weak mark
  closure precondition before directly testing weak assist draining.
