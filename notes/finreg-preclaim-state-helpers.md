# FINREG Preclaim State Helpers

## Summary

Routed GC2 cdata FINREG preclaim side-vector state through helper APIs:

- `gc2_finreg_cdata_preclaim_objvec_*()` owns the preclaimed cdata object
  vector pointer.
- `gc2_finreg_cdata_preclaim_finvec_*()` owns the copied finalizer value vector
  pointer.
- `gc2_finreg_cdata_preclaim_capacity_*()` owns the side-vector capacity.
- `gc2_finreg_cdata_preclaim_head_*()` owns the consumer cursor.
- `gc2_finreg_cdata_preclaim_count_*()` owns the one-past-last published
  preclaim record.

The existing slot protocol is unchanged: `gc2_finclaim_publish()` release-copies
the finalizer value, release-publishes the object slot as the ready marker, then
release-publishes `count`. Legacy root scans enter through
`lj_gc2_finreg_cdata_mark_roots()`, while GC2's pending-root scan calls the same
GC2-owned preclaim walker internally. Both acquire the head/count range before
marking preclaimed cdata/finalizers. Collector-specific callbacks still provide
the actual object/value/memory marking semantics, and finalizer dispatch
advances `head` through helper stores after clearing consumed slots.

## Coverage

`m7_ffi_finreg` and `m8_weak` own the observable cdata FINREG/finalizer
behavior. The helper-surface and root-scan ownership rules for
`finreg_cdata_preclaim_obj`, `finreg_cdata_preclaim_fin`,
`finreg_cdata_preclaim_capacity`, `finreg_cdata_preclaim_head`, and
`finreg_cdata_preclaim_count` live here and beside the helper surface instead
of in source-text predicates. Legacy root scans must stay behind
`lj_gc2_finreg_cdata_mark_roots()`.

## Follow-Up

Resize/compaction remains owner-side and tied to the current P_WEAK preclaim
bridge. A later slice should either prove the owner-quiesced resize invariant
with phase/worker guards or add retirement before allowing active readers to
race vector replacement.
