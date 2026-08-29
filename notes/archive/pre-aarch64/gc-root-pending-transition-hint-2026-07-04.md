# GC Root Pending Transition Hint

2026-07-04

- Pending-root publishers now dirty `global_State.gcroot_pending_hint` only when
  the TG-local pending stack changes from empty to non-empty.
- Transition publication still hints both before and after the local release
  store or CAS. That preserves the existing race coverage where a flusher clears
  the global hint between the local stack publication steps.
- Appending to an already non-empty pending stack no longer writes the global
  hint cache line. The earlier empty-to-nonempty transition is enough: if a
  flusher drains that stack first, a racing publisher retries against an empty
  head and republishes the hint for the next flush.
- This narrows another allocation-side global write in the legacy root bridge
  without changing the bridge's semantics. Full removal still depends on the
  planned bitmap/arena-owned root and sweep path.

Coverage:

- `m3_gc_root_pending` owns the observable pending-root flush semantics.
- `m6_jit_fnew_bump` and `m5_x64_tnew_empty_inline` cover JIT/VM allocation
  paths that publish through the pending-root stack.
