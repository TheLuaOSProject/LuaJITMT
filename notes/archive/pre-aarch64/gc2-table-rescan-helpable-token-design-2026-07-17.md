# GC2 helpable table-rescan token design

Date: 2026-07-17

Status: design for implementation; no source change is made by this note.

Correction: the locally incremented token-generation transitions below are
superseded by
`gc2-table-token-exact-target-correction-2026-07-18.md`. Production
handoff must target the exact global descriptor generation so delayed helpers
cannot recreate a completed request.

## Outcome

Replace the provisional global table-rescan count with two overlapping exact
locators:

1. a generation-tagged, helpable publication descriptor; and
2. a generation-tagged per-table `PENDING` token which remains visible for the
   complete traversal.

The token plane is the durable fallback queue. SSB and grey publication remain
fast-path accelerators, but correctness and phase closure do not depend on
either queue accepting an item.

There is deliberately no `CLAIMED` table-token state. Traversal is monotonic and
idempotent, so leaving the token `PENDING` lets any peer repeat or complete the
work if the first scanner is descheduled. An exact generation CAS makes stale
completion harmless.

## Defect being replaced

The current sequence in `gc2_table_rescan_pending_begin()` is:

```text
table_rescan_pending++
CAS table NONE -> INSTALLING
```

See `src/lj_gc2.c:15893-15914`. The sequence prevents an unsafe zero close, but
it is not lock-free. A publisher paused after the increment and before the table
CAS leaves a global close veto with no table identity that a helper can locate.
`INSTALLING` and `CANCELLED` then settle that publisher-owned reservation in
`gc2_table_rescan_pending_finish()` and
`gc2_table_rescan_pending_clear_held()` (`src/lj_gc2.c:15928-16005`), so a peer
still cannot complete a publisher paused before `INSTALLING` exists.

Moving the increment after the table CAS only reverses the hole: a local token
would then exist before the global close predicate could see it. Adding more
stages around an exactly-once increment does not solve the two-word transaction.
A helper cannot distinguish "the owner has not incremented yet" from "the owner
incremented and was descheduled before publishing the next stage".

The replacement therefore removes `table_rescan_pending` from correctness. It
may remain temporarily as telemetry or a conservative migration veto, but its
value must not grant phase closure or reclamation.

## Existing machinery to reuse

- `la_cas128()` is already an inline `cmpxchg16b` authority on x86-64
  (`src/lj_atomic.h:79-99`). `lj_universe_snapshot()` demonstrates an exact
  read using a no-op CX16 comparison (`src/lj_universe.h:181-207`). Use the same
  technique for the descriptor; a hi/lo/hi sample is insufficient when both
  pointer and state values can recur.
- The current table stamp already pairs a dirty epoch and scan cycle in one
  64-bit CAS word (`src/lj_gc2.c:13939-14039`). Keep this hot-path word and add
  a separate exact token word.
- The small-arena registry is authoritatively enumerable through
  `gc2_small_arena_tab_acq()` and `lj_arena_hugetab_next()`
  (`src/lj_arena.h:589-593`, `src/lj_arena.c:2276-2310`). The bounded recovery
  walker demonstrates a registry-slot/cell cursor at
  `src/lj_gc2.c:17524-17566`; terminal recovery preflight demonstrates a full
  joined-world scan at `src/lj_gc2.c:17799-17845`.
- Owner SSB append is a release-published fast path in
  `gc2_ssb_push_tg()` (`src/lj_gc2.c:12992-13021`). Worker grey append is a
  Chase-Lev owner operation in `gc2_grey_push()`
  (`src/lj_gc2.c:9704-9735`). Neither is a general helpable fallback.
- The typed activation word already contains the required root-publication
  gate: `OPEN`, `CLOSING`, `PENDING`, and `COMMIT`
  (`src/lj_gc2token.h:40-61`). Its legal exact transitions are defined at
  `src/lj_gc2token.h:145-163`.

Do not use the current recovery counter as the authoritative fallback for this
handoff. `gc2_recovery_count_reserve()` precedes the per-object recovery state
CAS (`src/lj_gc2.c:8974-8995`, with the small path at
`src/lj_gc2.c:9130-9239`), which is the same reserve-before-locator progress
defect. Recovery can become an accelerator after its own publication protocol
is made helpable; it is not needed for table-rescan correctness.

The allocated finalizer MPSC stack is also unsuitable: its nodes are distinct,
intrusive identities. An outstanding table rescan has no spare node, and node
allocation cannot be a correctness prerequisite.

## Chosen data model

### Global helpable descriptor

Add one 16-byte-aligned `la_u128 table_rescan_desc` to `GC2State` initially.

```text
lo = 0                         IDLE
lo = aligned GCtab pointer     ACTIVE
lo = 1                         PINNED / no-reclaim sentinel
hi = non-wrapping publication generation
```

All supported table pointers are at least 16-byte aligned, so `0` and `1` are
unambiguous. The initial single descriptor minimizes implementation surface.
It serializes only the short token-publication handoff, not table traversal.
It can later become a fixed shard array or a per-TG descriptor plus one global
fallback without changing the token protocol.

Descriptor generation increments on every `IDLE -> ACTIVE` publication. It is
not reset when the same table address recurs. Saturation changes the exact
descriptor to `PINNED` and enters typed activation `NO_RECLAIM`; it never wraps.

### Per-table scan and token side plane

Extend the current stamp cell from 8 to 16 bytes:

```c
typedef struct GC2TabStamp {
  uint64_t scan;   /* Low 32: dirty epoch. High 32: completed scan cycle. */
  uint64_t token;  /* generation << 2 | GC2TabTokenState. */
} GC2TabStamp;
```

The token states are:

```text
NONE     = 0
PENDING  = 1
PINNED   = 2
reserved = 3
```

The maximum generation is `UINT64_MAX >> 2`. `PINNED` is absorbing. The spare
state must not be turned into an owner-only `CLAIMED` state.

The existing `dirty32` and `cycle32` stamp fields are separate bounded
authorities. The current dirty bump wraps back to one at
`src/lj_gc2.c:14027-14030`, and the global cycle increment is also 32-bit. The
new token generation prevents stale token completion, but it does not make a
wrapped `gc2_table_scan_current()` comparison sound. Before this token becomes
final reclaim authority, either widen both stamp components or make saturation
select sticky no-reclaim instead of wrapping. The b1.2 correctness-first choice
may pin on saturation; a later layout/performance pass can widen the fields.

The current sidecar lookup rejects huge allocations and non-arena allocators at
`src/lj_gc2.c:13962-13989`. Both cases must be explicit:

- For a small arena, preallocate the complete stamp/token sidecar before the
  traversable arena becomes registry-visible. The current lazy `calloc()` at
  `src/lj_gc2.c:13975-13987` must not sit between an ACTIVE descriptor and its
  token, because a stalled libc allocator is not helpable. A sidecar allocation
  failure rejects that arena publication or selects sticky no-reclaim.
- A huge allocation has a unique `GCAhdr`. The header currently has 24 spare
  bytes at `src/lj_arena.h:107-133`. Use two 64-bit fields for the huge table's
  scan and token, leaving eight bytes of padding. This retains the 128-byte
  header geometry.
- Custom `lua_Alloc` is deliberately outside the temporary b1.2 allocator
  support. Until generalized metadata exists, encountering a table outside the
  internal arena allocator must select the documented no-reclaim/unsupported
  path; it must not silently return an unstamped success.

The small sidecar grows from 32 KiB to 64 KiB for 4096 cells. This is a
correctness-first b1.2 tradeoff. If measured memory overhead is unacceptable,
the clean alternative is one aligned 16-byte stamp embedded in `GCtab`
(`GCtab` is currently 80 bytes and would become 96 bytes). That alternative
covers small, huge, and later custom allocations directly but makes allocator
free-side inspection and registry enumeration more invasive. Do not implement
both representations.

### Advisory wake state

A sticky `table_token_scan_needed` hint may avoid unconditional fallback scans.
It is not a count and is never a close authority. Publication sets it before
clearing the descriptor. It may remain set forever after the first fallback;
an epoch-tagged full-pass acknowledgement can be added later as a performance
optimization. Blindly clearing a Boolean at end-of-pass would create another
lost-publication race.

`LJ_GC_NEEDSCAN` remains an advisory header mirror. It is not a locator, a
lifetime pin, or a close condition.

## Exact transition tables

### Descriptor

| From | To | Actor and precondition | Linearization/result |
| --- | --- | --- | --- |
| `IDLE(g)` | `ACTIVE(p,g+1)` | Publisher; exact CX16, generation below max | First globally discoverable identity for this request |
| `ACTIVE(p,g)` | `IDLE(g)` | Any helper; exact CX16 after token publication | Responsibility transfers to the per-table token plane |
| `IDLE(max)` | `PINNED(max)` | Publisher | Sticky no-reclaim; request is never fabricated or dropped |
| malformed or impossible stale identity | `PINNED` | Any observer with the exact snapshot | Fail closed |
| `PINNED` | `PINNED` | Any actor | Absorbing |

A helper always clears the exact `{pointer,generation}` value it observed. A
delayed helper cannot clear a later descriptor even when allocator reuse gives
the later descriptor the same pointer.

### Table token

| From | To | Meaning |
| --- | --- | --- |
| `NONE(g)` | `PENDING(g+1)` | Publish first durable request |
| `PENDING(g)` | `PENDING(g+1)` | Refresh an already-pending request and invalidate every older scanner |
| `PENDING(g)` | `NONE(g+1)` | Exact stable traversal completion |
| `NONE/PENDING(max)` | `PINNED(max)` | Generation saturation; retain table and select no-reclaim |
| malformed state | `PINNED` | Fail closed |
| `PINNED` | `PINNED` | Absorbing |

Every logical request performs a generation-changing CAS even when the token is
already `PENDING`. Merely observing `PENDING` and coalescing without a CAS is
incorrect: an older scanner may already have published its scan stamp and be
about to clear that exact token.

No token transition owns traversal. Multiple helpers may scan one `PENDING`
generation. At most one exact completion wins; the others observe a changed
generation or `NONE` and stop.

## Publication and helping algorithm

The first migration stage replaces the current aggregate reservation inside
`gc2_table_rescan_later_()` (`src/lj_gc2.c:16098-16119`). The surrounding legacy
barrier order remains in force until the activation-gate migration described
below.

Conceptual publisher/helper loop:

```text
publish_request(g, table):
  acquire exact table allocation admission
  repeat:
    d = exact_snapshot(table_rescan_desc)
    if d is PINNED:
      pin activation NO_RECLAIM
      release table allocation admission
      return conservative failure
    if d is ACTIVE:
      help(d); continue
    if d.gen is maximum:
      exact-CX16 IDLE -> PINNED; pin activation NO_RECLAIM
      release table allocation admission
      return conservative failure
    if exact_CX16(IDLE(d.gen), ACTIVE(table, d.gen + 1)):
      repeat help(the exact ACTIVE value just installed) until it is transferred
      release table allocation admission
      return success; the token may already have been scanned back to NONE

help(active):
  acquire only the arena/HugeTab mapping pin needed to reach side metadata
  exact-recheck table_rescan_desc == active
  if recheck lost: release mapping pin; return
  locate the small or huge stamp by address, without reading GCtab body bytes
  CAS-loop token NONE->PENDING or PENDING->PENDING, incrementing generation
  if token generation saturates: publish PINNED and activation NO_RECLAIM
  if token is already PINNED: retain global no-reclaim
  release-set LJ_GC_NEEDSCAN as an advisory mirror
  set sticky table-token scan hint and wake a worker
  optionally publish a raw owner SSB or worker-grey accelerator
  exact-CX16 active descriptor -> IDLE
  release mapping pin
```

Token publication must happen before descriptor clear. The token CAS is release
or acquire-release; the exact descriptor clear is release or acquire-release.
Thus every observer sees at least one exact locator:

```text
ACTIVE descriptor  -------------------------+
                         PENDING token ------+--------------------
```

Two helpers can both refresh the token. This is safe. They race to clear one
unchanged exact descriptor; one wins, while every extra token generation is a
conservative request. A helper holds a registry/mapping pin and rechecks the
exact descriptor before touching the side token, so a helper delayed across
descriptor reuse cannot touch unmapped metadata.

The publisher also holds exact allocation admission before its descriptor CAS
and through descriptor-to-token transfer. If it finds an unrelated ACTIVE
descriptor, it may help while retaining its own bounded admission or release and
retry; it must not publish a raw table pointer first and try to acquire lifetime
afterward.

The descriptor-to-token helper deliberately does not acquire or validate the
table body. The validated publisher made ACTIVE a trusted exact identity, and
free/unmap paths must treat ACTIVE as a mechanical pin. This lets a helper
transfer responsibility even while the table lifetime lane is `MUTATING`,
`RECOVERY`, or otherwise unavailable to a scanner. Requiring body admission at
this point would let one stalled table owner block the single global descriptor
and recreate an unhelpable publication state.

A transient registry/mapping-pin failure leaves the descriptor ACTIVE while the
reciprocal terminal owner observes and abandons/helps that descriptor. A missing
mapping, invalid cell address, or terminal `STALE` result while the exact ACTIVE
descriptor exists is an invariant violation: publish `PINNED`/`NO_RECLAIM`, do
not clear it as dead. Body acquisition and type validation happen later in the
token scan lane.

Use `gc2_ssb_push_tg(..., allow_drain=0)` only when the caller proves it owns
that TG's active SSB. A foreign helper must not advance another TG's SSB cursor.
A worker that owns the grey bottom may push grey. Otherwise the exact token lane
is sufficient.

Do not route the authoritative operation through
`gc2_publish_mutator_()` (`src/lj_gc2.c:13084-13095`) or
`gc2_publish_worker()` (`src/lj_gc2.c:13143-13149`): both currently fall into
the non-helpable recovery reservation on queue failure.

## Traversal and completion algorithm

A queue item carries only a table pointer, not a token generation. This is
intentional: a stale duplicate can help whatever request is current. The exact
generation belongs to the completion ticket.

```text
scan_pending_table(stamp, table):
  observe token PENDING(g)
  acquire and validate exact table allocation
  exact-recheck token is PENDING(g)
  capture dirty epoch D from scan word
  traverse table using existing SMR/table snapshots
  make every discovered child/weak follow-up handoff durably visible
  if admission or a structural snapshot is transient:
    leave token PENDING; wake/continue bounded scan lane; release admission
  CAS scan word to {current cycle, D}, only while dirty still equals D
  if scan CAS fails:
    leave token PENDING; wake/continue; release admission
  CAS exact token PENDING(g) -> NONE(g+1)
  if token CAS fails:
    a refreshed/newer request exists; leave it discoverable
  if token CAS succeeds:
    clear advisory LJ_GC_NEEDSCAN
    acquire-recheck token; restore NEEDSCAN if token is not NONE
  release admission
```

The completion order is scan proof first, token clear second. This retains the
existing useful order at `src/lj_gc2.c:16537-16550`, but replaces its separate
counted-token clear and post-clear repair with an exact generation check.
Token clear is permitted only after every child traversal/follow-up produced by
that scan has a durable grey, SSB, token-plane, or sticky no-reclaim outcome.

Important cases:

- A writer changes the dirty stamp before the scanner's scan CAS: scan CAS
  fails and `PENDING` remains.
- A writer starts after the scan CAS but before token clear: its descriptor is
  visible and it refreshes `PENDING(g) -> PENDING(g+1)`, so the old clear fails.
- The old scanner clears first: the writer changes `NONE -> PENDING`; no request
  is lost.
- A publisher is paused between its dirty update and token refresh: its ACTIVE
  descriptor remains helpable. Even if an old scanner clears the old token, a
  helper installs the new `PENDING` token before clearing the descriptor.
- A forced retry refreshes the token even if the dirty epoch did not change.
  Therefore a scanner must never discard a `PENDING` token merely because
  `gc2_table_scan_current()` reports the current cycle.
- A popped grey pointer or consumed SSB duplicate may disappear on transient
  admission because the exact token remains. Retaining the SSB slot is still a
  useful fast-path optimization, not a correctness requirement.

Partial marking and partial weak traversal are already monotonic/idempotent, as
the current retry path notes at `src/lj_gc2.c:16554-16557`.

## Bounded token scan lane

Add a worker/recovery-style lane which treats the token plane as an enumerable
queue.

For small arenas:

1. Hold the registry/SMR protection required by `lj_arena_hugetab_next()`.
2. Resume from `{registry slot, cell}`.
3. Read only the side token first. Skip `NONE` without reading an allocation
   body.
4. For `PENDING`, acquire exact arena/allocation admission, validate the cell is
   the current table start, exact-recheck the token, and scan it.
5. For transient lifetime ownership, leave the token untouched, advance the
   bounded cursor, and yield instead of spinning on that cell.

For huge allocations, walk stable TG HugeTabs using the existing huge recovery
lane pattern. Read the unique header token, then acquire a counted HugeTab/body
reader before validating `GCtab` or traversing it.

Ordinary worker turns can prioritize SSB and grey work, then inspect a bounded
number of token cells when the sticky hint is set. Phase close performs complete
passes until it observes no `PENDING`, ACTIVE, or PINNED state. Joined terminal
shutdown performs a full preflight, analogous to recovery terminal preflight,
before it clears metadata or frees sidecars.

Observing `PINNED` does not cause an endless close loop: it aborts
reclaim-authorizing close and preserves the absorbing global no-reclaim state
while mutators continue.

## Phase publication and closure

The token plane closes queue failure and publisher preemption, but a full scan
alone does not close a publisher racing the end of that scan. The final phase
protocol must use the existing typed activation root gate.

The table-rescan descriptor in this note names a mutation which has already
happened. It must **not** simply be moved before the heap store. A helper could
otherwise install and scan the token, clear the descriptor, and let close
commit before the original owner resumes and performs the still-future store.
The table pointer alone does not describe enough of that future mutation for a
helper to complete it.

Final store admission therefore has two layers:

1. Before the heap store, the owner publishes the broader per-TG root-operation
   descriptor with the old/new key/value roots, affected ranges, parent table,
   and any store intent needed for conservative help. `LJGC2RootDesc` is the
   existing starting primitive at `src/lj_gc2token.h:362-654`; table-store
   coverage may require extending its payload/API.
2. The owner samples the exact activation gate after that root descriptor is
   ACTIVE. If it sees `CLOSING` or `COMMIT`, it changes the exact gate to
   `PENDING` before the first unaccounted store.
3. After the store and dirty-stamp update, while the root-operation descriptor
   still covers the operation, the owner publishes the table-rescan descriptor
   from this note and completes its descriptor-to-token handoff.
4. Only after the token handoff may the owner finish the root-operation
   descriptor.

A root-operation helper can conservatively trace the described key/value/range
even if the store has not happened yet. The table-rescan helper can safely scan
and clear its descriptor because that descriptor is not published until after
the store. If a store class cannot provide enough pre-store payload to make a
future store safe, it is not ready to participate in a reclaim-authorizing
`COMMIT`.

The closer performs:

```text
activation gate OPEN -> CLOSING
help/snapshot every ACTIVE root-operation descriptor
help every ACTIVE table descriptor
drain SSB, grey, and table-token work
complete token-plane pass with no PENDING/PINNED tokens
exact-recheck root/table descriptors and ordinary close predicates
activation gate CLOSING -> COMMIT
```

The locked operations give the required dichotomy on the supported x86-64
targets:

- root-operation descriptor publication orders before `CLOSING`, so the closer
  enumerates and conservatively helps its complete payload; or
- `CLOSING` orders before root-operation descriptor publication, so the
  publisher's post-publication gate load observes non-OPEN and changes the
  exact gate to `PENDING`, making `CLOSING -> COMMIT` fail.

An after-store table descriptor which appears late is covered by the still-live
root-operation descriptor and the same exact gate generation. Close must not
interpret a table-token pass independently of that broader descriptor pass.

The runtime currently keeps the typed root gate dormant as positive reclaim
authority. During migration, descriptor/token non-emptiness is an additional
legacy veto, and every existing phase/worker/SMR predicate remains required.
Do not make `COMMIT` a reclaim grant until all relevant interpreted, JIT, C API,
FFI callback, and helper table stores publish a sufficient root-operation
descriptor before their heap store and retain it through table-token handoff.

## Lifetime and ABA rules

1. An ACTIVE descriptor is a table-lifetime claim. Small-cell destruction,
   huge `FREEING`/delete, and arena unmap must either help it or refuse the
   destructive transition.
2. A non-NONE token is an allocation-lifetime claim. It vetoes small-cell reuse,
   huge mapping free/reallocation, and containing arena unmap.
3. Free paths must recheck descriptor/token after any tentative destructive
   CAS and abandon/cancel if a publisher won the reciprocal race. This remains
   necessary until activation `COMMIT` is the sole reclaim authority.
   Descriptor help reaches only preallocated side metadata and therefore does
   not depend on the table body being readable.
4. Add table-token emptiness to `arena_unmap_side_empty()`, which currently
   checks recovery, root, lifetime, and destructor planes only
   (`src/lj_arena.c:1573-1576`). The current unconditional sidecar free at
   `src/lj_arena.c:1579-1582` must be unreachable while any token is non-NONE.
5. Small sidecar generations persist across cell reuse. Table construction must
   never zero a reused cell's token generation.
6. A huge stamp may be reset only after descriptor/token NONE and every counted
   body reader is gone, immediately before the mapping becomes unreachable.
7. Queue duplicates do not carry generations. A paused scanner does: it retains
   exact `PENDING(g)` plus allocation admission through completion. Therefore an
   old scanner cannot survive mapping reuse and clear a new incarnation.
8. Descriptor and token generation saturation is sticky no-reclaim, not a wrap
   or a request drop.
9. `LJ_GC_NEEDSCAN` is never consulted for physical lifetime.

`GCAhdr.gc2_tabstamp` is currently freed without contributing to unmap-side
emptiness (`src/lj_arena.h:107-133`, `src/lj_arena.c:1573-1582`); this must be
fixed as part of the same migration, not deferred after token authority ships.

## Core invariants

- **Locator overlap:** from the first request publication until exact scan
  completion, at least one of ACTIVE descriptor or PENDING/PINNED token exists.
- **No opaque owner:** no table-rescan state requires the thread that installed
  it to resume. Helpers may repeat all intermediate operations.
- **Exact refresh:** every logical request changes token generation, even if a
  request is already pending.
- **Exact completion:** only the scanner's captured generation can be cleared,
  and only after its captured dirty epoch has a published scan proof.
- **Queue independence:** SSB, grey, and recovery entries are accelerators;
  losing or failing one cannot erase the token.
- **Lifetime coupling:** descriptor/token authority is mechanically included in
  every free, reuse, realloc, and unmap predicate.
- **Gate closure:** only exact activation `CLOSING -> COMMIT`, after descriptor
  and token closure, can eventually authorize destructive phase progress.
- **Non-wrapping authority:** saturation selects sticky no-reclaim.
- **Advisory header:** `LJ_GC_NEEDSCAN` may be stale in either direction without
  affecting safety or liveness.

## Migration order

1. Add dormant descriptor/token types, exact snapshot helpers, encoders, static
   size/alignment assertions, and saturation tests. Keep current behavior.
2. Preallocate small sidecars before arena registry publication; add huge-header
   scan/token fields; make unsupported allocator handling explicitly pin. Add
   unmap/free vetoes before any producer can publish a token.
3. Add bounded small and huge token scan lanes and joined-world preflight. Keep
   them dormant and test synthetic token states.
4. Replace `gc2_table_rescan_pending_begin/finish/clear` with
   descriptor-to-token publication. Retain `table_rescan_pending` only as a
   conservative compatibility veto during this step; never repair it by blind
   zeroing.
5. Convert table traversal/requeue to the exact PENDING-generation completion
   protocol. Make SSB/grey publication best effort and prove forced queue failure
   drains through the token lane.
6. Convert every MARK, WEAK, SWEEP bridge, fixpoint, and sweep-close predicate
   from scalar table count to descriptor/token closure. Only then remove
   `GCtab.gc2_rescan_state`, the four `LJ_TAB_RESCAN_*` states, and authoritative
   `table_rescan_pending` accessors (`src/lj_obj.h:721-784` and
   `src/lj_obj.h:4598-4619`).
7. Instrument table stores with a sufficient pre-store root-operation
   descriptor, retain it through the after-store table-descriptor/token handoff,
   and wire the existing activation root gate through
   `OPEN/CLOSING/PENDING/COMMIT`. Do not move the pointer-only table descriptor
   before the store. Legacy phase predicates remain additional vetoes.
8. After schedule tests cover every store class, make typed `COMMIT` a necessary
   reclaim condition and remove the corresponding legacy publication windows.
9. Measure contention and memory. If needed, shard the descriptor or move it
   per-TG, and evaluate the embedded-`GCtab` stamp alternative. These are layout
   optimizations, not protocol changes.
10. Run Linux native validation first for b1.2.0; add MinGW/Wine and
    osxcross/Darling artifact/runtime validation before claiming those targets.

## Deterministic test checklist

### Publication and helping

- Pause publisher immediately after descriptor `IDLE -> ACTIVE`, before token
  CAS. A second mutator or closer must install PENDING and clear the descriptor
  while the original thread remains paused.
- Pause after dirty-stamp mutation but before token refresh. Let an old scanner
  clear the old token; a helper must still install a new PENDING token from the
  ACTIVE descriptor.
- Have two publishers collide on the one descriptor with different tables. The
  loser helps the first and then publishes the second; both tables complete.
- Put the named table lifetime in `MUTATING` or `RECOVERY` after ACTIVE is
  published. A helper still reaches the pinned side metadata, installs PENDING,
  and clears ACTIVE without reading the table body.
- Force registry/mapping admission to return transient `RETRY`. ACTIVE remains
  exact, the reciprocal terminal path refuses unmap, and a later helper
  completes it without body access or spinning.
- Force an impossible terminal `STALE` classification while ACTIVE and verify
  sticky no-reclaim rather than descriptor clear.

### ABA and exact completion

- Capture ACTIVE `{table,g}`; help and clear it; republish the same table address
  at generation `g+1`. The stale exact clear must fail.
- Capture token `PENDING(n)` in a scanner; complete it, publish a new request,
  and restore identical dirty/cycle values. The stale `PENDING(n) -> NONE` CAS
  must fail against the newer generation.
- Pause scanner after scan-stamp CAS and before token clear. Refresh
  `PENDING(n) -> PENDING(n+1)`; the old clear fails and another scan consumes
  the refreshed request.
- Run two scanners on one PENDING generation. Exactly one clear wins; the loser
  performs no destructive cleanup.
- Exercise descriptor and token maximum generations. Both enter PINNED /
  activation NO_RECLAIM and never wrap.
- Exercise dirty-stamp and global-cycle maximum values. They widen or pin; an
  old scan proof must never become current through 32-bit wrap.

### Queue independence and admission

- Fill the active SSB, deny SSB rotation, force grey growth failure, and disable
  recovery publication. The table token scan lane still traverses the table and
  permits close.
- Pop a grey table and force small lifetime `MUTATING`/`RECOVERY`. The pointer
  may disappear, but PENDING remains; after lifetime restoration a bounded token
  pass completes it.
- Consume a stale SSB/grey duplicate with token NONE; it must not decrement any
  aggregate or clear a future request.
- Consume a stale duplicate while a newer request is PENDING; it may help the
  newer request and can clear only the generation it captures.
- Verify that a current-cycle scan stamp does not cause a forced PENDING retry
  to be discarded.

### Lifetime

- Pause with ACTIVE and attempt small table free, huge table free, huge realloc,
  cell reuse, and arena unmap. Every operation helps/vetoes; none destroys the
  table bytes.
- Repeat with PENDING during traversal and during transient admission.
- After token NONE, descriptor IDLE, and all readers released, prove the same
  frees and unmap complete normally.
- Reuse one small table cell repeatedly and deliver old queue duplicates after
  each reuse. No stale scanner clears a newer generation.
- Fail sidecar preallocation. The arena is not published for traversable table
  allocation, or activation becomes sticky no-reclaim; no ACTIVE descriptor is
  silently cleared.
- Joined terminal preflight counts/scans every ACTIVE/PENDING/PINNED identity
  before sidecar free and aborts on mismatch.

### Phase gate

- Root-operation descriptor publication before `OPEN -> CLOSING`: closer sees
  and conservatively helps its complete store payload.
- `OPEN -> CLOSING` before root-operation descriptor publication: publisher
  changes the gate to PENDING and exact `CLOSING -> COMMIT` fails.
- Publisher meets COMMIT: it changes COMMIT to PENDING/routes the request before
  its first unaccounted store.
- Pause after the root-operation descriptor and heap store but before the table
  descriptor. The closer's root help covers the values; after resume, the table
  descriptor/token is routed without invalidating the committed phase.
- Prove that a pointer-only table descriptor published before a delayed store is
  rejected by assertions/test API; helpers must never clear such an operation.
- Repeat through IDLE -> MARK -> WEAK -> SWEEP -> IDLE and across a complete
  same-mark-epoch minor cycle to prove activation-generation ABA resistance.
- Pause a closer after its empty token pass while a publisher races. It either
  sees ACTIVE/PENDING or loses its exact COMMIT CAS; there is no third outcome.

### Advisory state and stress

- Race header NEEDSCAN clear with a new token refresh. The clearer rechecks the
  token and restores the bit, while close remains correct even if the test hook
  suppresses that repair.
- Stress one hot table, many independent tables, descriptor collisions, queue
  duplicates, phase transitions, and repeated address reuse. Assert reachable
  children survive and close makes progress with any one publisher/scanner
  paused.
- Verify the token lane is bounded under a permanently transient cell and still
  reaches later registry cells.
- Compare table-heavy throughput and memory before/after; descriptor contention
  and 64-KiB sidecar cost are explicit b1.2 metrics, not release blockers unless
  they cause extreme slowdown.

### Target artifact checks

- GCC and Clang Linux builds emit inline `cmpxchg16b` for descriptor snapshots
  and transitions, with no `libatomic` call/import.
- MinGW and Darwin cross artifacts satisfy the same CX16/alignment contract
  before Wine/Darling runtime testing is enabled for the release line.

## Acceptance boundary

For b1.2.0, the acceptable checkpoint is that GC2 and JIT run without a lost
table rescan or a publisher-owned close stall, Linux tests pass, and performance
is not catastrophically degraded. Descriptor sharding, sparse side metadata,
and full cross-platform tuning can follow in b1.2.1.

This checkpoint does not by itself make typed activation COMMIT the sole reclaim
authority. That requires the broader pre-store root-operation descriptor,
retaining it through the after-store table-token handoff, and the root-gate
migration. Until then, all existing legacy reclaim vetoes remain in force.
