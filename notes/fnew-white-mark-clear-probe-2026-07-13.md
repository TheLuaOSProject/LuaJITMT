# Conditional white FNEW mark clear

## Scope

This is a narrow allocation-path optimization for the arena bump helpers used
by interpreter and helper-backed Lua closure construction. It does not alter
the plan files, the active-MARK birth-mark rule, or the x64 traced FNEW
template.

The stopped-GC `closures_upval` profile at `c18c4a0e` attributed 11.64% of the
timed loop to `func_arena_set_alloc()`. About 83% of that function's samples
landed on the locked `AND` used to clear the mark bit of a white allocation.
The benchmark's outer loop remains in the interpreter because its immediate
closure call is not recordable; this cost is therefore in the C bump helper,
not the traced FNEW template.

## Invariant and rejected broad elision

Fresh arenas are zero-filled. A run selected from a reusable bin is completely
scrubbed by `arena_clear_extent_range()` before it becomes the private bump
window, and every interior cell in a directly swept run is also mark-clear.
Consequently almost every white bump start already has a zero mark bit.

Unconditionally deleting the clear is not exact. `arena_set_free_run()` marks
the run head while its `LJArenaFreeRun` metadata is visible, and
`lj_arena_sweep_one()` can install that largest run directly as the private
bump window. Its first allocation must still remove this seeded mark or it
would be retained as marked in the next cycle.

The implemented rule therefore acquire-tests the exact start bit and performs
the existing atomic clear only when it is nonzero. This keeps the rare swept
run-head transition unchanged while avoiding two locked RMWs for the common
closure/upvalue pair.

`func_bump_alloc_ready()` restricts the helper to the main TG, internal arena
allocator, no active or entering secondary VM, and zero GC2 workers. When the
allocation is white, `TG.mark_active` is also false; the post-checkpoint
constructor has no GC-capable step after reserving its CONSTRUCT lanes. Thus no
current-cycle marker can publish a new mark between the probe and discovery.
The conditional operation is an optimization of an exact clear, not a cached
phase or liveness certificate.

Black allocation is unchanged and still atomically sets the mark before READY
and block discovery. The x64 traced template is unchanged in this tranche.

## Measurement

Both detached worktrees were built from `c18c4a0e` with identical optimized
flags and pinned to CPU 8. The valid stopped-GC wrapper re-stops collection
after each benchmark-local explicit collection. Five alternating
`BENCH_SCALE=.5` pairs measured:

| Pair | Baseline (ns/op) | Conditional clear (ns/op) |
| ---: | ---: | ---: |
| 1 | 101.33 | 86.57 |
| 2 | 94.39 | 85.88 |
| 3 | 94.04 | 85.96 |
| 4 | 94.48 | 86.05 |
| 5 | 94.40 | 86.01 |
| **Median** | **94.40** | **86.01** |

The stopped median improves by 8.9%. A prior seven-pair run of the deliberately
unsafe unconditional-elision prototype measured 95.41 to 85.94 ns/op; the
conditional version retains nearly all of that signal without changing the
seeded-run-head result.

Seven alternating active-GC `BENCH_SCALE=.25` pairs for the final conditional
version measured a 304.96 ns/op baseline median and a 296.72 ns/op candidate
median, a 2.7% improvement. Every pair favored the candidate. Active
collection has higher run-to-run phase variance, so the stopped result remains
the cleaner isolation of this allocation operation.

## Coverage

`m6_jit_fnew_bump` includes a deterministic white direct-helper case which
seeds both prospective allocation-start mark bits, constructs the exact
closure/upvalue pair, and requires both bits to be clear afterward. It also
retains the existing active-black assertions that both bits are set.

The focused FNEW gate passes with strict `-Wall -Wextra -Werror`. Broader GC2
recovery, paranoia, and platform CI results are recorded with the integrating
commit rather than claimed by this design note.
