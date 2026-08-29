# GC2 root admission and TG lifetime authority

## Why the existing snapshots cannot close a cycle

The remaining stock-plan-86 failures are admission failures, not missing
one-shot scans.  A stack, frame, callback, coroutine, parser root, or `cur_L`
publication can begin after the last acknowledged root scan and before a
legacy phase store authorizes reclamation.  A dirty/fresh comparison is only a
point observation; rescanning or reopening a phase does not make it a lease,
and reopening WEAK/SWEEP after irreversible work is unsafe.

Raw `TGState *` TLS is also not a lifetime proof.  `registry_attached` can say
that a TG is enumerable, but it cannot stop another thread from retiring its
body or closing the containing `GG_State`.  Full-heap preservation on every
missing/wrong TLS lookup is neither bounded nor a valid close protocol.

## One exact activation and admission authority

`LJGC2Activation` carries mark epoch, non-wrapping transition generation,
phase state, and the root gate in one CX16 word.  The root gate is:

```text
OPEN -> CLOSING -> COMMIT
          |          |
          v          v
        PENDING <----+
```

A root writer publishes its per-TG descriptor with a locked CAS before it
samples the gate.  The collector exact-CASes OPEN to CLOSING before scanning
descriptors.  On x86-64, the two locked operations and the writer's subsequent
gate load give the required dichotomy:

- descriptor ACTIVE orders before CLOSING, so the closer enumerates it; or
- CLOSING orders before descriptor ACTIVE, so the writer observes a non-open
  gate and changes the same authority to PENDING before its first root store.

The collector helps every ACTIVE descriptor and can enter COMMIT only with an
exact CLOSING-to-COMMIT CAS.  A delayed PENDING CAS makes that commit lose.
Generation saturation, invalid descriptor layout, nesting, or unhelpable
storage selects sticky NO_RECLAIM rather than wrapping or guessing.

The committed descriptor is deliberately dormant today.  IDLE is not evidence
of coverage until every relevant writer is instrumented, and the token must
not authorize physical reclaim while legacy writers remain.

## Descriptor coverage

Each TG owns a 64-byte descriptor with a non-wrapping ACTIVE generation, two
explicit TValue snapshots, and up to two equal-sized stack spans.  The owner
alone starts and finishes it; helpers only take stable snapshots and never
clear ACTIVE.  For overlapping moves, helpers scan opposite the mutator copy:

- downward move: high to low;
- upward move: low to high.

Required descriptor sites include CALL, TAILCALL, RET/RETM, VARG, coroutine
resume/yield/handoff, stack relocation, `cur_L`/`thread_L` publication, parser
and loader native windows, JIT exits and side exits, FFI calls, callbacks, and
metamethod/native frames.  A range descriptor covers the source and destination
through the complete move, including stack-base relocation.

## Stable TG keys and body leases

`LJTGSlotToken` is the body-lifetime authority for a stable external handle.
A key is `{slot, incarnation}`; the slot address remains stable while TLS or a
control record can retain the key.  The CX16 word contains incarnation,
lifecycle, and body-lease count:

```text
EMPTY -> ATTACHING -> LIVE -> DETACHING -> RETIRED -> RECLAIMING -> EMPTY
             \----------------> RETIRED  (failed attach)
```

ATTACHING, LIVE, and DETACHING can be borrowed.  RETIRED closes admission while
preserving all leases, including one permanent owner-body lease.  Remote
releases can drain RETIRED only to that final lease.  Physical finalization is
admitted by the single exact `RETIRED, leases=1 -> RECLAIMING, leases=0` CAS,
which consumes the owner lease without ever exposing a reclaimable
`RETIRED, leases=0` interval.  Reuse increments incarnation, so a stale key
cannot borrow the same address.  Lease saturation makes a live body PINNED.
Incarnation exhaustion enters a distinct terminal-empty state; it must not
make a stale key look like a lease on an already-freed body.

The standalone external registry slot now contains:

```c
LJTGSlotToken token;
la_u128 body_value;            /* exact {body pointer, body incarnation} */
LJTGRegistrySlot *next_all;    /* immutable after registry publication */
```

`token` and `body_value` are separate aligned CX16 words.  For EMPTY
incarnation `i`, the body value is exactly `{NULL, i}`.  Claim atomically
publishes token `{ATTACHING, i+1, owner lease}` while the body remains
`{NULL, i}`; body publication then exact-CASes that value to `{body, i+1}`.
Once published, the tagged body is immutable for the remainder of
ATTACHING/LIVE/DETACHING/RETIRED and all borrow leases.  Reclamation
exact-CASes `{body, i+1}` to `{NULL, i+1}` before publishing EMPTY.
Consequently, a delayed clearer cannot erase a republished body even if
allocator reuse gives the new incarnation the identical pointer.

The body carries its `global_State` association.  The stable slot and its
`next_all` link remain allocated until universe shutdown; only the keyed body
incarnation is reclaimed and reused.  Any body/tag mismatch under an unchanged
borrowable token exact-pins that incarnation as no-reclaim.  The one expected
exception is the rootless ATTACHING publication gap described above.  A new
slot is still private then, while a reused slot is already on the immutable
registry spine.  Exact borrow and enumeration report bounded `BUSY` and
acquire no lease.  The attach descriptor/activation revalidation must cover a
collector which skipped that gap before any roots are published.

Body leases cover subordinate storage too: allocator state, root-anchor
blocks, temporary buffers, embedded published SSB nodes, and any storage a
helper can reach through the TG.  An installed TLS binding holds one ordinary
long-lived body borrow in addition to the registry's owner lease.  This keeps a
forgotten or stale TLS cache from becoming a use-after-free: RETIRED reclaim
stays busy until the binding is cleared.  It protects the TG body, not the
independent lifetime of the whole Lua universe.

## Attach ordering

Attachment must be split into prepare/commit.  Current code publishes Lua/TLS
roots before list insertion and performs handshake catch-up before the list
CAS; a leader can finish in that gap.  The replacement order is:

1. Keep the Lua universe alive through the entry operation.
2. Claim a stable slot in ATTACHING with its owner body lease.
3. Release-publish body/global pointers.  CAS-link a newly allocated stable
   slot; a reused slot is already on the immutable spine.  Root-bearing TG
   fields are still nil.
4. Install the stable key in TLS.
5. Publish an ATTACH root descriptor covering the intended thread, stack, and
   owner roots.
6. Sample the exact activation/gate.  On CLOSING/PENDING/COMMIT, publish
   PENDING and help before the first root store.
7. Publish `cur_L`, `thread_L`, `thread_ud`, and `tg_hint`, and apply phase
   mirrors from that same typed activation snapshot.
8. Revalidate activation, transition ATTACHING to LIVE, revalidate again, and
   only then finish the descriptor and permit VM/JIT/native execution.

If setup fails, exact ATTACHING-to-RETIRED abort closes helper admission while
preserving outstanding leases.  No attach path waits for a trace boundary; a
new TG can remain interpreted with trace entry disabled until its generation
is current.

## Detach and reclaim ordering

Detach publishes a descriptor and enters DETACHING before clearing remotely
readable roots.  It consumes requests and flushes SSB/accounting, then clears
root publications and the TLS binding before the final owner-side use of the
TG.  Only that completed boundary may transition to RETIRED.  Remote leases
then drain toward the permanent owner lease; any thread can attempt the exact
`RETIRED/1 -> RECLAIMING/0` CAS.  A busy attempt simply leaves work for another
helper, and no separate owner-release operation exists.

Existing TG-list SMR and handshake waits remain temporary compatibility
mechanisms.  They cannot be used in the final nonblocking protocol.  Stable
handles replace TG-body SMR; separate versioned authorities are still needed
for JIT code, string tables, and other independently retired metadata.

## Safe migration sequence

1. Land and test dormant activation/gate, root descriptor, and TG-slot tokens.
2. Add stable external handles and convert TLS/list enumeration to scoped body
   leases without changing GC close decisions.
3. Mirror every legacy phase edge through the typed activation word and assert
   coherence; legacy predicates remain additional prerequisites.
4. Instrument attach/detach and VM/native/JIT/FFI root writers.  Missing or
   saturated coverage forces NO_RECLAIM.
5. Make exact COMMIT a necessary reclaim condition, then migrate each physical
   reclaim site.
6. Remove legacy phase/freshness/SMR close authority only after schedule tests,
   stock stress, TSAN, Wine, and Darling prove complete writer coverage.

This sequence intentionally rejects point-freshness reopening, unbounded
full-heap overflow scans, and raw-TLS fallbacks.  Those experiments are being
removed rather than retained as alternate safety paths.
