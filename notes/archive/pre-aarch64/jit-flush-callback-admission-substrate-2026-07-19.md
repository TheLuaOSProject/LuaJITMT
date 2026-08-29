# Rooted FLUSH callback admission substrate (2026-07-19)

## Scope

This checkpoint completed the bounded admission and protected-call primitives
needed to move standalone TRACE `"flush"` delivery off the legacy global VM
event owner.  At this checkpoint it did **not** yet switch either production
FLUSH callsite.  The follow-up production cutover is documented in
`jit-flush-callback-production-cutover-2026-07-19.md`.

The substrate is deliberately production-shaped rather than a second
structural mock:

- the exact clocked `jit.attach()` state and generation are retained;
- the exact prepared `GCfunc` is rooted by the detached event session;
- the universe TRACE stream names that same callback session;
- the per-TG callback owner names that same stream and session; and
- the low JIT token is released only after all three publications agree.

The original structural `lj_jit_trace_flush_admit_l()` entry point remains for
focused stream tests.  Production code will use the new handler-rooted entry
point.

## Corrected handoff order

The earlier callback-owner note sketched stream publication, token release and
then callback-owner claim.  This tranche intentionally tightens that order:

```text
root exact session
  -> publish DETACHED_PENDING stream
  -> acquire exact session snapshot
  -> claim per-TG callback owner
  -> release the temporary session/SMR reader
  -> publish DETACHED_CALLBACK
  -> clear J->L and release {tid, 0}
  -> call Lua
```

Claim-before-release is the safer ordering.  Once the token reaches zero there
is no gap in which a local profile/callback collision can invalidate delivery,
and the immutable session already has its durable close exclusion before a
peer may acquire the recorder.  The temporary GC2 reader is still released
before token handoff and arbitrary Lua, so callback execution never pins an
SMR epoch.

This is a documented implementation divergence from that earlier sketch, not
a change to `plan/`.

## Production admission API

`lj_jit_trace_flush_callback_admit_l()` accepts:

- the initiating state and its held low JIT token;
- exact `INITIAL/0` or `PUBLISHED/nonzero` attachment identity;
- the prepared callback function;
- a linear stream handle; and
- a linear callback-owner handle.

Admission reserves sequence headroom for stream publish, callback-phase
publish and exact close before allocating or rooting the session.  Every odd
stream writer interval performs scalar stores only.  Saturated, odd, malformed
or occupied publications refuse without waiting or wrapping.

The detached session has no frozen trace payload and exactly one independent
callback root.  The pending stream and callback phase both name its exact slot
and generation.  Callback claim consumes an exact session snapshot, after
which the reader is immediately dropped and the owner becomes the session
close exclusion.

Success returns with:

- the stream in `DETACHED_CALLBACK`;
- the callback owner in `CALLING`;
- `J->L == NULL`; and
- the universe JIT owner word at zero.

No retry loop or peer wait is introduced.

## Rollback and close grammar

Every failure before token handoff retains the caller's original `{tid, 0}`
token and `J->L`.  Rollback is the reverse of publication:

1. move a claimed callback owner through `UNWINDING` and release it;
2. clear the exact pending/callback stream generation;
3. unpublish the exact rooted session; and
4. zero both output handles.

A forced handoff failure uses the same path.  An identity mismatch during
rollback is fail-stop because continuing would leak or clear somebody else's
linear authority.

Normal close is token-free and permits an unrelated peer to own the recorder.
It refuses while the callback owner is active, then clears the exact stream
before ending the immutable session.  The session therefore remains a root
through the entire callback and owner unwind.

## Protected Lua call boundary

`lj_jit_vmevent_call_l()` executes one already-admitted callback through
`lj_vm_pcall_unwind()` without touching `J->L`, the universe JIT owner word,
the legacy VM-event owner or the global hook mask.  It:

- validates the exact `CALLING` owner;
- requires the exact canonical `DETACHED_CALLBACK` stream named by that owner;
- bounds and aligns the byte stack offsets before any `TValue` dereference;
- proves the prepared stack function is the exact function rooted in the
  still-active session, rather than merely accepting a callable stack shape;
- records the pre-call STOPREQ state;
- publishes `UNWINDING` immediately after protected return;
- preserves the existing stderr diagnostic for handler errors;
- restores base, top, current-state and TG-hint state;
- releases the per-TG callback owner; and
- returns status, safepoint actions and the STOPREQ baseline to the transaction
  owner.

The eventual production caller must close stream/session before calling
`lj_safepoint_checkstop_fresh()`.  This prevents a deferred stop exception
from bypassing root cleanup.

The direct session-root check does not reacquire an SMR reader.  Active
callback ownership forbids exact session close, making the immutable root
vector stable until owner release.

The lower-level owner-claim primitive remains useful for scalar transition and
cross-TG exclusion tests, but a raw claim without a canonical stream is not a
Lua-execution authority.  Such structural tests perform no allocation, GC or
Lua call while active.  The protected-call boundary refuses that raw shape.

## GC2 coupling

When GC2 observes an active JIT callback owner, the session root scan now also
requires an exact active universe stream.  For this FLUSH tranche it proves:

- stream generation equals callback-owner stream generation;
- stream owner registry key, tid and physical actor name the scanned TG;
- phase is `DETACHED_PENDING` or `DETACHED_CALLBACK`;
- callback event, slot and session generation match the owner; and
- terminal event, slot and session generation match the same FLUSH session.

Odd, absent or malformed stream state requests a root-scan retry.  The pending
phase is intentionally accepted for the tiny claim-to-phase publication
window.  Once additional JIT event kinds land, this event-specific proof must
be extended rather than weakening it into a generic shape test.

## Validation

The focused regressions cover:

- sequence saturation before publication;
- local profiler collision and complete rollback;
- forced token-handoff failure after callback claim;
- exact rooted handler/session/stream identity;
- rejection of stale callback and stream generations;
- refusal to close while the callback owner is active;
- exact `INITIAL/0` attachment identity;
- successful, nested and failing protected Lua calls;
- restoration after VM unwind;
- byte-misaligned and otherwise malformed stack geometry;
- refusal of a valid but non-session-rooted function; and
- full GC while the exact callback owner and stream are active; and
- an end-to-end admitted session/stream/owner through protected Lua execution
  and token-free exact close.

The integrated callback-owner, FLUSH-stream, event-session,
dispatch/handshake, recorder-token and secondary-JIT matrix passes.  Strict
default, disabled-VM-event and combined no-JIT/no-VM-event builds pass.  The
ordinary and amalgamated GC2-only runtime/close tests and retired-symbol gates
also pass; no retired collector runtime is reintroduced.

## Next cutover

The next commit must wire both real standalone FLUSH producers into one shared
transaction:

1. prepare the exact clocked TRACE handler and `"flush"` argument while the
   newly acquired disposable low token is held;
2. call handler-rooted admission;
3. execute the protected callback with the token at zero;
4. close owner, stream and session exactly;
5. propagate deferred STOPREQ only after close; and
6. remove `trace_flush_vmevent_cp` once no production caller needs it.

Nested FLUSH from a caller which already owned the recorder token must remain a
bounded dropped instrumentation event rather than detaching somebody else's
recording transaction.  After live FLUSH, detached STOP/ABORT and continuation
START/RECORD will reuse the same exact ownership grammar.
