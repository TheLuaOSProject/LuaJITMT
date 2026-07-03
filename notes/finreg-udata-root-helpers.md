# FINREG Userdata Root Helpers

## Summary

Routed the GC2 userdata FINREG side-list roots through helper APIs:

- `gc2_finreg_udata_head_*()` owns the active discovery list root.
- `gc2_finreg_udata_retired_*()` owns the retired-node retention list root.

Legacy userdata finalizer discovery, GC2 registration, logical retire,
best-effort active-list unlink, forget, init, and teardown now use the helper
surface instead of ad hoc pointer atomics on `g->gc2.finreg_udata_head` or
`g->gc2.finreg_udata_retired`.

The list semantics are unchanged: nodes are logically retired before best-effort
physical unlink, retired nodes remain retained until GC2 teardown, and readers
that already acquired an active-list node continue through helper-backed node
links.

## Coverage

`m3_gc2_scaffold` and `m8_weak` own the observable userdata FINREG behavior.
Production access to `finreg_udata_head` and `finreg_udata_retired` in
`lj_gc.c` and `lj_gc2.c` must stay behind the documented helper surface instead
of source-text matching.
- Follow-up: `tests/t-gc2-traverse.c` active userdata FINREG counters use
  `gc2_finreg_udata_head_acq()` for fixture snapshots.

## Follow-Up

Follow-up counter helper work now routes userdata FINREG telemetry through
`gc2_finreg_udata_*()` helpers too. The side-list remains append-only plus
logical retirement. A later slice can decide whether to add epoch-based
reclamation before teardown, but the root publication and CAS contract is now
centralized.
