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
now uses a JIT-specific C helper that attempts the same conservative bump path;
the final no-call x64 mcode inline allocator remains pending.

2026-07-03 follow-up: `lj_tab_new0()` itself now shares the same conservative
arena-bump helper, and `lj_tab_new_ah(0, 0)` routes through it. This extends the
single-producer empty-table fast path to direct runtime users and the public
`lua_newtable()`/`lua_createtable(L, 0, 0)` path without changing non-empty table
construction. The helper still falls back to `newtab()` for active or entering
MT, GC workers, custom allocators, free runs that should be reused, exhausted
bump arenas, and local GC2 accounting flushes.

2026-07-03 direct-constructor follow-up: `lj_tab_new(L, 0, 0)` now also routes
through `lj_tab_new0()`. This covers internal exact-empty constructor users
without changing array/hash constructors or their publication paths.
