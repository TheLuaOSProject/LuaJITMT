# GC2 Sweep-Live Estimate Helper Slice

This slice routes the GC2 sweep-live publication cluster through helper
accessors:

- `gc2_live_estimate_*()` publishes and reads the byte estimate that feeds
  `lj_gc2_update_pacing()`.
- `gc2_sweep_live_huge_bytes_*()` publishes the HugeTab contribution exposed
  through `threading.gcstats()`.
- `gc2_sweep_live_updates_*()` tracks sweep-live refresh count telemetry.

Ordering:
- Init uses relaxed stores before the GC2 state is concurrently visible.
- GC2's internal `lj_gc2_sweep_live_aggregate()` release-publishes the
  huge-byte contribution and combined live estimate, then relaxed-increments
  update telemetry.
- `lj_gc2_update_pacing()` and `threading.gcstats()` acquire-load the
  helper-backed snapshot.

Coverage:
- `m9_gc_stats` owns the public sweep-live telemetry behavior.
- Production access to `sweep_live_updates`, `sweep_live_huge_bytes`, and
  `live_estimate` in `lj_gc2.c` and `lib_base.c` must stay behind the
  documented helper surface instead of source-text matching.
- `m3_gc2_worker_scheduler` keeps the aggregate helper private to
  `lj_gc2.c`; public cycle closure enters through sweep-to-idle or legacy
  cycle-end.
- Test internals still inspect the fields directly when asserting fixture state.

Validation:
- `tools/ci/lua_test.sh m9_gc_stats` passed.
- `tools/ci/lua_test.sh m3_gc2_paranoia` passed.
- `tools/ci/lua_test.sh m10_generational` passed.
- `tools/ci/lua_test.sh m6_jit_alloc_account` passed.
- `git diff --check` passed.
