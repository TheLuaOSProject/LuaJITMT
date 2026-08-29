# FNEW destructor publication fast path (2026-07-13)

## Problem

The rootless closure/upvalue bump path paid for destructor-sidecar discovery in
two stages. Each object first claimed its lifetime lane, then independently
reloaded block, READY, root, lifetime, and the complete destructor kind before
publishing each kind bit. After the bodies became discoverable, construction
commit repeated those discovery checks before the final lifetime CAS.

Profiles with GC stopped made these costs dominant after redundant white-mark
clears were removed: destructor-kind publication, lifetime reservation, and
construction commit accounted for most of the remaining traced FNEW overhead.

## Change

`arena_reserve_lifetime_kind()` now preflights the exact selected start mask
before claiming lifetime. It checks block, READY, and all four destructor
planes. The common pair case uses one combined mask and one load per packed
plane; the uncommon split-word case checks both exact bits. Unrelated bits in
the same words are ignored.

After the exact FREE -> CONSTRUCT lifetime claim, the arena bump owner is the
sole structural writer for those undiscoverable starts. It publishes the
immutable destructor-kind bits with ordinary word stores. The later release
publication of block orders those stores before any reader may decode a body.
No fallible metadata operation remains between the successful exact claim and
publication; assertion builds recheck the claimed lanes and final exact kinds.
If the preflight or claim fails, no authority is erased and the bump cursor is
restored to the unpublished span.

The rootless FNEW publishers then attempt the exact CONSTRUCT -> LIVE CAS
directly after READY/block publication. Any CAS miss, including recovery
interposition or a split lifetime-word pair, falls back to the full existing
recovery-aware commit helper. The fast path therefore removes redundant
discovery loads without weakening the slow-path state machine.

## Deterministic coverage

`t-arena-sweep.c` now verifies:

- block and READY veto before either pair lifetime lane is claimed;
- veto by each of the four destructor planes;
- an exact bump cursor and FREE lifetime lanes after every rejection;
- preservation of unrelated block, READY, and destructor bits in the same
  packed words;
- exact same-word pair publication; and
- the independent split-destructor-word pair arm.

The focused gates passed on the final source:

- `m2_arena_sweep`
- `m2_arena_gcsweep`
- `m6_jit_fnew_bump`
- `m3_gc2_recovery`, including its assertion/paranoia build

## Paired performance

Both binaries were production builds pinned to CPU 8. Each sample is
`BENCH_SCALE=0.5` `closures_upval` (best of five internal runs), with five
alternating baseline/candidate pairs. The baseline is the pushed white-mark
probe optimization (`b5b7a88d`, source-equivalent detached build).

| mode | baseline median | candidate median | change |
| --- | ---: | ---: | ---: |
| GC stopped after every harness collection | 86.39 ns/op | 72.47 ns/op | -16.11% |
| ordinary active GC2 | 296.73 ns/op | 283.46 ns/op | -4.47% |

All five stopped pairs and all five active pairs favored the candidate. This
is an allocation-side improvement; typed lifetime-word sweep batching is a
separate compatible tranche and was measured independently.
