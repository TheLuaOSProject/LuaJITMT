# Forward design: persistent TRACE stream descriptors

Date: 2026-07-19

Status: **forward design only; not implemented by this note**.

This note records the intended continuation of the rooted immutable JIT event
session substrate. It does not claim that TRACE/RECORD callbacks are token-free,
that the persistent stream grammar exists, or that the callback cutover is
complete. The plan files remain unchanged.

The design refines the event-session direction where doing so is necessary to
preserve LuaJIT's observable event grammar under concurrent execution. In
particular, an immutable detached payload may be safe beside an unrelated
recorder as a storage-lifetime matter, but the universe-global TRACE grammar
must still prevent a later START from overtaking the preceding terminal
callback.

## Objectives and required observable behavior

For every structurally admitted trace-compilation attempt, the universe must
maintain this grammar:

```text
START, RECORD*, exactly one of STOP or ABORT
```

`FLUSH` is a standalone one-event stream. Structural stream state is distinct
from handler delivery: a handler may be absent, replaced, attached midway, or
detached while the stream remains well-formed.

With one stable TRACE handler installed for an entire stream, its callbacks
must be globally noninterleaved and observe `start, record*, stop|abort`. Fresh
handler lookup at each event retains stock behavior:

- if handler A replaces itself with B during START, B receives the terminal;
- if A detaches itself during START, no terminal callback is invoked; and
- a handler attached after START may see only the terminal.

These behaviors were checked against stock LuaJIT while preparing this design.
Errors in an event handler are protected and reported. An error, including an
inner error caught with `pcall`, can asynchronously abort an active recorder;
that stream must still receive exactly one ABORT transition.

Different TGs may execute unrelated VM-event callbacks concurrently. A TRACE
or RECORD callback must not use a process-global callback mutex or wait for a
peer callback. Nested events on its own TG remain suppressed, while a terminal
for the current stream is retained and deferred rather than dropped.

## State ownership split

State is divided into one universe-global stream descriptor and per-TG
callback/session publications.

### Universe-global stream

The stream descriptor is logically universe-global. It should be stored in
tail-appended storage belonging to the embedded main TG and accessed through
`g->main_tg`. Adding it inside `global_State` or `jit_State` would move existing
GG/J/dispatch offsets. `TGState` is already the last GG member, so appending
there preserves all pre-existing offsets. Secondary TG copies of any embedded
universe-only structure remain unused.

A concrete representation is:

```c
typedef enum LJJitTraceStreamPhase {
  LJ_JIT_STREAM_IDLE = 0,
  LJ_JIT_STREAM_OPEN,
  LJ_JIT_STREAM_CONT_CALLBACK,
  LJ_JIT_STREAM_CONT_TERMINAL_PENDING,
  LJ_JIT_STREAM_DETACHED_PENDING,
  LJ_JIT_STREAM_DETACHED_CALLBACK
} LJJitTraceStreamPhase;

typedef struct LJJitTraceStream {
  uint64_t sequence;              /* Even stable, odd publication. */
  uint64_t next_generation;       /* Monotonic; zero is invalid. */
  uint64_t generation;            /* Current stream, or zero. */
  uint64_t event_ordinal;         /* START=1, RECORD*, terminal. */

  LJTGRegistryKey owner_key;      /* Stable slot plus incarnation. */
  uint32_t owner_tid;
  uint32_t owner_actor;
  uint32_t phase;
  uint32_t traceno;               /* Not identity without generation. */

  uint32_t callback_event;
  uint32_t callback_slot;
  uint64_t callback_session_generation;

  uint32_t terminal_event;        /* STOP, ABORT, or FLUSH. */
  uint32_t terminal_slot;
  uint64_t terminal_session_generation;
  uint32_t terminal_reason;
  uint32_t flags;
} LJJitTraceStream;
```

Every odd `sequence` interval is bounded and nonthrowing. It may contain only
atomic/scalar publication and exact validation. It must never contain memory
allocation, GC work, handler lookup, Lua execution, a safepoint handshake, or
a peer wait.

The complete owner identity is the stream generation, TG registry key and
incarnation, logical tid, physical actor, and initiating `lua_State` rooted by
the matching session. A trace number alone is never a stream identity because
trace slots may be reused.

### Per-TG callback ownership

Each TG needs its own callback owner, independent of the universe-global TRACE
stream:

```c
typedef struct LJJitEventCallbackOwner {
  uint64_t sequence;
  uint64_t next_generation;
  uint64_t generation;
  uint64_t stream_generation;     /* Zero for unrelated VM events. */
  uint64_t session_generation;
  uint32_t state;                 /* IDLE/CALLING/UNWINDING. */
  uint32_t owner_actor;
  uint32_t event;
  uint32_t session_slot;
  lua_State *owner_L;             /* Identity; rooted elsewhere. */
} LJJitEventCallbackOwner;
```

An exact CAS claims an idle callback owner for the current actor. A second
callback on the same TG returns busy immediately. Owners on different TGs are
independent and may overlap. Generation, actor, session and stream validation
prevent a stale leave/unwind from clearing a successor.

The callback sets `HOOK_VMEVENT` in `TGState.hookmask_th`, not in the shared
hookmask. Owner-local VM/hot/record gates must include this local bit. This
suppresses recursive events and recording only on the callback TG. The JIT
owner word or persistent stream phase separately excludes a competing TRACE
stream across the universe.

TRACE/RECORD should move to this path first. Legacy BC/TEXIT/ERRFIN ownership
can migrate separately, but those paths must recognize the TG-local busy bit so
they do not recursively enter on the same TG. A peer TEXIT callback is allowed
to overlap a TRACE callback once both use initiating-state stacks and no shared
mutable callback scratch.

## Session modes

The two session modes have deliberately different ownership contracts:

- `CONTINUATION` covers START and RECORD. Mutable recorder scratch remains
  reserved by `{token=0,lifecycle=tid}`. Callback completion must either resume
  the exact recorder generation or terminalize it without resumption.
- `DETACHED` covers completed STOP, ABORT and FLUSH payloads. It owns only its
  rooted immutable payload and exact TG identity. The ordinary JIT owner word
  may be zero while its callback is pending or running.

Storage-level DETACHED safety does not authorize a new TRACE stream. The
universe stream remains non-IDLE until the detached terminal has been consumed.

## Two-slot terminal-reservation proof

Two slots are sufficient only if one slot is reserved for the terminal for the
entire lifetime of every OPEN stream:

```text
slot A: mandatory terminal reservation for this OPEN stream
slot B: current START/RECORD CONTINUATION callback
```

The session slot state machine therefore needs at least:

```text
FREE
RESERVED_TERMINAL
BUILDING
PENDING
CALLING
CLOSED
CLEANING
```

The session aggregate must publish callback and terminal slots independently.
The current single `PUBLICATION_ACTIVE`/`active_slot` shape cannot represent an
active continuation and its guaranteed terminal reservation at the same time.

The proof relies on these admission rules:

1. A recorder cannot publish `OPEN` until it owns one
   `RESERVED_TERMINAL` slot.
2. If both slots are retained by paused readers, a hot edge returns immediately
   and Lua continues interpreted. It never starts an unfinishable stream.
3. START and RECORD callbacks may use only the non-terminal slot.
4. Before yielding to a CONTINUATION callback, the terminal reservation is
   pre-grown sufficiently to accept that callback's frozen view and root set.
   A same-callback `jit.off`, `jit.flush`, error, or asynchronous abort can then
   materialize ABORT without allocating or waiting.
5. If one old terminal is still reader-held, a later stream may reserve the
   remaining slot, but it cannot enter a CONTINUATION callback until the other
   slot becomes reusable. Its eventual terminal is still guaranteed.
6. If a required RECORD callback cannot acquire the non-terminal slot, the
   recorder aborts with retry semantics rather than calling `lj_record_ins`
   without satisfying the event contract.
7. GC scans both reserved/pending/calling slots. An empty reservation has no GC
   edges; once materialized it is an ordinary rooted event publication.

This remains nonwaiting: a paused session reader may temporarily suppress new
JIT compilation or an observational RECORD callback, but it cannot stop the
Lua mutator from continuing in the interpreter.

For an ordinary terminal outside a continuation, an unpublished ABORT view may
borrow immutable JIT scratch while the persistent stream excludes scratch
reuse. A published STOP or root-patch-lost ABORT can use the exact native source
pin. For a terminal generated inside a continuation, the reserved terminal
slot copies from the already-frozen continuation view; its capacity was secured
before invoking arbitrary Lua.

## Stream grammar and transitions

The legal phase transitions are:

```text
IDLE
  -> OPEN                         recorder and terminal reserve published

OPEN
  -> CONT_CALLBACK                START/RECORD callback begins
  -> DETACHED_PENDING             STOP/ABORT payload frozen
  -> OPEN                         START/RECORD handler absent

CONT_CALLBACK
  -> OPEN                         callback returned; continuation validates
  -> CONT_TERMINAL_PENDING        control/error/abort ended recorder

CONT_TERMINAL_PENDING
  -> DETACHED_PENDING             outer callback unwound; no token resume

DETACHED_PENDING
  -> DETACHED_CALLBACK            fresh terminal handler claimed
  -> IDLE                         handler definitively absent

DETACHED_CALLBACK
  -> IDLE                         protected callback returned or errored
```

FLUSH uses `IDLE -> DETACHED_PENDING -> DETACHED_CALLBACK|IDLE` and never
pretends to be the terminal of an OPEN compiler stream.

No `START` may occur while the phase is DETACHED_PENDING or
DETACHED_CALLBACK, even though the owner word may be `{0,0}`. All recorder
admission therefore validates both the owner word and stream generation.

### Recorder-start admission

A safe start sequence is:

1. Snapshot stable `{phase=IDLE,next_generation}`.
2. CAS the exact owner word to `{tid,0}`.
3. Revalidate the same idle generation; if it changed, release and return.
4. Complete eligibility checks, reserve the trace slot, and initialize
   `J->cur`.
5. Reserve the terminal session slot.
6. Publish a fresh nonzero stream generation as OPEN.
7. Deliver START through a CONTINUATION session when a handler is ready.

Failure before OPEN undoes only unpublished recorder state. It cannot emit a
terminal because no stream was structurally admitted.

### Normal terminal ordering

While holding `{tid,0}`:

1. Fill and root the reserved DETACHED slot.
2. Publish its roots, view, barriers and exact source pin where applicable.
3. Remove or freeze every dependency on mutable `J->cur`.
4. Publish JIT state IDLE.
5. Publish the stream as DETACHED_PENDING.
6. Release `{tid,0}->{0,0}`.
7. Claim the per-TG callback owner and perform a fresh handler lookup.
8. Invoke the protected callback if present.
9. Close the session and stream and then clear callback ownership.
10. Only after all invariants are repaired may STOPREQ or an outer error be
    propagated.

The terminal session publication must precede clearing an unpublished trace
slot, resetting `J->cur`, unpinning a published source, or permitting another
recorder to reuse scratch.

### Terminal generated during a continuation

A same-owner control uses the exact lifecycle borrow:

```text
{0,tid} -> {tid,0} -> {0,tid}
```

During the low-half borrow it materializes the pre-reserved terminal payload
before destructive JIT mutation and publishes CONT_TERMINAL_PENDING. The outer
callback may continue executing Lua safely against its own frozen view.

When the outer callback returns:

- do not resume the old recorder;
- close the CONTINUATION session while retaining the terminal payload;
- publish DETACHED_PENDING;
- clear `{0,tid}->{0,0}` exactly; and
- tail-deliver the terminal after a fresh handler lookup.

Nested FLUSH instrumentation is suppressed. A `jit.flush` invoked inside a
START/RECORD callback may abort the current stream, but it does not create a
second nested FLUSH callback before that terminal.

## Attachment publication and handler replacement

Handlers are looked up afresh for every event. The START handler is never
pinned as the terminal handler.

Each event attachment needs a universe-global publication clock:

```c
typedef struct LJJitEventAttachmentClock {
  uint64_t sequence;              /* Even stable, odd update. */
  uint64_t next_generation;
  uint64_t generation;
} LJJitEventAttachmentClock;
```

`jit.attach` must root and pre-resolve all potentially throwing table work
before entering the odd interval. The odd interval contains only an exact,
nonthrowing handler store plus generation publication. Concurrent attach or
detach may win in either order, but a reader must see one complete handler and
generation. Saturation is a terminal/fail-stop condition; generations never
wrap into ABA.

Handler preparation becomes tri-state:

```text
READY   exact function rooted, with attachment generation
ABSENT  stable, definitive miss
RETRY   transient attachment/SMR/publication collision
```

The selected handler itself is rooted in the callback session/stack;
attachment generation is not a substitute for that root. A queued terminal
keeps `attachment_generation == 0` until it is actually ready to call, so
replacement or detachment while pending affects it naturally.

Disposition is event-specific:

- START/RECORD ABSENT: no callback; structural stream continues.
- START RETRY before OPEN: abandon the recording attempt rather than lose a
  known START.
- RECORD resource failure after OPEN: terminalize with retry semantics.
- terminal RETRY or a busy per-TG callback owner: remain DETACHED_PENDING and
  arm owner-local delivery; never wait and never drop the terminal.
- terminal ABSENT: consume the structural terminal and close the stream.
- callback error: the event was called; repair all ownership before reporting
  or propagating any resulting STOPREQ.

A pending detached terminal needs an owner-local dispatch overlay/request so it
is retried before that TG executes another ordinary bytecode. The pending
session roots the initiating state, callback arguments, function/error/info
objects, trace view and source pin. TG detach is ineligible until it drains.

## Delivery result and continuation validation

A boolean result is insufficient. Separate handler disposition from recorder
continuation validity:

```c
typedef enum LJJitEventCallStatus {
  LJ_JIT_EVENT_ABSENT,
  LJ_JIT_EVENT_CALLED_OK,
  LJ_JIT_EVENT_CALLED_ERROR,
  LJ_JIT_EVENT_DEFERRED,
  LJ_JIT_EVENT_DROPPED_NONTERMINAL
} LJJitEventCallStatus;

typedef enum LJJitContinuationStatus {
  LJ_JIT_CONT_NONE,
  LJ_JIT_CONT_VALID,
  LJ_JIT_CONT_TERMINATED
} LJJitContinuationStatus;

typedef struct LJJitEventDeliveryResult {
  uint32_t call_status;
  uint32_t continuation_status;
  uint64_t stream_generation;
  uint64_t attachment_generation;
} LJJitEventDeliveryResult;
```

`trace_start` and the RECORD state may call `lj_record_setup` or
`lj_record_ins` only after exact revalidation proves:

- matching stream generation and OPEN phase;
- matching owner key, tid, actor and initiating state;
- matching trace number and expected JIT state;
- matching `J->cur` identity/header; and
- no terminal-pending flag.

A protected Lua callback returning normally is not proof that its reentrant
code left the recorder intact.

## Retry and exceptional transitions

- `LJ_TRERR_MCODELM`: internal assembler retry. It retains the same stream
  generation and emits neither a terminal nor another START.
- `LJ_TRERR_RETRY`: ABORT the current stream exactly once. A future hot retry
  receives a new generation even if it reuses the same TraceNo.
- `LJ_TRERR_SMRRETRY`: also ABORT exactly once. The existing callback
  suppression must be removed once rooted pending delivery exists. Its frozen
  view avoids unsafe dependence on the closed trace-body SMR gate.
- `LJ_TRERR_DOWNREC`: finish ABORT for the old stream before publishing START
  for the new down-recursion stream.
- `LJ_TRERR_MCODEAL`: finish ABORT, then form a separate standalone FLUSH
  stream if public flushing is required.
- root-patch-lost: publish ABORT once from the pinned published source; never
  publish STOP followed by ABORT.
- asynchronous abort during START/RECORD callback: transition to
  CONT_TERMINAL_PENDING and never resume `lj_record_setup`/`lj_record_ins`.
- handler error, including an inner error caught by Lua `pcall`: allow the
  ordinary asynchronous recorder abort to produce ABORT.
- terminal-handler error: consume and close that terminal; never manufacture a
  second terminal.
- STOPREQ at callback return: close session, stream, callback owner and owner
  word before throwing.

## Callsite map

The eventual cutover affects these concrete areas:

- `src/lib_jit.c::jit_attach`
  - publish TRACE/RECORD attachment sequence and generation;
  - detach only the exact function publication observed.
- `src/lj_vmevent.c`
  - tri-state handler preparation;
  - per-TG protected callback ownership;
  - a JIT-event path which does not claim process-global `vmevent_owner`.
- `src/lj_trace.c::trace_start`
  - reserve the global stream and terminal slot;
  - invoke START as CONTINUATION;
  - validate before `lj_record_setup`.
- `src/lj_trace.c::trace_state`, `LJ_TRACE_RECORD`
  - invoke RECORD as CONTINUATION;
  - validate before `lj_record_ins`.
- `src/lj_trace.c::trace_stop`
  - publish STOP detached;
  - publish root-patch-lost ABORT detached;
  - move callback execution after immutable publication and JIT IDLE.
- `src/lj_trace.c::trace_abort`
  - always publish ABORT, including SMRRETRY;
  - preserve payload before trace-slot/scratch cleanup;
  - sequence DOWNREC/MCODEAL work after terminal completion.
- `trace_flushall_direct`, `trace_flush_vmevent_cp`, and
  `trace_flushall_hs_impl`
  - converge on one standalone detached FLUSH path.
- `trace_terminal_release`
  - split recorder detachment from stream/session closure; it can no longer
  blindly release the token.
- `lj_trace_abort_owner` and `lj_trace_abort_owner_before_park`
  - use per-TG callback and stream generations instead of `vmevent_owner`;
  - retain a continuation terminal rather than destructively clearing scratch.
- `lj_trace_hot`, `trace_hotside`, stitch entry, and JIT control scopes
  - require global stream-idle admission and revalidation.
- `src/lj_dispatch.c` and x64 VM hot/record gates
  - include TG-local `HOOK_VMEVENT`;
  - support owner-local pending-terminal dispatch.
- `src/lib_jit.c` `jit.util.trace*` readers
  - consult the exact event view first, then the public trace vector.
- GC2 owner-TG root scanning
  - enumerate reserved, pending and calling slots independently.
- TG detach/finalization and VM close
  - require callback owner idle, no reserved/pending terminal, and no global
  stream naming that TG.

The separate same-owner JIT control-borrow audit must be applied to every
waiting token caller before CONTINUATION callbacks are enabled. A peer seeing a
callback lifecycle or detached pending stream returns BUSY/defer; it cannot
enter the old token wait loop.

## Verification matrix

### Focused C/model coverage

- every legal and illegal stream transition;
- odd-sequence readers return retry without waiting;
- stream, session, callback and attachment generation saturation;
- stale actor/key/session handles cannot clear a successor;
- callback owners on two TGs overlap while same-TG recursion fails;
- detached callback runs with owner word zero but blocks peer START admission;
- terminal reservation survives the other slot's paused reader;
- same-owner continuation borrow materializes a terminal without allocation;
- final-reader cleanup and TraceNo reuse cannot ABA a stream;
- callback error and STOPREQ unwind repair every descriptor exactly;
- detach and VM close reject all active/reserved/pending states.

### Runtime coverage

- stable handler observes only `start, record*, stop|abort`;
- A-to-B replacement at START;
- detach at START and attach midway through recording;
- errors in START, RECORD and terminal handlers;
- `pcall(error)`, `jit.off`, `jit.flush`, scoped flush, `jit.opt.start`,
  `collectgarbage`, channel park and join inside callbacks;
- peer recorder/control makes bounded progress while a callback is paused;
- peer TEXIT callback can overlap a TRACE callback after global ownership is
  removed;
- forced SMRRETRY still emits ABORT with every `jit.dump`/`jit.v` IR, snapshot
  and mcode reader mode;
- MCODELM, RETRY, DOWNREC, MCODEAL and root-patch-lost grammar;
- GC at each active, pending and close transition;
- handler, argument, function, error, KGC and source-pin lifetime;
- STOPREQ arriving at callback return;
- concurrent attach/replace/detach sees complete generations;
- clean no-JIT and `LUAJIT_DISABLE_VMEVENT` builds;
- no-handler and dump-enabled performance measurements.

Test-only stream snapshots should expose generation, phase, owner tuple,
callback/terminal session generations and event ordinal. This makes grammar and
ABA checks deterministic without treating a reusable TraceNo as identity.

## Smallest safe implementation slice after the substrate

The first callback cut should implement standalone TRACE `"flush"` only:

1. Add the universe-global stream descriptor in main-TG tail storage.
2. Add per-TG callback ownership and local `HOOK_VMEVENT` handling.
3. Prepare and root the current TRACE handler while `{tid,0}` still exists.
4. Publish an empty DETACHED FLUSH session and detached-callback stream phase.
5. Publish JIT IDLE and release `{tid,0}->{0,0}`.
6. Invoke the handler through the per-TG protected path.
7. Make peer hot-start attempts see the non-IDLE stream and return immediately.
8. Close session, stream and callback ownership before STOPREQ propagation.
9. Route both direct and handshake flush paths through this implementation.

This slice needs no raw trace decoder, START/terminal pairing, RECORD stream,
or simultaneous continuation/pending slot. It nevertheless proves the central
cutover properties: token-zero callback execution, no process-global callback
serialization, exact initiating-TG ownership, the owner-word-zero stream gate,
handler rooting, GC visibility, reentrant control cleanup, and peer recorder
progress.

The safe order after FLUSH is:

1. published STOP and root-patch-lost ABORT using exact source pins;
2. terminal-reservation and pending-delivery extensions; and
3. START/RECORD CONTINUATION callbacks with exact control borrowing and
   post-callback recorder validation.

Until all three stages land and pass their matrices, the persistent rich TRACE
stream design remains incomplete.
