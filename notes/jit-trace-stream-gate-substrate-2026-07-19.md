# Universe-global JIT TRACE stream-gate substrate (2026-07-19)

## Scope

This tranche adds the universe-global grammar descriptor and the first
structural transaction needed for eventually delivering TRACE events without
holding the recorder token. It implements one deliberately narrow shape: a
standalone `TRACE_FLUSH` becomes an immutable, payload-free event session plus
a globally visible `DETACHED_PENDING` stream while the JIT owner word is
`{0,0}`. Root- and side-trace hot admission refuse to start a new recorder
while that stream is non-IDLE or cannot be read canonically.

This is **not** the production TRACE callback cutover. Nothing in this tranche
calls a Lua VM-event handler, and the existing `lj_vmevent_send_l` paths remain
unchanged under their legacy JIT-token and universe-global VM-event owner/hook
protocol. In particular, an ordinary `jit.flush()` does not yet publish this
descriptor. The new admission/close API is presently structural substrate
exercised by the focused internal fixture.

The tranche therefore does not claim that concurrent `jit.attach()`
replacement, handler lookup, callback failure/unwind, nested delivery, or
durable pending delivery is solved. It also does not make START/RECORD/STOP or
ABORT use the new stream grammar.

## Descriptor and supported grammar

`LJJitTraceStream` is appended after `jit_event_sessions` at the end of
`TGState`; no original LuaJIT offset and no already-published event-session
offset moves. Every TG copy is initialized to zero for symmetric bootstrap,
but only the copy embedded in `g->main_tg` is ever consulted. That copy is the
single universe-global TRACE grammar authority.

The descriptor contains:

- an even/odd scalar-publication sequence;
- a monotonic nonzero stream generation and event ordinal;
- the exact owner registry slot/incarnation, logical TG id and physical actor;
- stream phase and trace number; and
- callback- and terminal-session identities for later multi-event streams.

The phase enum reserves names for the later continuation and callback grammar,
but snapshots currently accept only two canonical shapes:

1. `IDLE`, with generation, ordinal, owner identity, callback identity,
   terminal identity, reason and flags all empty; or
2. standalone FLUSH in `DETACHED_PENDING`, generation equal to
   `next_generation`, ordinal one, trace number zero, no callback session, and
   one exact terminal `TRACE_FLUSH` event session.

Any reserved future phase, half-empty registry key, noncanonical idle field or
malformed active tuple is `RETRY`, never an authority to start recording,
detach a named TG or tear down the universe. The no-callback slot uses the
out-of-range `LJ_JIT_EVENT_SESSION_SLOTS` sentinel while active; canonical
IDLE returns it to zero.

The active owner registry node is stable for the universe lifetime, so a
snapshot may validate its slot/incarnation and current published body pointer.
It never dereferences the raw TG body after that lookup: logical detach and
physical reclaim may race the scalar recheck, and the stream sequence alone is
not a TG-body lifetime lease. Admission and close perform stronger exact-owner
checks while the rooted terminal session remains authoritative.

## Provisional attachment identity

`lj_jit_trace_flush_admit_l()` takes a caller-supplied attachment generation.
It must be nonzero and is part of the exact session/handle/close identity, but
it is currently only an opaque nonce. It is **not** a `jit.attach()`
publication clock, does not identify the installed handler, and is not proof
against detach/replacement ABA.

Production delivery must replace this provisional input with an attachment
generation published atomically with the handler table/mask state. Until that
clock exists, this API cannot decide that a callback belongs to the same
attachment observed by an earlier TRACE event.

## Exact admission ordering

The caller enters structural FLUSH admission already owning the exact low JIT
token `{owner_tid,0}`, with `J->L` equal to its state. Admission additionally
requires an IDLE recorder, the current non-retired physical actor, a LIVE exact
registry slot/body, canonical stream IDLE and enough sequence/generation
headroom for the complete lifetime.

The publication order is:

1. publish an empty `DETACHED_IMMUTABLE` `TRACE_FLUSH` event session while
   `{owner_tid,0}` and `J->L` are still held;
2. claim the global stream's odd sequence, revalidate all ownership, identity
   and session fields, publish the complete `DETACHED_PENDING` descriptor, and
   return its sequence to even; then
3. clear `J->L` and release the exact owner word from
   `{owner_tid,0}` to `{0,0}`.

No allocation, GC action, handler lookup or Lua execution occurs while the
stream sequence is odd. A failed stream claim or failed pre-handoff
revalidation rolls back the still-token-protected event session. An injected
post-publication handoff failure first clears the exact stream, then rolls back
the exact session while the original token still exists. Losing that supposedly
unstealable token or failing the exact rollback is fail-stop.

The pending interval is consequently token-free: an unrelated TG may acquire
the ordinary recorder token after publication, but it cannot begin a root or
side trace because both hot paths check canonical stream IDLE before token
acquisition and again after acquiring it. The second check closes the race in
which FLUSH publishes before releasing the token that a hot peer subsequently
acquires. Refusal returns to interpreted execution; it does not wait for the
stream owner.

This hot gate prevents a new START from overtaking the structurally pending
standalone terminal. It does not stop an already-running trace or recorder and
does not yet provide the reservation needed to pair a future START with its
terminal event.

## Exact close ordering

Close requires the original registry slot/incarnation, TG id, current physical
actor, stream generation, opaque attachment nonce, terminal-session slot and
terminal-session generation. The complete descriptor and empty immutable
session must still match. A stale or modified handle cannot clear a successor.
Close also refuses while either half of the JIT owner word contains the
original TG id, catching a leaked same-owner recorder/lifecycle interval while
permitting an unrelated recorder to coexist with the immutable event storage.

The close order is deliberately asymmetric with admission:

1. claim and exactly revalidate the stream, clear every active field, and
   publish canonical stream `IDLE`; then
2. close the exact detached event session.

The global grammar becomes IDLE before retained storage is reclaimed, so a
peer may begin recording while a reader still holds the old immutable event
session. That overlap is safe at the storage layer: the session has no mutable
view, source, extra roots or borrowed control state. Session close may leave a
reader-held `CLOSED` slot whose last reader performs cleanup. Once stream IDLE
has been published, failure to close the already-proven exact session is an
impossible partial commit and is fail-stop rather than silently resurrecting
the global grammar.

## Fail-closed lifecycle and saturation rules

All public stream decisions are bounded try/refuse operations. An odd writer
sequence, changing snapshot, malformed stable shape, dead/mismatched registry
identity or unknown phase is conservative failure; none spins for a peer.

Admission reserves two complete even-to-even sequence transitions: one for
publication and one for close. Both the stream and generic event-session
publication therefore reject a sequence above `UINT64_MAX-4`; close rejects a
sequence above `UINT64_MAX-2`. A generation of `UINT64_MAX` is terminal
refusal, never wrap to zero. Event-session selectors are canonicalized as
well: stable IDLE requires the out-of-range active-slot sentinel and generation
zero, while ACTIVE requires `next_generation`, `active_generation` and the
selected slot generation to agree exactly.

Quiescence, logical detach and strict finalization fail closed when the global
stream names the target TG. Main-TG quiescence/finalization additionally
requires the entire universe stream to be canonical IDLE. Actor handoff,
same-owner `lua_close` claiming and VM/JIT freestate teardown cannot orphan an
active or corrupt stream. Naming authority is the exact registry
slot/incarnation; a torn tid or actor value cannot accidentally authorize
detaching the TG still named by that key.

## Callback ownership deliberately still missing

The existing protected VM-event callback remains serialized by the
universe-global callback owner and global hook-busy state. This tranche does
not add the required per-TG callback owner or TG-local VM-event hook bit. It
also has no persistent delivery record which distinguishes “no handler” from
“handler lookup temporarily could not acquire a safe snapshot.” Wiring the new
stream directly to the old boolean/observational lookup would therefore still
permit a terminal event to be dropped.

Those facts are why production callbacks remain untouched even though the
stream can already stay pending without holding the recorder token.

## Allocation boundary

Standalone FLUSH deliberately publishes no frozen view and no extra root
array, so this transaction performs no event-payload allocation. The broader
event-session implementation still uses C `malloc/realloc/free` for retained
view and overflow-root backing while unpublished. The project's explicitly
temporary custom-`lua_Alloc` omission is unchanged: public allocator ABI
parameters remain accepted under the separately documented internal-arena-only
policy, but arbitrary callbacks are still intentionally ignored. This tranche
neither generalizes nor narrows that temporary boundary.

## Focused evidence and platform boundary

`t-jit-flush-stream-gate.c` exercises the structural API with three attached
peer workers. While one FLUSH remains pending, every worker completes a hot Lua
loop in interpreted mode; at least one peer separately acquires the generic
JIT token and is refused by exact stream admission without changing the live
generation. The owner does not close until all peers finish, so their progress
does not depend on owner completion.

The fixture also covers odd/corrupt snapshots, half-empty registry keys,
canonical IDLE/ACTIVE selector corruption, sequence and generation saturation,
empty-session exactness, forced handoff rollback, stale stream/attachment/key/
tid/actor/session handles, stream and slot generation reuse, same-owner close
and actor-handoff refusal, secondary-TG detach refusal, and a retained session
reader spanning stream close and logical TG detach. The retained-reader case
proves stream IDLE does not imply physical slot/TG reclamation and that SMR
continues to gate the latter independently.

The tranche-specific focused concurrency execution is currently
Linux/x86-64 only. It uses pthread workers and the internal GC2 test helpers.
No macOS or Windows runtime result, sanitizer result, or performance-parity
claim is made by this note; those require later landing validation.

## Required next stages

Production cutover should proceed in this order:

1. add an atomic `jit.attach()` generation clock, tri-state exact attachment
   lookup (`FOUND`, `ABSENT`, `RETRY`) and durable pending-delivery state so
   temporary lookup contention cannot become a dropped terminal event;
2. add a per-TG callback owner and TG-local hook-busy bit, with exact nested,
   error, STOPREQ and unwind semantics;
3. cut production standalone FLUSH publication/delivery over to this stream;
4. add exact STOP and ABORT terminal shapes and retained payload/edge proofs;
5. add the persistent stream reservation which binds a recorder generation to
   its eventual terminal delivery; and
6. implement START and RECORD continuation phases on that reservation.

Only after those stages compose can the legacy TRACE callback paths and their
global serialization be retired. This note records an implementation
divergence/extension only; `plan/` is unchanged.
