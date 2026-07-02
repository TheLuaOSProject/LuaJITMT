# GC2 Weak Snapshot Vector Helpers

## Summary

Routed the GC2 weak snapshot backing vector state through helper APIs:

- `gc2_weak_stack_*()` owns the weak table `GCRef` vector pointer.
- `gc2_weak_ready_*()` owns the ready-byte vector pointer.
- `gc2_weak_capacity_*()` owns the published vector capacity.

The weak snapshot algorithm is unchanged. Weak table discovery still reserves a
slot through `gc2_weak_count_add()`, release-publishes the table slot, and then
release-publishes the ready byte. Snapshot readers still acquire the count and
ready prefix before exposing tables to weak scan/clear logic.

## Ordering

Resize publishes the two vector pointers with release stores and publishes
capacity last with a release store. Readers that need a tuple acquire the
capacity before loading the vector pointers. This preserves the current
owner-quiesced resize discipline while making the vector publication protocol
explicit at each access site.

Init/fini use relaxed helper stores because they run while the state is being
created or torn down. Mark-begin reset uses acquired vector snapshots before
clearing ready bytes and then resets the reservation/count cursors for the new
cycle.

## Guardrail

`tools/ci/m8_weak.sh` now requires the vector helper surface and documents why raw
production access to `weak_stack`, `weak_ready`, or `weak_capacity` in
`lj_gc2.c`.

## Follow-Up

This does not make weak snapshot vector resizing safe against active readers by
itself. The current bridge still relies on owner-quiesced resize/reset at cycle
boundaries. A later slice should either prove that invariant with stronger
scheduler phase guards or add a retire/RCU scheme before allowing concurrent
weak-vector growth.
