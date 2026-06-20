# GC2 Pacing Counter Atomic Helpers

GC2 allocation pacing uses four shared byte counters:

- `gc2.alloc_since_trigger`
- `gc2.cycle_alloc_bytes`
- `gc2.trigger_bytes`
- `gc2.hard_bytes`

Mutators update `alloc_since_trigger` when per-TG allocation counters flush,
while interpreter, trace, stats, and cycle-start paths read the pacing snapshot.
Those accesses now go through `lj_gc2_*` helpers in `lj_gc.h`.

`alloc_since_trigger` increments use relaxed fetch-add because this is
counter-only accounting; cycle and assist decisions acquire-load snapshots.
Cycle-start reset uses `lj_gc2_alloc_since_xchg()` so the snapshot and zeroing
cannot lose a concurrent flushed allocation. Pacing publication stores use
release ordering so x86-64 VM TSO loads and C acquire readers see complete
threshold updates.

The x86-64 VM already names the allocation-check loads through
`x64_vm_gc2_alloc_since_acq()` and `x64_vm_gc2_hard_bytes_acq()`. The new guard
keeps the C side aligned with those generated-code edges.

Guard: `tools/ci/m5_gc2_pacing_atomic.sh` rejects raw C-side access to the four
GC2 pacing fields outside `lj_gc.h` helper definitions and rejects raw x64 VM
GC2 hard-check memory operands.

Test-design note: an earlier Lua smoke used four workers doing 2000
`string.format()` allocations each while reading stats. That crashes on
baseline commit `c5be3f1b` too, so it is a pre-existing concurrency bug rather
than fallout from this helper layer. The committed smoke keeps this guard
focused on the pacing counters and uses the same table/string allocation shape
as the existing GC total smoke.

Validation:
- `tools/ci/m5_gc2_pacing_atomic.sh` passed.
- `tools/ci/m0_source_guard.sh` passed.
- Raw C-side GC2 pacing access scan passed.
- `tools/ci/m5_concurrent_objects.sh` passed.
- `git diff --check` passed before staging.
