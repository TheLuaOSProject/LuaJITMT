# GC2 Grey Deque Vector Helpers

## Summary

Routed the GC2 grey deque body state through helper APIs:

- `gc2_grey_stack_*()` owns the deque slot vector pointer.
- `gc2_grey_capacity_*()` owns the vector capacity snapshot.

Owner init/fini, mark-begin capacity checks, deque growth, owner push/pop, and
non-owner steal now use the helper surface instead of direct
`g->gc2.grey_stack` or `g->gc2.grey_capacity` access. Deque slots still use the
existing `gc2_queue_slot_*()` release/acquire helpers, and `grey_top` /
`grey_bottom` keep the Chase-Lev ordering from the earlier index-helper slice.

## Invariant check

`tools/ci/m3_gc2_worker_scheduler.sh` now requires the vector helper surface and
documents why raw production access to `grey_stack`, `grey_capacity`, `grey_top`, and
`grey_bottom` in `src/lj_gc2.c`.

## Follow-Up

The vector helper surface does not by itself make deque growth safe against
active thieves. The current bridge still relies on the temporary
`worker_active` single-worker ownership model and owner-quiesced growth. A later
scheduler slice should either keep that invariant around all growth or add
retirement/epoch protection before allowing workers to steal while the vector is
being replaced.
