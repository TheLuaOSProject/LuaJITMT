# Counted small-recovery reservation lifetime (2026-07-14)

## Failure attribution

An intermittent heavy JIT/GC stress failure reached the absorbing GC2
reclamation veto with this diagnostic shape:

```text
phase=MARK cycle=4 smr_reclaiming=0 recovery_items=2
huge_recovery_items=0 recovery_published=1 recovery_drained=0
```

This rules out a HugeTab recovery publisher and an SMR registry writer. The
failure was in the small-object fallback used after a mutator's SSB could not
rotate. One publisher had reserved the aggregate `recovery_items` close veto
and changed the object's lifetime lane, but had not yet published `PENDING` in
the per-cell recovery plane. A second publisher saw the occupied lifetime lane,
returned failure, and `gc2_publish_mutator_()` conservatively made
`recovery_failed` permanent. No edge was actually lost: the first publisher
already owned exact durable work.

The old encoding used generic `MUTATING` for both that recovery gap and other
body/layout/root ownership. Treating every `MUTATING` observation as a
successful coalescence would instead permit a real edge drop, because generic
ownership has no implied recovery count or exact retry identity.

## Dedicated `RECOVERY` lifetime

Small-object initial recovery publication now uses a distinct four-bit
`RECOVERY` state. Its publication order is:

1. reserve one aggregate `recovery_items` count;
2. claim `LIVE/CONSTRUCT -> RECOVERY` for the exact allocation start, or
   `DESTRUCT -> RESCUE` for cancellable collection reclaim;
3. validate the immutable type and allocation extent while that lifetime owns
   the body;
4. publish recovery `IDLE -> PENDING`;
5. restore the saved lifetime (`LIVE`, or `CONSTRUCT` only while its constructor
   remains incomplete); and
6. account the publication and wake a worker.

The count reservation precedes the visible `RECOVERY`/`RESCUE` state. A second
semantic publisher which observes either state can therefore return success:
the exact allocation already has a phase-close veto, and its owner must either
publish `PENDING` or take the existing fail-closed corruption path. A generic
`MUTATING` observation still returns failure because it proves no such work.
No loser waits for the first publisher.

Recovery draining is intentionally different. Once `PENDING` is visible, the
side plane already supplies durable exact identity, so the worker continues to
claim the body with generic `MUTATING`. Only the reserve-before-`PENDING` gap
uses `RECOVERY`.

A second sampled-state crossover also matters. An initial publisher can read
recovery `IDLE`, then observe `MUTATING` after a worker has admitted an existing
`PENDING` item. It now rechecks the side plane before rejecting the generic
state. Non-`IDLE` means durable work and retries normal side-state handling. If
the worker completed meanwhile, its lifetime restore happens before its
release publication of `IDLE`, so one final lifetime acquire observes the
restored state and retries. Only a stable `MUTATING + IDLE` pair is rejected.

## Encoding and generated code

`RECOVERY` retains packed nibble value 3. Generic `MUTATING` moves to value 6.
The existing x86-64 VM and traced-FNEW classifiers used nibble 3 specifically
for the constructor/recovery crossover, so preserving that value keeps their
bit decision trees valid. Named comparisons and comments now use `RECOVERY`,
and compile-time assertions pin the VM-visible `FREE=0`, `LIVE=1`,
`CONSTRUCT=2`, and `RECOVERY=3` contract. The generated FNEW state-matrix test
also proves that generic `MUTATING=6` reaches the corruption arm rather than
masquerading as recovery.

Rooted and rootless constructor commit/abandon paths now accept only
`RECOVERY` as the recovery crossover. Their symmetric post-restore repair is
unchanged. Generic mutation ownership does not gain constructor authority.

## Deterministic regression

The recovery fixture fills the active SSB, removes its sole replacement, and
pauses publisher A after it has reserved the count and installed `RECOVERY`
but before `PENDING`. A production table barrier then becomes publisher B. It
must coalesce without setting `recovery_failed`; after A resumes, the sole
recovery traversal discovers B's newly stored child and discharges exactly one
count. A negative control installs generic `MUTATING` and proves that direct
recovery publication neither succeeds nor invents a count or side-plane state.

A second fixture pauses publisher B immediately after its initial `IDLE`
sample, lets publisher A create a real counted `PENDING` identity, and models
the drain's exact `PENDING + MUTATING` pre-claim gap before resuming B. B must
coalesce without adding a count or setting the sticky veto. Direct state-pair
checks cover the completed-drain `IDLE + LIVE` retry and stable generic
`IDLE + MUTATING` rejection.

The focused normal, assertion+GC2-paranoia, and default-restore
`m3_gc2_recovery` variants pass with these fixtures. The checkpoint also passes
`m2_arena_hugetab`, `m3_gc_root_pending_race`, `m6_jit_fnew_bump`,
`m6_jit_token`, normal and heavy `m6_jit_flush_thread_stress`, and a clean
production amalgamated build followed by a JIT+GC+generic-iteration+FFI runtime
smoke. The heavy flush case completed four workers, 96 rounds, and 192
short-lived-thread churn iterations without a retry.

## Scope boundaries

This fixes the observed small-object reserve-before-locator race. Huge recovery
publication still has a separate transient SMR/registry-transfer retry problem
and is a later recovery milestone. The temporary b1.2.0 allocator boundary is
also unchanged: managed allocation currently ignores a custom `lua_Alloc`, as
documented elsewhere, and restoring compatible custom allocator behavior
remains required after GC2/JIT correctness is established. `plan/` is
unchanged.
