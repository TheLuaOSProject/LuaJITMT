# FNEW Active Arena Ownership

The traced/interpreter FNEW bump helpers skip legacy pending-root publication
only when the thread group is in standalone active black GC2 allocation
(`mark_active=1`, `alloc_black=1`, and no legacy mark bridge).

Why this is valid for this narrow path:

- The closure/upvalue cells are reserved from traversable arenas and have their
  arena mark bits set before any Lua-visible publication.
- FNEW closures and closed local-cell upvalues have no finalizer-side legacy
  ownership requirement; GC2 arena mark/sweep can own their lifetime.
- Idle allocation, active white allocation, and coupled legacy mark cycles still
  publish to the pending root chain. The stock runner's `lang/gc.lua` rechain
  and TSETM cases can force a full collection before closure-heavy metatable
  tests; allowing bridge-active FNEW elision there caused later table creation
  to see corrupted state.
- Constructor proto edges use a proto-specific SSB handoff even when the proto
  is already marked. Parser allocation can birth-mark a proto before traversal
  is queued, so "marked" alone is not enough proof for a fresh FNEW edge.

This removes most per-closure pending-root publication during long active-GC
closure allocation loops without adding a collector lock. On the local Linux/x64
probe, one million one-upvalue closure allocations no longer grew the legacy
root spine to one million entries; the final safe version stayed near 79k
objects and the loop moved from roughly 76 ns/op to roughly 73 ns/op. The
focused stock guard moved `closures_upval` at `BENCH_SCALE=0.2` from about
`2.06x` stock to about `1.07x` stock.

The regression harness check is `m6_jit_fnew_bump`: the active-black direct
FNEW helper test flushes pending roots, allocates a one-upvalue closure through
the bump path, and asserts that the pending-root head and global hint remain
unchanged.
