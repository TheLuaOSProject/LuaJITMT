# GC2 Sweep-Live Estimate Helper Slice

This slice routes the GC2 sweep-live publication cluster through helper
accessors:

- `gc2_live_estimate_*()` publishes and reads the byte estimate that feeds
  `lj_gc2_update_pacing()`.
- `gc2_sweep_live_huge_bytes_*()` publishes the HugeTab contribution exposed
  through `collectgarbage("stats")`.
- `gc2_sweep_live_updates_*()` tracks sweep-live refresh count telemetry.

Ordering:
- Init uses relaxed stores before the GC2 state is concurrently visible.
- GC2's internal `lj_gc2_sweep_live_aggregate()` release-publishes the
  huge-byte contribution and combined live estimate, then relaxed-increments
  update telemetry.
- `lj_gc2_update_pacing()` and `collectgarbage("stats")` acquire-load the
  helper-backed snapshot.

Guarding:
- `tools/ci/m9_gc_stats.sh` now requires all nine helper definitions.
- The same guard rejects raw production access to `sweep_live_updates`,
  `sweep_live_huge_bytes`, and `live_estimate` in `lj_gc2.c` and `lib_base.c`.
- `tools/ci/m3_gc2_worker_scheduler.sh` keeps the aggregate helper private to
  `lj_gc2.c`; public cycle closure enters through sweep-to-idle or legacy
  cycle-end.
- Test internals still inspect the fields directly when asserting fixture state.

Validation:
- `tools/ci/m9_gc_stats.sh` passed.
- `tools/ci/m3_gc2_paranoia.sh` passed.
- `tools/ci/m10_generational.sh` passed.
- `tools/ci/m6_jit_alloc_account.sh` passed.
- `tools/ci/m0_source_guard.sh` passed.
- Raw production sweep-live estimate access scan passed.
- `git diff --check` passed.
