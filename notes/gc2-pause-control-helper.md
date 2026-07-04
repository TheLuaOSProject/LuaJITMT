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

Coverage:
- `m5_gc2_pacing_atomic`, `m6_jit_alloc_account`, and `m9_gc_stats` own the
  observable pacing behavior.
- Raw C-side `gcpause_pct` field access outside helper definitions must stay
  behind the documented helper surface, alongside the existing GC2 pacing byte
  counters.

Validation:
- `tools/ci/lua_test.sh m5_gc2_pacing_atomic` passed.
- `tools/ci/lua_test.sh m6_jit_alloc_account` passed.
- `tools/ci/lua_test.sh m9_gc_stats` passed.
- `git diff --check` passed.

2026-07-04 follow-up:

- `lj_gc2_init()` now seeds `gc2.gcpause_pct` from the already-initialized
  legacy `g->gc.pause` instead of a separate constant. This keeps GC2's public
  pause mirror aligned with LuaJIT's stock `LUAI_GCPAUSE` default and with
  embedders that initialize the legacy field before GC2 comes online.
- The automatic pending-root trigger cap is unchanged. That cap is a bounded
  root-publication bridge while new objects still enter the legacy root spine;
  the init fix only removes the independent default so future cap changes and
  larger heaps do not inherit a silent 100% pause value.
- `t-gc2-pacing-atomic.c` now asserts the GC2 init mirror directly, because
  small heaps can hit the pending-root trigger cap and mask the pause value in
  runtime `trigger_bytes` telemetry.
