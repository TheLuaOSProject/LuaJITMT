# GC2 root-descriptor table-store coverage foundation

Date: 2026-07-19

This b1.2.1 tranche extends the dormant per-TG root-operation descriptor so a
future lockless phase closer can cover a table publisher whose owner remains
paused. It does not edit `plan/`, activate the exact table-token plane, or make
typed `COMMIT` a reclaim grant. All legacy GC2 close predicates remain
authoritative.

## Payload generalization

The original 64-byte descriptor carried two tagged scalar snapshots and two
TValue ranges. A scalar table mutation needs three independently retained
values: the parent table, key, and value. `LJGC2RootDesc` now has a third scalar
and an explicit table-store intent flag. The scalar convention is
`OLD=parent`, `NEW=key`, and `AUX=value`; validation rejects an incomplete
table-store payload.

The descriptor is now 96 bytes, remains 16-byte aligned, and is still one
preallocated object per TG. Its helper-written coverage word is placed after
the owner-written payload to limit false sharing on the control/payload portion.

## Paused-owner coverage certificate

Waiting for an ACTIVE descriptor to become IDLE would make phase close depend
on its owner resuming. Helpers therefore leave the descriptor ACTIVE and, only
after conservatively tracing the complete stable payload, may publish this
CX16 certificate:

```text
{ exact ACTIVE descriptor control, exact CLOSING activation generation }
```

The full control word is used rather than an address or bare generation. A
reused descriptor has a different non-wrapping control value. The activation
generation changes on every gate or phase transition, so a certificate from a
prior close cannot authorize a later close.

Coverage is monotonic by activation generation and then descriptor control. A
delayed helper cannot overwrite a newer certificate. Publication rechecks both
the descriptor and activation after the CX16 operation. Close acceptance also
rechecks both exact authorities and still requires its final exact
`CLOSING -> COMMIT` CAS. Thus a publisher appearing after descriptor
enumeration either is already covered or changes that same activation word to
PENDING and makes COMMIT lose.

Only the owner finishes a descriptor, and only after the eventual semantic
store, dirty/rescan publication, and exact table-token handoff. Helpers never
perform or clear a future store. An all-zero coverage word is the only empty
value; malformed/torn values pin the descriptor instead of being normalized.
The caller which observes that pin must also pin global activation before
reclaim can proceed.

## Remaining activation boundary

This is a storage and identity foundation, not the live gate cutover. Before
typed COMMIT can authorize progress, production still needs:

- stable borrow-carrying TG enumeration and fail-closed registry OOM handling;
- runtime helper tracing for every scalar/range descriptor form;
- publisher begin/admit/revalidate/finish guards around every table-store class;
- root-gate closure integrated with each MARK, WEAK, and SWEEP phase edge;
- a proven SWEEP crossover/rescue rule for a late table mutation;
- the post-store exact table-token handoff and metadata lifetime grace; and
- removal of every remaining scalar table-rescan close dependency as one
  coherent authority cutover.

In particular, this change does not make the current pointer-based table
descriptor safe to activate. A helper needs an independently protected
metadata lease before it can retain a token/stamp target across descriptor
clear and sidecar or Huge mapping reclamation.

## Evidence

The standalone markword/token fixture covers complete table-store payload
validation, descriptor-identity binding, absent versus exact coverage,
ACTIVE-through-COMMIT, a second close covering the same paused descriptor,
delayed-helper rejection, concurrent helper/helper and helper/pin races, IDLE
acceptance, maximum generations, and a malformed CAS winner appearing after a
helper snapshot.

`tests/t-gc2-root-gate-store-model.c` exhaustively explores publisher/closer
linearization orders. The real protocol reaches 11,604 states and 3,100 complete
schedules with no invariant failure. Five sensitivity variants all produce
counterexamples: omitting the post-publication gate check, tracing only the
parent, committing without exact coverage, finishing before token handoff, and
accepting stale coverage.

GCC and Clang strict builds pass, including ASan/UBSan. Artifact inspection
shows inline `cmpxchg16b` with no libatomic reference. A clean strict full
LuaJIT runtime build, thread-substrate fixture, and thread-lifecycle fixture
also pass. The uncached `m3_gc2_scaffold` aggregate passes end to end, including
the stock interpreter, JIT, paranoia, arena/GC, amalgamation, JIT-disabled, and
strict M0 matrix coverage.
