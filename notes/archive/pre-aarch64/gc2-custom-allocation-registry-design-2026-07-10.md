# GC2 exact custom-allocation registry and allocator-transition design

Date: 2026-07-10

Status: implementation design for the next P0 GC2 slice. This note does not
change `plan/`. It documents a necessary divergence from `plan/04_allocator.md`
section 4.9: the public `lua_Alloc` contract is preserved for ordinary LuaJIT
allocations instead of reducing a custom allocator to an arena-page hook.

## Why this is a correctness requirement

The current code confuses two different facts:

1. which allocator configuration new allocations should call; and
2. which lifetime domain owns an existing pointer.

`global_State.allocf_arena` only describes the first fact. It cannot classify
an old pointer after `lua_setallocf()`. It also cannot describe a wrapper which
is currently a custom `lua_Alloc`, but delegates to the previous internal arena
allocator and therefore returns an arena-backed pointer.

This causes both known failures:

- The extended `t-ffi-ccall-error-state.c` wrapper test installs an allocator
  which delegates to the old arena allocator. Setting `allocf_arena = 0` makes
  pre-existing and newly wrapped arena blocks look like untracked custom
  blocks. GC2 then stops setting their arena marks and loses graph traversal.
- A `LUAJIT_USE_SYSMALLOC` state accepts every plausible pointer as
  `GC2_OBJMEM_CUSTOM`, but `lj_gc2_markmem()` has no custom mark identity and
  returns zero. `lj_gc2_markobj()` consequently does not queue traversable
  objects. The sweep root cursor later calls `lj_arena_of()` on the custom
  `GG_State`/main-thread address and dereferences an unmapped 64 KiB-aligned
  pseudo-header.

The latter was reproduced at commit `4c323ec3` with:

```sh
make -j2 \
  XCFLAGS='-DLUAJIT_USE_SYSMALLOC -DLUA_USE_APICHECK' \
  CCDEBUG='-g3' CCOPT='-O0 -fno-omit-frame-pointer'
./src/luajit -joff -e \
  'local keep={}; for i=1,500 do local t={i,i+1,i+2}; if i%100==0 then keep[#keep+1]=t end end'
```

The invalid-read stack is:

```text
lj_arena_ishuge
  <- gc2_sweep_obj_old_generation
  <- lj_gc_sweep_gc2_unmarked
  <- lj_gc2_sweep_prepare_bridge_boundary
  <- lj_gc2_step_explicit
  <- gc2_step_auto
  <- gc_step_assist_top
  <- lj_gc_step_fixtop
  <- BC_TNEW
```

An assertion build can stop earlier because the main state cannot acquire a
GC2 mark. Merely guarding the bad arena dereference is therefore insufficient:
custom objects need an exact mark epoch, traversal identity, sweep ownership,
and delayed destructor path.

## Required invariants

The implementation should make the following executable invariants.

1. Pointer ownership never depends on the current allocator configuration.
2. No arena header is dereferenced until the global small-arena directory or a
   HugeTab has proved that the mapping is an arena mapping.
3. Every allocation returned through a non-direct-arena `lua_Alloc` call has
   one exact registry record before the pointer can be published to Lua.
4. Every external record retains the exact `(lua_Alloc, ud)` pair which created
   it. Later `lua_setallocf()` calls affect only new allocations. Realloc and
   free use the record's owner pair, including after multiple allocator
   transitions.
5. A custom allocator which delegates to `lj_arena_allocf` produces a custom
   callback-owned record with arena-backed storage. The record and arena mark
   identities are both updated; only one destructor may win.
6. A first mark in the current GC2 epoch returns true and queues a traversable
   object exactly as an arena mark does. “Custom” must never mean “implicitly
   live” or “valid without proof.”
7. An object record can move `RETIRED -> LIVE` while it is still rescuable.
   `FREEING` is the post-grace destructor linearization point and cannot be
   resurrected.
8. No arbitrary `lua_Alloc` callback runs from a record that has already been
   reused, and no registry node is reused before both index unlink and a GC2
   SMR grace.
9. The `GG_State` allocation is a bootstrap exception. It retains its boot
   owner pair separately and is released by that owner, never by whatever
   allocator happens to be current at `lua_close()`.
10. The normal internal-arena fast path pays no custom-registry record cost.
    A state which has never called `lua_setallocf()` keeps the existing VM/JIT
    arena gates.

## Concrete data model

The names below are proposed names; field widths and state transitions are the
important part.

```c
typedef struct LJAllocOwner {
  lua_Alloc f;
  void *ud;
} LJAllocOwner;

typedef enum LJAllocRecState {
  LJ_AR_LIVE = 0,
  LJ_AR_MOVING,       /* synchronous realloc owns the old record */
  LJ_AR_RETIRED,      /* detached, but sweep publication may rescue it */
  LJ_AR_FREEING,      /* post-grace destructor/free owns it */
  LJ_AR_DEAD
} LJAllocRecState;

typedef enum LJAllocRecKind {
  LJ_ARK_RAW = 0,     /* explicit-lifetime vector/buffer */
  LJ_ARK_PENDING_GCO, /* allocated as a GC body, header not published yet */
  LJ_ARK_GCO,         /* exact GCobj header and expected gct are published */
  LJ_ARK_STRING       /* exact interned string; string buckets own unlink */
} LJAllocRecKind;

typedef enum LJAllocBacking {
  LJ_ARB_EXTERNAL = 0,
  LJ_ARB_ARENA_SMALL,
  LJ_ARB_ARENA_HUGE
} LJAllocBacking;

typedef struct LJAllocRec {
  void *base;
  size_t size;
  GCobj *obj;              /* base for normal objects; interior for VLA cdata */
  struct LJAllocRec *base_next;
  struct LJAllocRec *obj_next; /* used only when obj != base */
  LJAllocOwner owner;
  uint64_t alloc_serial;
  uint64_t mark_epoch;
  uint64_t retire_hs_epoch;
  uint32_t state;
  uint16_t obj_offset;
  uint8_t kind;
  uint8_t backing;
  uint8_t expected_gct;
  uint8_t flags;           /* traversable/fixed/finalizer/old-generation */
  uint16_t reserved;
  uint64_t aux;            /* e.g. custom-table dirty/scan stamp */
} LJAllocRec;
```

The final layout should be packed and benchmarked, but not at the expense of
removing the explicit owner pair or epochs. A 64--80 byte record is acceptable
for the first correctness slice; a later size-classed record layout can make
raw records smaller than traversable records.

`GC2State` needs:

```c
typedef struct GC2AllocRegistry {
  LJAllocRec **base_bucket;       /* atomic bucket heads */
  LJAllocRec **obj_bucket;        /* small variable-header index */
  uint32_t base_mask;
  uint32_t obj_mask;
  uint64_t hash_seed;
  uint64_t alloc_serial;
  uint64_t mark_epoch;
  uint64_t sweep_cutoff;
  uint32_t sweep_bucket;
  void *meta_slab_head;
  la_u128 free_records;           /* tagged post-SMR recycle stack */
  uint64_t live_records;
  uint64_t live_external_bytes;
} GC2AllocRegistry;
```

The base directory is a keyed chained hash. It has no correctness capacity
limit: growth only shortens chains. The initial slice can use a fixed directory
and physically unlink dead records. The performance follow-up can publish a
larger immutable directory generation, dual-publish writers during migration,
and retire the old index nodes through the existing SMR epoch. Variable-offset
cdata is rare, so `obj_bucket` can start much smaller than `base_bucket`.

Registry metadata must not recurse through the public allocator. Allocate
metadata slabs directly with the existing lower-47-bit OS mapping machinery,
use an atomic bump/chunk reservation, and recycle records only after SMR. A
metadata allocation failure after a user allocation succeeds must call the
captured owner pair to roll the user block back, then report the ordinary Lua
OOM. This keeps `lua_Alloc` sizes and returned pointers ABI-compatible; adding
a hidden prefix to user allocations would not.

## Atomic allocator configuration

`g->allocf` and `g->allocd` are currently a tearable tuple. The authoritative
configuration should be a 16-byte aligned pair loaded and replaced atomically
with `cmpxchg16b` on the supported x86-64 targets:

```c
typedef union LJAllocPair {
  LJAllocOwner owner;
  la_u128 bits;
} LJAllocPair;

LJ_ALIGN(16) la_u128 alloc_pair;
```

An atomic no-op compare/exchange supplies a consistent pair snapshot. A
`lua_setallocf()` CAS loop is its linearization point, so concurrent/racy
setters still produce a real total order and the API remains allocation-free
despite its `void` return type. Each new external record copies the pair; it
does not point at mutable configuration storage.

The existing `allocf`/`allocd` fields may remain temporary mirrors for buildvm
offset compatibility, but C ownership decisions and `lua_getallocf()` must stop
reading them. On the first `lua_setallocf()` call, release-clear the VM/JIT
direct-arena fast gate permanently for that state before replacing the pair.
This conservative rule avoids a torn fast-path re-enable under concurrent
setters and leaves ordinary LuaJIT performance unchanged for the overwhelmingly
common never-switched state. A later optimized generation-aware gate may
re-enable the fast path after benchmarks prove it worthwhile.

`lua_newstate()` also stores a separate immutable boot owner pair and boot
backing kind for `GG_State`. Partial initialization and normal close both use
that pair. In particular, close must not test the current `g->allocf` to decide
between `lj_alloc_destroy()`, arena unmap, and a custom callback.

Holding an old `ud` until its last allocation is reclaimed is unavoidable if
incompatible allocator transitions are to be safe. This is a stronger safety
contract than stock LuaJIT's implicit requirement that the newly installed
allocator understand every old pointer. It does not change the public ABI.

## Exact pointer classification

Classification order is deliberately independent of the active allocator:

```text
1. Known embedded objects (main state, strempty, nil node) by exact address.
2. Custom registry object/base index, under an SMR read section.
3. Shared small-arena directory lookup by aligned address, without dereference.
4. Per-TG/global HugeTab exact or range lookup.
5. Invalid/unregistered.
```

If a custom record is found first and its backing is arena storage, it remains
callback-owned by the record but is also mark-owned by the proved arena. This
is the wrapper-around-arena case. No coincidental external allocation can
overlap a live arena mapping, so a directory hit is authoritative.

The following current shortcuts must be removed:

- `allocf_arena == 0 => memory is registered`;
- `g->allocf != lj_arena_allocf => object/stack is valid`;
- `g->allocf != lj_arena_allocf => size fits`;
- deriving and dereferencing `lj_arena_of(p)` before a registry/directory hit.

`gc2_size_fits_mem()` becomes an exact record-size check for external records,
an arena extent/huge-size check for arena records, and false for unknown
pointers. Stack validation likewise requires the exact stack-base record and
recorded size. `trace_body_fits_alloc()` and custom table stamp lookup use the
same record rather than current allocator mode.

## Allocation, publication, marking, and traversal

### Allocation

At the beginning of every major/minor mark epoch, increment a 64-bit registry
mark epoch and acquire-snapshot `alloc_serial` into `sweep_cutoff`.

An external allocation does this before returning to its caller:

1. atomically snapshot the current owner pair;
2. call the pair with the exact public `osize/nsize` values;
3. reserve and initialize a record;
4. safely determine external/small-arena/huge backing through directories;
5. assign a monotonically increasing `alloc_serial`;
6. if GC2 is active, initialize `mark_epoch` to the current epoch
   (allocation-black); and
7. release-CAS the record into the base index.

`lj_mem_newgco_raw_nothrow()` starts the record as `PENDING_GCO`. Pending
records are memory-valid and allocation-black but cannot be traversed or
destructor-dispatched. Once the header and all destructor-driving size fields
are initialized, an explicit publication hook stores `obj`, expected `gct`,
and flags, adds the variable-object index if needed, then release-publishes
`GCO`. If MARK/WEAK is active it queues the object after publication.

This hook is especially important for:

- prototypes currently linked by `lj_mem_newgco()` before their header is
  initialized;
- variable/over-aligned cdata, whose allocation base differs from `GCcdata`;
- interned strings, which use `lj_mem_new()` and never join the GC root spine.

The proto constructors should eventually become initialize-then-publish like
the newer table/function paths. Until then, a pending record conservatively
retains the body and forbids destructor inference.

### Mark

For a registry record, `lj_gc2_markmem()` atomically exchanges/CASes
`mark_epoch` to the current 64-bit epoch. It returns one only for the first mark
in that epoch. If the record has arena backing it also executes the existing
small-arena/HugeTab mark transition. If its state is `RETIRED`, marking rescues
it to `LIVE`; `FREEING` returns invalid.

`lj_gc2_markobj()` looks up the exact object record before reading the header.
After the first mark of a published traversable record, it pushes the object
through the normal SSB/grey traversal path. Thus custom tables, functions,
threads, prototypes, upvalues, userdata, traces, and cdata close the same graph
as arena objects. A pending record is marked but not traversed.

Raw side storage is marked by its exact base record when its owner is
traversed. Raw records are not independently collected during normal sweep;
the owning object/subsystem destructor releases them. Remaining raw records
are included in live accounting and terminal leak diagnostics.

### Custom table scan stamp

The current table stamp is attached to an arena header and disappears whenever
`g->allocf` is custom. Use `LJAllocRec.aux` for an external table's packed
dirty/scan-cycle stamp. Arena tables keep the existing per-arena sidecar. This
is required before custom tables can participate in concurrent rescan/fixpoint
closure.

## Sweep, rescue, and destructor ownership

The custom sweep scans the registry directory in bounded bucket batches under
an SMR read section. It examines only records with
`alloc_serial <= sweep_cutoff`; later records are allocation-black for the
current cycle.

For a published external `GCO` record:

1. fixed/finalizer-pending records are preserved or handed to FINREG;
2. a current `mark_epoch` is live;
3. an unmarked record is detached from the ownership root spine;
4. CAS `LIVE -> RETIRED`, store the current handshake retire epoch, and request
   a grace;
5. a sweep-time root publication may mark it and CAS `RETIRED -> LIVE`, in
   which case its exact header is reanchored;
6. after the required handshake epochs and with `smr_readers == 0`, CAS
   `RETIRED -> FREEING`;
7. dispatch the expected type-specific destructor; and
8. the final `lj_mem_free()` uses the record owner pair, removes both indexes,
   publishes `DEAD`, and retires the metadata node for another SMR grace.

This deliberately mirrors the existing arena `WHITE/LIVE/RETIRED/FREEING`
quarantine protocol. It must share the sweep worker token and
`sweep_grace_needed` gate so arena and custom destructors cannot cross the
root-snapshot grace independently.

Arena-backed custom records are not swept a second time by the custom-record
enumerator. The arena owner performs liveness/quarantine; the record supplies
object identity and the correct callback pair when `lj_mem_free()` is reached.
The record state and arena sweep state must be advanced together so only one
path can claim `FREEING`.

Interned `STRING` records are not generic GCO sweep candidates. Runtime string
sweep must first CAS-unlink/tombstone the bucket entry, wait an SMR grace for
interning readers, and only then move the record to `FREEING`. The allocation
registry supplies exact size, mark epoch, and owner callback, but does not
replace the separate string-bucket unlink protocol.

The first custom-registry checkpoint should force major sweep identity while
external records are live. Custom young/old flags and minor remembered-set
selection can be enabled only after focused generational tests pass. This is a
temporary performance gate, not a final design exemption.

## Realloc, explicit free, and allocator switches

### Realloc

Existing-pointer realloc first performs registry lookup, then arena-directory
lookup. It never selects a callback from the current allocator.

For an external record:

1. pre-reserve a replacement metadata record, so metadata OOM leaves the old
   allocation untouched;
2. CAS `LIVE -> MOVING` and conservatively mark it for an active epoch;
3. invoke the saved owner pair with the exact old pointer and sizes;
4. on failure with nonzero `nsize`, restore `LIVE`;
5. on same-address success, publish the new size and restore `LIVE`;
6. on moved success, fully publish the replacement record (including new
   backing classification) before making the old record `DEAD`; and
7. unlink/SMR-retire the old record.

The allocating subsystem still owns publication of a moved buffer to its
readers. Registry `MOVING` prevents GC from inferring a destructor or trusting
changing extent metadata during the callback.

### Explicit free

`lj_mem_free()` looks up a custom record before testing arena ownership. This
ordering is essential for a wrapper-owned arena pointer. It claims the record,
invokes its saved pair, and publishes `DEAD`; a pure internal-arena allocation
with no custom record follows the existing owner-TG route.

### `lua_setallocf()`

The atomic pair replacement affects new allocations only. Already published
records retain their pair. Multiple switches such as arena -> wrapper -> other
allocator -> arena are therefore safe, and racy setters have a total CAS order.
The old `ud` must remain valid until that generation's last record is freed.

## Allocation/free entry-point audit

All public-allocator traffic should funnel through the registry-aware central
primitives.

| Path | Required treatment |
|---|---|
| `lua_newstate()` GG allocation/failure | bootstrap owner pair; not a normal record |
| `lj_mem_new_nothrow()` / `lj_mem_realloc()` | raw external record or proved arena route |
| `lj_mem_newgco_raw_nothrow()` | pending-GCO external record or arena route |
| `lj_mem_free()` | custom record first, then proved arena owner |
| `lj_mem_freegco_defer()` | arena-only quarantine; custom uses record retirement |
| `lj_gc2.c` weak-overflow direct `g->allocf` call | replace with central nonthrowing raw allocation |
| state/thread stack rehome | classify the old pointer, choose current pair only for the new block |
| trace/table/function/VM bump paths | remain pure-arena-only; permanent slow gate after `lua_setallocf()` |
| `lua_close()` GG release | immutable boot owner/backing, never current pair |
| mcode/OS thread mappings | remain their existing independent retirement domains |

Every current test of `g->allocf == lj_arena_allocf`, `allocf_arena`, or
`g->allocd` in GC/GC2/trace/threading code must be classified as either:

- a **new-allocation fast gate**, which may use the conservative fast flag; or
- an **existing-pointer ownership query**, which must use registry/directory
  classification instead.

The latter category includes object validation, stack validation, trace body
extent checks, table scan stamps, root old-generation classification, mark,
ismarked, sweep, rehome/free routing, worker capability checks, and close.

## Shutdown and failed-state cleanup

After workers and spawned threads stop, terminal GC2 drain should:

1. finish FINREG callbacks;
2. drain root-spine and string objects using exact records/arena identities;
3. explicitly free registered raw subsystem allocations;
4. report any remaining live records with base/size/kind/owner generation;
5. retire/unmap registry metadata; and
6. release `GG_State` through its boot owner/backing.

Partial `lua_newstate()` failure follows the same boot rule even if registry
initialization itself failed. A custom state must never call the current pair
on `GG_State`, and an arena state which later switched allocator must still
destroy/unmap its original arena bootstrap.

## Implementation sequence

1. **Exact registry and mixed-domain safety.** Add atomic allocator pair,
   bootstrap owner, metadata slabs, exact base/object lookup, central
   allocation/free/realloc records, and safe arena-directory-first
   classification. Remove all “custom means valid” shortcuts.
2. **Custom marking and traversal.** Add 64-bit registry mark epochs,
   allocation-black initialization, object publication hooks, variable-cdata
   aliases, raw side-body marking, custom table stamps, and graph traversal.
3. **Bounded custom sweep.** Add the registry sweep cursor/cutoff,
   root-spine detach, RETIRED rescue, shared grace, type destructors, FINREG,
   live-byte aggregation, and forced-major gate.
4. **Runtime string retirement.** Use record epochs/sizes with lock-free bucket
   tombstones and SMR-delayed string frees.
5. **Remove ownership-spine dependence.** Share the exact object identity
   interface with the arena sidecar and delete root-spine destructor discovery.
6. **Performance completion.** RCU-grow registry directories, per-TG metadata
   record caches, compact raw records, custom minor-generation identity, and
   optionally a proven generation-aware arena fast-path re-enable.
7. **Platform matrix.** Linux native, Windows/Wine, macOS/Darling, APICHECK,
   ASan/UBSan, TSan where usable, sysmalloc, custom allocation failure, JIT,
   FFI/VLA/finalizers, and secondary TG stress.

## First bounded implementation slice

The first reviewable checkpoint should stop at a safe, testable boundary. It
should not claim runtime custom reclamation yet.

### Code scope

- Add `src/lj_allocreg.[ch]` with OS-backed metadata slabs, a chained exact
  base index, the variable-object alias index, record insertion/lookup/unlink,
  and test-only counters.
- Add the atomic authoritative allocator pair, boot owner/backing, registry
  state, and permanent post-`lua_setallocf()` arena-fast disable flag.
- Route `lua_getallocf()`, `lua_setallocf()`, central `lj_mem_*`, direct weak
  overflow allocation, and close-time GG release through the new APIs.
- Replace custom-mode blanket validity and unsafe arena-header probes with:
  exact record -> safe arena directory -> HugeTab -> invalid.
- Implement registry mark epochs and make a first custom GCO mark queue normal
  GC2 traversal. Add publication hooks for ordinary base-header objects,
  variable cdata, and strings.
- Keep external records conservatively live at sweep in this checkpoint, and
  force major identity. Arena-backed wrapper records still set arena marks.
  This removes corruption and invalid reads without inventing a premature
  destructor path.
- Make terminal root/string/raw frees use recorded owner pairs so the slice
  does not leak at `lua_close()`.

### Required tests before that checkpoint is pushed

1. Re-enable the default extended `LJ_FFI_ERRSTATE_ALLOC_STRESS` matrix and run
   it repeatedly across automatic GC2 cycles.
2. Add `t-gc2-custom-alloc-registry.c` with two deliberately incompatible
   allocators. Each block carries an allocator magic; allocate under A, switch
   to B, allocate, switch back, collect and close. Every realloc/free must reach
   its original owner and both live-block counts must end at zero.
3. Add a wrapper-around-internal-arena case and assert that both the record mark
   epoch and arena mark bit advance, with one physical free.
4. Run the exact sysmalloc repro above, then a larger graph containing nested
   tables/functions/coroutines, weak tables, strings, fixed/VLA cdata, and
   finalizers through repeated automatic and explicit cycles.
5. Add invalid-pointer validation probes proving custom mode no longer accepts
   an arbitrary aligned word or dereferences its 64 KiB base.
6. Race two `lua_setallocf()` setters with allocation on secondary TGs; owner
   magic and record counts must remain exact. Racy Lua data results may vary,
   but allocator metadata must not corrupt or cross-free.
7. Inject metadata-slab/record OOM after a successful user allocation and prove
   rollback uses the captured pair and leaves accounting unchanged.
8. Run existing GC2 no-legacy-entry, root traversal, arena sweep/quarantine,
   threading allocation, FFI finalizer, JIT token/xsave, and close tests.

The next checkpoint then adds actual custom record retirement/destructors and
removes the temporary conservative-live rule. Keeping these as two commits
makes the ownership/classification change independently reviewable while never
reintroducing the old collector.
