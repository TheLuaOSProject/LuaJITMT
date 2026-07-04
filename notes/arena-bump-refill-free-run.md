# Arena bump refill from reusable runs

## Problem

After a full sweep, traversable arenas mostly return as reusable free-run bins.
The traced one-upvalue numeric `FNEW` path can allocate directly only from the
TG-local bump window. Once the single post-sweep bump window was consumed, each
later closure allocation fell through to the C helper and then to generic
free-run allocation. A repeated `closures_upval` benchmark showed this as a
post-collect cliff: the second identical 200k-iteration loop took about
1080 ms instead of about 15 ms.

## Change

`lj_arena_reserve_bump()` now gives specialized bump callers a way to refill the
private bump window from a reusable free run. It removes one suitable run from
the normal bin list, reserves the first cells for the caller, and keeps the
remaining cells as the unpublished bump window. Generic `lj_arena_alloc()` still
uses normal free-run-first policy.

The FNEW/upvalue bump helpers use this reserve path because Lua observes
ordinary closure/upvalue identity, not arena address reuse order. Object
initialization, mark-bit setting, accounting, and pending-root publication stay
in the existing helper code.

## Coverage

- `m6_jit_fnew_bump` now includes a post-sweep traced FNEW rerun that asserts the
  generic fallback counter does not advance after sweep refill.
- `m2_arena_all`, `m3_gc2_scaffold`, and `m5_gc2_pacing_atomic` passed after the
  allocator API change.
- `LJ_BENCH_STOCK_FILTERS=closures_upval LJ_BENCH_STOCK_SCALE=0.05
  tools/ci/lua_test.sh m9_bench_stock_compare` reported geomean `0.914425`
  against `/usr/bin/luajit`.
