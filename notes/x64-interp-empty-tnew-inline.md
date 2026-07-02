# x64 Interpreter Empty TNEW Inline Bump

`BC_TNEW` now handles the `RD == 0` empty-table case with an inline arena-bump
fast path when all publication and accounting invariants are locally provable.
The path is limited to the main TG before secondary Lua threads or GC workers
exist, with the internal arena allocator still active and `g->allocd` matching
the main TG allocator descriptor.

The fast path only consumes the current traversable bump run when traversable
free-run bins that could satisfy a five-cell `GCtab` allocation are empty, and
only when the per-TG allocation counter can absorb `sizeof(GCtab)` without
flushing into the global GC2 counter. It falls back to `lj_tab_new0()` for
custom allocators, available free runs, missing bump arenas, full bump runs,
GC2 accounting flushes, active MT, and GC workers.

This keeps the permanent `lj_tab_new0()` semantics: empty table body
initialization, arena block/mark bit updates, GC total accounting, local GC2
allocation accounting, and pending-root publication order. JIT empty `TNEW`
remains helper-backed for now.
