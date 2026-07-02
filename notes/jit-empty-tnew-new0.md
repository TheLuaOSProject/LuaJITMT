# JIT empty TNEW helper split

Linux/x64 traced `TNEW(0, 0)` now uses the existing `lj_tab_new0()` helper
instead of packing a zero `ahsize` argument for `lj_tab_new1()`. This mirrors
the x64 interpreter `BC_TNEW` helper split and keeps the current trace-side
GC-step pacing, `lj_tab_new0()` body initialization, GC2 allocation accounting,
and pending-root publication order unchanged.

This is intentionally not the final inline TG bump allocator. The full inline
slice still needs the exact arena bitmap block/mark update, allocation color,
GC/GC2 accounting, table body initialization, and `lj_gc_linkobj_new()`
publication semantics before it can replace C allocation.

Focused guard: `m6_jit_gc2_readiness` now checks that escaped empty-table
`TNEW` traces route to `lj_tab_new0()` and that non-empty table constructors
continue to route through `lj_tab_new1()`.
