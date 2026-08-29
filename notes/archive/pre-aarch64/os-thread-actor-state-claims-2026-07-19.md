# Physical actor carriers for TG and `lua_State` ownership

This tranche replaces logical-TG-only stack authority with an exact physical
OS-thread carrier. It is an implementation divergence/extension of the plans;
the files under `plan/` remain unchanged.

## Representation and admission

- Each admitted OS thread receives a process-lifetime monotonic 32-bit actor
  ID. IDs use a separate saturating namespace from TG tids, never wrap, and are
  never recycled when the native thread handle is recycled.
- POSIX keeps the actor in compiler TLS and links a never-reclaimed identity
  record before exposing it. Windows keeps it in the existing cacheline-sized
  TLS cell and preserves `GetLastError()` across actor lookup and admission.
- `TGState.actor_id` publishes the physical actor currently allowed to operate
  that TG. Zero is reserved for an explicit live quiescent handoff. Detach
  clears raw/signal discovery and retires the registry entry, then atomically
  replaces the owner with a non-issued terminal sentinel before publishing
  `DEAD`. A stale binder can therefore never resurrect a retired TG through the
  zero-to-actor CAS domain.
- Multiple Lua universes on one OS thread naturally share an actor. Their TGs
  remain distinct by pointer/tid. Finalizer FIFO nesting is actor-owned, so a
  same-thread universe switch is reentrant while another pthread is excluded.

## Atomic state claim

`lua_State.thr_owner` is one aligned 64-bit atomic word:

```
high 32 bits: physical actor ID
low  32 bits: logical TG tid, GCSCAN/GCPREP sentinel, or zero
```

Normal authority requires a paired snapshot: TG actor, full state word, then a
second TG-actor validation. Claim publishes `{tid, actor}` with one CAS. Release
CASes that exact pair through `{GCSCAN, actor}`, performs NEEDSCAN handoff, then
publishes the all-zero ownerless word. Protocol sentinels also carry a nonzero
actor; actor zero is never a partial claim. The futex continues to observe the
numeric low 32 bits on both endian layouts.

All positive stack-geometry authority audited in GC2, JIT ownership, native FFI
frame scanning, table-store guards, worker startup, TG root scans and NEEDSCAN
owner scans now validates the full TG/state pair. Low-word reads remain only
where they classify wait/retry state without dereferencing owner-private stack
geometry.

## GCPREP queue handoff

The actor that wins terminal THREAD preparation publishes
`{GCPREP, producer_actor}`. After a unique queue pop, and before reading arena,
stack, callback or upvalue fields, the drainer atomically transfers that word to
`{GCPREP, drainer_actor}`. Queue linkage is the only operation covered by the
queue token itself; it does not silently supersede state actor authority.

## Quiescent moved close

`lj_thr_tg_handoff_current()` is an explicit quiescent handoff, not live TG
migration. It removes a raw target alias before CASing the TG actor to zero. If
raw/exact TLS names an unrelated nested universe it is preserved; an exact
lease naming the target must be returned first.

The raw actor-to-zero CAS is private to that handoff. Provisional teardown and
synthetic internal carrier switches use the same alias-closing primitive; no
separately callable helper can manufacture a LIVE actor-zero TG while leaving
its raw/signal mirror installed.

Actor zero on a registry-LIVE TG is therefore paired-transfer authority, not a
generic unowned TG. `lj_thr_tg_bind_current()` may consume zero only while the
body is still unpublished or its exact registry incarnation is ATTACHING (and
fails closed when shadow publication was missed). A LIVE zero can be consumed
only by `lj_thr_main_close_claim()`, which migrates the TG actor and main
`lua_State` owner word together. This prevents raw TLS installation, exact
registry installation, or a repeated lifecycle attach from creating a split
TG/state owner pair after a handoff.

`lua_close()` first claims the main TG/state pair. A moved closer may transfer
from TG actor zero, or on Linux from the exact old actor after its identity
record's destructor hint is clear and `tgkill(pid, tid, 0)` independently
returns `ESRCH`. The kernel query is the death authority: it runs after TLS
destructors; PID mismatch after `fork`, TID reuse, `EPERM`, unknown records,
macOS and Windows all fail closed. Either transfer also requires the main
carrier's C/native/table/string/parser/JIT/root-descriptor scopes to be
quiescent. Competing close actors race one TG CAS; the winner atomically retags
the main state and stays authoritative through terminal finalizers. A
completed losing arbitration performs no destructive work; this is not a
universe-lifetime admission guarantee. The coordinated two-closer fixture
waits for the loser to exit before allowing the winner to begin teardown.

This restores stock creator-exited moved close on Linux without a private
handoff. A creator which is still live but quiescent still requires the private
handoff today; a stable public/remotely-invalidatable carrier remains a b1.2.1
compatibility blocker. This does not claim arbitrary `lua_close` versus already
freed-universe lifetime safety.

This is intentionally narrower than general coroutine or live-TG migration.
Ordinary states still move only by releasing to zero and being claimed by a
valid destination TG.

A live exact owner whose TG is not the raw TLS TG (for example, a nested
universe), or whose TG is already terminal after detach, can release without a
TG-body lookup. The full state word itself is the physical capability. The
cold path CASes that exact pair to `{GCSCAN, actor}`, invalidates any earlier
owner-scan stamp, publishes an unconditional MPMC recovery identity, and only
then publishes owner zero. It neither waits for the metadata reader gate nor
dereferences a stale TG hint/body.

Owner release wakes old-tid futex sleepers while the `{GCSCAN, actor}` sentinel
still pins the complete state body. Waiters recheck ordinary owner words and
yield instead of sleeping on GCSCAN/GCPREP. Consequently a racing old-tid wait
either was already queued for that wake or fails its kernel comparison, and
the all-zero owner publication is the releasing actor's final access to the
`lua_State`. Terminal THREAD reclamation therefore cannot reuse the futex word
under a trailing wake.

## Compatibility and cost

The public Lua/LuaJIT API and ABI do not expose either internal structure.
`lua_State` grows internally and its bytecode/runtime-private layout changes;
TG layout consumes former alignment space after `tid`. The normal hot claim and
release use one 64-bit CAS each, matching the single-word arbitration model and
avoiding locks or a two-publication gap. A source-compatible internal
`lj_state_owner_rel()` remains for white-box fixtures, but it publishes a full
atomic pair and is not used by runtime writers.

Focused fixtures cover foreign-tid spoof rejection, atomic pair visibility,
actor non-reuse, cross-actor GCPREP drain transfer, nested-universe handoff, a
two-pthread moved-close race, and Linux creator-exited close with no private
handoff or raw unbind.
