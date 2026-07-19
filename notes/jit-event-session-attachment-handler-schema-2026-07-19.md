# JIT event-session attachment/handler schema (2026-07-19)

## Scope

This tranche extends the dormant immutable JIT event-session substrate. An
ACTIVE session can now retain the exact VM-event attachment classification and
one independently enumerated callback function root. It does **not** invoke a
callback or change production TRACE delivery. The existing standalone FLUSH
gate remains structural: it publishes `PUBLISHED` with its provisional nonzero
generation and callback count zero. Production FLUSH delivery must later
prepare a real handler and require callback count one before entering its
callback phase.

`plan/` is unchanged.

## Canonical attachment identity

The session carries `attachment_state` beside the existing
`attachment_generation`. Publication, snapshots, GC scanning and exact release
accept only these pairs:

| State | Generation |
| --- | --- |
| `INITIAL` | zero |
| `PUBLISHED` | nonzero |
| `UNCLOCKED` | zero |

`INVALID`, unknown classifications, `INITIAL/nonzero`, `PUBLISHED/zero` and
`UNCLOCKED/nonzero` are refused. `PUBLISHED/UINT64_MAX` is a valid final
observed identity; this field is not itself an incrementing session counter.

## Layout-preserving callback root

No `LJJitEventSessionSlot`, `LJJitEventSessions` or `TGState` storage was
appended. The old 32-bit `pad` after `source_traceno` is now
`attachment_state`, and the old `root_pad` is now the explicit 0/1
`callback_root_count`. Exact x86-64 offsets and the 344-byte slot size are
asserted. Consequently the already-published `jit_trace_stream`, attachment
clock and `vmevent_regkey` offsets do not move.

`root_count` remains exclusively the number of frozen-view proof roots. The
callback function occupies the reserved sentinel lane
`root_data[root_count]`; it is never counted as `EXACT_ROOTS` evidence. Every
slot reserves `root_count + 1` lanes even when callback count is zero. Seven
proof roots plus the sentinel fit the eight inline lanes; eight proof roots
take the bounded grow path. Addition, multiplication and capacity geometry are
checked before allocation or indexing. Retained-allocation alias rejection
covers the complete capacity, including the sentinel lane.

Callback count, `LJ_JIT_EVENT_SLOT_F_CALLBACK_ROOT` and sentinel nullness must
agree exactly. Count one additionally requires an admitted real `FUNC` object;
count zero is retained for reservation/pending structural sessions. Publication
takes an exact GC2 `FUNC` lease before its first handler-body access and retains
it through the sentinel store, even publication and second active-cycle
barrier; stale, foreign-universe and wrong-type candidates are rejected without
a raw header peek. The publisher performs those root barriers for the callback
independently of the proof vector both before and after the even publication
edge. Cleanup clears the sentinel separately before clearing the proof roots
and resets classification to `INVALID`.

## Snapshot and GC2 lifetime

An ACTIVE snapshot captures classification, generation, callback count and the
exact function pointer. Acquisition holds the session reader and GC2 SMR
scope, validates vector geometry and takes a short exact `FUNC` lease. Release
compares the complete new tuple and sentinel again. The retained SMR scope
keeps the raw address memory-stable across a racing close, but a stale release
after close is not callback authority. A CLOSED reader delays slot reset and
physical reuse; callback invocation remains future work.

The owner-TG GC2 scanner marks the callback through its own typed root path,
separate from `root_data[0..root_count)`, owner-state rooting and optional
source pinning. Callback flag/count/root mismatch, non-FUNC substitution,
malformed capacity or noncanonical attachment identity turns the owner scan
into a bounded retry. Edge-proof flag checks mask out the independent callback
bit, so adding a handler cannot strengthen a frozen-view proof.

## Focused evidence

`t-jit-event-session.c` covers every accepted/rejected state/generation pair,
callback cardinality and type rejection, the seven/eight proof-root capacity
boundary, sentinel-inclusive same-slot and cross-slot alias rejection,
snapshot state/root release exactness, malformed scan geometry, flag/count/
null/non-FUNC corruption, independent callback marking, weak-value survival
through a full detached ACTIVE-session collection, and retained-reader close
followed by exact sentinel/count/flag/state cleanup and weak-value collection.
The full-GC oracle deliberately uses the implemented detached owner-word-zero
contract: invoking `collectgarbage()` from a yielded START/RECORD continuation
depends on the later same-owner control-borrow transaction and is not simulated
by this storage-only tranche.

`t-jit-flush-stream-gate.c` verifies structural FLUSH snapshots carry
`PUBLISHED`, count zero and a null reserved sentinel. Exact close refuses a
malformed attachment pair or any fabricated callback count/flag/root while the
universe stream remains active, and a CLOSED reader retains the canonical
empty schema until its final release.

## Validation

The focused event-session, FLUSH-stream, attachment-clock and recorder-token
cases pass. The attachment/token matrix includes the no-JIT clock fallback and
ordinary JIT execution. Clean `-Werror` builds pass in default, compile-time
no-JIT, disabled-vmevent, and combined no-JIT/disabled-vmevent profiles. The
GC2-only runtime/retired-symbol gate passes in both normal and amalgamated
builds, including active-SWEEP `lua_close()`, and restores a default build.

The first version of the weak oracle attempted a full GC while the synthetic
continuation had already yielded `{tid,0}->{0,tid}` but still published
`J->state=RECORD`. That correctly could not close MARK: returning from the
future callback and borrowing/resuming its exact lifecycle are not implemented
by this tranche. The final oracle uses the existing detached STOP session,
whose owner word and recorder state are both idle, then verifies collection
after exact close. This was a fixture correction, not a relaxation of the
production contract.
