# Typed arena unmarked-sweep word packing and pre-grace batching audit

## Status and scope

This is a performance-design audit of
`gc2_sweep_arena_unmarked_impl()` at checkpoint `2da7b636`. The note itself is
recorded on the later `6c2bbd68` integration base. No production source change
is part of this audit.

The audit answers two narrow questions:

1. what is the smallest safe packed classifier for unmarked arena starts; and
2. how can the existing per-object pre-grace quiet proof be amortized without
   weakening rescue admission or adding a blocking protocol?

The recommended sequence is a low-risk 64-cell classifier followed by an
independent, higher-yield batch transaction restricted to one 16-cell lifetime
word. A broad cached global quiet epoch is explicitly rejected for this step.

## Profile and timing method

The benchmark was the closure-heavy active-GC workload:

```sh
taskset -c 8 env BENCH_SCALE=1 \
  src/luajit aux/bench/bench.lua closures_upval
```

The default binary was relinked with `TARGET_STRIP=:` so `perf` could retain
symbols; this does not change generated runtime code. The long profile used:

```sh
taskset -c 8 perf record -q -o /tmp/sweep-current.data \
  -e cpu-clock:u -F 3999 -- \
  env BENCH_SCALE=1 src/luajit aux/bench/bench.lua closures_upval
```

It collected about 32,000 user samples with zero lost samples and reported
322.38 ns/op. A separate `-g` object built with otherwise identical optimized
flags was used only to map hot function offsets back to inlined source sites.
Sampling skid places much of an SC fence's latency on the following
instruction, so the two post-fence entries below represent the fence plus the
start of the quiet scan rather than a cheap load in isolation.

The symbol totals were:

| Symbol | Whole-workload samples |
| --- | ---: |
| `gc2_sweep_arena_unmarked_impl.isra.0` | 21.08% |
| `gc2_sweep_dtor_obj` | 0.95% |
| `gc2_sweep_alloc_end` | 0.84% |
| `lj_func_free` | 0.15% |
| `lj_func_freeuv` | 0.15% |

The five hottest offsets inside the 21.08% aggregate were:

| Function offset | Whole workload | Mapped operation |
| --- | ---: | --- |
| `+0x49c` | 2.03% | packed lifetime `LIVE -> DESTRUCT` CAS loop |
| `+0x4a4` | 1.93% | first SC-fence skid / first quiet scan entry |
| `+0x94f` | 1.94% | second SC-fence skid / second quiet scan entry |
| `+0xc68` | 2.09% | packed lifetime `DESTRUCT -> FREE` CAS loop |
| `+0xc9a` | 2.10% | packed sweep `WHITE -> FREEING` CAS loop |

Together these sites account for about 10.1% of the entire workload and 48%
of the unmarked-body function. Destructor-plane decoding was distributed over
the outer scan and all three exact predicates, including offsets `+0x12a`,
`+0x132`, `+0x13a`, `+0x373`, `+0x37f`, `+0x874`, `+0x879`, `+0xbae`, and
`+0xbb8`.

The exact hot path for each eligible rootless typed start is currently:

1. scalar scan over cells 616 through 4095, loading block, sweep, mark, and up
   to four destructor planes;
2. scalar extent discovery;
3. exact side-plane predicate;
4. `LIVE -> DESTRUCT` lane CAS;
5. SC fence;
6. a dynamic quiet certificate of roughly twenty global fields, the
   activation veto, and exact arena `CLOSED|SEALED` state;
7. a second exact side-plane predicate;
8. typed body validation;
9. a second SC fence;
10. the complete dynamic quiet certificate again;
11. a third exact side-plane predicate;
12. `DESTRUCT -> FREE` lane CAS;
13. `WHITE -> FREEING` lane CAS; and
14. the semantic destructor.

The three lane CASes, two fences, two global quiet scans, and repeated exact
predicate loads dominate. The outer scalar classifier is worthwhile but is not
the main remaining cost.

## Smallest safe word-packed classifier

### Packed geometry

The arena has 4096 cells and 3480 usable cells. `block`, `mark`, `ready`,
`late`, and each destructor plane are 64-bit cell bitmaps. Sweep, root, and
recovery state use two bits per cell; lifetime uses four bits per cell.

Iterate one 64-cell bitmap word at a time and acquire once per word:

- `block` and `mark`;
- the two corresponding 32-lane sweep words; and
- all four destructor planes.

For one packed sweep word, first turn every nonzero two-bit state into its
lane's even bit and then compact the 32 even bits into a `uint32_t`:

```c
x = (s | (s >> 1)) & UINT64_C(0x5555555555555555);
x = (x | (x >> 1)) & UINT64_C(0x3333333333333333);
x = (x | (x >> 2)) & UINT64_C(0x0f0f0f0f0f0f0f);
x = (x | (x >> 4)) & UINT64_C(0x00ff00ff00ff00ff);
x = (x | (x >> 8)) & UINT64_C(0x0000ffff0000ffff);
x = (x | (x >> 16)) & UINT64_C(0x00000000ffffffff);
```

The result has one bit for every non-`WHITE` cell. Concatenate the two results
into a 64-cell `nonwhite` bitmap. This branch-free SWAR form avoids requiring
BMI2/`pext`, so it retains the current generic x86-64 portability boundary. An
audit-only arithmetic check passed all 65,536 possible lower-eight-lane input
states and one million randomized 64-bit inputs. Production coverage must
still test the helper in the repository's C harness.

Let `p0` through `p3` be the four destructor planes. The exact bitmap for the
three supported one-hot kinds is:

```c
typed = p0 | p1 | p2 | p3;
multi = (p0 & p1) | (p0 & p2) | (p1 & p2);
supported = (p0 | p1 | p2) & ~multi & ~p3;
todo = block & ~mark & ~nonwhite & valid_suffix;
```

`valid_suffix` masks cells below `LJ_AFIRST_CELL` in the first word. Then:

- `todo & (~typed | ~supported)` is conservatively retained with one relaxed
  atomic OR into the mark word; and
- `todo & supported` is enumerated with `ctz`, deriving its tentative kind
  from the cached planes.

The old per-cell pin operation ignored the result of
`la_bit_test_and_set64()`. A relaxed word OR has the same retain-only effect,
does not clear a racing mark, and must likewise not change
`marks_this_round` accounting.

### Authority boundary

A packed snapshot is a classifier, never body-access or terminal-commit
authority. Atomic word loads may represent different instants. Therefore:

- skipping a snapshot-ineligible cell is a conservative false negative for
  this generation;
- bulk pinning is always conservative;
- every supported candidate enters an unchanged exact per-object helper that
  reloads kind and all commit lanes;
- a stale or malformed tentative kind is pinned or sent through the unchanged
  scalar fallback after an exact reload; and
- destructor-plane, extent, READY, mark, root, recovery, late, lifetime, or
  sweep disagreement cannot authorize a body read.

The arena seal prevents allocation geometry reuse while this classifier runs,
and immutable kind remains the semantic identity. Even so, the implementation
should preserve an exact reload in the candidate helper rather than relying on
immutability as an excuse to broaden the snapshot's authority.

### Audit-only directional prototype

A temporary worktree prototype replaced the scalar outer loop with the word
classifier and bulk pin. It intentionally kept all exact inner transactions.
Its nonwhite conversion enumerated occupied even bits with `ctz` instead of
using the branch-free SWAR sequence above, so its result is directional evidence
for word-level classification rather than a measurement of the intended
helper. It built cleanly but was not run through the correctness matrix and
must not be cherry-picked.

Five short independent processes produced:

| Variant | Samples (ns/op) | Median |
| --- | --- | ---: |
| clean `2da7b636` | 322.12, 320.84, 322.33, 321.15, 321.66 | 321.66 |
| temporary word scan | 319.10, 318.39, 315.36, 316.51, 315.82 | 316.51 |

Three longer independent processes produced:

| Variant | Samples (ns/op) | Median |
| --- | --- | ---: |
| clean `2da7b636` | 321.96, 322.46, 321.99 | 321.99 |
| temporary word scan | 315.97, 318.24, 314.66 | 315.97 |

The measured median improvement was 1.60% in the short set and 1.87% in the
long set. A second long profile reported 317.72 ns/op and reduced the aggregate
function only from 21.08% to 20.59%, which is consistent with the outer scan
being useful but secondary and is also close enough to profile noise that the
wall-clock A/B is the better signal.

## Same-lifetime-word pre-grace batch

### Why this is the narrow batch boundary

A lifetime word contains 16 four-bit lanes. It lies entirely inside one
32-cell sweep word and inside one word of every 64-cell bitmap. Root and
recovery are also two-bit planes. Consequently, all selected starts in one
lifetime word can be claimed and committed with one selected-lane CAS per
state plane while unrelated lanes are preserved.

The batch remains under the existing `pregrace_owned` capability: locally held
MT exclusion, worker token, SMR lease, exact arena seal, current root/fixpoint
evidence, closed JIT/recorder state, and main-TG ownership. It introduces no
wait, lock, global epoch, or unbounded retry. The selected-lane mask can
represent at most 16 starts; fixed three-cell closure/upvalue geometry normally
admits five or six real candidates in such a window.

### Transaction

For one lifetime word:

1. Use the packed classifier to form tentative supported starts.
2. Reload exact kind and derive the fixed expected size for each start.
   Preflight its exact allocation boundary and reject overlapping or malformed
   extents before claiming any body.
3. Build a nibble mask for the admitted starts. Use one acq-rel CAS that
   succeeds only if every selected nibble is `LIVE`, replacing all selected
   nibbles with `DESTRUCT`. CAS retries caused by unrelated-nibble changes must
   rebuild from the returned word while preserving those unrelated nibbles.
4. Execute one SC fence.
5. Run the unchanged full `gc2_sweep_pregrace_quiet()` certificate once.
6. Acquire and exactly validate all selected bits in block, mark, sweep,
   destructor kind, READY, root, recovery, lifetime, and late planes. Every
   selected lifetime lane must still be `DESTRUCT`.
7. Validate each fixed-layout body with the existing immutable-kind body
   validator and retain its pointer for dispatch. A single disagreement aborts
   the whole batch before any destructor runs.
8. Execute the second SC fence, repeat the unchanged full quiet certificate,
   and repeat the exact packed selected-lane predicate.
9. Use one all-or-none selected-lane CAS to change every admitted lifetime
   nibble from `DESTRUCT` to `FREE`.
10. Use one selected-lane CAS in the containing sweep word to change every
    admitted start from `WHITE` to `FREEING`, preserving unrelated lanes.
11. Dispatch each saved object exactly once according to its exact immutable
    kind, and release-store `sweep_grace_needed=1` once for the nonempty batch.

The selected-lane CAS helper is the natural generalization of the existing
packed pair helper:

```text
if ((old & selected_mask) != expected_selected_bits)
  fail without modifying any selected lane
next = (old & ~selected_mask) | replacement_selected_bits
CAS(old, next), retrying only when all selected lanes still match
```

On any failure before the lifetime `FREE` commit, restore every still-
`DESTRUCT` selected lane individually with the existing rollback helper.
`RESCUE` and recovery-restored `LIVE` are accepted outcomes; rollback must not
steal either. Then use the unchanged scalar retirement/pin fallback. If the
terminal sweep transition contradicts the fully revalidated post-`FREE`
invariant, retain the current release-build fail-stop rather than attempting a
partial rollback after the body-ownership linearization point.

### Per-lane no-both-miss proof

The batch changes the number of lanes covered by one transaction, not the
reader/writer ordering:

```text
writer: selected LIVE->DESTRUCT CAS; SC fence; quiet/gate acquire
reader: count/gate admission RMW; SC fence; exact lifetime acquire
```

For every selected lane, a semantic reader is therefore either visible in the
writer's quiet certificate or observes `DESTRUCT` and does not read the body.
If a reader races the final batch commit, there are only two outcomes:

- its exact `DESTRUCT -> RESCUE` wins, so the writer's all-or-none masked
  `DESTRUCT -> FREE` CAS fails and no selected lane becomes `FREE`; or
- the writer's all-or-none CAS wins, so the reader cannot acquire `RESCUE` and
  observes a terminal lifetime rather than accessing the body.

One rescued lane aborts the entire selected-lane CAS. This is stronger than a
sequence of per-lane final CASes, which could otherwise leave an unsafe
partially committed batch. The reserved recovery count precedes `RESCUE`, so
the full quiet recheck also sees earlier recovery publication. The arena MT,
SMR, and seal capabilities remain held over both fences, both predicates, the
terminal transition, and destructor dispatch.

Body validation remains per object because layout agreement is not naturally a
packed property. It occurs only after all selected starts are `DESTRUCT` and
after the admission fence/quiet proof. No header byte is read from a merely
`LIVE` candidate.

## Why not add a broad cached quiet epoch

A scan-wide quiet sequence or dirty epoch appears to offer one certificate for
many independent per-object claims. It is not the smallest safe change.
Correctness would require every semantic producer to dirty the sequence before
making any work or body reachability durable, including at least:

- small and huge recovery publication;
- SSB and grey publication/consumption;
- thread-root and table rescan publication;
- assist, weak-read, weak-write, and weak-drain activity;
- finalizer queues and active finalization;
- root preparation and pending-root hints;
- JIT/recorder and activation veto transitions; and
- exact arena remote generation admission.

Missing one transition creates a body-use-after-destruction window. Updating
all of them adds an atomic operation to hot producer paths and creates a much
larger proof and test surface than the optimization being sought. A sequence
also cannot replace the per-lane `DESTRUCT` rescue handshake.

The same-lifetime-word transaction instead amortizes the existing proven
certificate across a bounded set of simultaneously claimed lanes. It requires
no new producer-side operation and makes any exact-lane recovery cancel the
whole batch atomically.

## Expected performance

The word-only audit prototype measured about 2% wall-clock improvement.
For this benchmark the supported objects occupy three cells, giving about 5.3
starts per populated lifetime word. Amortizing the five 10.1%-of-workload
CAS/fence sites by that factor has an upper contribution of approximately:

```text
10.1% * (1 - 1/5.3) = 8.2 percentage points
```

The two duplicated quiet scans and three exact predicates have additional
distributed samples, while body validation and semantic destructor cost remain
per object. A conservative combined target for classifier plus batch is a
10-15% wall-clock reduction on the fresh 321-322 ns/op baseline, or roughly
275-290 ns/op. The unmarked-body aggregate should fall from about 21% toward
6-10%. These are profile-derived targets, not release claims; allocator and
other collector costs remain outside this function.

Eliminating the entire 21.08% aggregate would be an impossible-function-only
ceiling near 254 ns/op on the same baseline, and the separately attributed
extent/body/destructor work would still remain. Measurements must therefore be
reported as paired A/B medians plus profiles, not inferred only from the
aggregate percentage.

## Focused proof and test matrix

### Packed classifier

- Exhaust every two-bit sweep state through the SWAR conversion and cover the
  first partial word beginning at cell 616, the 31/32 sweep-word boundary, and
  cell 63.
- Exhaust destructor codes for none, each supported one-hot kind, every
  multi-bit combination, and any plane-3 bit.
- Generate randomized arena plane snapshots and compare packed partitioning
  into skip, conservative pin, and supported candidate against a scalar oracle.
- Race bulk pin against concurrent mark publication and sweep-state change;
  prove the OR loses no bit and does not modify `marks_this_round` accounting.
- Inject a cached-kind versus exact-kind disagreement and prove no body read,
  conservative pin/retirement, and unchanged physical metadata.

### Selected-lane atomic helpers

- Exercise one selected lane, all 16 helper lanes, lower/upper lifetime-word
  edges, and sweep bits 31/32/63.
- Mutate an unrelated nibble or sweep lane between load and CAS and prove retry
  preserves it exactly.
- Mutate one selected lane and prove the helper changes no selected lane.
- Verify success/failure memory orders in assertions and release builds.

### Real typed batches

- Cover one, five, and six physically valid candidates, batches aligned to
  both halves of a sweep word, and mixed `LFUNC0`, `LFUNC1`, and `CLOSED_UV`
  kinds.
- Check exact semantic accounting, one destructor per object,
  `FREE|FREEING`, retained READY/kind/block/body until grace, one boolean grace
  request, and no deferred tickets.
- Inject block/mark/sweep/kind/READY/root/recovery/lifetime/late disagreement
  before claim, after the first fence, after body validation, and before final
  CAS. Every case must produce either all terminal success or no selected
  `FREE` lane.
- Inject extent overlap, an interior mark/start, wrong fixed size, cdata,
  permanent flags, wrong header type, wrong closure upvalue count, and malformed
  closed-upvalue self-reference. Prove whole-batch rollback before dispatch.
- Pause after the batch claim and publish recovery for one lane. Its
  `DESTRUCT -> RESCUE` must make the batch commit none; all other still-
  `DESTRUCT` lanes must restore to `LIVE`, and recovery count/work must remain
  durable.
- Run the reader no-both-miss litmus with reader admission before and after the
  writer's first SC fence. A reader is counted or sees `DESTRUCT`; it never
  reads a body that the batch destroys.
- Lose MT ownership, SMR admission, arena seal/generation cleanliness,
  activation/JIT capability, and worker/root fixpoint conditions. The old
  scalar sidecar-only path must remain balanced and make progress.

### Regression and performance matrix

- Run `m2_arena_sweep`, `m2_arena_gcsweep`, recovery in ordinary and paranoia
  modes, all GC2 paranoia C oracles, JIT FNEW and JIT/GC2 readiness, and worker
  scheduler fixtures with JIT both off and on.
- Preserve the existing post-grace terminal-word tests and prove no second
  destructor, charge, deferred ticket, or premature reuse.
- Repeat `closures_upval` with clean default builds, pinned CPU, independent
  processes, paired baseline/candidate order, hardware `perf stat`, and
  symbolized profiles. Reject a false-positive timing only after a clean retry;
  do not weaken a safety predicate solely from one noisy sample.

## Recommended implementation order

1. Add and exhaustively test the SWAR and one-hot helpers.
2. Land only the word classifier with exact scalar candidate authority and
   paired performance evidence.
3. Add generic selected-lane lifetime and sweep CAS helpers with standalone
   preservation/failure tests.
4. Add the one-lifetime-word pre-grace batch behind the unchanged capability,
   retaining scalar fallback for every disagreement.
5. Run the full safety matrix before treating the projected speedup as real.

This order keeps each checkpoint bisectable and preserves the central GC2
rule: packed summaries may accelerate classification, but only exact claimed
lanes plus the established admission handshake may authorize semantic body
destruction.
