# GC2 SWEEP-stable Huge admission (2026-07-14)

## Problem

The central SWEEP reclaimer previously used the same binary registry-SMR
writer state as an IDLE metadata reclaimer. Once that state was published, an
ordinary marker could no longer enter the TG/HugeTab registry. This was safe
only for pointers admitted before the writer. It was not safe for a semantic
edge published after SWEEP admission:

1. the SWEEP owner could already hold an unmarked, retired HugeTab snapshot;
2. a mutator could publish the only new semantic reference after that owner
   closed ordinary registry SMR;
3. the marker could neither acquire a HugeReader nor publish recovery; and
4. the stale SWEEP snapshot could win terminal ownership.

Failing closed after step 2 is too late. Sticky `NO_RECLAIM` prevents later
cycles, but it cannot revoke a physical claim made by the already-admitted
SWEEP owner.

## Tagged reclaimer modes

`smr_reclaiming` is now a mode rather than a boolean:

- `LJ_GC2_SMR_OPEN` permits ordinary registry readers;
- `LJ_GC2_SMR_META_EXCLUSIVE` is used by IDLE retired-metadata and dead-TG
  reclamation, which may change registry-container topology; and
- `LJ_GC2_SMR_SWEEP_STABLE` is used by the active SWEEP body reclaimer. It may
  change exact object entries and reclaim bodies, but it does not remove the
  TG/HugeTab containers through which those entries are found.

Existing worker/reclaimer exclusion continues to treat every non-OPEN value as
closed. IDLE and TG writers require `META_EXCLUSIVE`; the JIT retired-body drain
accepts the exact phase-appropriate mode.

## Narrow Huge registry lease

An ordinary SMR reader still requires `OPEN`. A new internal Huge-only lease is
also admitted while an existing owner holds `SWEEP_STABLE`:

1. increment the process-visible registry reader count;
2. take the matching sequentially consistent fence;
3. recheck that the mode is still `SWEEP_STABLE`; and
4. roll the count back if the mode changed.

This lease deliberately does not install ordinary-reader TLS. Nested generic
registry operations still require `OPEN` and cannot inherit a weaker proof.

The narrow lease protects only the TG/HugeTab container scan. Any positive
result that will inspect a body must finish, before releasing the lease, in an
exact-slot HugeTab operation that atomically publishes `MARK` and acquires a
`HugeReader`; a negative scan touches registry slots only. No payload byte is
read before that CAS. Therefore:

- if the marker wins, the stale SWEEP claim observes `MARK`, a reader, or
  recovery and loses;
- if the SWEEP claim wins, the marker's full-slot CAS loses and it never reads
  the body; and
- after the registry lease is released, the slot-local reader alone pins the
  mapping for every body read and recovery publication.

The same count also prevents a later metadata-topology writer from entering
after the current SWEEP owner reopens the mode. The current SWEEP owner does not
need to wait for the count because all object deletion is arbitrated in the
same exact HugeTab slot.

The specialized path is used for exact and range Huge marking, observed-object
validation, return-PC validation, semantic Huge candidates, and raw registered
memory marking. The raw-memory path first attempts the existing shared
small-arena registry rescue transaction, then the exact Huge mark-reader CAS.

## JIT overlap

An intermediate design added another exclusive submode around trace retirement.
That was rejected. It recreated the late-Huge-marker hole and caused small
semantic marks to be classified dead while the submode was held.

Trace logical retirement may overlap admitted readers:

- trace graph/link fields changed by disconnect are atomic;
- compact IR and snapshot geometry read by traversal are immutable;
- retirement preserves the trace graph before disconnect; and
- exittab destruction, type invalidation, accounting, and body release occur
  only after the exact small lifetime or HugeTab destructor claim.

Consequently JIT reclaim remains inside `SWEEP_STABLE`; physical storage still
uses the per-allocation arbitration rather than a process-wide reader stop.

## Pre-destructor Huge ownership is not raw external free

The audit exposed a second bug in the old Huge GC destructor entry. It reused
`lj_arena_hugetab_claim_external_free()`. On reader contention that raw-free
API irrevocably publishes `DEFER_FREE`, because its caller has already
relinquished the allocation. A GC destructor has not: exittab/type/finalizer
side work and accounting run only after destructor admission.

If a pre-destructor attempt published `DEFER_FREE`, the last HugeReader could
terminalize the mapping and a later attempt could observe `FREEING` as owned.
For a trace that skips exittab release and `gct = 0`; for a string it skips the
type CAS and string-count decrement.

`lj_arena_hugetab_destruct_acquire()` now supplies the distinct contract used
by both ordinary and reclaim-held GC destructor entry:

- an existing `FREEING` means another terminal semantic owner is responsible
  and returns `OWNED`;
- readers, recovery, roots, a nonterminal `BUSY`, or existing deferred intent
  return `LOST` without changing any bit; and
- an uncontended exact slot is changed to `FREEING|BUSY`, clearing mark and
  retired state, before semantic mutation begins.

`LOST` is therefore retryable and never manufactures terminal intent before
the semantic destructor. The allocator's raw external-free/realloc paths are
unchanged. After an admitted semantic destructor calls `lj_mem_free()`, that
raw call sees the destructor's BUSY token and records the allocator handoff;
`lj_gc_destructor_leave()` then completes the exact mapping transaction.

## Deterministic verification

The recovery fixture creates the original no-both-miss ordering in an isolated
child: an unlinked huge userdata is retired and snapshotted unmarked, a real
SWEEP-stable writer is paused after admission, the sole spare SSB node is held,
and all 1024 active SSB slots are filled. A production `lj_gc2_markobj()` that
arrives afterward must publish exact `MARK|PENDING`; the stale writer loses and
the mapping remains live.

Separate HugeTab and production GC2 lease fixtures hold one real HugeReader and
verify that pre-destructor admission returns `LOST` without `DEFER_FREE` or
`FREEING`. Releasing the reader returns the ordinary released result, and the
next destructor attempt acquires and completes normally.

The trace-retirement fixture also allocates a valid compact trace larger than
`LJ_HUGE_THRESHOLD`, retires it, and holds its exact HugeReader through a real
IDLE reclaim pass. The first pass must keep the same trace on the retired list
without terminal flags; releasing the reader must return ordinary `RELEASED`,
and a second pass at the same completed epoch must run the trace destructor and
remove the body. This covers the actual JIT requeue path rather than only the
allocator primitive.

No last-reader wake is required for correctness. A lost trace destructor is
requeued without setting the same-epoch suppression memo; small and huge sweep
cursors retain persistent pending work; and `lj_gc2_sweep_pending()` prevents
the phase from reaching IDLE. Parked active-phase workers retry on their 10 ms
timeout, while synchronous collection keeps driving the phase. A future
latency-only wake optimization must first initialize `GCAhdr.progress_g` for
huge allocations and must snapshot it before the final reader CAS; reading the
header after a `1 -> 0` release can race immediate unmap.

The focused recovery fixture passes both release-like and
assertion-plus-`LJ_GC2_PARANOIA` builds. Neighboring arena, activation,
root-publication, sidecar, and JIT retirement suites are part of the checkpoint
gate. The no-legacy-runtime and retired-symbol gates also pass in both ordinary
and amalgamated builds.

## Performance scope

The OPEN hot path remains the ordinary nested SMR path. The extra global RMW is
paid only by a Huge registry operation that collides with active SWEEP, where
it replaces a lost mark/recovery edge rather than adding steady-state work.
Per the b1.2.0 release scope, ordinary tuning is deferred to b1.2.1 unless a
regression is runaway or on the order of hundreds of times slower.

This is an implementation extension to the design in `plan/`; no plan file was
edited.
