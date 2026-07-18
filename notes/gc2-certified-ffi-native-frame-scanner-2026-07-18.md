# Certified GC2 scanning of generic FFI native frames (2026-07-18)

This is post-`b1.2.0` work toward the generic traced-FFI and `b1.2.1`
boundary.  It adds the first GC2 consumer for the per-TG
`LJFFINativeFrame` stack, but it does not activate `CALLXS`, consume XSAVE
staging, relax a JIT/trace-flush veto, or replace the existing conservative
stack scan.

## Why the sequence word is not a stack lease

`ffi_native_seq` proves that a copied frame payload came from one stable
publication generation.  It cannot make an old Lua stack allocation remain
mapped while the owner grows, relocates, or frees that stack.  Consequently an
arbitrary GC worker must not turn a stable frame snapshot into permission to
dereference the saved stack offsets.

The first scanner is reached only through the existing remote-native
safepoint acknowledgement.  The leader must first observe the peer in native
state and consume that peer's request.  The existing close/fence/poll protocol
then gives one of two outcomes:

- the native owner consumes the request itself; the remote scanner does not
  apply the action; or
- the remote leader consumes it, and the owner's native-leave edge remains
  parked on the consumed poll until leader completion.

Only the second outcome carries the new `native_parked` certificate.  Public
TG-only/attach catch-up and ordinary owner-root entrypoints remain
uncertified.  This distinction is explicit in the call graph; the GC does not
try to reconstruct it later from a sampled `in_native` bit.

## Exact validation and marking

Within the certified boundary and the existing GC2 SMR read lease, the scanner
takes one coherent whole-stack frame snapshot.  Before it dereferences a Lua
stack or trace body it verifies:

- the frame's `lua_State *` is the TG's currently published state;
- the state has this TG's owner id and belongs to this global state;
- the thread header, bounded `stacksize`, exact max-stack relation, base/top,
  and full allocator coverage are valid;
- the exact stack allocation has a held raw-memory mark/reader lease (including
  HugeTab-backed stacks above the small-arena threshold);
- every saved byte offset is TValue-aligned and within the current stack
  extent;
- root, live base/JIT-base, and exclusive top offsets have the required strict
  bounds and ordering, including room for the function/frame prefix;
- the diagnostic trace number still resolves through the current TraceVec to
  the exact pointer stored in the frame; and
- that exact trace body has a nonzero native pin.

The trace number is only an exact-slot cross-check.  It never substitutes for
the frame's pointer and pin.  A live-to-retired transition is allowed during
the scan because the pin keeps the exact body and its slot reservation alive.

The scanner marks the exact trace, scans materialized TValue roots through the
published exclusive top, and walks frame functions from the published base
instead of the possibly stale interpreter `L->base`.  Retirement may already
have cleared `T->traceno`, so merely queueing the trace would intentionally skip
its graph.  The pinned-body helper therefore preserves the compact body and
checks every KGC, start-prototype, per-snapshot PC owner, trace link, and
separate exit table synchronously.  Snapshot segments are bounded by their
next snapshot rather than only by the total map.  Permanent C-function and
`GG_State.bcff` fast-function pseudo-PCs are recognized as global-owned edges
instead of being misclassified as missing prototypes.  Any child admission
failure rejects the exact result.  The scanner finally rechecks thread/stack
identity and the whole-stack sequence after all reads.

## Fail-closed, additive semantics

EMPTY is a successful no-work snapshot.  An odd/changing generation requests
a retry.  A stable malformed frame, mismatched owner, invalid geometry,
mismatched trace slot, missing pin, failed semantic admission, or final
sequence mismatch also reopens the current GC root snapshot.

Regardless of exact-scan success, the pre-existing owner-root scan still runs
and native/JIT current stacks still use the broad max-stack fallback.  Exact
results therefore do not yet authorize mark closure, trace retirement, mcode
reclamation, or narrower stack scanning.  A defect in this new proof can only
retain more work and request another root pass; it cannot expand reclamation
authority.

The GC2 statistics expose certified attempts, accepted frames,
incomplete/retry results, and stable invalid snapshots.  EMPTY attempts are
counted so the safepoint fixture can prove that only the remote-native
acknowledgement branch reaches the certified consumer.

## Performance boundary

There is no new ordinary VM, allocation, JIT-entry, or interpreted FFI-call
work.  Snapshot and exact marking occur only inside a root action which already
stopped a remote native TG.  The dormant fixed frame stack remains
allocation-free and uncontended for its single owner.

The focused x64 fixture covers a retired pinned body whose public slot remains
reserved, a fast-function snapshot pseudo-PC, while the checked resolver also
admits the two permanent C-function pseudo-PCs used by stopped/stitched traces,
a materialized table root, a
HugeTab-backed Lua stack, strict offset rejection, zero-pin rejection, bogus
raw pointers, and odd-sequence retry.  The safepoint fixture separately proves
that same-TG/native catch-up does not enter this path while the consumed-poll
remote-native branch does.

## Next activation tranche

The next step is an authentic generated-code publisher which consumes the
three XSAVE staging fields, acquires the exact trace pin before losing the
ordinary JIT lifetime proof, publishes the even frame, and only then enters
native state.  Its leave path must retain the frame through the consumed-poll
boundary, preserve the foreign error pair, select an exact pinned trace body
for any forced post-call exit, and release the pin exactly once after snapshot
restore no longer needs it.

The existing FLUSHJ/JIT-base veto remains mandatory for that activation:
native pinning makes the compact body resident, but retirement may still
clear or repurpose outgoing trace-link metadata.  Generated native code must
be forced to a pinned post-call restore/exit before any such retired frame can
resume.

Callbacks, unwind/error edges, generic `CALLXS` activation, and removal of the
recorder's temporary generic-call blacklist remain later steps.  None of those
steps will reintroduce explicit C-signature/shape matching.
