# GC2 finalizer ring splice acquire read

## Context

`lj_gc2_finalizer_drain_owned()` drains the MPSC producer stack, reverses it,
and splices that reversed run into the single-consumer finalizer ring. When an
old ring tail exists, the code copied `oldtail->nextgc` through a stack-local
`GCRef` with a plain `setgcrefr()` before release-publishing it onto `newtail`.

Other finalizer queue links are now read with acquire loads and published with
release stores.

## Change

The old ring head is now loaded with `lj_obj_gcw_acq(oldtail)` and stored onto
`newtail` with `lj_obj_setgcwrel()`.

The rest of the splice is unchanged:

- `oldtail->nextgc` is release-updated to the reversed producer run.
- `finalizer_tail` is release-updated to `newtail`.

## Scope

This removes the remaining runtime direct `setgcrefr()` use from the GC2
finalizer splice. The remaining direct `setgcref*` hits after this are helper
definitions, stack-local CAS expected values, parser-local state, startup
initialization, or macro forms that need separate broader passes.
