# FINREG Userdata Counter Helpers

## Summary

Routed userdata FINREG telemetry counters through `gc2_finreg_udata_*()`
helpers:

- `sets`, `clears`, and `queued`
- side-list `registered` and `retired_nodes`
- discovery `discovered` and stale-reference `forgets`

Stats reads now use acquire helper loads, GC2 initialization uses relaxed helper
stores, and producers use helper add wrappers. This keeps the userdata FINREG
side-list telemetry publication contract beside the existing root and node-link
helper surface.

## Coverage

`m3_gc2_scaffold`, `m8_weak`, and `m9_gc_stats` own the observable userdata
FINREG behavior and telemetry. Counter publication in `lj_gc.c`, `lj_gc2.c`,
and `lib_base.c` must stay behind the documented helper surface instead of
source-text matching.
- Follow-up fixture cleanup routes `tests/t-gc2-traverse.c` userdata FINREG
  telemetry snapshots through the same acquire helpers.

## Follow-Up

The remaining userdata FINREG work is semantic rather than telemetry plumbing:
continue moving discovery and dispatch ownership toward the planned scheduler
path while preserving userdata `__gc` behavior.
