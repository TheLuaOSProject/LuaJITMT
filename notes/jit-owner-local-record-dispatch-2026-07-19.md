# Owner-local recording dispatch cutover

Date: 2026-07-19

This b1.2.1 checkpoint removes the active `TRACE_START` recorder's dependency
on the global dispatch-update claim. It also closes a peer-write race exposed
while auditing that dependency. It does not claim that the mutable global
dispatch template or the remaining JIT control/event paths are fully
nonblocking.

## Defects removed

After `trace_start()` succeeded, `trace_state()` called
`lj_dispatch_update()` while retaining the recorder token. A concurrent
dispatch updater could therefore make ordinary trace start wait on
`DISPMODE_UPDATE`; a multi-thread update could then add a handshake dependency
while the same recorder token remained live.

The general updater also discovered the active recorder's TG and called
`dispatch_setrecord()` on that table, even when the updater ran on a different
TG. That was an uncertified remote write to a table the recorder's VM could
consume. A REDISPATCH acknowledgement had the opposite liveness bug: its raw
template copy could erase the recording overlay after an asynchronous abort,
leaving no recording callback to consume the abort and release `J->cur` and the
token.

## Bounded owner protocol

`lj_dispatch_record_start(L, J)` validates one exact published tuple:

```text
J belongs to G(L)
TG belongs to G(L)
token == TG.tid != 0
J.owner_L == L
TG.cur_L == L
TG actor certificate owns L
J.state != IDLE
```

It then writes only that TG's local dispatch table and revalidates the same
tuple. `dispatch_setrecord()` fills every dynamic instruction/call cell and
reads only the owner TG's static loop cells. `trace_state()` is outside VM
dispatch during this operation, so its own table cannot be consumed halfway
through the bounded overlay. Neither `DISPMODE_UPDATE`, a peer, allocation nor
a handshake is involved. A failed revalidation asynchronously aborts the
recording turn and immediately follows the existing terminal state machine.
On x64 POSIX, the overlay mode also reads the TG-local pending-profile bit so a
one-shot `lj_vm_profhook` remains ahead of `lj_vm_record`; after the profile
hook consumes that bit, the exact-owner refresh reinstalls recording.

`lj_dispatch_sync_tg()` first copies the ordinary template/profile overlay and
then reapplies the recorder overlay only after a fresh exact tuple validation.
Asynchronously aborted, non-IDLE recorders deliberately qualify: they retain
the token and need one final recording dispatch to enter `trace_state()` and
finish cleanup. REDISPATCH acknowledgements use this helper.

The general updater now refreshes only `G2TG(g)`. It no longer searches for an
active recorder or obtains a peer TG pointer. A foreign recorder's stopped
owner/ACK path is the only place that repairs that foreign table.

## Evidence

The focused C fixture forces `DISPMODE_UPDATE` busy while starting an exact
recorder and proves the owner-local overlay returns without consuming the
claim. It applies REDISPATCH before and after an asynchronous abort and proves
the recording instruction/call cells survive until IDLE, after which the
ordinary template is restored. The same fixture covers TG-local profile-hook
precedence at both start and redispatch.

A synthetic exact peer TG then publishes a recorder tuple, installs its
overlay through the authorized ACK helper and replaces one cell with a test
sentinel. A general update from the main TG leaves the peer sentinel untouched;
an explicit peer ACK reinstalls the overlay. This catches the former remote
`dispatch_setrecord(peer->dispatch, ...)` behavior deterministically without a
racy test observation.

The monotonic JIT source gate rejects a token-held global update returning to
active `TRACE_START`, unexpected direct TG overlay sites, or a raw REDISPATCH
ACK.

The clean assertion/helper `m6_dispatch_redispatch` and `m6_jit_token` cases
pass with their C fixtures compiled under `-Wall -Wextra -Werror`. A clean
`LUAJIT_DISABLE_JIT` feature build also passes. Promoting all warnings to errors
in that feature profile still exposes older unrelated unused-parameter
warnings in string/meta/table/C conversion sources; the ordinary feature build
is the relevant no-JIT compile evidence for this dispatch-only change.

## Remaining dispatch work

The global GG dispatch template is still mutable. An updater releases
`DISPMODE_UPDATE` before its multi-TG handshake, so a following updater can
mutate the template while a TG acknowledgement copies it. Besides being a
mixed-generation risk, concurrent `memcpy` and cell writes are a C data race.

The complete nonblocking design is immutable precomputed dispatch images for
the finite policy modes. A policy change atomically publishes an image
generation and marks each TG dispatch-dirty; the TG owner or a certified
native-park acknowledgement copies one coherent immutable image, applies its
profile and exact-recorder overlays, and rechecks the generation. This removes
the global update claim and redispatch handshake dependency rather than merely
making their current critical section shorter.

TRACE/RECORD callbacks and several JIT/profile control paths also still retain
the recorder token. Their rooted event-session/lifecycle cutover is a separate
next tranche.
