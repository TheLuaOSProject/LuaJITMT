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

## Guardrail

`tools/ci/m3_gc2_scaffold.sh` now requires the userdata FINREG root helper
surface and documents why raw production access to `finreg_udata_head` and
`finreg_udata_retired` in `src/lj_gc.c` and `src/lj_gc2.c`.
- Follow-up: the same guard now covers `tests/t-gc2-traverse.c`, whose active
  userdata FINREG counters use `gc2_finreg_udata_head_acq()`.

## Follow-Up

Follow-up counter helper work now routes userdata FINREG telemetry through
`gc2_finreg_udata_*()` helpers too. The side-list remains append-only plus
logical retirement. A later slice can decide whether to add epoch-based
reclamation before teardown, but the root publication and CAS contract is now
centralized.
