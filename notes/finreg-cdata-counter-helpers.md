# FINREG Cdata Counter Helpers

## Summary

Routed cdata FINREG registration, queue, P_WEAK, preclaim, and sweep-invariant
telemetry counters through `gc2_finreg_cdata_*()` helpers.

Stats reads now use acquire helper loads, GC2 initialization uses relaxed helper
stores, and producers use helper add wrappers instead of spelling ad hoc
atomics against `g->gc2`.

## Guardrail

`tools/ci/m7_ffi_finreg.sh` now requires the cdata FINREG counter helper
surface and documents why raw production access to these counter fields in
`src/lj_gc.c`, `src/lj_gc2.c`, `src/lj_cdata.c`, and `src/lib_base.c`.

## Follow-Up

Follow-up fixture cleanup routes `tests/t-gc2-traverse.c` counter snapshots
through the same acquire helpers and extends the M7 guard over that fixture.
