# Atomic legacy GC pacing controls

The legacy `GCState.pause` and `GCState.stepmul` controls are shared through
the public `lua_gc()` API while GC step/pacing code reads them. They now use
`lj_gc_pause_*()` and `lj_gc_stepmul_*()` helpers in `lj_gc.h`.

- `LUA_GCSETPAUSE` and `LUA_GCSETSTEPMUL` use atomic exchange helpers so the
  API still returns the previous value while avoiding a data race on the shared
  control word.
- Restart-threshold calculation, incremental step-limit calculation, GC2 init,
  and state initialization all use the same helper spelling.
- The existing `m5_gc2_pacing_atomic` guard now covers these legacy controls as
  well as the GC2 pacing counters. It documents why raw runtime access to
  `g->gc.pause` and `g->gc.stepmul` outside the helper definitions.

Verification:

- `make -C src -j$(nproc)`
- `tools/ci/lua_test.sh m5_gc2_pacing_atomic`
- `tools/ci/lua_test.sh m5_stock_api_surface`
- `tools/ci/lua_test.sh run_stock_tests -- --quiet`

Known pre-existing failures found while widening focused coverage:

- `tools/ci/lua_test.sh m2_arena_gcsweep` fails at
  `tests/t-arena-gcsweep.c:669` on clean `HEAD` (`602fb8aa`) and on this tree.
- `tools/ci/lua_test.sh m6_jit_alloc_account` fails at
  `tests/t-gc2-alloc-account.c:502` on clean `HEAD` (`602fb8aa`) and on this
  tree.
