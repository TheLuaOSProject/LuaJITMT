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

## Coverage

`m3_gc2_worker_scheduler`, `m8_weak`, and `m9_gc_stats` own the observable
finalizer behavior and telemetry. Finalizer counter publication must stay
behind the helper surface in `lj_gc.c`, `lj_gc2.c`, `lib_base.c`, and
`lib_threading.c`, with fixtures using the same acquire helpers when they need
snapshots. That rule is documented here and beside the helpers instead of in a
source-text predicate.

## Follow-Up

The counters are telemetry plumbing only; the remaining finalizer work is to
continue moving dispatch ownership toward the scheduler-owned path without
changing Lua finalizer ordering or callback-state semantics.
