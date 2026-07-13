# Packed typed-arena unmarked classifier

## Status and scope

This implements steps 1 and 2 from
`typed-arena-unmarked-word-batch-audit-2026-07-13.md` on base `04fcb68a`.
It replaces only the outer scalar scan in
`gc2_sweep_arena_unmarked_bodies()`. The exact per-object retirement and
pre-grace destructor transactions are unchanged. In particular, this tranche
does not add the separately audited same-lifetime-word transaction.

## Packed classifier

The scanner now visits one 64-cell bitmap word at a time. It acquire-loads the
word's block and mark planes, its two 32-lane sweep words, and all four
destructor planes once. The first word masks every lane below
`LJ_AFIRST_CELL`; later words admit all 64 lanes.

Each two-bit sweep word is converted to a compact 32-bit non-WHITE bitmap with
the branch-free SWAR sequence from the audit. No BMI2 instruction or target
feature assumption is required. The destructor classifier accepts exactly one
of planes 0, 1, and 2 while plane 3 is clear:

```c
multi = (p0 & p1) | (p0 & p2) | (p1 & p2);
supported = (p0 | p1 | p2) & ~multi & ~p3;
```

This admits only immutable kinds `LFUNC1`, `CLOSED_UV`, and `LFUNC0`. For the
current word snapshot:

```text
todo       = block & ~mark & ~nonwhite & valid
pin        = todo & ~supported
candidates = todo &  supported
```

`pin` is retained with one relaxed atomic OR into the mark word. This is the
same retain-only effect as the old per-cell bit-test-and-set operations, whose
old-bit results were ignored. It neither clears a racing mark nor changes
`marks_this_round`. Candidate bits are enumerated with `ctz`.

## Correctness boundary

The packed snapshot has deliberately narrow authority:

- It may skip work for this generation or conservatively retain a start.
- It cannot read an object body, retire an allocation, or publish a terminal
  lifetime/sweep state.
- Every tentative supported start reloads exact block, sweep, mark, and all
  destructor-kind planes through the original scalar accessors.
- A cached/exact kind disagreement or any unsupported exact kind sets the
  exact start's mark bit and returns.
- The existing READY/root/lifetime checks, exact extent discovery, pre-grace
  `LIVE -> DESTRUCT -> FREE` transaction, and detached retirement path remain
  the only authority for their respective effects.

Loads from separate planes may describe different instants. This is safe by
direction: a false ineligible result only defers work, and a false pin result
only retains. A tentative candidate is harmless until the exact helper
revalidates it. NEEDSWEEP arenas are not reused during classification and
quarantine; the main-TG early-destructor path additionally holds its existing
exact arena seal. Thus a stale allocation bit cannot authorize work on a new
incarnation.

The first scanned bitmap word begins at cell 576 with the current arena
layout, while `LJ_AFIRST_CELL` is cell 616. Its valid mask is therefore lanes
40 through 63. The two compact sweep halves map lanes 0--31 and 32--63 without
crossing that boundary.

## Test coverage

The guarded helpers in `lj_gc.h` expose only pure classifier operations and
the relaxed bulk OR to the C fixture. `t-arena-gcsweep.c` covers:

- all four two-bit states in every one of 32 sweep lanes;
- all 65,536 possible eight-lane sweep fragments at each of the four offsets;
- all 16 destructor-plane codes over a whole word and individually at bits 0,
  31, 32, and 63;
- the 31/32 compact-sweep boundary, bit 63, and the partial first arena word;
- 20,000 deterministic random packed snapshots against an independent scalar
  partition oracle;
- relaxed bulk pinning, preservation of all old mark bits, and unchanged
  `marks_this_round` accounting; and
- an actual arena scan containing below-prefix, first-cell, first-word-last,
  31/32/63-boundary, non-WHITE, untyped, malformed, and premarked starts.

## Validation

The implementation passed:

- strict split static/shared builds with
  `-Werror -DLJ_GC2_TEST_HELPERS`;
- forced-clean `m2_arena_sweep` and `m2_arena_gcsweep`;
- `m3_gc2_recovery`, including its paranoia variant;
- `m3_gc2_paranoia`, including 509 stock JIT and 387 stock no-JIT tests;
- `m3_gc2_worker_scheduler` with JIT off and on;
- `m6_jit_fnew_bump`; and
- `m6_jit_gc2_readiness`, including traced allocation during MARK and SWEEP.

## Paired performance

Both detached worktrees were based on `04fcb68a`, cleaned, and built with the
same default optimized configuration and `TARGET_STRIP=:`. Independent
`closures_upval` processes were pinned to CPU 8. Baseline-first and
candidate-first order alternated, and no other repository build or benchmark
ran during the samples.

Five `BENCH_SCALE=.5` pairs produced:

| Pair | Baseline (ns/op) | Packed classifier (ns/op) |
| ---: | ---: | ---: |
| 1 | 307.19 | 305.07 |
| 2 | 307.24 | 305.82 |
| 3 | 308.00 | 304.81 |
| 4 | 310.79 | 306.80 |
| 5 | 308.41 | 303.80 |
| **Median** | **308.00** | **305.07** |

The short median improves by 0.95%. Because this was smaller than the audit's
directional prototype, three independent full-scale pairs were run as a clean
retry:

| Pair | Baseline (ns/op) | Packed classifier (ns/op) |
| ---: | ---: | ---: |
| 1 | 307.36 | 305.73 |
| 2 | 307.33 | 304.70 |
| 3 | 308.28 | 305.83 |
| **Median** | **307.36** | **305.73** |

The full-scale median improves by 0.53%. All eight paired comparisons favored
the packed classifier. This is a small but repeatable improvement, not the
larger gain projected for the separately audited lifetime-word transaction.
