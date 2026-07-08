# GC2 root-spine and huge sweep bridge, 2026-07-08

The legacy root list is an ownership spine while GC2 owns arena body lifetime.
It must not be treated as a semantic root set at the sweep bridge: semantic
roots are closed by `lj_gc2_trace_sweep_roots()`, and preserving every root-list
body lets unreachable objects survive explicit collections.

The bridge callback now only flushes and repairs the root spine. The owner
sweep prepass dispatches unmarked freeable bodies instead. Fixed roots remain
pinned, pending finalizer cdata stays under FINREG ownership, and root-spine
tombstones are unlinked before arena reuse.

Huge traversable allocations need one extra rule. Small arena survivors lose
their mark bit during `lj_arena_sweep_words()` on major sweeps, but huge-table
marks can remain as the completed-cycle live set. A later legacy full collection
can prove such a huge function/proto dead while the huge-table mark is still
set, so the root-spine sweep frees legacy-dead huge objects even when their
stale huge mark is present.

Coverage:

- `tools/ci/lua_test.sh m2_arena_gcsweep`
