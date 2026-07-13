# Exact empty reclaimed-arena adoption (2026-07-13)

## Profile and scope

After same-lifetime-word typed destruction batching and streamlined FNEW
publication were composed at `daa9260e`, one symbolized Linux/x86-64
`BENCH_SCALE=1 closures_upval` profile measured 274.11 ns/op. It collected
about 27,000 user CPU-clock samples with zero lost samples. The leading costs
were:

| Symbol | Whole workload |
| --- | ---: |
| `gc2_sweep_pregrace_batch` | 13.27% |
| `func_newL_gc1tv_bump` | 6.70% |
| `lj_arena_scan_free_runs` | 5.14% |
| `lj_arena_hugetab_sweep_next` | 4.02% |
| `hugetab_search` | 3.65% |
| `arena_reserve_lifetime_kind` | 3.45% |
| `arena_lifetime_block_bits` | 3.16% |
| `lj_mem_freegco_defer` | 3.06% |

The packed batch's hottest internal sites were its two required sequentially
consistent admission fences and three selected-lane CAS transactions. Those
operations are the rescue-reader no-both-miss proof and terminal ownership
linearization, so this tranche does not weaken, cache, or remove them.

The next independent cost was reclaimed-arena adoption. A closure-only arena
normally reaches quarantine finish with every typed body already terminal.
Quarantine's exact word proof records `live_cells=0`, but the later allocator
still enumerated every old closure/upvalue mark boundary through
`lj_arena_scan_free_runs()` before publishing the single coalesced run.

## Change

`arena_adopt_reclaimed_one()` now treats `live_cells=0` as an optimization hint
only for lifetime-managed traversable arenas. While it already owns the exact
reclaimed arena under `SEALED`, has drained remote frees, and has committed a
clean publication generation, it attempts one `arena_set_free_run()` over the
complete payload and links that one run into the private staged bins.

The existing whole-range transform remains the sole authority. Before changing
any structural boundary it checks every word for:

- zero root and recovery ownership;
- zero immutable destructor identity; and
- `FREE` in every lifetime lane.

It then scrubs READY/cdata/destructor coverage and repeats the stricter interior
ownership validation before removing old boundaries. Any mismatch returns
false and runs the unchanged `lj_arena_scan_free_runs()` fallback. The final
owner-local bin publication and exact `SEALED -> OPEN` arbitration are
unchanged.

Plain arenas are deliberately excluded. They do not have lifetime lanes, so a
stale zero live count plus owner-free side planes cannot prove that an opaque
block allocation is dead. Extending this shortcut to unmanaged arenas would
require a separate exact liveness certificate; the traversable-only form is
sufficient for FNEW closure churn.

## Deterministic coverage

The arena sweep fixture now proves:

- a genuinely terminal traversable arena takes the exact whole-payload path
  and becomes allocatable;
- a block-zero destructor kind survives terminal apply, rejects whole-payload
  coalescing, remains intact, and selects the unchanged safe-run fallback; and
- a plain arena with a deliberately stale `live_cells=0` hint and a live block
  does not take the shortcut or mutate that block.

The test-only counter records only successful exact whole-range adoption; it is
absent from production builds.

Final-source focused validation passed `m2_arena_sweep` and
`m2_arena_gcsweep`. The complete `m3_gc2_recovery` normal and
assertion/paranoia cases were rerun after the lifetime-managed restriction,
followed by the harness's default-build restore. Earlier candidate-source
validation also passed `m6_jit_fnew_bump`.

## Paired performance

Clean `daa9260e` and the candidate used otherwise identical optimized builds.
Independent processes were pinned to CPU 8 and run in alternating order at
`BENCH_SCALE=.5`:

| Pair | Clean `daa9260e` (ns/op) | Candidate (ns/op) |
| ---: | ---: | ---: |
| 1 | 259.67 | 248.44 |
| 2 | 261.31 | 248.28 |
| 3 | 261.40 | 249.02 |
| **Median** | **261.31** | **248.44** |

All three pairs favor the candidate. The median improves by 12.87 ns/op, or
4.92%. The single earlier profile's 274.11 ns/op absolute result came from a
different sampling session and is used only for cost attribution; the paired
same-session result is the performance claim.

The next independent typed-sweep opportunity is the per-object
`lj_mem_freegco_defer()` chain after the batch has already committed exact
`FREE|FREEING`. Any such change must retain immutable-kind dispatch and prove
that the supported leaf destructors have no semantic work beyond exact byte
accounting; it is not part of this tranche.
