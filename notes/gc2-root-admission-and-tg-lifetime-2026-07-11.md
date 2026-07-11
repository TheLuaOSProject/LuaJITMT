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
preserving all leases.  Physical finalization requires the exact
`RETIRED, leases=0 -> RECLAIMING` transition.  Reuse increments incarnation,
so a stale key cannot borrow the same address.  Lease saturation makes a live
body PINNED.  Incarnation exhaustion enters a distinct terminal-empty state;
it must not make a stale key look like a lease on an already-freed body.

The future external handle contains at least:

```c
LJTGSlotToken token;
TGState *body;          /* release-published, acquire after body lease */
global_State *gl;       /* same lifetime rule */
uint32_t stable_refs;   /* slot/TLS-key lifetime, not a body lease */
```

Body leases cover subordinate storage too: allocator state, root-anchor
blocks, temporary buffers, embedded published SSB nodes, and any storage a
helper can reach through the TG.  An idle TLS key keeps the small stable handle
alive but must not by itself pin the whole Lua universe.

## Attach ordering

Attachment must be split into prepare/commit.  Current code publishes Lua/TLS
roots before list insertion and performs handshake catch-up before the list
CAS; a leader can finish in that gap.  The replacement order is:

1. Keep the Lua universe alive through the entry operation.
2. Claim a stable slot in ATTACHING with its owner body lease.
3. Release-publish body/global pointers, then link the empty ATTACHING handle;
   root-bearing TG fields are still nil.
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
readable roots.  It consumes requests, flushes SSB/accounting, transitions to
RETIRED, clears TLS to the stable key, then drops the owner body lease.  Any
thread can attempt bounded reclamation; failure to acquire the exact zero-lease
transition simply leaves work for another helper.

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
