# Finalizer Counter Helpers

## Summary

Routed GC2 finalizer telemetry counters through `gc2_finalizer_*()` helpers:

- queue publish and dequeue counts
- MPSC-drained count
- finalizer guard enter and leave counts
- sweep-block count
- finalizer-spawn deferral and release-wake counts

Stats reads now use acquire helper loads, GC2 initialization uses relaxed helper
stores, and producers use helper add wrappers. The existing MPSC-drained helper
is now used by stats as well as worker-drain accounting.
Phase and traversal fixtures read finalizer telemetry through the same acquire
helpers.

## Invariant check

`tools/ci/m3_gc2_worker_scheduler.sh` now requires the finalizer counter helper
surface and documents why raw production access in `src/lj_gc.c`, `src/lj_gc2.c`,
`src/lib_base.c`, and `src/lib_threading.c`, plus raw fixture access in
`tests/t-gc2-phase.c` and `tests/t-gc2-traverse.c`.

## Follow-Up

The counters are telemetry plumbing only; the remaining finalizer work is to
continue moving dispatch ownership toward the scheduler-owned path without
changing Lua finalizer ordering or callback-state semantics.
