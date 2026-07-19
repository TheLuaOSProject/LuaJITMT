# Per-TG JIT event callback-owner substrate (2026-07-19)

## Scope

This tranche adds the bounded per-thread-group ownership and local hook
exclusion needed before a rooted JIT event session may invoke Lua with the
universe JIT owner word at zero.  It remains structural: production TRACE
FLUSH/START/RECORD/STOP/ABORT delivery is not switched by this change.

The callback handler itself is not rooted by the owner descriptor.  The exact
ACTIVE `LJJitEventSessionSlot` remains the sole root authority for both the
initiating `lua_State` and the admitted function.  Consequently callback owner
release must precede session and stream close.

## Tail-only layout

`LJJitEventCallbackOwner` is appended after `TGState.vmevent_regkey`.  It does
not move any original LuaJIT field or any previously published GC2, session,
stream, attachment-clock, FFI, dispatch, or VM offset.  On x86-64 the new
descriptor is exactly 64 bytes and its five generation/sequence words are
eight-byte aligned.

The descriptor publishes:

- an even/odd scalar sequence;
- a monotonic callback generation;
- the exact stream and session generations;
- CALLING or UNWINDING state;
- physical actor, event, and session slot; and
- an `owner_L` comparison identity rooted by the named session.

Logical tid and registry incarnation are already authorities in the enclosing
TG/session/stream identities, so they are not duplicated in this per-TG tail.

## Bounded state machine

The only legal progression is:

```text
IDLE -> CALLING -> UNWINDING -> IDLE
```

Each edge claims one even sequence with a single CAS, performs only bounded
scalar validation/publication while odd, and publishes the next even sequence.
No edge allocates, executes Lua, enters GC, performs a safepoint, waits for a
peer, or retries behind a peer.  Admission reserves sequence headroom for all
three transitions and refuses generation exhaustion rather than wrapping.

Canonical IDLE has every current identity field zero/null while retaining
`next_generation`.  Active states require a nonzero generation equal to
`next_generation`, a nonzero stream and session generation, a live physical
actor, a valid event/slot, and a non-null owner state.  Snapshot readers reject
odd, changing, unknown, or half-empty publications.  Exact transition handles
prevent stale actor/session/stream/generation authorities from clearing a
successor.

Claim consumes an already reader-held exact session snapshot with one admitted
callback root.  This both avoids a second handler-table observation and binds
the callback owner to the session identity the GC scanner can prove.  After a
successful claim, the temporary snapshot reader is released before entering
Lua: the owner now prevents session close, while avoiding a long GC2 SMR scope
across `collectgarbage()`, allocation, yielding, or arbitrary callback code.

## TG-local hook exclusion

Claim uses one atomic try-CAS to install `HOOK_ACTIVE|HOOK_VMEVENT` in
`TGState.hookmask_th`.  It refuses immediately if local ACTIVE, VMEVENT, or
PROFILE is already present.  Release clears exactly ACTIVE/VMEVENT with one
atomic fetch-and, preserving unrelated local bits.

This path never claims `g->vmevent_owner` and never writes the universe-global
hook mask.  Callback owners on separate TGs can therefore overlap, while a
nested callback on the same TG returns BUSY without waiting.

Execution gates which need owner-local suppression observe:

```text
global hookmask | current TG hookmask_th
```

The x86-64 VM performs the same acquire observation in its pcall, recording,
return-hook, and instruction-hook gates.  C dispatch recording/debug gates and
root/side-trace admission use the initiating TG.  Universe lifecycle policy,
public debug-hook queries, and GC-hook ownership continue to consult the global
mask where their semantics are genuinely global.

The per-TG profiler cannot install PROFILE over an active local callback, and
callback claim refuses an already installed profile overlay.  A defensive
profile dispatch which lost that race clears only PROFILE and defers its
accumulated samples; callback bits remain intact.  A later timer tick may
publish a fresh request.  Legacy BC/TEXIT/ERRFIN delivery also refuses local
callback recursion before claiming its temporary global owner, and the JIT
owner symmetrically refuses when that legacy owner already names the same TG.
An unrelated TG's legacy owner does not serialize this local path.

## GC and lifecycle coupling

GC2 continues to mark roots exclusively through the event session.  Alongside
that scan it snapshots the callback descriptor:

- IDLE is valid with an ACTIVE but not-yet-called session;
- CALLING/UNWINDING must exactly match session TG, L, generation, slot, event,
  actor, and the presence of one callback root;
- the local ACTIVE/VMEVENT pair must agree with owner state; and
- odd or malformed owner publication requests a root-scan retry.

Normal session unpublication, session quiescence, logical TG detach, physical
TG finalization, and VM close all fail closed until the callback owner is
canonically IDLE.  This prevents the comparison-only `owner_L` from outliving
its root authority.

## Production handoff still required

The first production consumer remains standalone TRACE `"flush"`:

1. prepare the exact clocked handler while the low JIT token is held;
2. publish a detached FLUSH session carrying that callback root;
3. publish the global stream callback phase and release the JIT token;
4. acquire an exact session snapshot, claim this per-TG owner, then release the
   snapshot reader before invoking Lua;
5. catch error/STOPREQ and move CALLING to UNWINDING;
6. restore stack/base/current-state/TG/J fields;
7. release the callback owner, close stream/session, and only then propagate
   deferred STOPREQ.

After FLUSH, the same mechanism will be applied to detached STOP/ABORT and then
to START/RECORD continuation sessions.  Legacy non-JIT VM events can migrate
off the global owner separately.

## Validation at landing

- focused owner regression, including same-TG refusal, simultaneous owners on
  two TGs, stale-handle rejection, hook/profile collision, saturation, and a
  full GC with owner active after releasing the temporary session reader;
- event-session, FLUSH stream-gate, dispatch/redispatch, recorder-token,
  secondary-recorder, and x86-64 explicit-exit gates;
- strict default, no-JIT, disabled-VM-event, and combined no-JIT/no-VM-event
  builds;
- normal and amalgamated GC2-only runtime/retired-symbol gates; and
- cross-platform CI after push.
