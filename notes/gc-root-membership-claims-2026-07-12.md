# GC-root membership and allocation-lifetime claims (2026-07-12)

## Why membership alone was insufficient

GC2 uses `GChead.nextgc` as an intrusive ownership spine.  The original
membership tranche added a packed `NONE/LINKING/UNLINKING/MEMBER` state for
each allocation start, preventing a repeated `lj_gc_linkobj*()` from
overwriting an already-linked object's successor.

Adversarial review found that this was necessary but not sufficient.  Small
free, remote-free, and bitmap-sweep paths sampled the membership plane and
mutated `READY`, `block[]`, or the allocation body later.  A linker could win
`NONE -> LINKING` between those operations, after which free could overwrite a
claimed object.  The separate recovery plane had the same two-location race.
The checkpoint was therefore withheld before commit or release.

This implementation deliberately diverges from the earlier two-bit-only
draft.  `plan/` is unchanged.

## Small-allocation lifetime descriptor

Every traversable small-allocation start has a second packed four-bit lifetime
state:

- `FREE`: reusable/unallocated storage; interior cells remain in this state;
- `LIVE`: an ordinary published or non-root-owned allocation;
- `CONSTRUCT`: an owner-private root-spine constructor which cannot be freed;
- `MUTATING`: one bounded non-destructive allocation, recovery-publication, or
  ordinary-link arbitration owns the allocation metadata;
- `DESTRUCT`: a free/sweep owner has claimed the incarnation but has not
  changed any body, `READY`, cdata, or block byte; and
- `RESCUE`: a semantic publisher won the exact cancellation race against
  `DESTRUCT`, so the untouched body is pinned and reader-visible.

No loser waits for a state transition.  It preserves or defers the object and
lets the current owner finish.  The descriptor is not a mutex: it has no wait
queue, no blocking acquisition, and no owner may sleep while another thread
depends on it for global progress.

Allocation, free, root publication, and initial recovery publication all use
this same word as their first arbitration point.  A physical free must win
`LIVE -> DESTRUCT`, perform the paired reader-count/root/recovery rechecks, and
then win `DESTRUCT -> FREE` before changing `READY`, cdata coverage, `block[]`,
sweep state, or body bytes.  A racy semantic barrier instead races
`DESTRUCT -> RESCUE`.  Exactly one CAS wins: a successful rescue leaves the
writer with no byte it may change, while a successful free makes later readers
reject the allocation before the body changes.  The span is not allocator-
visible merely because its lane is `FREE`; the destructive owner publishes the
bin/bump/bitmap identity only after its byte work is complete.

Reader admission and destructive ownership use a paired sequentially
consistent fence around the arena admission counter and lifetime lane.  A
writer therefore observes every reader which admitted before its claim, while
a later reader observes `DESTRUCT`/`FREE` before touching bytes.  This closes
the cross-location no-both-miss litmus without waiting for a reader.  Ordinary
readers accept `LIVE`, `CONSTRUCT`, or `RESCUE`; only a semantic publication
path may perform the cancelling `DESTRUCT -> RESCUE` CAS.

Sweep and quarantine claim each dead allocation start before their bulk bitmap
commit and reach the same `DESTRUCT -> FREE` terminal point before a type
destructor or bitmap/body mutation.  A conflicting root/recovery publisher or
semantic rescue makes that start live for the pass.  This removes the old
sample-then-clear race without adding a global lock or an unbounded retry loop.

## Explicit body leases

A validity result or semantic mark is not, by itself, permission to keep
dereferencing an allocation.  In particular, a reader admitted during a
committed arena-generation handoff can observe an already-live object without
changing `mark[]`; after that reader drops its admission, adoption or an
irrevocable external free may continue immediately.  Returning a pointer from
such a validator silently reintroduced the same check-then-free race that the
lifetime descriptor is intended to remove.

`LJGC2Lease` makes the read interval explicit.  `lj_gc2_obj_lease_acquire()`
validates the exact optional GC type, performs the normal semantic/direct-body
publication, and transfers the small arena's counted admission to the caller.
`lj_gc2_mem_lease_acquire()` does the same for an exact registered raw
allocation.  The caller retains the lease through its final payload byte and
then calls the idempotent `lj_gc2_lease_release()`.  HugeTab `MARK` is already
the mapping-lifetime certificate, and the temporarily disabled custom
`lua_Alloc` path cannot physically free managed bodies, so those paths may
return a valid no-op lease.

Prototype snapshot/record readers now hold typed leases through child upvalue
layout reads.  Buffer concatenation separately leases the userdata descriptor
and its COW object or exact raw backing allocation, transferring the storage
lease through the actual copy.  Thread-live and state-registry readers carry a
typed lease through every returned userdata/thread field access.  Table
readers additionally need an SMR reader (or the unique side-vector reclaimer
token): the object lease protects `GCtab`, while SMR protects array/node
generations retired by a concurrent resize.  The held table-snapshot APIs never
manufacture semantic liveness and are legal only under both certificates.

## Owner-only construction

Root-spine constructors use the request-only `LJ_AF_ROOT_CONSTRUCT` allocation
mode.  A small constructor reserves `FREE -> CONSTRUCT` and root
`NONE -> LINKING` before any header, `READY`, or `block[]` discovery.  It keeps
both claims until the complete object or chain is visible on a TG pending
stack or the global spine.  Publication then makes lifetime `LIVE` and commits
root `LINKING -> MEMBER`.  A stalled constructor therefore pins its own
incarnation and cannot resume on a reused address.

`lj_gc_linkobj_new*()` is owner-only and consumes this pre-existing
construction claim.  Ordinary requeue/link APIs never consume an observed
`LINKING`; they defer.  Constructor cancellation is also owner-only and is
separate from ordinary free.

The construction mode is intentionally not implied by `LJ_AF_TRAVERSABLE`.
Strings are owned by the intern table, and open upvalues temporarily use
`nextgc` for their per-state open list.  Those allocations become ordinary
`LIVE` objects.  Tables, functions, closed upvalues, prototypes, userdata,
threads, cdata, and traces which are destined for the ownership spine use the
explicit construction mode.  Variable/interior cdata keeps membership at its
allocation base.

Ordinary relinking is restricted to internal callers which already hold an
independent lifetime token: FINREG queue identity, a GC2 LIVE/huge TICKET, an
open-upvalue/closure owner, or terminal single-owner shutdown.  This boundary
is required because an untagged stale raw pointer cannot, in general,
distinguish a later allocation at the same address.  Public Lua code cannot
invoke the intrusive-link helpers directly.

## Recovery and huge mappings

Initial small recovery publication temporarily owns the same lifetime
descriptor, revalidates the exact allocation, publishes durable recovery
identity, and then restores the prior `LIVE` or `CONSTRUCT` state.  Free cannot
cross that transition.  If recovery meets `DESTRUCT`, it may win the exact
`DESTRUCT -> RESCUE` cancellation, publish the recovery identity, and restore
`LIVE`.  Existing `PENDING/CLAIMED/REDIRTY` recovery identity continues to veto
free by itself.  A tentative GC reclaim which loses to recovery/`RESCUE`
cancels untouched, because that publication may be the semantic edge which
made the object live.  An irrevocable external/remote logical free instead
release-publishes `late[]` before attempting `LIVE -> DESTRUCT`, without
touching the body.  If blocked it leaves that intent durable.  If it reaches
`FREE`, the owner clears `late[]` only after old-body discovery is gone.
Recovery consumes its own
identity exactly once, leaves that logical-free intent durable, and wakes the
later sweep owner.  These are separate APIs/provenance rules: `late[]` must not
turn a successful GC resurrection back into a free.

Recovery can also overlap a completed constructor's final commit.  If it owns
`CONSTRUCT -> MUTATING`, the constructor may commit root
`LINKING -> MEMBER` (or abandon to `NONE`) without waiting.  Recovery restores
`CONSTRUCT` only while root remains `LINKING`; otherwise it restores `LIVE`.
The symmetric post-restore check closes both interleavings.

Huge mappings already combine root state, recovery state, free/retire flags,
and address identity in one 128-bit HugeTab slot.  Fresh root constructors
start with `LINKING` in that slot.  External free records `DEFER_FREE` when a
root/recovery owner exists; final root/recovery completion folds the deferred
operation into a fresh-grace `FREEING|SWEEP_OLD` handoff.  Prepare/abort/finish
sweep preserve the root bits.  Address reuse still relies on the internal
caller lifetime token described above; the packed slot closes overlap, not an
arbitrary stale-pointer ABA after delete and reinsertion.

## Required terminal rule (not implemented in this checkpoint)

Normal shutdown still needs a PRE reconciliation which independently
enumerates arena and HugeTab membership, so a damaged incoming edge cannot
make a disconnected `MEMBER`, `LINKING`, or `UNLINKING` allocation lose its
only locator.  After each ownership-spine drain, POST reconciliation must
reanchor valid survivors and request another drain until quiescent.  Non-root
LIVE trace/string/table-retirement bodies must not be cleared at this point:
their subsystem destructors still own them.  A FINAL verification must run
after those subsystems drain but before GC2 registries and allocator/table
handles are destroyed.  Terminal code may reconcile abandoned states only
after all publishers, workers, JIT readers, and finalizers have joined, and an
ambiguous valid body must be a fail-stop error rather than permission to erase
its locator.  This PRE/POST/FINAL implementation and its adversarial tests are
the next lifetime tranche; the present checkpoint must not be mistaken for
terminal completion.

## Remaining verification gate

Before this lifetime tranche is complete it must cover:

- free-versus-link and recovery-versus-free in both linearization orders;
- remote body overwrite and bitmap-sweep sample pauses;
- constructor pause, cancellation, chain publication, and exact-once retry;
- small and huge LIVE reanchor without duplicate incoming edges;
- HugeTab finish for every root state, mark-preservation mode, and deferred
  free combination;
- x64 TNEW/FNEW machine-code state transitions, rollback targets, cold GC-step
  restart, packed-word boundaries, and final states;
- disconnected terminal membership with no lost arena/table locator;
- Linux assertion/paranoia and release stress, followed by x64 Windows/Wine
  and macOS/Darling build/runtime smokes.

The four-bit lifetime plane costs two KiB per 64 KiB small arena, one KiB more
than the rejected two-bit draft.  Normal root construction adds only per-
allocation packed CAS operations; it adds no allocation, global scan, wait, or
lock.  Performance measurements follow the correctness gate, and the b1.2.0
release remains blocked on fully working GC2 with JIT rather than on the final
performance target.
