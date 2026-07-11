# GC2 arena epoch-markword design

Status: design only; no source in this note's audit was changed.  The audit was
made against `def23d1a269d` plus the uncommitted terminal-quarantine work in
progress on 2026-07-11.  The terminal gate is a dependency, not something this
design replaces.

## Problem and binding constraints

The current small-arena metadata gives one `mark[]` bit two unrelated jobs:

1. with `block=1`, it is GC liveness; and
2. with `block=0`, it is the structural start bit for a reusable free run.

At the same time, owner allocation, rebuilding, sweeping, tests, the VM, and
some JIT paths access `block[]` or `mark[]` with plain C or unlocked x86 RMWs,
while marker and late-rescue paths use atomic RMWs.  This is formal data-race
UB.  Replacing the plain expressions with atomic loads and stores is not enough:
an atomic load/modify/store of a whole word can still overwrite an unrelated
bit set by a concurrent marker.

The cycle boundary has a second independent bug.  A major cycle currently
publishes MARK and clears allocated `mark[]` bits before the barrier and
black-allocation handshake has reached every TG.  A birth mark, fixed-object
mark, or sweep rescue can therefore linearize before the destructive clear and
be erased by it.  Moving the clear after the handshake has the symmetric late
publisher problem, while doing locked `fetch_and` on every idle allocation
would put a locked instruction on the dominant allocation path.

The replacement must satisfy all of these constraints:

- every shared C access is through `lj_atomic`; no mixed plain/atomic word;
- no locked RMW, mutex, futex, or peer-dependent spin on ordinary white idle
  allocation;
- a mark in one cell cannot be lost by structural work on another cell;
- cycle start is O(1) in heap size and performs no destructive mark reset;
- major, minor, generational, black-birth, late-rescue, abort/restore, and
  reclaimed-arena adoption have explicit meanings;
- the owner remains the sole `block`/free-structure writer, with terminal
  mutation transferred only under the arena publication gate;
- x86-64 Linux, Darwin, and Windows use the same logical protocol; and
- the main-TG VM/JIT raw-memory exception is retained only in its proven
  single-observer idle window.

## Callsite audit

The source callsites fall into these groups.  This grouping is more useful than
line numbers because the terminal work was being edited during the audit.

| Area | Current role | Required conversion |
| --- | --- | --- |
| `lj_arena.c`: `arena_set_alloc`, extent/free-run helpers, bin validation, bump publication, free/drain/adopt | owner structural writes and tests | typed atomic `block`/`freehead` accessors; no liveness write for a white allocation |
| `lj_arena.c`: old sweep identities, free-run scan, clear-marks, quarantine readiness/apply, restore | bulk structural and liveness mutation | explicit keep/dead transform under SEALED; delete cycle-start clear |
| `lj_gc.c`: registered-body tests, old-generation detach, allocation-end scan, arena classifier/reclaimer | validation, mark tests, late rescue | acquired `block`, `block|freehead` boundaries, epoch-latched liveness |
| `lj_gc2.c`: registered-pointer/queue/stack validation | allocation membership | acquired `block` only |
| `lj_gc2.c`: `gc2_mark_small_cell`, raw/object marking, `ismarked`, paranoia | first mark, rescue, liveness test | epoch markword helpers plus terminal admission |
| `lj_tab.c`, `lj_func.c`, generic arena allocation | C allocation publication | birth helper: optional epoch mark, then release-publish `block` |
| `vm_x64.dasc` empty TNEW | main-TG single-observer allocation | idle-only `block` publication; no liveness clear; active black falls back initially |
| `lj_asm_x86.h` inline FNEW | traced single-observer allocation | same rule; regenerate `buildvm_arch.h` |
| unit tests and paranoia scanners | direct bitmap setup/read | pre-publication fixture initialization or typed atomic test helpers, never direct shared access |

`HugeTab` does not access the small-arena arrays, but its one-bit MARK clear has
the analogous generation problem.  A compatible follow-up is specified below;
the small-arena conversion must not be presented as proof that huge reset is
finished.

## New small-arena fields

Use three independent concepts instead of the differential two-bitmap alias:

```c
typedef struct LJArenaMarkWord {
  uint64_t bits;
  uint64_t epoch;
} LJ_ALIGN(16) LJArenaMarkWord;

struct GCArena {
  GCAhdr hdr;
  uint64_t block[LJ_ARENA_WORDS];       /* allocated block starts */
  uint64_t freehead[LJ_ARENA_WORDS];    /* free block/run starts */
  LJArenaMarkWord live[LJ_ARENA_WORDS]; /* 64 cells + 64-bit major epoch */
  uint64_t sweep[LJ_ARENA_SWEEP_WORDS];
  uint64_t late[LJ_ARENA_WORDS];
};
```

`LJArenaMarkWord` is layout-compatible with `la_u128` and is always 16-byte
aligned.  Its full `{bits, epoch}` pair is changed only with `la_cas128`.
Atomic loads of its aligned halves are allowed for a stable read, matching the
already-used HugeTab pair-snapshot convention.  No 64-bit store is allowed to
either half after arena publication.

`GCAhdr` also gains a 64-bit `sweep_mark_epoch`, taken from existing padding.
It is immutable from NEEDSWEEP publication through terminal commit or restore.
The global GC2 state gains a monotonically increasing, nonzero 64-bit epoch
counter and an immutable `cycle_mark_epoch`.  The phase machine gains a short
nonblocking `MARK_PREP` publication state so the next epoch and empty work
counters are never exposed as an IDLE generation.  A TG allocator replaces the
boolean color as the authoritative field with `alloc_mark_epoch` (`0` means
white allocation, otherwise it is the exact birth epoch).  A compatibility
`alloc_black` mirror may exist briefly for generated-code migration, but cannot
decide liveness after the conversion.

The structural state is now:

| `block` | `freehead` | meaning |
| --- | --- | --- |
| 0 | 0 | extent / never allocated cell |
| 0 | 1 | free block or run start |
| 1 | 0 | allocated start |
| 1 | 1 | invalid |

For an allocated start, white/black is determined separately by
`arena_mark_test(a, cell, E)`.  `block & freehead` must always be zero.  The
start mask used by size reconstruction and free-run scanning is
`block | freehead`.

This deliberately diverges from the differential identities in
`plan/04_allocator.md`.  The original identities are compact, but their
structural/liveness alias forces allocation to mutate a concurrently marked
word.  The explicit fields preserve the same heap states while making the
ownership proof local to each field.

## Epoch markword operations

Let `M = 1ull << (cell & 63)` and `W = &a->live[cell >> 6]`.  Logical liveness
for epoch `E` is:

```text
W.epoch == E && (W.bits & M) != 0
```

An epoch mismatch denotes an all-zero liveness word without changing memory.
A major start increments the global epoch.  A minor start reuses the current
epoch so the prior major and prior minor survivors remain the old set.

The first-mark CAS loop is conceptually:

```c
for (;;) {
  old = stable_atomic_snapshot(W);
  if (old.epoch > E)
    return RETRY_WITH_NEW_GLOBAL_EPOCH;  /* Never regress a word. */
  if (old.epoch == E && (old.bits & M))
    return ALREADY_MARKED;
  next.epoch = E;
  next.bits = old.epoch == E ? old.bits | M : M;
  if (la_cas128((la_u128 *)W, &old, next))
    return FIRST_MARK;
}
```

The caller acquires a stable phase/epoch snapshot and retries if the word
reports a later epoch.  A normal MARK/WEAK mark or barrier also reloads the
phase/epoch after the CAS or duplicate result and repeats before returning if
it changed.  This closes a barrier operation which began just before a major
flip but publishes its heap edge just after it.  Such an operation is still
covered by the cycle's TG handshakes while it is between the CAS and final
epoch recheck; it cannot acknowledge a later root/fixpoint cut halfway through
the barrier.  Birth uses the owning TG's exact handshake token, and sweep
rescue uses the arena's immutable latched epoch instead of this global retry
loop.

A direct idle mark which began before `MARK_PREP` can still publish an
old-epoch pair, but it cannot overwrite a pair from the new epoch.  If it
observes the newer pair it is forbidden to write the older epoch.  During
`MARK_PREP`, traversable-object marks and store-barrier targets are published
to pending/SSB work rather than setting a not-yet-active liveness bit.  This is
the property a separate tag store plus `fetch_or` cannot provide: the tag and
bits have one CAS linearization point.

`mark_clear(E, mask)` also uses a full-pair CAS.  It only clears bits when the
word epoch equals `E`, preserves every unrelated bit, and never changes the
epoch.  It is used at an actual free/terminal LP, not at allocation.

A read uses `epoch/bits/epoch` acquire snapshots and retries when the two epoch
loads differ.  Within one epoch bits may be added concurrently; a stale false
causes an ordinary retry/mark CAS, and a stale true in a liveness query is
conservative.  Terminal classification performs the read only under the arena
gate, where the result is stable.

The epoch is 64 bit and must not wrap.  Increment checks `next != 0`; reaching
the guard is a fatal implementation invariant rather than silently reusing an
epoch.  This avoids a 32-bit wrap/rebase protocol that torture mode could
eventually exercise.

### Why not a separate epoch array

The tempting layout of 64-bit marks plus a separate 32/64-bit word epoch is
incorrect.  An initializer can publish the new tag and be preempted before
clearing bits, or clear bits after another marker has initialized and marked
the word.  A stale marker can also OR a bit after a new tag is visible, making a
current bit whose object was never queued for current traversal.  Full-pair CAS
is the simple bounded solution on the mandatory `cmpxchg16b` targets.

## Atomic access and ownership rules

After arena registration, all C reads and writes of `block`, `freehead`,
`live`, `sweep`, and `late` use atomic helpers.  Being under SEALED does not
license a plain C access; it only proves that an atomic owner store cannot lose
a concurrent writer.

The public helper surface should be typed and narrow:

- `arena_block_test_acq(a, cell)`;
- `arena_block_word_acq(a, word)` and owner-only `..._store_rel`;
- `arena_freehead_test_acq` and owner-only word store;
- `arena_mark_test(a, cell, epoch)`;
- `arena_mark_set(a, cell, epoch)`;
- `arena_mark_clear_mask(a, word, epoch, mask)`; and
- pre-publication fixture/map initialization helpers.

Delete the generic `lj_arena_bm_get/set/clear(uint64_t *)` API.  It erases
whether a caller is touching membership, structure, or liveness and makes a
future plain regression easy.

`block` and `freehead` each have one writer: the owning TG while the arena is
OPEN/owned, or the terminal owner while SEALED.  Markers, rescues, and remote
free publishers never write either array.  Therefore an owner bit update can
be an atomic load followed by an atomic release store of the whole 64-bit word;
on x86-64 both are MOVs, not locked RMWs.  This is functionally safe only
because writer transfer is enforced by the arena gate and RESET_ALLOC
handshake.  A code-review assertion should reject structural stores when the
arena is neither locally owned/open nor terminally SEALED.

Marker/rescue threads write only `live` through full-pair CAS.  Remote frees
write only their gate/queue/late sidecars until an owner consumes them.

## Allocation and free transitions

### White idle allocation

The reusable-space invariant is:

```text
Every cell offered by a bump window or free bin has no current-epoch live bit.
```

The free/sweep path establishes the invariant.  Allocation does not clear a
mark.  It removes/adjusts `freehead` as owner-local structural work and
release-publishes the `block` bit.  The common bump path is consequently:

1. reserve cells from the owner-local bump cursor;
2. load `alloc_mark_epoch` (one ordinary atomic MOV, normally zero);
3. if zero, release-publish `block`; and
4. return the memory / complete the typed publication protocol.

There is no locked instruction and no liveness write in the normal idle path.
Compared with the current path it also removes the mark-bit clear/BTR.

### Black allocation

If `alloc_mark_epoch=E`, the allocation is born black:

1. reserve the structurally free cells;
2. initialize every header field which a concurrent validator/traverser may
   inspect (typed constructors should use reserve-then-commit);
3. `arena_mark_set(a, start, E)` without requiring `block` to be visible;
4. release-publish `block`; and
5. publish the exact object/header ownership edge and constructor barriers.

The order is mark-before-block.  An acquired block observation can therefore
never see an active black birth without its liveness.  Birth marking does not
increment `marks_this_round` and does not enqueue grey work; the existing
black-constructor publication barriers remain mandatory for every outgoing
edge.  Raw/nontraversable memory needs no body traversal.

An allocator operation cannot poll halfway through this sequence.  If its TG
still carries the previous epoch while a major handshake is approaching, it
may stamp the old epoch; the object is published before that TG acknowledges,
and the post-ack root/pending scan treats it as an ordinary white birth in the
new epoch.  Once the TG acknowledges ALLOC_BLACK, its local token is the exact
new epoch.

### Free and reuse

A cell becomes reusable only after the appropriate lifecycle/SMR/grace LP has
made new valid marks impossible for that incarnation.  The owner then:

1. clears its live bit for the latched/current epoch with full-pair CAS;
2. release-clears `block`;
3. release-publishes a `freehead` bit; and
4. publishes the owner-local bin/bump metadata.

Ordinary explicit/raw free may pay the CAS; allocation is the performance-
critical side.  A remote free never performs these steps directly.  During
sweep, dead starts are transformed in word batches under SEALED.

The allocator must not use a liveness bit as an allocation-incarnation tag.
Stale-pointer ABA safety remains the responsibility of allocation validation,
the terminal state, and the existing SMR grace.  A mark bit alone never
authorizes dereferencing an object header.

## Cycle protocol

### Major / incremental cycle

At IDLE, after the prior sweep has closed, the leader:

1. CAS-publishes `IDLE -> MARK_PREP` before changing any epoch-visible field;
2. resets `marks_this_round`, grey/weak cursors, and the new-cycle work state;
3. selects major and release-publishes the next nonzero epoch as
   `cycle_mark_epoch`;
4. release-publishes MARK, which is the activation LP for that epoch;
5. runs the existing barrier/ALLOC_BLACK/root-exit handshake; and
6. drains PREP publications, starts root scan, and begins fixpoint work.

There is no arena walk and no clear.  A word is logically empty until its first
CAS into the new epoch.  Marks racing the boundary are monotonic: an old-epoch
CAS cannot regress a word which already carries the new epoch.

`MARK_PREP` never makes a mutator wait.  Allocations remain white and use the
normal exact pending-object publication.  A traversable object or store target
encountered during PREP goes to an existing SSB/pending-root carrier (or a
small dedicated MPSC premark carrier) without setting the new epoch bit; raw
side memory is retained through its published owner root.  MARK drains those
publications after the handshake.  This state is required: publishing a new
epoch while phase still reads IDLE would let a liveness-only fixed/direct mark
set the new bit without grey work, after which root scan could mistake it for
already traversed.  Likewise, resetting `marks_this_round` after MARK is
visible can erase a real first-mark event.

The MARK-before-ALLOC_BLACK handshake window remains covered by the root and
pending snapshot, as it is today, but it no longer overlaps a destructive
bitmap reset or uninitialized fixpoint counters.

### Generational baseline and minor cycles

A forced/baseline major allocates a new epoch and marks the full reachable set.
If generational mode remains enabled, terminal sweep preserves current-epoch
bits for survivors.  Those bits are the old-generation set.

A minor cycle does **not** increment `mark_epoch`.  Old objects remain marked;
young cells born after the preceding sweep have no bit.  SSB/remembered-parent
processing traverses old parents directly, while first marks of reachable young
objects set the current bit and enqueue them.  Minor terminal sweep preserves
all survivor bits, promoting young survivors into the old set.

This means a GC cycle counter and a liveness epoch are different fields.  Do
not reuse `gc2.cycle` as the markword epoch, and reconcile the custom allocator
registry design accordingly: its liveness epoch must remain stable across a
minor series, while any per-cycle scan stamp remains a separate counter.

Turning generational mode off, requesting a full collection, or forcing a
major allocates a new mark epoch.  Turning it on forces the documented major
baseline before any minor.

An IDLE generational write barrier queues remembered work and does not directly
mark a traversable child.  Similarly, an IDLE direct mark of a traversable
object must publish pending/SSB traversal work; only leaf/nontraversable or raw
ownership metadata may be made liveness-only.  Otherwise an idle mark could
promote a young object without ever scanning its children in the next minor.

### Allocation colors by phase

| State | `alloc_mark_epoch` |
| --- | --- |
| IDLE incremental | 0 |
| IDLE generational | 0 (young) |
| MARK / WEAK | current cycle epoch |
| major SWEEP | current cycle epoch, preserving post-snapshot births |
| minor SWEEP | 0; post-snapshot births are young and not in detached arenas |

Attach/catch-up and safepoint actions must publish this exact token, not infer
it later from a boolean and a possibly changed global epoch.

## Sweep, terminal rescue, and restore

### Prepare and classify

RESET_ALLOC/prepare latches `sweep_mark_epoch=cycle_mark_epoch` in every
detached arena before NEEDSWEEP publication.  Structural scans use acquired
`block|freehead`; liveness tests use only the latched epoch.  An epoch mismatch
is unmarked.

For a stable structural word, let `B` be allocated starts, `F` free starts,
and `K` the starts terminal classification keeps.  The old differential sweep
becomes explicit:

```text
dead      = B & ~K
block'    = K
freehead' = F | dead
```

Extent cells remain zero in both structural planes.  Quarantine side states,
late pins, finalizers, and exact detached headers still decide `K`; a markword
sample by itself does not bypass those protocols.

For non-generational major sweep, terminal commit clears current-epoch bits of
surviving and dead starts (or clears the whole current word mask in one CAS),
returning survivors to white.  A later idle fixed/root mark may set one again,
which is conservative.  For a generational major or any minor, it keeps bits
for `K` and clears bits for dead starts.  In every mode reusable starts are
unmarked before bin publication.

### Late rescue

The arena terminal gate remains authoritative:

1. a rescue enters/counts or publishes the gate's sticky pending intent;
2. it validates acquired `block` and the sweep incarnation;
3. it CAS-marks `live` with `sweep_mark_epoch`;
4. if necessary it changes `RETIRED -> LIVE` and repairs deferred accounting;
5. it release-publishes grey/ownership work; and
6. it leaves the gate and wakes the sweep owner.

A SEALED bit-only rescue may CAS the epoch markword only if it first dirties the
exact gate word in a way that defeats terminal commit.  Terminal finish must
reconcile pending, rescan, and win the clean exact gate CAS before applying
`B/F`.  A post-commit rescue reads stable terminal state and cannot turn a
FREEING/dead block into a live allocation.

This ordering closes both reset races: there is no clear at cycle start, and a
terminal clear cannot pass an admitted/pending rescue.

### Restore / abort

Restore seals the arena before reading headers or changing structural words.
It does not synthesize marks and does not run a reverse sweep identity:

- allocations whose destructor did not complete retain `block` and their
  existing epoch mark;
- a completed FREEING cell has its liveness cleared, `block` cleared, and
  `freehead` published;
- detached LIVE headers are reanchored before their side state is reset;
- the free bins are rebuilt from `block|freehead`; and
- `sweep_mark_epoch` is cleared only after OPEN/owned publication.

Reclaimed adoption uses the same SEALED discipline.  It consumes only late
pins assigned to the next sweep generation; current committed `block`,
`freehead`, and preserved liveness are immutable until the exact OPEN CAS.

## VM and JIT fast paths

The x64 interpreter empty-TNEW and traced FNEW bump paths may keep an unlocked
memory BTS/owner store for `block` only when all of these are proven in emitted
code before reservation:

- main TG and its private bump arena;
- `mt_active == 0` and `mt_entering == 0`;
- no GC2 worker/assist can observe allocator state;
- no pending safepoint/RESET_ALLOC transition can detach the arena;
- `alloc_mark_epoch == 0`; and
- the arena is OPEN/owned.

They perform no `live` operation.  This is the single-observer exception from
the P0 audit; it is not a general permission for unlocked bitmap assembly.
Initially, active-black VM/JIT allocation should branch to the C commit helper
before reserving cells.  A later measured optimization can emit the epoch
markword CAS and mark-before-block order.  Generated `host/buildvm_arch.h` must
be regenerated, and artifact tests must reject BTR/mark writes on the idle
path.

All three supported OSes are x86-64 TSO, so release/acquire 64-bit structural
accesses compile to MOV.  First marks use `lock cmpxchg16b`.  The project already
requires CX16 for HugeTab; make the inline implementation available to both GCC
and Clang rather than allowing an out-of-line/libatomic lock fallback.
MinGW/Clang Windows builds may use GNU inline `cmpxchg16b` or
`_InterlockedCompareExchange128`; the 16-byte alignment and expected-value
failure semantics must be identical.  Darwin Clang must be artifact-checked for
an inline instruction as well.  Wine and Darling tests are runtime gates, not
substitutes for inspecting the generated instruction.

## Huge allocation counterpart

`HugeTab` has no mixed plain/atomic bitmap access, but its destructive
`LJ_HUGEF_MARK` scan has the same birth/reset generation hazard.  Do not keep
calling `lj_arena_hugetab_clear_marks()` after MARK publication.

The clean long-term representation is an immutable, SMR-retired Huge record
referenced from each open-addressed slot:

```c
typedef struct LJHugeRec {
  void *base;                 /* immutable */
  size_t size;                /* immutable */
  la_u128 life;               /* low: lifecycle flags, high: mark_epoch */
} LJHugeRec;
```

The slot release-publishes a record pointer; deletion tombstones the slot and
retires the record only after the mapping lifecycle and an SMR grace.  A full
CAS of `life` sets the current 64-bit epoch and clears RETIRED, or rejects
FREEING.  Insert initializes a black birth epoch before publishing the record.
Sweep tests the latched epoch and clears/preserves it with the lifecycle CAS.
Major start then has no HugeTab scan either.  This also avoids packing an
eventually wrapping generation into the size/flag word.

If the small-arena P0 lands first, the temporary HugeTab rule is: perform its
CAS clear during `MARK_PREP`, before publishing MARK, route PREP marks through
durable pending ownership, and prove that the following root/pending handshake
covers every pre-cut birth/fixed mark.  Keep this explicitly documented as
temporary; GC2 generation-reset completion requires the epoch-record conversion
or an equivalently proved non-destructive scheme.

## Migration sequence

1. Add the new metadata and typed accessors, update static layout assertions,
   and convert the standalone bitmap model to `block/freehead/live` shadow
   states.  No runtime reader should use the new plane yet.
2. Add 64-bit global/cycle/sweep/TG epoch fields and helpers.  Initialize epoch
   to one, publish exact alloc tokens in safepoint and attach catch-up paths.
3. Convert mark/test/paranoia and terminal rescue to markword CAS while still
   retaining debug comparison against the old mark bit in serialized tests.
4. Convert structural allocation/free/bin/scan code to `block/freehead`, with
   mark-before-block black birth and free-before-bin cleanup.  Convert typed
   table/function commit paths.
5. Replace sweep/quarantine/apply/restore identities with explicit `K/dead`
   transforms and latch `sweep_mark_epoch`.
6. Delete small-arena cycle-start clear, the old mark bitmap, differential
   state helpers, and every direct/shared array access.  Update
   `src/lj_mtfields.md`.
7. Convert VM/JIT fast paths and regenerate buildvm artifacts.  Keep active
   black on the C fallback until its own artifact/model gate passes.
8. Convert HugeTab liveness to an epoch-bearing record, then remove the final
   clear-at-start compatibility path.

A dual-write bridge is acceptable only in assertion builds and only when all
participating threads use atomic operations.  Do not ship a transition where
the old plain word remains authoritative beside the new plane.

## Required models and tests

### Exhaustive C11 markword model

Enumerate atomic LP interleavings for:

- delayed epoch-E marker, major E+1 publication, and E+1 marker;
- a store barrier spanning the global epoch flip and its final epoch recheck;
- a direct/fixed mark during `MARK_PREP`, plus MARK activation and drain;
- two first markers in the same word and in the same cell;
- current mark versus exact-cell clear/free;
- unrelated-cell clear versus mark in the same word;
- black birth mark versus block publication;
- late rescue versus seal/readiness/final commit; and
- stale old-epoch CAS observing a future word.

The invariants are:

- no E+1 bit is lost by an E operation;
- a word epoch never decreases;
- at most one same-epoch first mark is reported per clear-free incarnation;
- an acquired black `block` publication has its epoch bit;
- no reusable/bin cell has a current bit; and
- terminal commit cannot free an admitted or pending rescue.

Broken variants must all produce counterexamples: separate tag and bits,
plain/atomic reset, tag-before-clear, clear-after-publish, stale epoch allowed to
regress, allocation-time whole-word clear, block-before-black-mark, and
terminal mark without gate pending.  Omitting the marker's final global epoch
recheck must also produce the spanning-barrier counterexample.

### Structural/random model

Replace the old plan bitmap model with a shadow allocator that runs randomized
sequences of:

- fresh/bump/bin allocation and split/coalesce;
- incremental major;
- generational major followed by several minors;
- dead-young reclamation and survivor promotion;
- forced major after minors;
- remote FREEING, late pin, terminal keep/dead conversion;
- restore before and after destructor completion; and
- reclaimed adoption.

Check `block&freehead==0`, exact start/extent reconstruction, free-run coverage,
no overlap, liveness by latched epoch, and reusable-space cleanliness after
every operation.

### Deterministic runtime fixtures

Add pause hooks at the LPs and cover at least:

1. old marker paused before CAS; new epoch/current mark wins; old resumes;
2. major epoch publication before a TG's ALLOC_BLACK ack, with a white birth
   recovered by pending/root scan;
3. PREP fixed/raw/traversable publications, including a counter-reset pause;
4. black allocations in MARK, WEAK, and major SWEEP, plus white allocation in
   IDLE and minor SWEEP;
5. same-word unrelated marker versus free/restore;
6. RETIRED rescue immediately before and after terminal seal/readiness;
7. restore with LIVE, RETIRED, FREEING, raw WHITE, and late-pinned cells;
8. baseline major, multiple true minors, mode toggle, and forced major;
9. fresh and partially consumed bump windows surviving several epoch changes;
10. C, interpreter TNEW, and traced FNEW births; and
11. Huge birth/fixed/rescue at its temporary clear cut and final epoch-record
    protocol.

### Audit and build gates

- A source audit must find no direct `block[]`, `freehead[]`, or markword half
  access outside their implementation/initialization allowlist, and no old
  `lj_arena_bm_*` helper.
- TSAN C fixtures exercise marker/free/sweep interleavings; ASAN/UBSan and the
  GC2 paranoia oracle run with JIT both off and on.
- Run the arena/GC2 focused suites, stock suite, exact paranoia suite, repeated
  fresh-process GC stress, and allocator/JIT stress.
- Build and run x86-64 Linux; cross-build/run Windows under Wine; build/run the
  Darwin target under Darling.  Assert 16-byte markword alignment at runtime.
- Disassemble all three targets: idle allocation has no `lock`, no markword
  write, and one structural publication; first-mark paths contain inline CX16,
  not a mutex/libatomic call.

## Metadata and hot-path cost

Current WIP `GCArena` metadata is 2,688 bytes (`LJ_AFIRST_CELL=168`).  Reusing
the old 512-byte `mark[]` storage as `freehead[]` and adding 1,024 bytes of
epoch markwords makes it 3,712 bytes (`LJ_AFIRST_CELL=232`) if the other WIP
sidecars remain unchanged.  That is 64 fewer 16-byte payload cells per arena,
about 1.56 percentage points of arena capacity (4.10% metadata to 5.66%, or a
1.63% reduction in currently usable cells).

Expected instruction cost:

- idle bump allocation: one local epoch MOV, structural cursor work, and
  atomic MOV load/store for `block`; no locked RMW and one fewer mark mutation
  than today;
- idle VM TNEW/FNEW specialization: remove mark BTR and retain one gated block
  publication;
- first mark/current black birth: one contended-at-most-by-markers
  `cmpxchg16b` loop, followed by existing grey/ownership publication;
- duplicate mark: stable atomic loads can return without any locked RMW, an
  improvement over unconditional fetch-OR/BTS; and
- terminal sweep: one markword CAS per changed 64-cell word, not per cell,
  while SEALED.

Benchmark before considering the design performance-complete: single-thread
`alloc_tables`, empty TNEW, zero/one-upvalue FNEW, raw buffer allocation,
marker throughput at 1/2/N workers, duplicate-root-heavy fixpoints, minor-cycle
throughput, resident heap capacity, and full stock comparison.  Record both
wall time and locked-instruction counts.  If CX16 first-mark cost is material,
optimize only after preserving the full-pair generation invariant; a separate
tag plus fetch-OR is not an acceptable shortcut.

## Completion conditions for this P0

This design slice is complete only when:

1. every small-arena structural/liveness callsite uses the typed atomic
   protocol or the explicitly gated single-observer x64 exception;
2. no cycle-start small-arena mark clear exists;
3. idle allocation artifacts contain no locked RMW;
4. major/minor/restore/late-rescue models and deterministic races pass;
5. all three target OS builds pass their runtime and artifact gates; and
6. HugeTab either carries an epoch-safe liveness record or remains explicitly
   tracked as a separate temporary reset P0.

It does not by itself prove body SMR, terminal helpability, raw allocation
ownership, or canonical-string reclamation.  Those protocols remain required;
the markword is liveness metadata, not a substitute for allocation-incarnation
or header-dereference authority.
