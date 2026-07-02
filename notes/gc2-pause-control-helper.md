# GC2 Pause Control Helper Slice

The GC2 pacing pause percentage is a public control: `lua_gc(LUA_GCSETPAUSE)`
publishes it, `lj_gc2_update_pacing()` reads it to compute trigger/hard bytes,
and `lj_gc2_init()` seeds the default.

This slice adds `gc2_gcpause_pct_acq()`, `gc2_gcpause_pct_store_rlx()`, and
`gc2_gcpause_pct_rel()` in `lj_obj.h`, then routes all production C-side
accesses through those helpers.

Ordering:
- Init uses a relaxed store because the state is not yet concurrently visible.
- `lua_gc(LUA_GCSETPAUSE)` uses a release store before recomputing pacing.
- `lj_gc2_update_pacing()` uses an acquire load to pair with public updates.

Guarding:
- `tools/ci/m5_gc2_pacing_atomic.sh` now requires the three helper definitions.
- The same guard documents why raw C-side `gcpause_pct` field access outside helper
  definitions, alongside the existing GC2 pacing byte counters.

Validation:
- `tools/ci/m5_gc2_pacing_atomic.sh` passed.
- `tools/ci/m6_jit_alloc_account.sh` passed.
- `tools/ci/m9_gc_stats.sh` passed.
- passed.
- Raw C-side GC2 pacing access scan reports only helper definitions.
- `git diff --check` passed.
