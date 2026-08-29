# Same-lifetime-word typed-arena pre-grace batch

## Status and scope

This implements steps 3 and 4 from
`typed-arena-unmarked-word-batch-audit-2026-07-13.md` on the committed packed
classifier base `827033b1`. It amortizes the existing rootless typed-object
pre-grace destruction transaction across the supported starts selected in one
16-cell lifetime word.

The change does not cache a quiet epoch, add a wait or lock, weaken the
existing capability/certificate, infer identity from an adjacent allocation,
or replace immutable destructor-kind dispatch with a C-call shape match. Every
disagreement still reaches the existing scalar destruction/retirement/pin
fallback. No plan divergence was needed for this tranche.

## Generic selected-lane CAS

The new internal helper accepts a packed word, a bitmap containing the least
significant bit of each selected lane, a lane width, and exact source and
destination values. It derives the complete selected mask and performs:

```text
old = acquire_load(word)
if (old & selected_mask) != exact_source_bits:
  fail
next = (old & ~selected_mask) | exact_destination_bits
acq_rel_CAS(old, next), with acquire failure ordering
```

A CAS loss updates `old`. If every selected lane still agrees, the helper
rebuilds `next` around the returned word, preserving unrelated changes. If any
selected lane disagrees, it returns without modifying any selected lane. The
lifetime wrapper uses four-bit lanes and the sweep wrapper uses two-bit lanes.

The masks passed by production code contain only correctly spaced lane-low
bits built from exact cell indices. A lifetime word has at most 16 selected
starts and lies wholly within one sweep/root/recovery word and one word of all
64-bit cell planes. The helper also rejects a mask containing any non-low lane
bit without changing the word.

## Batch construction and preflight

The packed classifier's supported candidates are split into four consecutive
16-cell groups per bitmap word. For one nonempty group, preflight:

- reloads the exact immutable destructor kind and requires it to equal the
  tentative packed kind;
- accepts only `LFUNC0`, `LFUNC1`, and `CLOSED_UV`;
- derives the kind's fixed size and requires the exact sealed allocation
  extent to contain that number of cells;
- rejects overlapping admitted extents;
- reloads exact block, mark, WHITE sweep, kind, READY, root, recovery,
  lifetime, and late state; and
- constructs exact selected masks for the lifetime/sweep words and each of
  the four destructor planes.

Before the first claim, one packed predicate repeats every selected side-plane
check. It ignores unrelated lanes but requires exact agreement for every
selected start. The already-held arena seal protects the extent snapshot and
prevents allocation reuse while the bounded transaction runs.

## Transaction and linearization

For every admitted start in one lifetime word, the batch executes:

1. one all-or-none `LIVE -> DESTRUCT` selected-lane lifetime CAS;
2. the original sequentially consistent admission fence;
3. the unchanged complete dynamic `gc2_sweep_pregrace_quiet()` certificate;
4. one exact packed selected-side-plane predicate requiring `DESTRUCT`;
5. the existing immutable-kind body validator for each fixed-layout object,
   saving its pointer without dispatching a destructor;
6. the second original sequentially consistent fence;
7. the unchanged complete quiet certificate and exact packed predicate again;
8. one all-or-none `DESTRUCT -> FREE` selected-lane lifetime CAS;
9. one all-or-none `WHITE -> FREEING` selected-lane sweep CAS;
10. exact immutable-kind dispatch through `lj_func_free()` or
    `lj_func_freeuv()` once per saved object; and
11. one release publication of the boolean grace request.

The lifetime `FREE` CAS is the body-ownership linearization point. A recovery
reader which wins any selected `DESTRUCT -> RESCUE` transition makes the whole
selected CAS fail. If the writer wins, no selected reader can subsequently
acquire `RESCUE` or access a body through that lane. The per-lane reader/writer
SC-fence proof is unchanged; only the number of simultaneously claimed lanes
has changed.

An unexpected sweep CAS loss after the fully revalidated lifetime `FREE`
linearization remains fail-stop in release builds, matching the scalar path.
There is no unsafe partial rollback after ownership has become terminal.

## Rollback and scalar fallback

Every failure before the terminal lifetime CAS dispatches no destructor. The
batch invokes the existing exact rollback helper for every admitted start:

- a still-owned `DESTRUCT` lane is restored to `LIVE`;
- a recovery-owned `RESCUE` lane is not stolen;
- a recovery-restored `LIVE` lane is accepted; and
- an already complete `FREE|FREEING` terminal owner is accepted.

The outer scanner then sends every uncommitted candidate through the unchanged
scalar helper. Thus malformed extents, mutable-body disagreement, capability
loss, or a recovery race cannot strand work or broaden packed-snapshot
authority. A batch may admit the valid subset left after exact preflight, but
its admitted subset always claims, rolls back, and commits all-or-none.

## Focused coverage

Guarded C helpers exercise the selected-lane primitive independently:

- every one of 16 lifetime lanes and every one of 32 sweep lanes;
- all lifetime lanes and all sweep lanes in one operation;
- malformed low-bit masks with zero mutation;
- selected-lane disagreement with zero mutation;
- deterministic unrelated-lane mutation after the helper's first load,
  proving a losing CAS retries without clobbering that lane; and
- deterministic selected-lane `RESCUE` mutation, proving the retry does not
  steal it.

The runtime sweep fixture covers:

- one `LFUNC0` batch plus a mixed `LFUNC1`/`CLOSED_UV` batch;
- dense physically valid mixed batches of five and six starts in one lifetime
  word;
- exact accounting, zero deferred tickets, `FREE|FREEING`, and retained
  block/READY/kind metadata until quarantine grace;
- physical metadata clearing after grace without a second semantic charge;
- cdata disagreement, fixed flags, a wrong header type, and a malformed exact
  extent, with no batch destructor on rejection;
- scalar fallback progress for the independent valid peer and post-grace
  recovery of rejected work; and
- synchronous recovery publication after the packed claim. Recovery steals
  one `DESTRUCT` lane through `RESCUE`, restores it to `LIVE`, the peer rolls
  back without being stolen, the batch commits zero objects, both starts
  retire safely, and the durable recovery item drains normally.

The five- and six-object fixtures retain all generated closures until an exact
mixed lifetime word is found, then remove the Lua roots with GC stopped. This
uses real allocator geometry instead of manufacturing body layouts.

## Validation

The implementation passed:

- a clean production split static/shared build with `-Werror`;
- a strict helper build through the `m2_arena_gcsweep` fixture;
- eight additional fresh repetitions of the focused fixture;
- the focused fixture linked to an assertion/paranoia helper build;
- forced-clean `m2_arena_sweep` and `m2_arena_gcsweep`;
- `m3_gc2_recovery`, including its paranoia variant;
- `m3_gc2_paranoia`, including 509 stock JIT and 387 stock no-JIT tests;
- `m3_gc2_worker_scheduler` with JIT off and on;
- `m6_jit_fnew_bump`; and
- `m6_jit_gc2_readiness`, including traced allocation during MARK and SWEEP.

The initial broad run also found a build-configuration defect in the new test
hook path constants: production code referenced constants that were declared
inside the helper-only header guard. Moving the enum to the always-visible
header section fixed the production build; the complete matrix above was then
rerun from clean builds.

The explicit paranoia-linked focused fixture found a second assertion-order
defect in the pre-existing immutable-kind body validator. Its `gco2uv()` and
`gco2func()` conversions asserted the expected header type before the code
could reject a mismatched header. The validator now acquire-checks the header
first and performs the typed conversion only after agreement. Ordinary builds
already returned fail-closed through the subsequent check; the reordered check
makes that intended rejection valid under assertions too.

## Paired performance

A detached baseline worktree at `827033b1` and the candidate worktree were
clean-built with the same optimized default configuration and
`TARGET_STRIP=:`. Independent `closures_upval` processes were pinned to CPU 8,
and no other repository build or benchmark ran during the samples.

Five baseline-then-candidate `BENCH_SCALE=.5` pairs produced:

| Pair | Classifier base (ns/op) | Lifetime batch (ns/op) |
| ---: | ---: | ---: |
| 1 | 304.07 | 281.81 |
| 2 | 304.61 | 283.69 |
| 3 | 304.91 | 280.39 |
| 4 | 304.63 | 282.04 |
| 5 | 305.42 | 280.88 |
| **Median** | **304.63** | **281.81** |

The short median improves by 7.49%.

Three full-scale samples per variant used order
baseline/candidate, candidate/baseline, baseline/candidate:

| Sample | Classifier base (ns/op) | Lifetime batch (ns/op) |
| ---: | ---: | ---: |
| 1 | 306.85 | 283.36 |
| 2 | 305.56 | 282.24 |
| 3 | 305.65 | 282.43 |
| **Median** | **305.65** | **282.43** |

The full-scale median improves by 7.60%. All eight paired comparisons favor
the batch. The candidate's 282.43 ns/op median is within the audit's 275--290
ns/op absolute target. The gain over the already-landed classifier is smaller
than the audit's projected 10--15% combined improvement from its older
321--322 ns/op base, so the measured claim remains 7.60% against `827033b1`.

CPU-clock profiles collected at `BENCH_SCALE=1` lost zero samples:

| Variant | Timed ns/op | Samples | Main typed unmarked symbol |
| --- | ---: | ---: | ---: |
| classifier base | 306.83 | about 30,000 | 21.31% |
| lifetime batch | 285.13 | about 28,000 | 12.23% |

In the candidate profile, `gc2_sweep_dtor_obj` and
`gc2_sweep_alloc_end` appeared separately at 2.31% and 1.33%; they remain
per-object work. The packed batch removes most of the repeated claim, fence,
quiet-scan, and terminal-CAS cost while retaining exact body validation.
