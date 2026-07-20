# Production TRACE FLUSH callback cutover (2026-07-19)

## Scope

Standalone public TRACE `"flush"` callbacks now use the rooted detached event
transaction in both production producers:

- the pre-MT/direct full-flush path; and
- the sticky-MT safepoint-handshake full-flush path.

This removes production FLUSH delivery from the legacy universe-global VM-event
owner and from the shared `J->L` restoration protocol.  Non-JIT BC/TEXIT/ERRFIN
events and the remaining TRACE/RECORD phases are separate later cutovers.

The substrate and its corrected claim-before-token-release ordering are
documented in `jit-flush-callback-admission-substrate-2026-07-19.md`.

## One shared transaction

Both callsites delegate to one wrapper which consumes responsibility for a
**newly acquired disposable** low JIT token.  The transaction is:

```text
retire/flush traces while {tid, 0} is held
  -> prepare exact clocked TRACE handler on the initiating L
  -> load and publish the fixed bootstrap "flush" argument
  -> root handler in exact detached session
  -> publish pending stream and claim per-TG callback owner
  -> publish DETACHED_CALLBACK
  -> clear J->L and release {tid, 0}
  -> protected Lua callback
  -> CALLING -> UNWINDING -> IDLE
  -> clear exact stream and end exact rooted session
  -> propagate only a fresh deferred STOPREQ
```

Handler lookup and possible stack growth occur inside `lj_vm_cpcall()`.  The
`"flush"` argument is interned and fixed during single-threaded state bootstrap;
the live transaction only acquire-loads its immutable main-TG pointer and never
enters the string-table writer/wait protocol while holding the JIT token.  An
exception before handoff leaves the token owned; the wrapper releases it once
while preserving the cpcall error object for rethrow.
ABSENT, RETRY, attachment mismatch, local callback/profile collision and
stream collision are bounded dropped instrumentation events.  They restore the
entry top and release the disposable token without waiting.

Once admission succeeds, no throwing operation is allowed before exact close.
The callback itself is protected by `lj_vm_pcall_unwind()`: user handler errors
retain the stock diagnostic-and-continue behavior.  A failure of the exact
owner/stream/session cleanup grammar is fail-stop rather than permission to
strand or clear somebody else's linear authority.

## Pre-owned-token rule

`lj_jit_token_acquire_wait()` returns zero when the caller entered with an
outer recorder/control transaction.  Such a token is not disposable.  Full
flush may perform its required trace retirement, but it now drops nested FLUSH
instrumentation and neither overwrites `J->L`, detaches the token nor releases
the outer owner.

This matters for a callback which invokes `jit.flush()` recursively and for
internal control paths.  A nested flush reached from the new detached callback
normally acquires a fresh token because the outer callback has already handed
its token off; its callback admission then sees the occupied global stream and
drops immediately.  No recursion and no self-wait are introduced.

## Peer progress during the callback

The callback runs with:

- JIT owner word `{0, 0}`;
- `J->L == NULL`;
- one exact `DETACHED_CALLBACK` universe stream;
- one exact CALLING/UNWINDING per-TG callback owner; and
- one immutable session rooting the initiating state and exact handler.

A peer can therefore acquire and release the recorder token and continue in
the interpreter.  Trace publication remains fail-closed behind the active
stream descriptor, so no peer can publish a START/RECORD stream through the
detached terminal.  Once callback close makes the stream IDLE, ordinary
recording resumes.

This is the intended nonblocking distinction: the callback does not monopolize
the recorder, while the global event grammar still prevents overlapping trace
publication.

## GC2 and STOPREQ behavior

A full GC may run inside the callback.  GC2 proves the exact stream, callback
owner and session identities before marking the callback root and initiating
state.  The callback carries no long-lived SMR reader.

The protected helper captures the pre-call STOPREQ state and returns any native
safepoint actions.  The outer wrapper does not check them until callback owner,
stream and session are all closed.  A deterministic test injects
`TGF_STOPREQ|TGF_STOPREQ_FRESH` after protected return, while the owner is
UNWINDING and the stream remains active.  The public `jit.flush()` throws the
shutdown interruption only after the test can prove every JIT event authority
is idle.

## Removed legacy path

`trace_flush_vmevent_cp()` is removed.  Production FLUSH no longer calls
`lj_vmevent_call()`, claims `g->vmevent_owner`, changes the universe hook mask,
or snapshots/restores a recorder-owned `J->L` across arbitrary Lua.

The generic legacy VM-event call remains for event kinds which have not yet
received their own rooted transaction.  Removing it entirely is later work,
not implied by this FLUSH-only cutover.

## Validation

Focused production coverage proves:

- direct pre-MT and sticky-MT handshake callbacks observe token `{0,0}` and
  `J->L == NULL`;
- exact stream/TG/key/tid/actor, callback-owner and rooted-session identities;
- a full collection during the live callback followed by exact revalidation;
- guarded same-TG nested `jit.flush()` produces exactly one callback;
- a forced pre-handoff protected setup error preserves the exact error object,
  releases the disposable token and leaves every event authority idle;
- handler errors are reported, swallowed as before and followed by immediate
  successful handler reuse;
- a competing peer `jit.flush()` completes while the outer callback is paused;
- a recorder peer completes interpreted work but publishes no trace while the
  stream is active;
- the same peer records after close;
- deferred fresh STOPREQ is thrown only after stream, callback owner and
  session are idle; and
- secondary recording, explicit exits and ordinary token behavior remain
  intact.

Strict GCC default, disabled-VM-event and combined no-JIT/no-VM-event builds
pass, as does a strict Clang default build.  The production VM-event FLUSH and
recorder-token suites pass.  Normal and amalgamated GC2-only runtime/close and
retired-symbol gates pass after the cutover, so no retired collector runtime is
reintroduced.  The preceding rooted-admission checkpoint also passed hosted
Linux, macOS and Windows CI.

## Next event phases

Detached TRACE STOP and ABORT are next because they can reuse the same
token-free terminal lifetime with pinned/frozen trace evidence.  START and
RECORD follow: they require continuation-session control borrowing and exact
resume/terminal pairing rather than the simpler standalone FLUSH grammar.

The GC stream proof remains intentionally FLUSH-specific until each additional
phase lands with its complete writer, reader, callback, terminal and rollback
grammar.
