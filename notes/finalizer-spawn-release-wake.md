# Finalizer Spawn Release Wake

## 2026-06-20

Problem:
- A finalizer that spawned a worker could intentionally leave legacy GC in
  `GCSfinalize` until that worker exited, but the last worker exit only restored
  thresholds and woke `mt_live` waiters.
- The parked GC2 worker scheduler had no explicit signal that the deferred
  finalizer phase had become resumable.

Fix:
- The final secondary-thread exit now detects `GCSfinalize` with released
  `mt_gc_exclusive`, records `finalizer_spawn_release_wakes`, and publishes a
  GC2 worker wake request.
- This keeps callback stack ownership unchanged: user finalizers still execute
  on the claimed collector caller state, while the deferral release becomes a
  scheduler-visible event.

Regression:
- `tests/t-gc2-traverse.c` now asserts a cdata finalizer-spawn deferral records
  a release wake after the spawned worker exits and before the next full GC
  closes the cycle.
- `tests/t-gc-stats.lua` requires
  `collectgarbage("stats").finalizer_spawn_release_wakes`.

Verification:
- Clean `make -C src`, focused `t-gc2-traverse`, `tests/t-gc-stats.lua`,
  `tools/ci/m3_gc2_worker_scheduler.sh`, and `tools/ci/m8_weak.sh` passed.
