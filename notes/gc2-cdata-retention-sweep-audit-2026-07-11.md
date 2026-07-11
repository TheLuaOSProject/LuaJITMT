# GC2 cdata retention and sweep audit (2026-07-11)

Status: design/audit only. This note does not claim that the current cdata,
arena-terminal, HugeTab, object-validation, or sweep protocols are complete.

## Result

Two independent correctness holes exist.

1. GC2 reads `GCcdata` bytes before retaining the allocation that contains
   them. An over-aligned small cdata header can be on a block-zero extent cell,
   and a huge variable cdata header is an interior pointer. The current
   validate-then-mark order can therefore reject a live object or race physical
   reclamation.
2. `lj_cdata_newv()` uses `lj_mem_new_nothrow()`. With the internal allocator
   this selects the PLAIN arena kind, or a huge entry without
   `LJ_HUGEF_TRAVERSABLE`. `HS_RESET_ALLOC` restores PLAIN arenas without
   sweeping them, and `lj_arena_hugetab_prepare_sweep()` selects only huge
   TRAVERSABLE entries. Thus variable/VLA/over-aligned cdata never satisfies
   `gc2_sweep_obj_old_generation()`, never reaches root detachment, and leaks.

Both must be fixed. Correct allocation-base marking alone does not make these
objects collectable.

## Allocation layouts

Fixed cdata is allocated by `lj_cdata_new_l()` / `lj_cdata_new_()` through
`lj_mem_newgco_unlinked_nothrow(sizeof(GCcdata) + payload)`. Its allocation
base is the `GCcdata *`, and the current implementation selects the
TRAVERSABLE/sweepable arena class.

`lj_cdata_newv()` allocates this raw layout:

```
allocation base p
  [prefix/padding ...]
  [GCcdataVar immediately before cd]
cd: [GCcdata]
    [aligned payload]
```

It computes:

```
extra = sizeof(GCcdataVar) + sizeof(GCcdata) + alignment_slack
cd = align_up(p + sizeof(GCcdataVar) + sizeof(GCcdata), align)
     - sizeof(GCcdata)
```

The immutable `GCcdataVar.offset` gives `p = cd - offset`. Allocation also
copies that offset to the first two bytes at `p`; this is existing
allocation-owned header-to-base metadata. With ordinary VLA alignment,
`cd == p + 8` and the base and header share a 16-byte cell. With alignment 16,
`cd` can be `p + 16`; larger alignments can put it farther into block-zero
extent cells. Total small allocation size is at most
`LJ_HUGE_THRESHOLD == 16 KiB`, so its start is at most 1024 cells, or 16
64-bit block-bitmap words, behind any interior header. Alignment is capped at
`2^15`; a variable header remains in the first 64 KiB chunk of a huge mapping.

The base prefix, `GCcdataVar`, `GCcdata.gct`, and `GCcdata.ctypeid` are fully
initialized before `lj_gc_linkobj_new()` publishes the object. They remain
immutable until the destructor owns the allocation. The variable bit is stored
in `GCcdata.marked`, a byte which also contains concurrently mutated GC/FINREG
flags; concurrent code must test it with an atomic acquire load, not the plain
`cdataisv(cd)` macro.

## The current bad order

`gc2_markobj_preserve_status()` currently has two failing paths.

* Its small path calls `gc2_mark_small_cell_begin(..., cellof(o), ...)` before
  resolving variable cdata. For an over-aligned header on an extent cell,
  `block[cellof(o)] == 0`, so a live object is reported DEAD before
  `lj_cdata_validate()` can find the real base.
* Its huge/fallback path calls `gc2_markobj_base_valid()`, which reads `o->gct`,
  `cdataisv`, `ctypeid`, and `GCcdataVar` through `lj_cdata_validate()` before
  `gc2_markmem_status(base)` marks the HugeTab entry. A range lookup followed
  by the existing exact mark is not a fix: delete/unmap can win between the
  lookup and mark.

The same pre-retention decoder feeds `lj_gc2_ismarked()`, public/queued object
validation, TValue type checks, and FINREG validation.

## Required retained-object view

Replace validate-then-mark with one internal scoped operation, conceptually:

```
gc2_markobj_view_begin(g, candidate, &view)
  -> GC2_MARK_DEAD / GC2_MARK_LIVE_ALREADY / GC2_MARK_NEW

view = { object header, exact allocation base, gct, memory kind,
         huge flags, small admission scope }

gc2_markobj_view_end(&view)
```

The operation must locate and mark/rescue the containing allocation without
reading candidate object bytes. Only a live result permits `gct`, cdata layout,
direct-body, or graph reads. Keep a counted small-arena admission until all
validation/direct reads finish. A huge mark is the durable certificate.

If post-retention semantic validation fails, return DEAD to the object caller
but leave the allocation conservatively marked. Undoing another marker's bit is
not legal. The bitmap discovery may cause one extra fixpoint round, but the
invalid header must never be queued.

### Bounded small-arena containing start

1. Establish small-arena membership from the shared arena registry before
   dereferencing `arena_of(candidate)`. Do not use payload bytes or an
   unregistered arena header to distinguish small from huge.
2. Enter `lj_arena_rescue_enter(a)` and retain that admission through
   validation.
3. Reverse-search the acquired `block[]` words from `cellof(candidate)` for the
   nearest set block bit, bounded by `LJ_HUGE_THRESHOLD / LJ_CELL_SIZE` cells.
   A legitimate allocation has one block bit at its start and zero extent bits,
   so there can be no intervening block start. Implement this as at most 16
   reverse word loads, not a 1024-iteration object hot path.
4. Call `gc2_mark_small_cell_admitted()` on that start. It must reject
   block-zero, `late[]`, FREEING, or an unrescuable terminal generation and
   preserve the tri-state NEW/ALREADY/DEAD contract.
5. Let `base = cellptr(a, start)`. If `candidate == base`, it may be a fixed
   cdata and the retained header can now be checked. Otherwise, acquire-load
   the copied `uint16_t offset` at `base` and require
   `base + offset == candidate` before reading any byte at `candidate`.
6. Under the same admission, use an atomic variable-bit test and
   `lj_cdata_validate()`. Require its returned base to equal the retained base,
   its size to be `<= LJ_HUGE_THRESHOLD`, its cell span to fit the arena, and
   no structural start to occur inside the claimed span.

The copied offset is sufficient for the minimal VM-published-edge path. For a
formal conservative-arbitrary-word proof, make the base descriptor stronger:
duplicate the immutable `{offset, extra, len}` plus a type/version tag at the
allocation base, or maintain an allocation-generation side plane. A marker
must still mark the candidate start before consulting that descriptor.

### Atomic HugeTab containing mark

Add a single operation such as:

```
lj_arena_hugetab_mark_range(ht, interior, &base, &info)
```

It needs distinct results for MISSING, DEAD/FREEING, ALREADY, NEW, and
RETIRED-rescued. MISSING must be distinguishable from a containing FREEING
entry when searching several TG tables.

For every candidate slot it must:

1. acquire a stable `{addr, meta}` snapshot;
2. check `interior - addr < decoded_size` with subtraction bounds;
3. reject a containing `LJ_HUGEF_FREEING` entry;
4. CAS the same 128-bit slot from `{addr, meta}` to
   `{addr, (meta | MARK) & ~RETIRED}`;
5. only after that successful full-slot CAS return the exact base and metadata.

The CAS is the lookup/mark linearization point and prevents slot deletion,
reuse, and metadata ABA between a range lookup and exact mark. Preserve
SWEEP_OLD, TICKET, BUSY, size, traversal, and finalizer metadata. Map first mark
and RETIRED rescue to `GC2_MARK_NEW`; an existing mark is
`GC2_MARK_LIVE_ALREADY`; FREEING/missing is DEAD at the GC2 layer. Increment
`marks_this_round` only for NEW.

The operation can try an exact hashed address first. For cdata specifically,
the immutable layout cap means the likely huge allocation base is
`arena_of(candidate) + sizeof(GCAhdr)`; trying that exact base in each live
HugeTab avoids a full-table scan. The general range-CAS fallback is still
required and must not read mapping/header bytes.

After the CAS, validate cdata and require the layout-derived base and size to
match the returned HugeTab base and authoritative size exactly. A failed
semantic check remains conservatively marked.

### Huge mark durability prerequisites

A HugeTab MARK is not a lifetime certificate if a live entry can be deleted or
migrated behind it.

* Runtime deletion must require terminal FREEING ownership. Keep a separate
  quiescent teardown delete for tests/shutdown.
* `lj_arena_hugetab_transfer()` currently inserts in the destination and then
  deletes the source. A marker can mark the source after the copy and lose that
  mark on deletion. Either eliminate per-TG migration with a global HugeTab,
  add a MOVING/claim protocol which transfers concurrent marks, or strictly
  gate dead-TG transfer to IDLE with no workers, assists, SMR readers, or mark
  publishers.
* The HugeTab header/table object itself must remain alive while a range marker
  scans it. `TGF_HUGETAB` plus a plain `ht->h` load is not a lease against
  concurrent `hugetab_fini()`.

## Collection-class fix

The minimal implementation should allocate `lj_cdata_newv()` storage through
the GC-object raw allocator with the existing sweepable/TRAVERSABLE arena flag,
instead of `lj_mem_new_nothrow()`'s PLAIN class. This gives small variable
cdata a prepared/quarantined arena and gives huge variable cdata
`LJ_HUGEF_TRAVERSABLE`/SWEEP_OLD. `gc2_gct_may_traverse()` already excludes
`LJ_TCDATA`, so this does not enqueue cdata graph work.

Longer term, split "sweepable typed allocation" from "has graph edges" in the
arena flags/kinds; cdata is sweepable but graphless. Do not enable bitmap sweep
for the existing generic PLAIN arenas, which contain opaque auxiliary buffers.

The existing reclamation machinery is otherwise shaped for interior cdata:

* `gc2_sweep_cell_obj()` starts from a retained allocation base, reads the
  copied offset, validates `GCcdataVar`, and reconstructs the exact header;
* small root detachment stores LIVE/RETIRED state at the allocation-base cell;
* huge `lj_arena_hugetab_retire()` stores the exact interior object in
  `GCAhdr.retire_obj` under BUSY/TICKET;
* post-grace `lj_gc_reclaim_gc2_arena()` / `lj_gc_reclaim_gc2_huge()` claim
  FREEING before `gc2_free_unmarked_obj()` dispatches `lj_cdata_free()`; and
* `lj_cdata_free()` validates the retained header and frees the exact base and
  allocation size. Its validation is safe only because the caller already owns
  terminal destruction.

## Callsite map

The retained view must replace the pre-header path used by:

* `gc2_markobj_preserve_status()`, and therefore `lj_gc2_markobj()`,
  `lj_gc2_markobj_direct()`, `lj_gc2_markobj_nogrey()`, worker marking, SSB
  consumption, thread/global root marking, `lj_gc2_preserve_sweep_root()`, and
  `lj_gc2_trace_sweep_root()`;
* `gc2_markobj_base_valid()` and `gc2_mark_base()` wherever they are used to
  authorize a later body read;
* `lj_gc2_ismarked()` for cdata. A non-mutating marked query is observational,
  not a dereference lease; root-spine/weak callers must keep their independent
  root, generation, phase, or SMR lease while resolving the containing base;
* `gc2_registered_obj_valid()` / `lj_gc2_obj_valid()` and
  `gc2_queue_obj_valid()` / `lj_gc2_obj_valid_queued()` when their callers read
  `gct` after the boolean result;
* `gc2_tv_gcref_type_match()` and `_known()`, including stack, weak, barrier,
  table-child, and public `lj_gc2_tv_gcref_valid_edge()` paths; and
* `gc2_finreg_cdata_obj_valid()`, ordered FINREG resolution, queue/enqueue,
  preclaim/take, and dispatch. The active ordered node, preclaim slot, finalizer
  queue, or anchored stack value must be stated as the independent identity
  lease where a mark is intentionally not installed.

`gc2_finreg_markobj()` must consume a live status before its current
post-marker `o->gct` read and possible table traversal. A void marker followed
by a raw type read is not a lifetime protocol.

These owner paths may keep direct layout validation, but their lease must be
explicit in comments/assertions:

* `gc2_sweep_obj_base()` and `gc2_valid_freeable_obj()` while the object is
  still owned by the root spine or by the just-won detach claim;
* `gc2_sweep_cell_obj()` while the sealed quarantine owner and completed grace
  retain the whole small allocation;
* `lj_gc_reclaim_gc2_huge()` after BUSY/FREEING/TICKET and grace establish
  mapping ownership; and
* `lj_cdata_free()` after the sole destructor claim.

Ordinary FFI cdata accessors operate on an anchored Lua value and are not a
substitute for GC2 validation. Their plain `cdataisv` reads should still be
audited because that byte shares atomic FINREG flags.

## Minimal implementation sequence

1. Add the HugeTab atomic containing-mark API and deterministic transition
   tests. Close delete/transfer/table-lifetime prerequisites before treating
   its result as a certificate.
2. Add the bounded small allocation-start resolver and retained view. Use the
   existing base offset first; add a tagged/full base descriptor or generation
   side metadata before claiming arbitrary conservative-word validation.
3. Route all mark variants through the retained view. Preserve the public
   NEW-only boolean API and internal tri-state accounting.
4. Route cdata `ismarked`, TValue validation, root-spine validation, and FINREG
   validation through scoped/leased variants; remove pre-retention
   `gc2_markobj_base_valid()` use.
5. Make `cdataisv` atomic in concurrent readers.
6. Move `lj_cdata_newv()` to a sweepable typed allocation class. Keep cdata
   graphless in traversal.
7. Verify root detachment, small reconstruction, huge TICKET reanchor, and
   destructor/free accounting for fixed, VLA, over-aligned, and huge cdata.

## Required tests

### Layout and mark semantics

* Allocate ordinary VLA plus alignments 16, 32, 64, 4096, 8192, 16384, and
  32768. Force at least one small header onto a different cell from its base.
  Assert the header cell is block-zero, the base cell is block-one, the copied
  offset is exact, and marking the object marks the base.
* For small and huge interior cdata, assert first mark/rescue is NEW, the second
  is ALREADY, `marks_this_round` increments once, `ismarked` finds the base,
  and no grey/SSB item is produced solely for `LJ_TCDATA`.
* Cover total allocation sizes immediately below, at, and above
  `LJ_HUGE_THRESHOLD`, not merely payload sizes.

### HugeTab linearization

* Range-mark base, first interior byte, cdata header, and last byte; reject the
  byte just outside the entry.
* Cover unmarked, already marked, RETIRED/TICKET rescue, BUSY publication, and
  FREEING rejection while preserving every unrelated metadata bit.
* Pause after the stable range snapshot and race delete/tombstone/reinsert in
  the same slot. The old lookup-then-mark mutant must lose; the full-slot CAS
  must never touch unmapped payload and must classify only the entry it CASed.
* Race mark against RETIRED->FREEING repeatedly. Exactly one terminal side may
  win. Add a transfer/migration schedule or enforce and test the quiescent
  transfer precondition.
* `mprotect(PROT_NONE)` (or `VirtualProtect`) a registered payload and verify
  that the HugeTab range-mark primitive itself succeeds without reading it.

### Small terminal schedules

* Reverse-scan across block-word boundaries and at the maximum bound; reject no
  start, a mismatched base offset, an intervening structural start, late, and
  FREEING.
* Pause before admission, after admission, after start discovery, and after
  base mark while racing RETIRED rescue, terminal seal/commit, restore, and
  reuse. A live result must keep every header byte readable until view end; a
  dead result must perform no candidate-header read.
* Add the old `cellof(header)` mutant: align-16 cdata on an extent cell must
  demonstrate the previous false-DEAD result.

### Collection and FINREG

* Capture small and huge variable cdata bases in a C harness, drop all Lua
  roots, run enough full cycles/graces, and assert the small block is reclaimed
  and the HugeTab entry disappears. This is the direct regression for the
  current PLAIN leak.
* Repeat with `ffi.gc` on fixed, VLA, over-aligned-small, and interior-huge
  cdata. Assert one callback, FINREG clear before free, optional resurrection,
  one reanchor, and eventual reclamation.
* Exercise cdata in stack, array, hash, weak-key, weak-value, ordered FINREG,
  preclaim, finalizer queue, and JIT-created CNEW/CNEWI roots while collections
  run on peers.
* Run alignment churn with JIT on/off and multiple TGs; memory/GC total and
  HugeTab live bytes must return near baseline rather than monotonically grow.

Run focused tests under assertion/paranoia builds, ASan/UBSan, TSAN, repeated
terminal stress, Linux, Wine/Windows, and Darling/macOS. Add mutant gates for
lookup-then-mark, header-cell marking, and PLAIN allocation so each original
bug is independently observable.
