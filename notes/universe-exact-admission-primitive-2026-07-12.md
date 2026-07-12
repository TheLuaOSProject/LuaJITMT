# Dormant exact universe admission and publication-epoch primitive

Date: 2026-07-12

Status: implemented and tested as a dormant control-plane primitive. It is not
connected to `lua_newstate`, exported API entry, TG attach/detach, callbacks,
GC, JIT, FFI, or `lua_close`, and it does not make the lockless-lifetime feature
ready. No plan file was changed.

This checkpoint implements the universe-token slice described in
`notes/universe-admission-and-tg-lease-lifetime-design-2026-07-12.md` without
prematurely activating any production lifetime path.

## Source and test surface

- `src/lj_universe.h` defines the stable slot, exact key, linear build,
  transaction, and close-owner handles, states, results, snapshots, and public
  dormant API.
- `src/lj_universe.c` implements claim/publish/abort, transaction admission and
  exact release, close ownership, both epoch freezes, sealing, recycling, and
  absorbing POISONED behavior.
- `tests/t-universe-token.c` is a standalone deterministic/stress fixture.
- `m4_universe_token_model` wires that fixture into
  `tests/suites/m4_threading.lua` without linking it into the LuaJIT runtime.

The slot itself is allocated by the caller at a safe control-plane point and
must remain aligned and address-stable. The module allocates and frees no
memory. Supplying slots from a fixed process-owned bounded slab, including
capacity accounting and process registry publication, is intentionally still
pending.

## Exact slot representation

`LJUniverseSlot` is at least 16-byte aligned. Its first field is a 16-byte
`LJUniverseToken`; the body and both frozen epochs are separately aligned
16-byte words:

```text
token.lo = exact current or reserved-successor incarnation
token.hi = transaction_count << 4 | state
body.lo  = numeric global_State/body pointer
body.hi  = exact owning incarnation
external_epoch.lo/terminal_epoch.lo = zero or frozen next-ticket value
external_epoch.hi/terminal_epoch.hi = exact owning incarnation
```

The token states are `EMPTY`, `BUILDING`, `OPEN`, `CLOSING`, `FINALIZING`,
`FINAL_DRAIN`, `SEALED`, `POISONED`, and `EXHAUSTED`. Component validation is
state-specific. Incarnation zero is permitted only for the never-claimed
`EMPTY/0` sentinel. `EXHAUSTED` is terminal, count zero, and incarnation
`UINT64_MAX`.

An `EMPTY/N` token already reserves N as the next claim identity. The initial
sentinel claims incarnation 1 and retags all side words while `BUILDING`;
subsequent claims keep the reserved non-zero identity. Abort or terminal
recycle of incarnation N publishes `EMPTY/N+1`, including N+1 tags on the null
body and both zero epochs, so every old key becomes stale at that terminal
token CAS. Incarnations never wrap: `EMPTY/UINT64_MAX` exact-transitions to
terminal `EXHAUSTED`, while all live states reject `UINT64_MAX` as malformed.
`POISONED/UINT64_MAX` remains representable solely so a forged live-max token
can fail closed without wrapping any side tag.

### Why the token snapshot uses CX16

A hi/lo/hi subload is not exact enough for this token. Unlike a body
incarnation, the state/count hi half deliberately recurs after a full
lifecycle, such as `OPEN/0` for incarnation N and N+1. A reader could otherwise
observe an old lo between equal recurring hi values. Even though a later keyed
CAS would reject that old incarnation, pre-admission corruption handling could
mistake the split snapshot for the current universe and poison a legitimate
reuse.

`lj_universe_snapshot()` therefore linearizes an exact 128-bit read with
`cmpxchg16b` using an all-zero comparison. A claimed slot does not equal zero,
so the instruction only returns the exact word. The initial all-zero slot takes
the success path but writes the identical zero value. This stays inline and
lock-free on the supported x86-64 targets; it does not call libatomic.

The body uses incarnation/pointer/incarnation subloads. That high half is the
non-wrapping incarnation itself and never returns to an older value. Within one
incarnation, the only pointer change after publication is the terminal
body-to-null decision, which simultaneously retags the null body to the
successor incarnation. No body is dereferenced from this snapshot alone. Epoch
snapshots use CX16 because zero deliberately recurs after exact recycle.

## Linear construction authority and failed-build recovery

`EMPTY -> BUILDING` returns an active `LJUniverseBuild`, not a public
`LJUniverseKey`. This handle is linear and private to construction.

Publish and abort compete on one exact body-decision CAS for incarnation N:

```text
publish: {NULL, N} -> {body, N}
abort:   {NULL, N} -> {NULL, N+1}
```

Only the body-decision winner may complete the token transition. Publish then
release-transitions `BUILDING/N -> OPEN/N`, consumes the build handle, and
returns the public exact key. Abort advances both zero-epoch tags, restores the
first ticket, transitions `BUILDING/N -> EMPTY/N+1`, and consumes the build
handle. Thus a private `lua_newstate` failure can return the slot for later
claim without reusing an incarnation.

A copied/racing build loser recognizes the winner's exact body marker, consumes
its local handle, and returns `LOST` or `DENIED`. It never infers recycle
authority and never poisons a legitimately published, aborted, or subsequently
reused universe. A malformed body association which is not either legal marker
fails closed instead.

## Transactions, tickets, and body handoff

An external transaction increments the exact token only from `OPEN`. A
privileged finalizer transaction increments it only from `FINALIZING` and must
present the active winner-only close handle. Count saturation returns
`SATURATED` without changing existing ownership.

Body validation happens only after the transaction-count CAS wins. This order
is essential: a pre-CAS token snapshot has no lifetime authority, so close may
legitimately recycle the slot before admission. Validating first could falsely
diagnose that reuse as corruption. After the count increment, close preserves
the count in `CLOSING` and cannot advance to finalization or recycle; the exact
body snapshot is then stable and safe to return in the transaction handle.

Every successful transaction reserves one unique non-zero ticket from
`next_publication`. It starts at one. `UINT64_MAX-1` is the final issued ticket;
`UINT64_MAX` is a sticky exhausted next-ticket sentinel and never increments or
wraps. A failed ticket reservation poisons the exact incarnation, drains the
detector's own count in POISONED, and returns `TICKET_EXHAUSTED` without an
active handle.

Transaction handles are linear and record the exact key, body, ticket, and
external/finalizer class. Release decrements only in that class's valid open or
drain state, or in absorbing `POISONED`. The last decrement performs an acquire
fence. A stale or invalid release does not consume the handle or guess a count.

## Close ownership and epoch freezes

`lj_universe_try_close()` is the `OPEN/N -> CLOSING/N` logical-close LP. Exactly
one `LJ_UNIVERSE_OK` result creates an active `LJUniverseClose`. `LOST`,
`DENIED`, `STALE`, `POISONED`, and validation failures leave the output
inactive. In particular, merely observing `CLOSING` never lets a losing caller
reconstruct close ownership.

After external count zero, the winner freezes `next_publication` into
`external_final_publication` and transitions to `FINALIZING`. Every external
ticket is strictly below that next-ticket epoch. Finalizer transactions use the
same monotonic sequence. The winner later transitions
`FINALIZING/N -> FINAL_DRAIN/N` to deny new privileged transactions. After the
privileged count drains, it freezes `terminal_final_publication` and publishes
`SEALED`.

Each tagged epoch CAS is the exact decision marker for its close stage. A
copied close value that finds the identical marker may help the suspended
publisher perform the token transition; it does not report corruption. Markers
are irreversible for that incarnation, even if the original marker publisher
loses its later token CAS after a helper has advanced another stage. Exact
recycle is the sole marker reset. A handle-local cached epoch disagreement
returns `CORRUPT` to that caller but never poisons otherwise-valid shared state.

Only the still-active close winner may recycle. After the caller's future
terminal proof and physical body destruction, recycle first decides
`{body,N}->{NULL,N+1}`. Only that body-CAS winner may verify and retag both
epochs, reset the ticket, and publish `SEALED/N -> EMPTY/N+1`. Copied recycler
losers recognize the null/successor decision marker and perform no side writes,
including no ticket reset. This primitive does not itself run finalizers, free
GG, or perform those terminal proofs.

## Fail-closed corruption and POISONED drain

Malformed token components, body associations, or epoch fields deny admission.
When the detector has exact-incarnation authority, it completes a CAS
transition to absorbing `POISONED` without changing the transaction count.
Already-admitted external and finalizer handles may decrement that count all
the way to zero, but zero never reopens, seals, clears, frees, or recycles the
slot.

Poison completion has no fixed retry cutoff. Every detector supplies the exact
token snapshot which established its authority plus an explicit set of states
through which that particular corruption remains authoritative. It may retry
through permitted same-incarnation count churn, but it never adopts an
unrelated later close stage and stops stale if the slot reaches another
incarnation. Thus an old snapshot can neither follow reuse nor poison a valid
later stage merely because its first CAS lost.

The fixture forces 96 real poison-CAS losses. A cooperating release thread
decrements an exact active transaction after every poison snapshot, so every
attempt loses until all 96 handles drain. The detector then wins POISONED/1 and
releases its own anonymous admitted count to POISONED/0. This specifically
guards against a fixed retry budget leaving corrupt metadata in `OPEN` or
`FINALIZING`.

## Nonblocking and performance properties

The dormant module contains no allocator, mutex, futex, condition wait, sleep,
yield, or OS call. Operations use validation plus CX16/64-bit atomics. Token
updates generally make one CAS attempt and may return `LOST`; exact snapshots,
body snapshots, ticket reservation, and corruption poisoning contain lock-free
retry loops. None is claimed to be wait-free or bounded under adversarial
contention. The deterministic wait hooks exist only under
`LJ_UNIVERSE_TEST_HELPERS` and are absent from a production object.

Costs are deliberately cold and currently irrelevant to ordinary LuaJIT
performance because no runtime path calls this module. The exact token snapshot
uses one locked CX16, transaction admission adds one state/count CX16 and one
64-bit ticket CAS, and release uses one CX16. A proven exact-current read-only
Lua API fast path will not take a universe transaction once production API
guards are designed; this checkpoint does not alter that future requirement.

## Fixture coverage

The standalone model covers:

- layout/alignment and every token state's component constraints;
- scalar claim, publish, external admission, close, finalizer admission, both
  publication freezes, seal, recycle, and stale-key rejection;
- out-of-order transaction completion with exact external and final epochs;
- failed-build abort/retry and 200 concurrent copied publish-versus-abort races;
- same-incarnation copied external/terminal stage races, including a paused
  marker publisher while a helper advances through `FINAL_DRAIN` or `SEALED`,
  proving that the late publisher cannot erase the authoritative marker;
- stale external-freeze and terminal-seal copies paused after validation while
  a twin recycles and opens N+1, proving stale returns and untouched N+1 epoch
  fields;
- a delayed copied recycler across recycle/reuse, proving it cannot reset the
  successor ticket or duplicate an issued ticket;
- handle-local cached-epoch mismatches which return corruption while a good
  twin continues without any shared poison;
- an exact old-snapshot poison CAS paused while a good twin advances from
  `FINALIZING` to `FINAL_DRAIN`, proving the detector returns `DENIED` rather
  than adopting authority over the new stage;
- 12 concurrent close contenders with exactly one active winner handle;
- no external publication after `CLOSING` and no privileged publication after
  `FINAL_DRAIN`;
- count saturation without mutation, final-ticket issuance, sticky ticket
  exhaustion, incarnation exhaustion, forged live `UINT64_MAX`, malformed
  state/body/epoch/recycle proofs, old-key poison rejection, and
  misaligned/invalid inputs;
- exact external and finalizer POISONED decrements to zero;
- the deterministic 96-loss concurrent poison/drain schedule;
- four readers across 750 complete slot reincarnations, continuously taking
  exact snapshots and attempting keyed admission while state/count hi values
  recur; and
- an eight-thread transaction-versus-close stress with unique ticket checking.

Validation completed for this checkpoint:

- native x86-64 Linux GCC and Clang warning-clean optimized builds and repeated
  stress execution;
- GCC and Clang AddressSanitizer plus UndefinedBehaviorSanitizer;
- GCC ThreadSanitizer (with GCC's unsupported-`atomic_thread_fence`
  instrumentation warning demoted from `-Werror`);
- a warning-clean Clang static-analyzer pass and production-object checks which
  found no undefined helper symbols and confirmed inline `cmpxchg16b`;
- MinGW UCRT x86-64 build and Wine execution; and
- osxcross macOS x86-64 build and Darling execution.

The normal LuaJIT build and the focused `m4_universe_token_model` suite case
were also run after the standalone validation. No public Lua/LuaJIT ABI or
runtime object layout changed because this module is not linked or included by
production code.

## Remaining activation blockers

This checkpoint must remain dormant until at least the following exist and are
integrated atomically:

1. A bounded process allocator/slab and stable registry must supply universe
   slots, close records, state-map slots, guard records, and capacity/failure
   telemetry without hot allocation.
2. Every `lua_State` needs the stable address map, exact physical pin,
   transient-root/RETIRING arbitration, and two-phase tombstone/free protocol.
3. Every exported API state argument needs a durable, bounded, unwind-aware API
   guard (including longjmp, DWARF/C++, JIT external unwind, and Windows SEH).
4. FFI callbacks need process-stable veneers/descriptors and exact nested
   cross-universe admission before any raw universe/state dereference.
5. TG leases must acquire whole-universe teardown meaning, all attach/detach
   paths must become preallocated and nonblocking, and all production raw TG
   setters/ungated borrows must disappear in one feature-gated migration.
6. A durable nonblocking close executor, wake protocol, close-record transfer,
   finalizer authority, reclaimer seal, state/TG/callback enumeration, both
   terminal proofs, and ordered physical teardown must be implemented.
7. Publication producers must record commit or explicit abort/tombstone for
   every ticket below the frozen epochs; this token deliberately does not yet
   implement those state/callback/TG publication ledgers.
8. Cross-platform ABI/artifact/performance gates and the authoritative
   lifetime-readiness feature barrier must pass before activation.

Until those blockers are resolved, constructing and testing this primitive is
safe, but using it to advertise racy close or exact production lifetime safety
would be incorrect.
