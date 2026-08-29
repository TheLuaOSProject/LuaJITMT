# JIT empty TNEW helper split

Linux/x64 traced `TNEW(0, 0)` now uses the JIT-specific
`lj_tab_new0_forjit()` helper instead of packing a zero `ahsize` argument for
`lj_tab_new1()`. The helper first tries the same conservative empty-table arena
bump conditions as the x64 interpreter fast path: main TG, no active/entering
MT, no GC workers, internal arena allocator, empty traversable free-run bins
that could satisfy the allocation, available bump space, and local GC2
accounting capacity. If any predicate fails, it falls back to `lj_tab_new0()`.

This is intentionally not the final no-call x64 mcode inline TG bump allocator.
It removes the generic table-constructor helper path for escaped traced empty
tables while keeping the GC-step check, table body initialization, arena
block/mark update, allocation color, GC/GC2 accounting, and pending-root
publication semantics in auditable C.

Focused regression test: `m6_jit_gc2_readiness` now checks that escaped
empty-table `TNEW` traces route to `lj_tab_new0_forjit()` and that non-empty
table constructors continue to route through `lj_tab_new1()`.
