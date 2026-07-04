# x64 Interpreter Empty TNEW Inline Bump

`BC_TNEW` now handles the `RD == 0` empty-table case with an inline arena-bump
fast path when all publication and accounting invariants are locally provable.
The path is limited to the main TG before secondary Lua threads or GC workers
exist, with the internal arena allocator still active and `g->allocd` matching
the main TG allocator descriptor.

The fast path only consumes the current traversable bump run when the per-TG
allocation counter can absorb `sizeof(GCtab)` without flushing into the global
GC2 counter. It falls back to `lj_tab_new0()` for custom allocators, missing
bump arenas, full bump runs, GC2 accounting flushes, active MT, and GC workers.
It does not fall back merely because a reusable free-run bin exists: table
address reuse order is not Lua-visible, the generic allocator still owns those
bins, and the active bump window is never published into them.

This keeps the permanent `lj_tab_new0()` semantics: empty table body
initialization, arena block/mark bit updates, GC total accounting, local GC2
allocation accounting, and pending-root publication order. JIT empty `TNEW`
now uses a JIT-specific C helper that attempts the same conservative bump path;
the final no-call x64 mcode inline allocator remains pending.

2026-07-03 follow-up: `lj_tab_new0()` itself now shares the same conservative
arena-bump helper, and `lj_tab_new_ah(0, 0)` routes through it. This extends the
single-producer empty-table fast path to direct runtime users and the public
`lua_newtable()`/`lua_createtable(L, 0, 0)` path without changing non-empty table
construction. The helper still falls back to `newtab()` for active or entering
MT, GC workers, custom allocators, exhausted bump arenas, and local GC2
accounting flushes.

2026-07-04 follow-up: exact empty-table C and x64 inline paths now mirror the
FNEW bump-window policy. They may use the active bump window even when a
same-sized reusable traversable run is present for generic allocation. This
keeps table construction semantics intact because Lua does not expose allocation
address reuse order, and the active bump window is disjoint from the reusable
free-run lists.

2026-07-03 direct-constructor follow-up: `lj_tab_new(L, 0, 0)` now also routes
through `lj_tab_new0()`. This covers internal exact-empty constructor users
without changing array/hash constructors or their publication paths.
