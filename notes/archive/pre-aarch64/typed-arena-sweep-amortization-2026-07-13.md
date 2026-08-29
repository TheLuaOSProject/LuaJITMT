# Typed arena sweep amortization

## Status and motivation

The pre-grace typed semantic-destruction checkpoint was correct and CI-green,
but its first valid `closures_upval` benchmark regressed from 433.63 ns/op to
488.80 ns/op with GC active. A loop-only hardware profile attributed about
396.9 of the 404.1 ns/op active-minus-stopped gap to GC/arena work. Two newly
exposed costs dominated:

- every eligible typed start repeated the complete global/arena certificate
  four times and rescanned its allocation extent five times; and
- after early semantic destruction, quarantine still revisited every one of an
  arena's 3480 usable cells in 64-cell batches even though the hot starts were
  already terminal `FREE|FREEING` no-ops.

This tranche amortizes those observations without batching semantic ownership,
weakening rescue admission, or permitting physical reuse before grace.

## Scan-wide capability versus per-object closure

The full pre-grace capability is now checked only before the exact arena seal
and once after clearing the completed pending generation. It still proves the
SWEEP/root bridge, main-TG and internal allocator identity, locally owned MT
gate, zero peers/configured workers, current worker token, outer SMR reader,
closed JIT/recorder state, and exact arena flags/owner.

Those facts remain stable while the caller retains the MT gate, worker token,
SMR lease, and arena seal. Dynamic semantic closure is deliberately not
hoisted. Each object still performs, after the first SC admission fence and
again immediately before `DESTRUCT -> FREE`:

- zero recovery/assist/weak/SSB/grey/root/finalizer work;
- no activation or SMR-reclaim veto; and
- exact arena remote state `CLOSED|SEALED`.

The exact object predicate independently reloads block, start mark, sweep,
immutable kind, READY, root, recovery, lifetime, and late lanes. The scanner
derives the allocation end once while the arena is sealed and the fixed kind's
immutable size must match it before the first claim. Block geometry cannot
change under that capability, so later predicates reuse the cached end.

The redundant body-wrapper certificate was removed. The surviving order is:

1. exact LIVE side-plane check and `LIVE -> DESTRUCT`;
2. SC fence;
3. dynamic quiet plus exact DESTRUCT check;
4. read-only typed body validation;
5. SC fence;
6. dynamic quiet plus exact DESTRUCT check;
7. `DESTRUCT -> FREE`, `WHITE -> FREEING`, and one ordinary typed destructor.

Recovery retains the same cancellation race: an earlier publisher dirties the
remote generation and fails `quiet`; a later publisher must win
`DESTRUCT -> RESCUE` against the final exact FREE CAS. No header byte is read
before the first claim/fence, and both SC fences remain per object in this
low-risk tranche.

## Packed no-op quarantine summaries

`lj_gc_reclaim_gc2_arena()` now summarizes the remaining suffix of one 64-cell
bitmap word before taking the old per-cell path. For each corresponding packed
sweep word, `WHITE` (`00`) and `FREEING` (`11`) have equal bits, while `LIVE`
(`01`) and `RETIRED` (`10`) differ. Therefore:

```text
(sweep ^ (sweep >> 1)) & 0x5555555555555555
```

is an exact one-bit-per-lane action summary. A suffix advances to its next
64-cell boundary only when it has no allocation start requiring LIVE/RETIRED
work, no late start, no root lane, and no recovery lane. An actionable word
keeps the original one-cell budget and all existing CAS/destructor behavior.
A block-zero suffix intentionally advances immediately because the old loop
also skipped every block-zero cell before consulting its side planes.

The summary is cursor progress, never reuse authority. A publisher racing the
snapshot dirties the sealed generation. Quarantine finish independently scans
for LIVE/RETIRED/recovery backedges and lowers `reclaim_cell` to the exact
actionable start; late/root publication also defeats the generation commit.
Thus a stale summary causes a bounded retry, not premature bitmap apply.

With a 64-unit limit, the hot terminal arena now reaches numeric EOF in one
reclaim visit instead of about 55 sealed visits. Completing one arena ends the
current global physical-commit quantum through a separate boolean result. This
preserves the existing `LJ_GC2_SWEEP_BATCH` limit on scheduler-visible arena
completions even though a single reclaim visit is now substantially more
productive, while the numeric result continues to report only actual work.

## Validation

The focused summary fixture checks:

- a one-unit jump over the partial first bitmap word;
- exact fallback to one-cell progress when a root lane makes the word
  actionable;
- later-cell RETIRED, recovery-PENDING, and late lanes each preventing a
  summarized jump without consuming the blocker;
- complete EOF traversal within the 64-word budget; and
- LIVE publication behind summarized EOF causing finish to retain the arena
  and rewind to the exact cell.

The production worker fixture also drives classification and grace separately,
then proves a summarized physical commit with a 64-unit limit reports exactly
one real unit and ends the global physical-commit quantum.

The existing typed fixture continues to cover exact accounting, body
preservation, independent closure/upvalue destruction, post-grace no-double
charge, locally unowned MT fallback, and MT-win/SMR-loss fallback. The first
forced-clean `m2_arena_gcsweep` run exposed an owner-quantum overshoot after the
summary made arena completion faster. Charging a completed arena as the
end of the global physical-commit quantum fixed the real scheduler contract;
the numeric progress count remains exact. The next forced-clean run passed.

The forced-clean post-change matrix passed:

- arena sweep and arena GC sweep, including a second arena-GC-sweep run after
  replacing synthetic owner-budget accounting with the explicit completion
  signal;
- GC2 recovery in ordinary and assertions/paranoia configurations;
- all four GC2 paranoia C oracles, the 509/509 stock-JIT corpus, and the
  387/387 no-JIT corpus;
- JIT FNEW bump and JIT/GC2 readiness, including hard checks and cooperative
  MARK/SWEEP TNEW, CNEW, and SNEW trace checks; and
- the GC2 worker scheduler C fixture plus its JIT-off and JIT-on Lua fixtures.

The tree was restored to the default build after every configuration-changing
suite. Two independent source/concurrency audits found no concrete safety,
liveness, mask-boundary, release-balance, or telemetry defect.

## Performance result

After a clean default rebuild, five independent active-GC samples were:

```text
387.75, 389.19, 396.32, 387.67, 393.07 ns/op; median 389.19 ns/op
```

The valid stopped-GC wrapper and contemporaneous stock binary produced:

```text
current stopped: 113.31, 107.08, 115.78, 103.68, 121.72; median 113.31 ns/op
stock active:     37.42, 39.22, 37.59, 39.39, 37.50; median 37.59 ns/op
stock stopped:    20.97, 20.81, 21.06, 20.97, 21.53; median 20.97 ns/op
```

The current active result is 20.38% faster than the pushed 488.80 ns/op
pre-amortization checkpoint and 10.25% faster than the 433.63 ns/op
rootless-kind checkpoint. Active-minus-stopped cost fell from 385.97 to
275.88 ns/op, a 28.52% improvement. The stopped current samples are notably
noisier and 10.2% above the prior 102.83 ns/op median, so that change is not
treated as a regression without replication. Current remains 10.35x stock
active and 5.40x stock stopped on this microbenchmark, so b1.2.0 is still
performance-blocked. The next profile-directed work is the terminal-word
bitmap/free-run apply cost and, separately, a narrow active-MARK numeric FNEW
certificate.
