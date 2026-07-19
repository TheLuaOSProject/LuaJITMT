# GC2 table-token full pass and HugeTab admission close

Date: 2026-07-18

This note records a b1.2.1 implementation tranche. It does not change `plan/`
and does not declare the live table-rescan cutover complete. The new full-pass
entry points are deliberately test-only while the production activation/close
gate is still being designed and validated.

## Problem closed by this tranche

The earlier exact table descriptor and embedded table tokens made one request
helpable, but they did not prove that a bounded scanner had visited the complete
physical table universe without a concurrent identity appearing, disappearing,
or moving. A cursor wrap and the sticky requested-generation maximum are both
insufficient certificates:

- the requested maximum is only a wake/diagnostic hint and may be raised after
  the token has already become PENDING;
- the small-arena directory and every stable-TG HugeTab are separate lanes;
- a publisher can pause at any point around a membership or token LP; and
- a counted HugeTab reader can complete a `1 -> 2 -> 1` ABA while a remover is
  preparing to consume its apparent sole reader.

This tranche supplies the lower mechanism needed to close those gaps without a
peer wait.

## Exact topology authority

`LJGC2TableTopology` is an aligned 128-bit authority containing a nonzero
completed-membership-change epoch and an OPEN/PINNED state. Every bound table
publishes a post-success change after an insertion, tombstone, transfer, stable
TG-head/body change, or terminal removal that changes the enumerated universe.
The epoch never wraps: saturation moves the authority into absorbing PINNED,
which permanently forbids manufacturing reclamation authority.

The authority is initialized before the small registry and main HugeTab become
visible. Main, secondary-TG, and registry HugeTabs bind to the same global
authority before their first membership publication. A transfer involving a
bound table is accepted only when both source and destination have exactly the
same authority; bound/unbound and differently bound moves fail closed.

The topology epoch is intentionally not an in-flight writer count. A publisher
may pause after its membership LP and before the epoch bump. Safety across that
window is mechanical:

1. a traversable insertion is admitted only with embedded token NONE and a
   clear table descriptor;
2. a removal or transfer cannot pass while the token is PENDING or the exact
   descriptor is ACTIVE; and
3. a token publication racing a pass changes the exact descriptor generation,
   which is part of the pass authority.

This avoids an anonymous outstanding-owner counter that a paused thread could
strand indefinitely.

## Non-ABA HugeTab admission close

The all-ones HugeTab reader encoding (`0xffff`) is now reserved as
ADMISSION_CLOSED. Ordinary reader admission saturates at `0xfffe`. A destructive
operation first acquires its one internal certificate, then replaces exactly
that sole count with CLOSED in one full-slot CAS. All later body-reader,
mark-reader, sweep, rescue, token, and certificate admissions reject CLOSED.
Only after the close succeeds may the destructive owner revalidate token,
descriptor, recovery, root, and deferred-free state and attempt its tombstone.

This closes the counted-reader ABA. If another publisher entered and left before
the close CAS, its durable PENDING token or still-ACTIVE exact descriptor makes
the post-close validation fail. No publisher can enter after CLOSED is visible.
On any veto, reopening clears only the reserved reader encoding, preserves all
concurrent flags, folds a deferred free when eligible, and emits the normal
progress wake.

Two additional review findings are part of the protocol:

- A mark cannot return MARK_INTENT behind CLOSED because a delete/transfer
  owner is not a traversal owner. Reader-mark admission instead atomically
  reopens and marks the mapping, returns MARK_SATURATED without a body lease,
  and forces the stale destructive CAS to lose. Metadata-only mark operations
  likewise reopen and durably publish MARK before returning success.
- Transfer rechecks DEFER_FREE after the close and before destination insert.
  If a concurrent external-free claim added DEFER while the internal lease was
  held, transfer reopens the source and lets the normal fold/wake path retain
  sole free ownership. It never copies an orphaned DEFER entry into the
  destination.

## Bounded two-lane pass

The dormant full-pass helper captures all of the following before scanning:

- the exact topology epoch;
- an IDLE table-descriptor generation and empty table pointer;
- the full activation tuple (mark epoch, generation, gate, and state);
- legacy phase and cycle;
- a zero incomplete-registry count; and
- the stable TG registry head used by the Huge lane.

It then walks the global small-arena directory followed by the captured stable
TG/HugeTab spine. A call consumes no more physical identities than its supplied
budget, so the pass can resume without waiting for a peer. Ad-hoc diagnostic
scans cannot share or disturb a live retained pass cursor. Transient snapshots,
structural inconsistencies, or unavailable SMR admission make the pass restart;
malformed/saturated authorities pin it fail closed.

After the final Huge identity, the helper performs a complete double
revalidation of topology, descriptor, activation, phase, cycle, and registry
completeness. Only a clean result publishes the paired multiword acknowledgement.
The current acknowledgement is serialized by the existing bounded
`worker_active` try-claim. It is observational test authority only: it may become
stale immediately after its validation LP and is not yet used to close a live
phase or reclaim memory.

## Deterministic evidence

Focused tests cover:

- exact topology increment under contention and absorbing saturation;
- bound insertion rejection with a preseeded PENDING token;
- same-authority transfer success and different/bound-unbound rejection;
- the pre-close reader `1 -> 2 -> 1` ABA with descriptor transfer both before
  and after token publication;
- a mark racing a successfully installed CLOSED encoding;
- a deferred external-free claim racing transfer close;
- incomplete-registry veto and bounded one-identity progress;
- descriptor races before transfer and after PENDING transfer but before IDLE;
- PENDING completion and acknowledgement in one clean pass;
- irrelevance of a saturated sticky requested hint; and
- phase/activation mutation invalidating an acknowledgement.

The tranche is validated with strict standalone helper builds, repeated focused
runs, assertion/paranoia configurations, ASan/UBSan, Valgrind, the normal GC2
and JIT smoke, and the broader milestone matrix before publication. Platform CI
remains the release evidence for Linux, macOS, and Windows.

The broad matrix also exposed an older GCC interprocedural object-size false
positive at the public sweep-boundary function: GCC retained a hypothetical
null `global_State` path through an out-of-line predicate and diagnosed later
inlined atomics as zero-sized accesses. A direct null guard now documents that
boundary and makes the clean `XCFLAGS=-Werror` matrix pass without suppressing
the warning class.

## Deliberate production boundary and next work

The legacy `table_rescan_pending` counter remains the live conservative
authority. This tranche does not enable the pass in production and does not
restore or retain an old-GC fallback. The next coherent cutover must add an
activation/publisher protocol equivalent to OPEN -> CLOSING -> COMMIT, with
bounded publisher retry/help and final authority revalidation. Only then can
phase-close predicates, traversal completion, and token publication migrate as
one unit from the legacy count.

Before live reclaim authority, the following remain mandatory:

- prove every publisher's ordering against the production close gate;
- remove any direct legacy token refresh outside the exact descriptor namespace;
- make production acknowledgement publication/read exact without a blocking
  ownership dependency;
- either retain and mechanically prove transfer's current source-wrapper
  quiescence contract, or add owner/version discrimination before permitting
  fully arbitrary concurrent transfer: CLOSED itself has no owner generation,
  so a mark may reopen it and a different destructive owner may later recreate
  the same encoding;
- define bounded fairness between ordinary scanner work and the retained pass;
- repair or permanently pin on incomplete TG-registry publication;
- keep every dirty/cycle/generation authority nonwrapping and fail closed; and
- rerun delayed-helper, same-address reuse, unmap, terminal free, saturation,
  phase-close, sanitizer, and platform stress matrices on the live path.

Physical minor collection remains gated, arbitrary custom `lua_Alloc` remains
the documented temporary internal-allocator-only exclusion, and the requested
Lua `atomic(4)` library is still sequenced after the core GC/JIT/FFI cutovers.
