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

## Guardrail

`tools/ci/m3_gc2_scaffold.sh` now requires the userdata FINREG counter helper
surface and rejects raw production access in `src/lj_gc.c`, `src/lj_gc2.c`, and
`src/lib_base.c`.
- Follow-up fixture cleanup routes `tests/t-gc2-traverse.c` userdata FINREG
  telemetry snapshots through the same acquire helpers and extends the guard
  over that fixture.

## Follow-Up

The remaining userdata FINREG work is semantic rather than telemetry plumbing:
continue moving discovery and dispatch ownership toward the planned scheduler
path while preserving userdata `__gc` behavior.
