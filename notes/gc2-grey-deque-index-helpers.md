# GC2 Grey Deque Index Helpers

## Summary

Routed the GC2 grey deque `grey_top` and `grey_bottom` index state through
small helper APIs in `lj_obj.h`.

The deque algorithm is unchanged. Owner push/grow/pop and thief steal still use
the existing Chase-Lev ordering:

- `grey_top` snapshots use acquire loads.
- `grey_bottom` owner snapshots and reset stores use relaxed operations.
- bottom publications after slot writes/restores use release stores.
- owner single-item and thief steal claims use the existing seq-cst top CAS
  with acquire failure ordering.

## Invariant check

`tools/ci/m3_gc2_worker_scheduler.sh` now requires the helper surface and rejects
raw production access to `g->gc2.grey_top` or `g->gc2.grey_bottom` in
`lj_gc2.c`.

This is intentionally separate from the queue-slot guard in
`m3_gc2_scaffold.sh`: slot publication helpers protect the `GCRef` entries,
while these helpers protect the shared deque index protocol.

## Follow-Up

Follow-up vector helper work routes `grey_stack` and `grey_capacity` through
`gc2_grey_stack_*()` and `gc2_grey_capacity_*()` helpers and extends the same
M3 note documenting raw production access. Deque growth is still intentionally
owner-quiesced under the current single-worker bridge; a later scheduler slice
must add a retirement/epoch scheme before allowing steals to race vector
replacement.
