# GC sweep completion acquire read

## Context

The root-list sweep walker uses acquire loads while advancing through `g->gc.root`
links:

- `gc_sweep()` reads each `GCRef` with `gcref_acq(*p)`.
- Dead-object unlinking uses `gc_chain_splice()` with an acquire-release CAS.

After a partial sweep step, `gc_onestep()` checked whether the returned sweep
cursor pointed at the end of the list with a plain `gcref()` load. That was an
outlier compared with the rest of the shared sweep-list protocol.

## Change

`GCSsweep` now checks the returned cursor with:

```c
gcref_acq(*mref(g->gc.sweep, GCRef))
```

This keeps the end-of-sweep decision on the same acquire-read discipline as the
list walk itself.

## Scope

This does not change `g->gc.sweep` cursor publication. The cursor is GC state,
not the root-list link being inspected here. The change is limited to the root
link read used to decide whether the root sweep is complete.
