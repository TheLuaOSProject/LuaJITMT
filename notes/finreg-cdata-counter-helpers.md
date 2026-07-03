# FINREG Cdata Counter Helpers

## Summary

Routed cdata FINREG registration, queue, P_WEAK, preclaim, and sweep-invariant
telemetry counters through `gc2_finreg_cdata_*()` helpers.

Stats reads now use acquire helper loads, GC2 initialization uses relaxed helper
stores, and producers use helper add wrappers instead of spelling ad hoc
atomics against `g->gc2`.

## Coverage

`m7_ffi_finreg` owns the observable cdata FINREG behavior and `m9_gc_stats`
owns the public telemetry surface. Cdata FINREG counter publication must stay
behind the helper surface in `lj_gc.c`, `lj_gc2.c`, `lj_cdata.c`, and
`lib_base.c`; that rule is documented here and beside the helpers instead of in
a source-text predicate.

## Follow-Up

Follow-up fixture cleanup routes `tests/t-gc2-traverse.c` counter snapshots
through the same acquire helpers and extends the M7 invariant coverage for that fixture.
