# Universe admission, state-entry pins, and whole-universe TG lifetime

Date: 2026-07-12

Status: revised design/audit only. This note changes no source or plan file.
It supersedes the earlier version of this note after adversarial lifetime,
unwind, callback, reclamation, and nonblocking review.

The target remains x86-64 Linux, macOS, and desktop Windows. This design builds
on:

- `notes/tg-registry-runtime-shadow-spine-2026-07-11.md`;
- `notes/tg-registry-tls-tagged-binding-2026-07-12.md`; and
- `notes/tg-exact-tls-lifecycle-migration-audit-2026-07-12.md`.

The tagged TG primitive remains dormant until the feature-enable barrier in
this note is satisfied.

## Final decisions

### Keep the whole-universe TG-lease invariant

Do not add a second lifetime refcount to each tagged TG binding. Every admitted
ordinary TG-token lease pins:

- its exact `TGState` body and registry slot;
- the containing `global_State` and `GG_State`;
- the immutable TG registry spine; and
- the allocator, GC, JIT, FFI, and other shared subsystem container/control
  storage which close destroys only during whole-universe teardown.

Here, "whole-universe" means a veto on destructive universe teardown, not a
reachability root for every object in the universe. A TG lease alone does not
pin an individually reclaimable `lua_State`, GC object, callback body, or
other leaf allocation. Such bodies still require their exact state pin,
owner/root or GC reachability authority, callback descriptor/invocation pin,
or another object-specific lifetime authority.

A tagged TLS binding and every displaced callback hold already own ordinary TG
leases. Giving those existing counts whole-universe meaning avoids another hot
word, another per-binding CX16, and two linear handles which could disagree.

### Keep a short external universe gate

An external exact universe slot closes all new process-visible publication,
attach, TG-borrow, publication-capable API-entry, and callback-entry
transactions. Its count covers only an OPEN transaction which has not yet
completed its publication and handed continuing lifetime authority to a
durable state-entry pin, root, or TG lease.

### Replace close-time hazards with exact state-entry pins

Do not use a separate close-only hazard list for raw `lua_State *` addresses.
Every `lua_State` has a process-stable address-map entry with an exact,
non-wrapping incarnation and pin count. Lookup pins that stable entry before
dereferencing the raw state. The pin remains active until exact state-owner and
root authority has been established, and normally for the entire exported API
guard scope.

### Make API guards durable and unwind-aware

Every exported API entry which receives state pointers pushes a frame on a
preallocated process-owned per-OS-thread guard stack. It protects every state
argument before dereference with an exact map pin and GC/root authority, except
for the sole exact-tagged-current-state fast path which already has owner/root
authority. Lua longjmp, DWARF/C++ unwind, Windows SEH, normal return, callback
unwind, and `lua_close` transfer all consume these frames exactly once.

### Make callback entry process-stable

An FFI callback pointer targets a process-stable executable veneer and
descriptor which are never unmapped or reassigned to another callback. The
veneer reaches no raw `CTState *`, `global_State *`, or Lua body before exact
descriptor, universe, and state admission succeeds.

### Choose deferred racy-close semantics

Uncontended `lua_close` remains synchronous. If another API guard, tagged TG,
state-entry pin, callback scope, publication transaction, or reclaimer is
active, `lua_close` performs logical close, transfers ownership to a durable
preallocated close record, enqueues nonblocking deferred finalization, and
returns. Finalizers run exactly once later under close-owner/main-TG authority.

This semantic is feature-gated and must be publicly reported. Production exact
TLS migration is not enabled until the deferred-close executor, API guards,
callback veneers, finalizer protocol, and source audits all pass.

### Define the raw-pointer boundary

Calls whose physical-pin and GC/root admission completes against a live
published state before overlapping `lua_close` are safe. A raw `lua_State *`
used after close has completed remains outside the valid Lua API lifetime. The
pointer-only ABI cannot distinguish such a stale address from a later state
allocated at the same address. Stronger behavior requires a generation-bearing
public handle or permanent address nonreuse.

Standard Lua GC semantics are unchanged: merely retaining a `lua_State *` in C
does not permanently root the coroutine or anything on its stack. The caller
must retain an ordinary Lua-visible anchor when the API contract requires one.
The address map can keep state bytes from being freed during admission, but it
does not itself create reachability or resurrect a coroutine after the
collector has published RETIRING. Only a transient API root which wins while
the state is still LIVE can anchor it for that guard scope.

## Threat model and non-goals

This design covers:

- tagged execution on one OS thread racing close on another;
- TLS-empty foreign API or callback entry;
- every state argument to exported Lua/LuaJIT APIs;
- main, spawned, foreign, and GC-worker TG attachment;
- A -> B -> C cross-universe callbacks;
- Lua errors, longjmp, C++ exceptions, DWARF unwind, and Windows SEH;
- state collection racing a foreign API call whose standard GC anchor remains
  live, or whose transient entry root wins before retirement;
- close racing state/callback/TG publication;
- close racing ordinary TG reclamation; and
- conservative retention after abnormal thread loss or corruption.

It does not turn arbitrary post-close use-after-free or an unanchored dead
coroutine pointer into a valid API program. It also does not make forced OS
thread termination recoverable by guessing which fungible counts belonged to
the terminated thread.

## Lifetime authorities

| Authority | Representation | Protects |
| --- | --- | --- |
| Universe OPEN transaction | Exact external universe-slot count | Private work which may publish process-visible state, callback, TG, or borrow authority |
| State-entry pin | Exact process state-map slot/incarnation/count | One `lua_State` allocation, its immutable universe publication, and a universe-teardown veto until handoff; not stack-referent reachability |
| Ordinary TG lease | Exact TG slot/incarnation/count or tagged TLS | Exact TG body/slot plus GG/global and teardown-only shared subsystem containers; not individually reclaimable state/GC/callback bodies |
| State ownership/root authority | Existing exact owner/root descriptor protocol | Semantic state reachability plus mutable stack/state and referent access after entry validation |
| Close-owner authority | Durable external close record plus main state/TG holds | Finalizers and terminal teardown after admission closes |
| Reclaimer activity | Exact sealable reclaimer token | RETIRED -> RECLAIMING body ownership |
| Legacy gates | SMR, worker, allocator, raw-list/controller state | Positive authority not yet replaced by tokens |

`mt_entering` and `mt_live` remain compatibility telemetry, scheduling, STOPREQ,
join, and GC policy fields. They never independently authorize physical free.

## Normative invariants

### U1. A TG lease pins the universe

While any ordinary TG-token lease is active, its exact `TGState` body and slot,
the `global_State`/`GG_State`, registry spine, and shared allocator, GC, JIT,
FFI, and other subsystem container/control storage needed to use the lease may
not be physically destroyed. This vetoes whole-universe teardown.

It does not keep every object reachable through those containers alive.
Individually reclaimable `lua_State`, GC object, callback body, and similar
leaf storage still require their own exact state-entry pin, owner/root or GC
reachability authority, callback descriptor/invocation pin, or other
object-specific lifetime authority.

### U2. Every state is mapped before exposure

Every `lua_State`, including main states, coroutines, callback carrier states,
spawned states, and states created during close finalizers, has an exact
process address-map publication before its pointer becomes observable outside
private initialization.

### U3. Pin before dereference

An exported entry never loads `G(L)`, state fields, stack fields, or ownership
from a raw state argument until its exact address-map entry has been pinned and
revalidated. A map pin alone permits only the bounded header/lifecycle reads
needed to acquire GC/root authority. No stack slot or state-held GC referent is
read until existing reachability/root authority has been acquired, or a
transient root has been atomically published and caught up to the current mark
epoch. The only zero-lookup exception is defined below and requires raw pointer
equality with the current-state value loaded from the exact tagged TG, plus
independent owner/root authority protecting that state body and graph.

### U4. State pins survive authority handoff

A state-entry pin remains active until the path has established exact state
ownership/root authority or another documented durable pin. Exported API
guards normally retain it until the whole API frame exits.

The map pin is physical acquisition, not a GC root. State entry must either
acquire an already-live GC-visible root/owner descriptor or win the exact
LIVE/root-admission gate, publish a transient root, and complete any required
bounded insertion-barrier/mark catch-up. A RETIRING state or a sweep phase
which can no longer accept that catch-up rejects the entrant before it reads
stack or state-held referents.

Because state lookup precedes universe-transaction acquisition, the active
state pin itself vetoes physical universe destruction. Close drains these pins
before clearing the universe body. It is not merely protection for the bytes of
`lua_State` in isolation.

### U5. OPEN publication is unified

Every new process-visible state entry, callback descriptor, TG slot, TG borrow,
worker control record, and external root publication is created under one
exact universe OPEN transaction. Close atomically denies all new transactions,
drains existing transactions, and captures a final publication epoch.

### U6. Continuous lifetime handoff

There is no interval between raw entry and final exit in which all of these are
absent:

- a universe OPEN transaction;
- an exact state-entry pin;
- a TG-token lease/tag;
- exact state owner/root authority under an active API guard; or
- close-owner authority.

### U7. Durable unwind ownership

No active state pin, universe transaction, TG borrow, displaced binding, or
close record exists only in an ordinary C stack local across an operation which
may throw, longjmp, or unwind. Durable guard/controller state owns it first.

### U8. State free is two-phase

Ordinary GC first publishes RETIRING after winning against transient root
admission. `lj_state_free()` then publishes TOMBSTONING on the exact state-map
incarnation, preventing new pins, and frees the state only after existing pins,
state ownership, root descriptors, and callback/controller references drain.
A busy state is queued for deferred state reclaim; it is never freed
optimistically.

### U9. Reclaimer seal precedes terminal proof

Close seals new TG reclamation and drains active reclaimers before the final
registry proof. Generic close scans never dereference a RECLAIMING body.

### U10. Finalizers precede the final proof

Close runs finalizers under durable close-owner, main-state, and main-TG
authority. It then transitions close-local publications to TOMBSTONING,
re-drains all pins and transactions, seals reclamation, and performs the final
whole-registry proof before freeall or physical subsystem destruction.

### U11. Final release means no later access

After a path successfully releases its final state pin or TG lease, it performs
no load, store, error formatting, telemetry update, allocator action, or unwind
which dereferences that state, TG, or universe.

### U12. Fail closed and drain POISONED counts

POISONED/PINNED states deny new admission and physical free, but exact already-
admitted handles may still decrement their counts. Reaching count zero does not
unpoison or authorize reuse; it improves diagnostics and bounds leaked logical
ownership.

## Process-stable universe slot

### Shape

Use a process-global stable slot analogous to the TG registry:

```c
typedef struct LJUniverseSlot {
  LJUniverseToken token;         /* incarnation, state, exact txn count */
  la_u128 body_value;            /* lo = global_State *, hi = incarnation */
  uint64_t next_publication;     /* non-wrapping publication ticket */
  uint64_t external_final_publication;
  uint64_t terminal_final_publication;
  struct LJUniverseSlot *next_all;
} LJUniverseSlot;

typedef struct LJUniverseKey {
  LJUniverseSlot *slot;
  uint64_t incarnation;
} LJUniverseKey;

typedef struct LJUniverseTxn {
  LJUniverseKey key;
  uint64_t publication_ticket;
  uint8_t active;
} LJUniverseTxn;
```

The actual layout may use separate aligned words, but all fields are process-
owned and survive GG. `global_State` contains its exact key, not ownership of
the slot.

### Token states

Recommended exact states:

- `EMPTY`: no universe body, count zero, reusable;
- `BUILDING`: private `lua_newstate` construction;
- `OPEN`: external transactions admitted;
- `CLOSING`: external admission denied, existing transactions may leave;
- `FINALIZING`: close owner is running finalizers/close-local work;
- `FINAL_DRAIN`: privileged close-local admission denied, existing privileged
  transactions may leave;
- `SEALED`: final publication and reclaimer gates are closed;
- `POISONED`: absorbing no-free state; counts may drain; and
- `EXHAUSTED`: terminal empty slot after incarnation exhaustion.

The 128-bit token encodes:

```text
lo = non-zero universe incarnation
hi = transaction_count << STATE_BITS | state
```

Ordinary transitions are:

```text
EMPTY/0 -> BUILDING/0 -> OPEN/0
OPEN/N  -> OPEN/N+1                enter transaction
OPEN/N  -> CLOSING/N               logical-close LP
OPEN/CLOSING/POISONED N -> N-1     exact transaction leave
CLOSING/0 -> FINALIZING/0
FINALIZING/N -> FINALIZING/N+1     privileged close-local transaction
FINALIZING/N -> FINAL_DRAIN/N       finalizer-publication close LP
FINALIZING/FINAL_DRAIN/POISONED N -> N-1
FINAL_DRAIN/0 -> SEALED/0
SEALED/0 -> EMPTY/0                after body clear and GG free
```

No count or incarnation wraps. Saturation rejects a new transaction without
changing existing ownership. Invalid components/body association transition to
POISONED where possible; existing transactions can still leave.

### Unified OPEN transaction

An OPEN transaction is required for any operation which may publish:

- a state-map incarnation;
- a callback veneer/descriptor;
- a TG registry slot/body;
- a new TG ordinary borrow;
- an externally visible worker/controller;
- a process-visible root or owner mapping; or
- an API entry which may publish a transient root or another authority above.

Transaction enter performs an exact CAS only from OPEN and reserves a unique,
non-zero, non-wrapping publication ticket. Private fields are initialized before
the publication's release commit. Successful publication records that ticket
in its immutable per-incarnation metadata before transaction leave.

Close CASes OPEN -> CLOSING, drains transaction count to zero, acquire-loads
`next_publication`, and stores it as the external final-publication epoch. The
drain proves every ticket below that epoch is either committed or explicitly
aborted/tombstoned. No external publication with a later ticket can exist.

Finalizers may create close-local Lua objects/states. The close owner uses a
separate privileged FINALIZING transaction class. These publications are
marked `CLOSE_LOCAL`, are not pinnable by external lookup, and receive tickets
from the same monotonic sequence. After finalizers, close CASes FINALIZING ->
FINAL_DRAIN to deny new privileged transactions, drains existing transactions,
and freezes a second and final publication epoch before SEALED.

### Body publication and reuse

The builder publishes exact `{g, incarnation}` before BUILDING -> OPEN. A
transaction which wins its OPEN CAS snapshots and verifies that body before
returning `g`.

Close keeps the body intact in CLOSING/FINALIZING/FINAL_DRAIN while admitted
guards and finalizers may use it. After SEALED and the terminal proofs, close
clears the body, frees GG, and publishes EMPTY. A CLOSING/FINALIZING/
FINAL_DRAIN/SEALED entrant never loads `g`.

Universe slots come from bounded process slabs, remain address-stable, and are
recycled only from EMPTY under a new incarnation. Incarnation exhaustion makes
one slot EXHAUSTED and consumes another free slot.

## Stable process state-address map

### No separate hazard list

The address map itself supplies stable acquisition memory. A lookup never
touches `L`; it hashes the numeric pointer and probes process-stable slots. It
then pins an exact immutable publication before any state dereference. There is
no window requiring an additional close-time raw-address hazard.

### Slot shape

A state-map slot is permanently address-stable and recyclable by incarnation:

```c
typedef struct LJStateMapSlot {
  LJStateMapToken token;         /* incarnation, state, pin/root-admit counts */
  la_u128 address_value;         /* lo = lua_State *, hi = incarnation */
  LJUniverseKey universe_key;    /* immutable while this incarnation exists */
  uint64_t publication_ticket;
  uint64_t reachability_epoch;
  uint32_t state_kind;
} LJStateMapSlot;

typedef struct LJStatePin {
  LJStateMapSlot *slot;
  uint64_t incarnation;
  lua_State *L;
  LJUniverseKey universe_key;
  uint8_t active;
} LJStatePin;
```

Map states are:

- `EMPTY`;
- `PUBLISHING` (private under an OPEN/FINALIZING transaction);
- `LIVE` (external physical pins and transient entry roots admitted);
- `RETIRING` (GC won reachability retirement; no new pins/roots or sweep
  resurrection, existing exact authorities drain);
- `CLOSE_LOCAL` (close-owner-only, never external-pin admitted);
- `TOMBSTONING` (new external pins denied and old pins drain; only the exact
  FINALIZING close owner may acquire a privileged pin before its final seal);
- `TOMBSTONE` (state body gone, reusable by new incarnation);
- `POISONED` (no new pins/free/reuse, old pins may drain); and
- `EXHAUSTED`.

### Publication

Every state is published:

1. enter universe OPEN or privileged FINALIZING transaction;
2. claim an EMPTY/TOMBSTONE map slot under a new non-wrapping incarnation;
3. initialize state memory privately;
4. store exact address, universe key, state kind, and publication ticket;
5. link any universe-local enumeration metadata;
6. release-transition PUBLISHING -> LIVE or CLOSE_LOCAL; and
7. leave the universe transaction only after all external roots/owners which
   expose the pointer are committed.

This applies to main states, `lua_newthread`, internal VM/callback states,
spawned states, and finalizer-created states. A private state which never
publishes a pointer still receives a CLOSE_LOCAL/private map incarnation so
terminal accounting covers every state body.

### Lookup and pin

Given only a raw pointer value:

1. hash/probe the stable map without dereferencing that pointer;
2. acquire-snapshot address/incarnation and require exact pointer equality;
3. exact-CAS increment pin count only from LIVE;
4. acquire-revalidate the address, incarnation, immutable universe key, and
   publication ticket;
5. begin/confirm the matching universe OPEN transaction as required by the API
   entry protocol; and
6. only then read bounded state header/lifecycle metadata needed for root and
   owner admission.

A failed revalidation decrements the pin and continues/fails. RETIRING,
TOMBSTONING, TOMBSTONE, CLOSE_LOCAL, POISONED, and stale incarnations deny new
external pins. A successful physical pin does not yet authorize stack or
state-held referent access.

### GC reachability and transient entry roots

Before any stack slot or state-held GC referent is read, each entry must do one
of the following:

1. acquire an existing exact owner/root descriptor whose GC-visible authority
   covers the state and current collection epoch; or
2. claim a preallocated transient API-root descriptor, exact-CAS increment the
   slot's root-admission count only while LIVE, release-publish that root to the
   collector, revalidate LIVE/incarnation/GC epoch, and complete the required
   bounded insertion barrier and mark catch-up for the state, stack, and
   referents.

The collector's unreachable decision and API root admission arbitrate on this
same exact gate. GC may CAS LIVE -> RETIRING only after ordinary Lua
reachability says the coroutine is unreachable and the root-admission count is
zero. RETIRING is published before sweep may unlink or reclaim the state or
anything reachable only through its stack. It permanently denies a late
transient root for that incarnation.

If retirement wins, sweep has reached a phase which cannot accept the
insertion barrier, the epoch changes during catch-up, or the bounded catch-up
cannot complete, entry removes any tentative root, releases its physical pin,
and rejects before referent dereference. Result-bearing APIs return the
documented dead/BUSY result; infallible legacy entries follow the pre-body
fail-stop ABI policy. They do not call into Lua using the unadmitted state.

A raw C `lua_State *` is therefore neither a permanent GC root nor proof that
the coroutine remains semantically live between API calls. A transient entry
root lasts only for the durable API guard and is removed on every normal or
unwind exit.

### Pin duration

The pin is not a momentary lookup hazard. It protects the state allocation and
its immutable universe publication, but not reachability of the stack graph.
It remains active until:

- exact state ownership has been claimed and a root descriptor covers every
  stack/state access; or
- a stronger documented owner pin has been installed.

Exported API guards retain all argument pins until normal/unwind exit unless a
specific proven transfer consumes them. This conservative rule makes
`lj_state_free` and racy API calls exact.

### `lj_state_free` integration

`lj_state_free(g, L)` becomes a two-phase operation:

1. locate the exact state-map publication from state-owned metadata;
2. on an ordinary GC path, require that GC already won LIVE -> RETIRING after
   the reachability/root-admission arbitration above;
3. the exact GC state-free owner CASes RETIRING -> TOMBSTONING, or, for close
   after external root admission is sealed, the exact close state-free owner
   CASes LIVE/CLOSE_LOCAL -> TOMBSTONING, denying new physical pins; owner
   transfer/helping must itself be exact and is never inferred from state alone;
4. remove external owner/callback publications and clear roots only after
   transient root admissions and durable owner/root guards drain;
5. if physical pins, root admissions, or durable guards remain, enqueue the
   slot's preallocated deferred-state reclaim linkage and return without
   freeing `L`;
6. after all exact counts and owner/root checks pass, the exact TOMBSTONING
   state-free owner physically frees the state body and stack; and
7. only after that physical free, release-clear the address/body publication
   and transition TOMBSTONING -> TOMBSTONE.

No close or GC path infers that unlinking a GC edge means state memory is free.
No scanner dereferences a TOMBSTONING state's stack without its own exact pin/
root authority.

### Address reuse boundary

Recycling a TOMBSTONE map slot under a new incarnation prevents stale internal
handles from aliasing. The public raw address still lacks an incarnation. Use
after completed state destruction is invalid API behavior; optional debug
builds may quarantine state virtual addresses, but the production ABI is not
changed.

## Bounded process slabs and maps

The final design performs no `malloc`, `free`, futex wait, condition wait, or
unbounded sleep in API entry, attach, callback entry, detach, or close retry.

### Process resources

Reserve bounded, aligned slabs for:

- universe slots;
- state-map slots and immutable bucket/directory storage;
- transient API-root descriptors and bounded mark-catch-up records;
- per-OS-thread API control records and platform TLS/signal-mirror cells;
- one dedicated durable close record claimed by each universe before OPEN;
- the process-lifetime close executor and its pre-registered control/guard
  record;
- deferred state-reclaim records;
- callback descriptors and executable veneers; and
- any process publication records.

Slabs are reserved/committed at process initialization or explicit safe
control-plane growth before a universe becomes OPEN. A universe may reserve
additional bounded per-universe TG/controller/state capacity during
`lua_newstate` or an explicit non-hot prepare operation. Attach never grows a
slab.

### Recycling

- Universe slots recycle only from EMPTY under a new incarnation.
- State-map slots recycle only from TOMBSTONE with physical-pin and
  root-admission counts zero under a new incarnation.
- A state-map slot carries its own deferred-reclaim queue linkage, so
  `lj_state_free` never needs to allocate a side record.
- API thread records recycle only after clean OS-thread unregister, guard depth
  zero, and exact thread incarnation change.
- Close and deferred-state records recycle only after terminal completion.
- TG slots follow their existing EMPTY/incarnation rules.
- Callback veneer addresses are never reassigned; their bounded arena is a
  process-lifetime capability space.

All capacities have named compile-time/runtime limits and saturation telemetry.
Exhaustion follows the ABI failure policy below; it never falls back to raw
untracked execution.

### Address-map organization

Use a fixed or segmented open-addressed process hash table whose directories
and slots remain mapped. Immutable per-incarnation publications avoid mutable
linked-node reclamation during lookup. Segment addition is allowed only in an
explicit process control transaction, never from normal API entry or attach.

Probe length is bounded. Saturated tables fail state publication before the
state pointer is exposed.

## Durable per-OS-thread API guard stack

### Thread control record

Each OS thread uses a process-owned `LJApiThreadControl` claimed from a bounded
slab under an exact non-reused thread incarnation. Runtime-created threads
receive one in their controller before start. A foreign thread performs one
nonblocking table claim; no heap allocation or wait occurs.

The record contains:

- fixed-depth API guard frames;
- current depth and unwind generation;
- cached process state-map probe metadata;
- binding guard stack linkage;
- deferred cleanup flags; and
- exact registration/thread incarnation.

The depth bound is at least `LJ_MAX_XLEVEL` plus documented API/callback
nesting. Exceeding it follows the ABI failure policy; no frame is silently
overwritten.

### Every state argument is protected

Generated/manual entry veneers enumerate all `lua_State *` arguments before
the C implementation reads any of them. Each argument takes an exact map pin,
except the sole zero-lookup exact-current argument below; all arguments take or
already hold GC/root authority. Examples include both `from` and `to` for
`lua_xmove`, debug/coroutine state arguments, auxiliary-library functions,
JIT/profile APIs, serialization helpers, and internal callback owners.

The frame records one exact `LJStatePin` per distinct state argument, the
associated universe transaction/keys, relationship checks, acquired state
owner/root authority, and any TG binding move.

For each non-zero-lookup argument, the frame also acquires an existing
GC-visible owner/root authority or publishes and catches up a transient entry
root as specified above. Cross-state relationship checks and all stack/
referent reads occur only after every argument has both physical and semantic
authority. A pointer equality check alone never substitutes for either.

### Sole zero-lookup optimization

A raw state argument may skip the process address-map lookup only if:

1. the current TLS word is an exact tagged TG binding;
2. that tag's ordinary TG lease is valid and therefore pins the universe-
   teardown infrastructure defined by U1;
3. the raw argument value equals the exact current-state value loaded from the
   protected TG (`cur_L` or the explicitly defined carrier state); and
4. state ownership/root authority already covers that same state for this
   entry.

Merely belonging to the same universe, matching `G(L)`, matching `tg_hint`, or
being the main state is insufficient. An arbitrary coroutine in the same
universe still requires an exact state-map pin.

### API-entry gate split

Zero lookup never means zero guard or permission to publish after close. Every
top-level exported entry first pushes a durable API guard and validates the
exact tagged/current-state owner authority above. An entry which may allocate
or publish any externally visible state, transient root, callback, TG,
controller, worker, or other root must then enter the universe OPEN transaction
before body use which can lead to that publication. A nested call may inherit
the enclosing pre-close guard/transaction; finalizer code instead requires its
privileged close-owner FINALIZING frame.

A proven read-only exact-current entry may omit the OPEN-transaction CX16: its
existing tag and owner/root authority protect the body/graph, its durable guard
covers the scope, and it cannot change a publication epoch. It must take the
full gate if it changes course toward allocation or external publication.
Consequently, a tag which survives OPEN -> CLOSING can support bounded
read-only/unwind work, but cannot be used to create a late state, root,
callback, TG, or controller publication.

### Normal exit

Frames pop in reverse order. The exit path releases/transfers TG handles,
state-owner/root authority, state pins, and universe transactions exactly once.
After the last pin/lease release, it touches no protected body.

### Lua longjmp and VM unwind

Every protected C/VM boundary records the API guard depth and unwind
generation in durable thread/cframe metadata. All Lua error paths which jump
past frames call a non-allocating guard-unwind routine before the target frame
becomes active.

The routine consumes frames down to the saved depth, restores bindings,
releases pins/transactions, and preserves the selected Lua error plus
`errno`/Win32 `LastError` state. A guard is marked consumed before any release
which could trigger deferred close/state reclaim.

### C++/DWARF/SEH unwind

The external unwind personalities and Windows exception handlers must expose a
cleanup phase for API guard frames. Required integration includes:

- DWARF personality cleanup landing pads for exceptions crossing Lua frames;
- C++ exception paths through C API veneers;
- Windows x64 SEH/unwind handlers;
- JIT/interpreter external-frame unwind; and
- callback setup/body/result unwinds.

If a toolchain artifact cannot prove cleanup, the lockless lifetime feature is
not enabled for that artifact. Leaking a frame on C++ unwind is not accepted as
a conservative ordinary case because it can permanently retain close.

### `lua_close` frame transfer

`lua_close(L)` enters with an exact state pin like every other API. The winner
of universe OPEN -> CLOSING transfers, rather than releases:

- the main state pin;
- close-owner identity;
- any main TG lease/binding authority; and
- the API frame's unwind responsibility

into a durable preallocated `LJCloseRecord`. The API stack frame is then marked
transferred and may return. The close executor consumes the record exactly
once. A losing concurrent close releases its ordinary frame and never runs
finalizers.

## ABI failure policy for guard resources

Many Lua C API functions cannot report admission failure before safely
dereferencing `L`: some return `void`, others return values with no reserved
error code. Calling `lua_error` is also impossible before the state is safely
admitted.

Therefore:

- runtime-created threads and callback carriers are pre-registered;
- an optional additive `luaJIT_thread_register`/preflight API may let embedders
  reserve a foreign control record explicitly;
- attach/callback APIs with a failure result return false/NULL/BUSY on resource
  exhaustion;
- state-entry root admission which loses to RETIRING/sweep returns a documented
  dead/BUSY result where the ABI permits, and otherwise fail-stops before any
  stack/referent dereference;
- state creation fails before publishing the state if no map entry exists; and
- an unregistered infallible ABI entry which cannot claim a preallocated guard
  record fail-stops the process rather than dereferencing an unprotected state.

Fail-stop is a last-resort ABI compatibility policy, not a hot allocation
fallback. Resource limits must be sized, reported, tested, and optionally
preflighted so ordinary programs never reach it.

## Process-stable FFI callback veneers

### Never call unmapped callback code

Callback machine-code addresses returned to C remain executable and mapped for
the process lifetime. Close never `munmap`s/`VirtualFree`s a veneer which an
external C caller may retain. A stale callback call reaches a live process
veneer and deterministically observes a closed descriptor.

### Veneer and descriptor

Each unique callback pointer owns a non-reassigned veneer slot and a process-
stable descriptor:

```c
typedef struct LJCallbackDescriptor {
  LJCallbackToken token;         /* state, invocation pins, unique identity */
  LJUniverseKey universe_key;
  LJStateMapKey owner_state_key;
  uint64_t publication_ticket;
  uint32_t abi_shape;
  uint32_t return_policy;
} LJCallbackDescriptor;
```

The veneer embeds only a stable descriptor index/address and branches to a
process dispatcher. It contains no raw `CTState *`, `global_State *`, Lua
function, TG, or state pointer.

Descriptor states include PUBLISHING, LIVE, CLOSING, CLOSED, and POISONED.
Invocation pins increment only in LIVE. CLOSING/CLOSED calls return the ABI-
defined inert result, set a documented callback error, and never touch Lua
memory. POISONED denies new calls but allows exact invocation-pin decrements.

### Creation

Callback creation under a universe OPEN transaction:

1. reserves a never-reassigned veneer/descriptor slot from the bounded arena;
2. pins/publishes the exact owner state entry;
3. initializes ABI shape, stable universe/state keys, and ticket;
4. publishes descriptor LIVE with release semantics; and
5. exposes the function pointer only after the LIVE LP.

Arena exhaustion raises the existing callback-allocation error before exposing
a pointer. Veneer slots are not recycled to a different callback because a
stale C function pointer carries no incarnation.

### Invocation

The dispatcher:

1. exact-pins the stable descriptor while LIVE;
2. enters the descriptor's exact universe OPEN transaction;
3. pins the exact owner state-map incarnation;
4. revalidates descriptor LIVE and all immutable keys;
5. claims/prepares a durable API/callback guard;
6. acquires an existing owner/root descriptor or publishes the transient root
   and completes bounded current-epoch mark catch-up;
7. acquires/installs/swaps the target TG lease;
8. releases the short universe transaction after durable TG/state authority;
9. only then reads `g`, `CTState`, callback owner/function, or Lua stack; and
10. releases descriptor pin last after guard cleanup.

No veneer or tiny pre-dispatch helper reads raw universe memory.

### Close and disown

Close/disown transitions descriptor LIVE -> CLOSING, denies new invocation
pins, and defers owner-state/TG cleanup until existing invocation pins and
callback guards drain. It then publishes CLOSED. The veneer remains mapped and
returns its inert ABI value forever.

## Nested TG binding guards

### Guard ownership

Each callback/API guard frame may contain:

- target exact TG key;
- active displaced TG borrow;
- install/swap mode and binding stage;
- target state pins;
- descriptor invocation pin;
- auto-attach lifecycle stage; and
- close/error ownership flags.

There is no extra universe ref. The current tag and displaced ordinary TG
handles pin their universes.

### A -> B -> C

At maximum depth:

- TLS tag C pins universe C;
- the upper durable frame's displaced B hold pins B;
- the lower durable frame's displaced A hold pins A; and
- exact state/descriptor pins protect every callback carrier publication.

Short universe transactions have already been released after durable handoff.
Close of any universe sees its TG/state/descriptor counts and defers.

### Restore

Normal return and every unwind path:

1. save error/OS-error state;
2. mark the durable frame RESTORING;
3. swap current target back to the displaced exact hold;
4. receive the target TG hold;
5. finish target state ownership/root and auto-detach lifecycle while that hold
   remains active;
6. decrement `mt_live`/controller state before final TG release;
7. publish target RETIRED where this scope owns its lifecycle;
8. release returned TG, state, and descriptor pins in proven order;
9. mark frame CONSUMED/pop; and
10. restore the correct error pair.

For auto-attached B returning to A, B -> A swap is B's Stage C detach. The
returned B hold pins B through Stage D.

Close sets the universe gate CLOSING but does not force a guarded TG out of the
state needed for restoration. It requests unwind, arms the stable close wake
generation, and parks the close record; the exact restore/release event wakes
it. No path waits on a futex or blocking primitive.

## Four-stage attach without blocking allocation

### Stage A: claim preallocated authority

Enter universe OPEN transaction and claim all required records from bounded
preallocated slabs:

- TG body/storage;
- TG registry slot;
- lifecycle/controller record;
- state-map slot if a state is being created; and
- API/worker guard record.

No `malloc`, futex wait, blocking state-owner wait, or unbounded retry occurs.
Resource absence returns BUSY/OOM through a result-bearing path before roots.

Initialize TG/state privately with no hint, root, list, callback, or TLS
publication.

### Stage B: exact TG preparation

Under the same OPEN transaction:

1. claim/link ATTACHING TG slot;
2. publish initialized body/key;
3. acquire an ordinary TG borrow;
4. record it in durable transaction/controller state; and
5. revalidate universe transaction and publication epoch.

A close-race loser aborts ATTACHING and releases records without exposing
roots. TG slot OOM/capacity exhaustion is a normal attach failure; no keyless
shadow fallback remains.

### Stage C: TLS install/swap

Install on an empty carrier or swap under a durable binding guard. On success,
the tagged TG borrow becomes the whole-universe-teardown pin defined by U1.
Record the successful TLS LP in durable state before any throwing operation.

On Windows, first use claims a preallocated process-lifetime `LJThrTGCell` and
publishes only that stable cell pointer with `TlsSetValue`. Failure to admit the
cell occurs before the tagged word changes and leaves the incoming TG hold
active. Once the thread is admitted, install, swap, and clear mutate only the
cell's atomic tagged word. An existing exact tag therefore proves the cell
exists, and swap/clear cannot fail with `TLS_FAILURE`.

On supported POSIX targets, initial signal-mirror registration can fail before
install. After `fork(2)`, swap may separately fail while re-admitting the
inherited mirror into the child process incarnation. Either failure leaves all
bindings and input/output holds unchanged. This cold mirror re-admission is not
a failure of the normal tagged-word swap/clear store.

No failure authorizes DETACHING/RETIRE or release of the still-installed old
lease.

### Stage D: state/root/LIVE commit

After the tag:

1. commit exact state-map publication if applicable;
2. publish root descriptor and TG root fields;
3. publish state hint/owner relationship;
4. perform bounded activation catch-up or return BUSY before entry;
5. publish legacy/controller/`mt_live` state;
6. commit ATTACHING -> LIVE in a non-throwing tail;
7. publish the transaction ticket as complete; and
8. leave universe OPEN transaction last.

No attach path calls a futex wait. `lj_threading_attach_wait` must become a
nonblocking try protocol; callback entry returns an inert failure if the state
carrier is busy rather than waiting.

## Caller-specific attach ownership

### Main

`lua_newstate` claims universe/state/TG/control slots before public exposure,
initializes the main state and TG privately, and commits all process-visible
publications under BUILDING -> OPEN. It installs a main tag only if TLS is
empty.

If A is current while B is created, B never overwrites A. B execution later
pins the B state-map entry, enters B OPEN transaction, borrows B TG, and swaps
A -> B under a durable API guard.

### Spawned worker

The parent claims every child record and TG/state slot before OS-thread start.
The child receives an active durable OPEN transaction/controller, installs its
tag before VM/protected entry, commits roots/LIVE, and leaves the transaction
last. Parent cancellation only signals through lock-free controller fields and
joins through the project's eventual nonblocking join/handoff policy; it does
not free child storage.

### Foreign attach

The API veneer pins the target state-map entry without dereferencing L, claims
a preallocated thread guard and universe OPEN transaction, acquires/publishes
GC-root authority with any required bounded mark catch-up, then one-shot claims
state ownership. A RETIRING/sweep loser or busy ownership returns false/BUSY
before stack/referent access; it does not futex-wait.

TG/control storage comes from pre-reserved universe slabs. Success hands
lifetime to tag + state owner/root and leaves OPEN transaction. Failure unwinds
durable stages and releases state pin last.

### GC worker

The controller preallocates thread API control, TG slot/body, startup result,
and close/handoff state. The worker installs before native entry. It never
allocates or blocks in attach. Controller storage is recycled only after exact
join/exit, state/TG release, and registry proof.

## Four-stage detach

### Stage A: DETACHING and quiesce

LIVE -> DETACHING closes lifecycle publication while tag/state pins remain.
Abort recorder ownership, quiesce JIT/native/FFI, consume requests, and flush
SSB/accounting/strings/pending roots with bounded nonblocking work. If more work
remains, publish a resumable controller stage and return/requeue without
releasing lifetime.

### Stage B: state/root release

Clear TG roots, state hints, and state ownership under the active TG/state
pins. Tombstone any state publication which this detach owns, but defer state
free while pins remain.

### Stage C: TLS move-out

Clear TLS or swap back to a displaced universe. The returned ordinary TG hold
pins the exact TG plus the universe-teardown infrastructure defined by U1. On
Windows, an existing exact tag proves a stable cell, so clear/swap completes
with atomic cell stores and materializes the output hold without a fallible
`TlsSetValue`. A POSIX swap after `fork(2)` may instead fail child-local signal-
mirror re-admission; that leaves the old tag installed, `new_hold` active, and
`old_hold` inactive, so detach remains at Stage C and requeues.

### Stage D: controller/RETIRED/final release

While returned TG/state pins remain:

1. publish controller exit/handoff;
2. decrement `mt_live` and finish every final `g` access;
3. publish legacy death and DETACHING -> RETIRED;
4. enqueue any deferred state/TG reclaim records; and
5. release state pins and TG hold as the final body-access operations.

After successful final release, code touches only process-stable controller
memory.

## Sealable TG reclaimer

### Reclaimer token

Add an exact universe-local or external reclaimer token with:

- OPEN/SEALED/POISONED state;
- active reclaimer count; and
- close generation/incarnation.

An ordinary TG body reclaimer enters only while OPEN, increments active count,
captures exact body/key before RETIRED -> RECLAIMING, and owns completion to
EMPTY. Leave decrements in OPEN, SEALED, or POISONED.

### Close seal

After finalizers and close-local publication drain, close CASes reclaimer OPEN
-> SEALED. New reclaimers fail immediately. Active reclaimers may reschedule
their own bounded work, but their exact final count decrement wakes the parked
close record. Close does not poll or hot-requeue while the count is nonzero.

Only after zero may the final registry proof run.

### Never dereference RECLAIMING

RECLAIMING has owner count zero. The body may already be finalized, freed, or
owned only by the reclaimer's private captured pointer. Generic scanners,
registry-close proofs, telemetry, and cleanup never load/dereference the slot's
body in RECLAIMING.

After the reclaimer seal drains, terminal scan requires every formerly
RECLAIMING slot to be EMPTY/EXHAUSTED. A stuck RECLAIMING token is BUSY or
POISONED and retains the universe without inspecting its body.

## Close-owner and finalizer protocol

### Firm nonblocking policy

The final design contains no futex wait, blocking join, sleep loop, or attach-
time allocation. Each universe claims its dedicated `LJCloseRecord` before
OPEN. Close is a resumable exact state machine in that record on a lock-free
work queue.

If ready immediately, the calling thread executes it synchronously. If any
phase reports BUSY, it publishes the next stage, enqueues the record once,
transfers its API guard/pins, and returns. Enabling the feature also starts one
process-lifetime, pre-registered close executor with all guard/controller
resources preallocated. The executor and ordinary API/GC helpers perform only
bounded close steps. Completion does not depend solely on another Lua API call
occurring, and no thread waits for a count to change.

### Event-driven pending close

A durable close record has exact RUNNING, ARMING, PENDING, QUEUED, COMPLETE,
and POISONED states plus a non-wrapping wake generation. When a bounded close
step finds a nonzero transaction, physical pin, transient root, guard,
callback, worker, TG, reclaimer, or other drain count, it:

1. publishes the current close record/generation into that authority class's
   process-stable wake field;
2. release-transitions RUNNING -> ARMING;
3. acquire-rechecks every observed blocker after the wake is armed; and
4. CASes ARMING -> RUNNING and continues if all blockers vanished, otherwise
   CASes ARMING -> PENDING and returns the executor thread to other work. If
   either CAS observes QUEUED, the release-side winner already owns the next
   run and the current worker returns.

Every exact release or relevant state transition checks the armed generation.
It CASes PENDING/ARMING -> QUEUED and enqueues the record only if it wins; a
queued bit coalesces simultaneous releases. Arm-before-recheck plus the
release-side wake prevents a lost event. No per-wait allocation or removable
wait-list node is required: the one close record and wake fields were reserved
before OPEN.

A bounded, backoff diagnostic timer may occasionally enqueue a still-PENDING
record to detect corruption or a lost platform event. It is not the progress
mechanism and may not form a tight retry loop. A genuinely leaked/PINNED/
POISONED authority leaves close stably parked and the universe retained while
telemetry reports the exact blocker; it does not burn an executor core.

### Phase 1: logical close

1. Winner CAS universe OPEN -> CLOSING.
2. Transfer main state pin/API frame into close record.
3. Close new TG borrow/attach/list/SMR/external-root transactions and publish
   shutdown/STOPREQ/controller requests.
4. Attempt to drain existing external OPEN transactions; if the exact count is
   nonzero, arm its release wake and park the close record.
5. Freeze the external final-publication epoch after count zero.
6. Enumerate every committed ticket below that epoch and seal external state-
   root admission. Exact-close-own each LIVE -> TOMBSTONING transition. For a
   RETIRING entry already owned by GC, request/help its exact owner and park on
   its terminal wake unless an explicit owner transfer succeeds; never steal it
   from the map state. Transition every LIVE callback descriptor to CLOSING.
7. Require every ticket below the epoch to be committed or explicitly aborted/
   tombstoned and no PUBLISHING record to remain; otherwise POISON and retain.

No subsystem is physically destroyed in this phase.

### Phase 2: initial owner drain

Boundedly drive the following classes; if one remains active, arm its exact
release/terminal-transition wake and park:

- API guard stack exits;
- state-entry pins and state-owner releases;
- callback invocation/guard unwind;
- spawned/foreign/GC worker detach;
- TG tags and scanner borrows;
- SMR readers; and
- publication/controller transactions.

The close record retains GG. A leaked/PINNED/POISONED authority retains the
record and universe rather than triggering partial teardown.

### Phase 3: finalizers under explicit authority

When external execution has drained, close exact-transitions CLOSING/0 ->
FINALIZING/0, retains the exact main state pin transferred by `lua_close`, and
acquires a privileged close-owner main TG borrow/tag, state ownership, and root
descriptor. These are recorded durably before any finalizer.

Run Lua/FFI finalizers exactly once. Finalizer-created states are published as
CLOSE_LOCAL under privileged FINALIZING transactions. Attempts to expose new
threads, callbacks, roots, or process-visible state fail deterministically
because external admission is CLOSING.

Finalizer Lua APIs use the zero-lookup fast path only for the exact protected
main current state. Any other state argument still uses exact close-owner map
and root authority, including CLOSE_LOCAL or not-yet-freed TOMBSTONING states.
That privileged pin is admitted only for the exact close record while
FINALIZING, counts in the final re-drain, and is unavailable to external
lookup.

### Phase 4: final re-drain and publication freeze

After finalizers:

1. CAS FINALIZING -> FINAL_DRAIN, sealing privileged close-local publication
   and new close-owner state-pin/root admission;
2. drain finalizer API guards/state pins/TG holds and existing privileged
   transactions, parking on their exact release wakes if needed;
3. freeze the final publication epoch after privileged transaction count zero;
4. enumerate every final-epoch ticket, transition every CLOSE_LOCAL state
   publication to TOMBSTONING, and close/disown every finalizer-created callback
   descriptor;
5. require every finalizer ticket to be committed or explicitly aborted/
   tombstoned and no PUBLISHING record to remain;
6. seal and drain the TG reclaimer;
7. seal SMR/list/attach writers; and
8. perform the final state-map, callback, controller, and TG-registry proofs.

Only this second proof authorizes freeall. The initial owner drain is not a
terminal proof because finalizers can create internal states/roots.

### Phase 5: destructive teardown

Under sole close-owner authority and after final proof:

- for every remaining state body, require the exact LIVE/CLOSE_LOCAL/RETIRING
  -> TOMBSTONING transition to have completed in Phase 1 or Phase 4, acquire
  its exclusive state-free owner, and revalidate that external physical pins,
  transient roots, and ordinary owner/root guards are zero; any non-
  TOMBSTONING entry returns close to the non-destructive proof phase;
- run freeall as an ordered traversal in which that TOMBSTONING/free-owner
  transition always precedes the corresponding state body/stack free;
- immediately after each state body is physically gone, release-clear its map
  address/body publication and publish TOMBSTONE, then consume the state-free
  owner;
- run remaining non-Lua subsystem destructors only under the same terminal
  authority;
- consume main/secondary TG owner leases through exact lifecycle;
- clear every TG slot to EMPTY/EXHAUSTED;
- free per-universe mutable slabs/registry storage;
- clear the universe slot body;
- physically free GG; and
- transition SEALED -> EMPTY/EXHAUSTED.

The close record then releases only process-stable resources and recycles.

## Registry-wide terminal proof

With universe publication closed, final epoch frozen, reclaimers/SMR/list
writers sealed, and active counts zero, walk stable process/universe
publications without telemetry bounds.

### State map

For this exact universe incarnation, every entry must be TOMBSTONE/EMPTY or a
TOMBSTONING close-owner entry scheduled for immediate terminal free with no
external physical pin, transient root, or ordinary owner/root guard. The close
record must own its exact exclusive state-free token. LIVE, RETIRING,
PUBLISHING, CLOSE_LOCAL, TOMBSTONING with external authority, or POISONED vetoes
universe free.

### Callback descriptors

Every descriptor is CLOSING/CLOSED with invocation count zero. Veneers remain
mapped. A POISONED descriptor vetoes universe free unless its contract retains
the universe forever.

### TG slots

- PINNED/malformed/incomplete/keyless states veto free.
- ATTACHING/LIVE/DETACHING/RETIRED bodies may be inspected only while their
  owner lease makes the body valid and exact lifecycle/controller proof exists.
- Any ordinary count above owner-only vetoes free.
- RECLAIMING body is never read; after sealed reclaimer drain, RECLAIMING is
  not accepted as terminal-ready.
- EMPTY/EXHAUSTED require count zero and canonical null body metadata.

The final proof precedes freeall. It is repeated/validated after owner-lease
consumption before registry nodes are recycled/freed.

## POISONED and PINNED behavior

POISONED/PINNED is fail-closed, not count-frozen.

- New universe transactions, state pins, descriptor invocations, TG borrows,
  and reclaimers are denied.
- Existing exact handles may decrement their own count with CAS retry.
- Count zero does not return a POISONED publication to EMPTY and does not permit
  physical free or slot reuse.
- Close records remain durable and report the exact poisoned slot/incarnation.
- Forced operator/debug policy may intentionally leak the bounded process slot;
  it never guesses a release or clears a body.

Release helpers must explicitly accept POISONED/PINNED with positive counts and
preserve the absorbing state while decrementing.

## Memory-order contract

All structures use plain ABI types with GCC/Clang atomics, matching the existing
atomic layer.

### Publication

- Initialize per-incarnation immutable fields privately.
- Publish address/body/list links with release stores or release successful
  head CAS.
- Release-transition PUBLISHING/BUILDING -> LIVE/OPEN only after all immutable
  fields and roots required by entry are visible.
- Lookup/acquisition uses acquire snapshots before dereference.

### Exact counts

- Successful admission/pin/invocation/reclaimer increment CAS is ACQ_REL.
- Failed CAS reload is ACQUIRE.
- Non-last decrement is RELEASE or ACQ_REL where the replacement helper already
  requires it.
- The thread observing/decrementing the final count performs an ACQUIRE fence
  before reclamation/proof work.
- Transient root-admission release is RELEASE; its zero observer ACQUIRE-fences
  before RETIRING/sweep or state-free proof.

### Close and epochs

- OPEN -> CLOSING is ACQ_REL and is the external-admission close LP.
- Publication ticket allocation is non-wrapping atomic fetch/CAS.
- A publication writes its ticket/immutable record before RELEASE transaction
  leave.
- Close observes transaction count zero with ACQUIRE, then ACQUIRE-loads the
  final ticket epoch.
- FINALIZING -> FINAL_DRAIN is ACQ_REL and denies new privileged publication;
  FINAL_DRAIN -> SEALED is RELEASE after existing finalizer publications drain
  and the final epoch scan completes; terminal scanners enter with ACQUIRE.
- Close wake-field/generation publication is RELEASE. Exact releasers ACQUIRE-
  validate the generation and use ACQ_REL to win ARMING/PENDING -> QUEUED; queue
  publication is RELEASE and executor dequeue is ACQUIRE. The arming worker's
  post-arm blocker scan is ACQUIRE before its ARMING -> RUNNING/PENDING CAS.

### State tombstone

- Transient root-admission increment is ACQ_REL only from LIVE; root descriptor
  publication and insertion-barrier work are RELEASE before admission becomes
  usable, and entrants ACQUIRE-revalidate the GC epoch.
- GC's LIVE -> RETIRING CAS is ACQ_REL, requires root-admission count zero, and
  precedes any sweep/unlink of the state or stack-only referents.
- RETIRING/LIVE/CLOSE_LOCAL -> TOMBSTONING is ACQ_REL on the path authorized
  above.
- Existing pin release is RELEASE; zero observer ACQUIRE-fences before state
  body free.
- Address/body clear is RELEASE before TOMBSTONE/EMPTY publication.

### TLS handoff

The existing tagged TLS release-store/acquire-load contract remains. Durable
guard stage publication occurs before any throwing edge after install/swap.

No correctness argument relies on relaxed telemetry such as node counts.
Sequential consistency is reserved for an independently justified cross-object
protocol; it is not used as a substitute for missing ownership.

## Allocation, capacity, and failure matrix

| Failure | Required nonblocking result |
| --- | --- |
| Universe slot/slab unavailable during `lua_newstate` | Return NULL before OPEN |
| State-map slot unavailable | Fail state creation before exposing pointer |
| API thread-control/guard unavailable on result-bearing API | Return documented failure/BUSY |
| API guard unavailable on infallible legacy ABI | Fail-stop before dereferencing any state |
| TG/control slab exhausted during attach | Return OOM/BUSY; no malloc fallback |
| State owner busy | Return BUSY/false; no futex wait |
| Universe transaction saturated/CLOSING | Reject entry/publication; existing handles unchanged |
| State physical-pin/root-admission/descriptor count saturated | Reject and optionally POISON; never wrap |
| State RETIRING or non-catch-up sweep phase | Reject before stack/referent access; return dead/BUSY or use infallible-ABI fail-stop policy |
| Incarnation/ticket exhausted | Mark slot/universe EXHAUSTED; never reuse/wrap |
| Windows first cell-admission failure | TLS remains empty/unchanged; incoming TG hold remains active; no tagged publication occurred |
| Windows post-admission install/swap/clear | Infallible atomic stable-cell stores with the normal successful handle transfer |
| POSIX signal-mirror admission/re-admission failure | All bindings and handles unchanged; initial install or post-fork swap may retry/requeue |
| Close finds active authority | Transfer/enqueue once, arm exact release wake, then park durable close record; no blocking or hot retry |
| Reclaimer active | Park close until exact final decrement wakes it; never inspect RECLAIMING body |
| POISONED/PINNED | Drain existing exact counts; retain affected body/universe |
| Clean thread exit with guards/tag | Controller transfers durable records before OS exit |
| Forced termination | Retain records/universe; unsupported for successful reclamation |
| Callback called after close | Stable veneer returns inert ABI value; no Lua dereference |

## Main and multiple-universe behavior

- An empty OS thread may install the new main TG tag.
- Creating B while A is current does not replace A.
- Any raw B state argument is map-pinned and receives GC/root authority before
  B stack/referent dereference.
- Scoped B execution enters B OPEN transaction, pins exact B state, borrows B
  TG, and swaps A -> B under a durable guard.
- Closing B while A is current never clears A.
- Closing A while an A tag/displaced hold/state pin exists transfers to deferred
  close and parks pending until an exact release event, not by blocking or hot
  requeue.
- `G2TG(B)` fallback is valid only under exact B state/TG/close-owner authority.
- Pointer equality is never an incarnation check.

## Public API and ABI

Exported Lua/LuaJIT signatures and the one-word TG TLS ABI remain unchanged.
`lua_State` remains opaque. Internal state/global/TG/callback/controller layouts
may change and require all generated offset artifacts to rebuild.

Additive APIs/features may include:

- foreign OS-thread guard pre-registration/preflight;
- process resource-capacity telemetry;
- deferred-close completion/status diagnostics; and
- a feature query for lockless racy-close semantics.

### Feature-enable barrier

Introduce one authoritative build/runtime feature, e.g.
`LJ_LOCKLESS_LIFETIME_READY`, initially false. Exact production TG TLS and
deferred racy-close behavior are enabled only when all of these are true:

- every state is map-published, arbitrates transient roots against RETIRING,
  and is two-phase freed;
- every exported state argument is guarded or exact-current zero-looked-up;
- all supported unwind artifacts drain guards;
- callback veneers/descriptors are process-stable;
- all publication producers use OPEN/FINALIZING transactions;
- every publication-capable API entry uses/inherits that transaction while the
  exact-current read-only fast path is proven publication-free;
- attach/detach paths contain no malloc/futex/blocking wait;
- finalizers use close-owner authority and final re-drain/proof;
- reclaimers seal and RECLAIMING bodies are never generically read;
- deferred close parks on exact release events rather than polling blockers;
- all production raw TG setters and ungated TG borrows are gone; and
- platform/ABI/performance tests pass.

The runtime reports the feature and deferred-finalizer semantics. A release may
not claim racy close support while this barrier is false.

## Code surface map

Suggested modules:

- `src/lj_universe.h/.c`: universe slot/token/transactions, epochs, close
  records, bounded process slabs, feature state;
- `src/lj_statemap.h/.c`: stable address map, physical pin, transient-root/
  RETIRING gate, tombstone/recycle, publication enumeration;
- `src/lj_apiguard.h/.c`: per-thread fixed guard stack, entry veneers, normal
  and unwind cleanup;
- `src/lj_cbveneer.h/.c`: process-stable callback arena/descriptors/dispatcher;
  and
- `src/lj_reclaimgate.h/.c` or integrated universe code: reclaimer seal/count.

Existing surfaces:

- `src/lj_state.c/.h`: publish every state, `lj_state_free` tombstone/defer,
  main OPEN construction, close transfer and finalizer phases;
- `src/lj_obj.h`: exact universe/state publication keys in private structs;
- `src/lj_tgregistry.h`: whole-universe lease invariant, POISONED/PINNED
  release behavior;
- `src/lj_tg.c/.h`: admitted borrow wrapper, nonblocking prepare/commit/detach,
  sealed terminal proof;
- `src/lj_thr.c/.h`: tagged primitive contract and no raw production setter;
- `src/lib_threading.c`: preallocated controllers, try-only state claim,
  no attach futex/malloc;
- `src/lj_gc2.c`: preallocated worker controls, state-retirement arbitration,
  bounded transient-root mark catch-up, SMR/reclaimer sealing;
- `src/lj_ctype.h`, `src/lj_ccallback.c`, and callback assembly: stable veneer
  entry and durable nested guards;
- `src/lj_err.c` and platform unwind metadata: guard cleanup on every unwind;
- `src/lua.h`/generated API veneer table: complete state-argument metadata;
  and
- CI source audits: no unguarded exported `G(L)`, direct production borrow,
  blocking attach wait, or raw TG setter.

## Implementation slices

The feature barrier stays false through these slices:

1. **Bounded process allocator/slabs**: stable universe/state/thread/close/
   callback resources and exact saturation tests.
2. **Universe transaction token**: OPEN/CLOSING/FINALIZING/FINAL_DRAIN/SEALED,
   tickets, final epochs, POISONED decrements.
3. **State address map and GC-entry gate**: publish every state, exact physical
   pins, transient-root/RETIRING arbitration, bounded mark catch-up,
   tombstone/deferred `lj_state_free`, state-map stress.
4. **API guard stack**: generated all-state-argument veneers, zero-lookup proof,
   normal return cleanup.
5. **Unwind integration**: Lua longjmp, DWARF/C++, JIT/interpreter, and Windows
   SEH guard draining.
6. **Stable callback veneers**: process arena, descriptors, closed inert return,
   no pre-admission raw CTState/g access.
7. **Whole-universe TG invariant**: gate-aware borrow wrapper and source audit;
   still no production exact tag.
8. **Nonblocking TG lifecycle**: preallocated main/spawn/foreign/GC controls,
   no futex/malloc/wait, staged attach/detach.
9. **Deferred close state machine**: API-frame transfer, finalizer authority,
   final re-drain/epoch, reclaimer seal, terminal proof.
10. **Atomic production TLS migration**: replace all raw setters and enable
    exact binding only with all callers migrated.
11. **Feature enable**: publish semantics/telemetry, set readiness barrier,
    performance/ABI gates, then remove compatibility lifetime claims.

Each slice is independently tested and fail-closed. No intermediate commit
mixes an exact tag with a later raw clear.

## Required tests

### Universe transactions and epochs

- OPEN transaction versus CLOSING CAS schedules;
- external final epoch with out-of-order publication tickets;
- FINALIZING -> FINAL_DRAIN close-local publication seal and second final
  epoch;
- saturation, incarnation/ticket exhaustion, POISONED decrement;
- slot recycle under stale exact keys; and
- no post-CLOSING external publication.

### State map

- publish every state creation path;
- raw lookup pin versus concurrent GC RETIRING and `lj_state_free`
  TOMBSTONING;
- existing GC anchor/root acquisition versus transient entry-root publication;
- transient root wins before RETIRING and completes current-epoch mark catch-up;
- RETIRING or non-catch-up sweep phase wins and entry rejects before any
  stack/referent read;
- a raw C `lua_State *` with no Lua anchor is not a permanent root between
  calls;
- free deferral until pin/root/owner zero;
- TOMBSTONING precedes each physical state/stack free and TOMBSTONE follows it;
- map slot recycle/incarnation rejection;
- bounded probe/table exhaustion before pointer exposure;
- same address after valid lifetime demonstrates documented raw-pointer
  boundary; and
- ASAN/TSAN model stress with no state header read before physical pin and no
  stack/referent read before GC/root admission.

### API guard coverage

- generated audit of every exported state argument;
- `lua_xmove` and other multi-state functions protect all arguments with map
  pins or the sole exact-current exception plus GC/root authority;
- zero lookup only for exact tagged `cur_L` equality;
- exact-current read-only getter omits OPEN CX16, while a publication-capable
  exact-current call after CLOSING is rejected;
- arbitrary same-universe coroutine still map-looked-up;
- nested API depth/exhaustion policy;
- normal return, Lua error, panic, longjmp, C++ throw, DWARF cleanup, JIT
  external unwind, and Windows SEH;
- `lua_close` winning frame transfer and losing concurrent close; and
- no post-final-release body access under sanitizer instrumentation.

### Callback veneers and nesting

- call stale veneer after descriptor CLOSED and after GG free;
- prove veneer/dispatcher touches no raw g/CTState before exact admission;
- descriptor invocation versus close/disown schedules;
- A -> B -> C normal and setup/body/result unwind;
- errno/Win32 LastError preservation;
- callback arena exhaustion before pointer exposure;
- auto-attached target nonblocking BUSY behavior; and
- veneer addresses never unmapped/reassigned.

### Attach/detach

Pause every stage for main/spawn/foreign/GC worker and race close. Assert a
universe transaction, state pin, or TG lease continuously protects bodies.

Cover:

- state/TG/controller slab exhaustion;
- owner ID exhaustion;
- state owner BUSY without futex;
- Windows first-cell admission failure and infallible post-admission
  install/swap/clear ownership;
- POSIX initial/post-fork signal-mirror admission failure ownership;
- no attach malloc/futex symbols/artifacts;
- resumable detach work;
- exact TOMBSTONING before state free and TOMBSTONE after body free; and
- final TG/state release followed by process-only access.

### Finalizers and close

- uncontended close remains synchronous;
- each active guard/tag/pin/txn/reclaimer causes close-record transfer and
  nonblocking return;
- finalizers run exactly once later;
- finalizer creates coroutines/internal states as CLOSE_LOCAL publications;
- attempted external state/thread/callback publication during CLOSING fails;
- finalizer error/unwind drains guards;
- final re-drain catches a finalizer-created pin/root;
- no freeall before final epoch/proof;
- freeall performs each state TOMBSTONING/body-free/TOMBSTONE sequence in
  order;
- leaked/PINNED/POISONED authority retains whole universe; and
- deferred completion status/feature query.

### Reclaimer seal

- close seal loses/wins against RETIRED -> RECLAIMING;
- active reclaimer completes to EMPTY before count leave;
- long-lived/leaked blocker leaves close PENDING with no executor spin, and its
  exact final release enqueues the close record once;
- terminal scanner never loads body for RECLAIMING;
- stuck RECLAIMING yields BUSY/POISON without dereference; and
- POISONED active reclaimer count drains without unpoisoning.

### Platform, ABI, and performance

- GCC/Clang Linux, static/PIC, ASAN/UBSAN/TSAN;
- osxcross/Darling and native macOS when available;
- MinGW UCRT/Wine and native Windows when available;
- public symbol/calling-convention comparison;
- callback code remains mapped after close;
- hot `lj_thr_get_tg` disassembly unchanged;
- no malloc/futex/blocking wait in attach/callback/API-entry artifacts;
- bounded cold costs for state-map pin, guard push, OPEN txn, swap, and close
  wake/step handling, with no OPEN CX16 in the proven exact-current read-only
  getter path; and
- ordinary Lua/JIT/FFI corpus with feature disabled and enabled.

## Acceptance checklist

The feature barrier may become true only when:

- every `lua_State` creation publishes an exact state-map incarnation;
- every ordinary GC retirement arbitrates against transient roots and completes
  required mark catch-up before stack/referent access;
- every state free publishes RETIRING/TOMBSTONING before body free, defers while
  pins/roots/owners remain, and publishes TOMBSTONE only after body free;
- every exported state argument has a physical map pin or the sole
  exact-current exception, plus GC/root authority before referent access;
- zero lookup is restricted to exact tagged-current-state equality;
- publication-capable top-level entries acquire/inherit OPEN (or privileged
  FINALIZING) while proven exact-current read-only entries remain CX16-free;
- every unwind path consumes durable API/callback guards exactly once;
- all required process resources are bounded/preallocated/recyclable;
- attach/API/callback hot admission performs no malloc/futex/blocking wait;
- every external publication uses OPEN/FINALIZING transaction tickets;
- close freezes both external and post-finalizer publication epochs;
- callback veneers remain mapped and contain no raw universe pointer;
- finalizers run under close-owner main state/TG authority before final proof;
- reclaimer/SMR/list writers are sealed before terminal scan;
- generic code never dereferences a RECLAIMING body;
- POISONED/PINNED counts can drain but never authorize reuse/free;
- Windows first-cell failure and infallible post-admission cell-store ownership
  match the stable-cell backend;
- POSIX signal-mirror admission/re-admission remains a separate cold failure;
- every production TG borrow is gate-aware and pins the exact TG plus U1
  universe-teardown infrastructure;
- every raw production TG setter is gone;
- racy close is nonblocking and deferred-finalizer semantics are reported;
- deferred close parks on exact release events and never hot-requeues a leaked
  authority;
- post-close stale raw state use remains explicitly invalid; and
- ABI, artifact, sanitizer, stress, and performance gates pass on all supported
  targets.

Until all items hold, the stable TG registry remains an additional negative
reclamation veto and the tagged TG TLS primitive remains dormant in production
lifecycle callers.
