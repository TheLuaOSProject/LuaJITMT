# Interpreter No-Upvalue FNEW Bump

`lj_func_newL_gc()` now uses a conservative arena-bump fast path for
no-upvalue `BC_FNEW` closures after the interpreter has already run
`lj_gc_check_fixtop()`. The helper only replaces the allocation and pending-root
publication body; the interpreter remains responsible for stock GC-step pacing.

The predicate matches the other single-producer bump bridges: main TG only, no
active or entering MT, no GC2 workers, internal arena allocator, matching
allocator descriptor, no suitable traversable free run, available bump space,
and enough local GC2 accounting capacity to avoid a flush.

Fallback remains the normal `func_newL_gc_base()` path for active MT, entering
MT, workers, custom allocators, reusable free runs, exhausted bump runs,
accounting flushes, and any closure with upvalues.

2026-07-03 follow-up: traced no-upvalue `FNEW` now uses the same bump body via
`lj_func_newL_gc_forjit()`. Trace assembly owns the allocation check before
CALLA helpers, matching the existing traced one-upvalue FNEW split; the helper
still falls back to the generic closure path under the same predicates above.
