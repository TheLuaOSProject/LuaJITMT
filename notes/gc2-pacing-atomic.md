# GC2 Pacing Atomic Helpers

GC2 allocation pacing uses four shared byte counters and one public pause
control:

- `gc2.alloc_since_trigger`
- `gc2.cycle_alloc_bytes`
- `gc2.trigger_bytes`
- `gc2.hard_bytes`
- `gc2.gcpause_pct`

Mutators update `alloc_since_trigger` when per-TG allocation counters flush,
while interpreter, trace, stats, and cycle-start paths read the pacing snapshot.
Those accesses now go through `lj_gc2_*` helpers in `lj_gc.h`.
The pause percentage is the `lua_gc(LUA_GCSETPAUSE)` control that feeds
`lj_gc2_update_pacing()`; its init, publication, and acquire read now go
through `gc2_gcpause_pct_*` helpers in `lj_obj.h`.

`alloc_since_trigger` increments use relaxed fetch-add because this is
counter-only accounting; cycle and assist decisions acquire-load snapshots.
Cycle-start reset uses `lj_gc2_alloc_since_xchg()` so the snapshot and zeroing
cannot lose a concurrent flushed allocation. Pacing publication stores use
release ordering so helper-side acquire readers see complete threshold updates.

The x86-64 VM now reaches GC2 hard-limit checks through
`lj_gc_should_step_vm()`, keeping allocation-check pacing reads in C helper code
instead of generated interpreter assembly.

Guard: `tools/ci/m5_gc2_pacing_atomic.sh` rejects raw C-side access to the GC2
pacing fields outside helper definitions in `lj_gc.h`/`lj_obj.h` and rejects raw
x64 VM GC2 hard-check memory operands or reintroduced load macros.

Test-design note: an earlier Lua smoke used four workers doing 2000
`string.format()` allocations each while reading stats. That crashes on
baseline commit `c5be3f1b` too, so it is a pre-existing concurrency bug rather
than fallout from this helper layer. The committed smoke keeps this guard
focused on the pacing counters and uses the same table/string allocation shape
as the existing GC total smoke.

Validation:
- `tools/ci/m5_gc2_pacing_atomic.sh` passed.
- `tools/ci/m6_jit_alloc_account.sh` passed.
- `tools/ci/m9_gc_stats.sh` passed.
- `tools/ci/m0_source_guard.sh` passed.
- Raw C-side GC2 pacing access scan passed.
- `git diff --check` passed before staging.
