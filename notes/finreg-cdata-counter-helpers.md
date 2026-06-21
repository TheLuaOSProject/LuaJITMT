# FINREG Cdata Counter Helpers

## Summary

Routed cdata FINREG registration, queue, P_WEAK, preclaim, and sweep-invariant
telemetry counters through `gc2_finreg_cdata_*()` helpers.

Stats reads now use acquire helper loads, GC2 initialization uses relaxed helper
stores, and producers use helper add wrappers instead of spelling ad hoc
atomics against `g->gc2`.

## Guardrail

`tools/ci/m7_ffi_finreg.sh` now requires the cdata FINREG counter helper
surface and rejects raw production access to these counter fields in
`src/lj_gc.c`, `src/lj_gc2.c`, `src/lj_cdata.c`, and `src/lib_base.c`.

## Follow-Up

Userdata FINREG counters still have direct ad hoc increments and can be routed
through the same style of helper surface in a later slice.
