# GC2 HugeReader recovery publication (2026-07-14)

## Failure window

A huge graph object can be marked through a counted `LJHugeReader`, then need
the allocation-free recovery plane because the mutator SSB is full. The old
fallback discarded that already-held exact admission and tried to enter the
thread-group registry SMR again. If a registry writer owned SMR, publication
failed even though the caller still held a stronger slot-local lifetime token.
The common mutator publisher then set the absorbing `recovery_failed` veto.

This was a false failure. A successful HugeReader admission pins the stable
HugeTab header and the exact mapping slot. Transfer, deletion, terminal-free
claim, and header teardown all mechanically reject a nonzero reader count. The
token remains valid after registry SMR closes and is sufficient to publish the
exact mapping's recovery state without touching TG topology.

## Scoped publication

Semantic marking paths now pass their existing `GC2MarkScope` into the common
mutator publisher. For an exact-base huge graph object whose scope owns a
covering HugeReader, recovery publication constructs a temporary wrapper over
the token's stable header and updates that exact slot directly. Counter order
is unchanged: reserve aggregate and huge-lane counts, publish
`IDLE -> PENDING`, then wake the worker. Existing `PENDING` work coalesces and
`CLAIMED` work becomes `REDIRTY` as before. The caller retains and releases its
reader; the borrowed recovery scope never changes token ownership.

The hint is used only for an exact object base. Interior cdata identity is not
a graph traversal source and does not use this path. Generic publishers retain
their registry lookup because they do not own a slot-local token. No public
structure, ABI, bytecode, or serialized representation changes.

## Terminal external free

The same audit found a deterministic liveness bug independent of registry SMR.
With a reader held, external free publishes `DEFER_FREE`; the old huge recovery
preflight accepted the entry, reserved both counters, and then lost the
`IDLE -> PENDING` CAS because that CAS correctly rejects deferred-free work.
The reader prevented the deferred handoff, so the publisher could reserve and
roll back forever.

`FREEING` and `DEFER_FREE` now discharge an otherwise-idle recovery publication
as success without a count. Terminal free has already consumed that object's
graph role. If deferred intent races after a reservation, the failed CAS rolls
back once and the next snapshot observes terminal ownership. Existing
`PENDING` or `CLAIMED` recovery still follows the ordinary coalescing/redirty
rules, so already-counted work is not abandoned.

## Deterministic coverage

The recovery fixture now performs two real races:

- it acquires a real HugeReader, pauses a real IDLE reclaimer after it owns
  registry SMR, consumes the only spare SSB node, fills all 1024 active slots,
  and invokes the production scoped mutator publisher. Publication succeeds
  exactly once through the huge recovery lane while the SSB remains full, the
  reader count remains one, and `recovery_failed` remains clear;
- an isolated child holds a reader, publishes terminal `DEFER_FREE`, and calls
  the huge recovery publisher under a two-second alarm. It returns terminal
  success with no reserved count. The previous implementation spins until the
  alarm terminates the child.

The focused recovery suite passes both release-like and
assertion-plus-`LJ_GC2_PARANOIA` builds and restores the default build. The
dependent `m2_arena_hugetab`, `m3_gc_root_pending_race`, and
`m6_jit_fnew_bump` checks also pass.

## Remaining pre-admission boundary

This checkpoint closes publication after successful HugeReader admission. It
does **not** claim that `gc2_mark_huge_candidate()` is safe when its initial
registry admission loses to an already-entered SWEEP reclaimer: at that point
there is no reader, exact locator, or body lease to pass into recovery, and a
full SSB cannot by itself revoke the reclaimer's earlier decision.

The next correctness tranche separates topology-destructive registry writers
from topology-stable physical SWEEP ownership. Huge semantic mark admission may
join the latter's existing reader count, recheck the writer state, and then
arbitrate mark-versus-free in the same 128-bit HugeTab slot. Metadata transfer
and table teardown remain exclusive. A deterministic reclaimer-after-entry
fixture is required before that boundary is considered closed. A conservative
all-huge rescan plus writer commit handshake remains the fallback design if the
narrow admission proof fails.

Ordinary throughput tuning is intentionally deferred to `b1.2.1`; hangs,
runaway behavior, and extreme regressions still block `b1.2.0`. The temporary
managed-allocation boundary still ignores custom `lua_Alloc`, as documented
elsewhere. `plan/` is unchanged.
