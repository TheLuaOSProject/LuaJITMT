# 04. The Allocator: Lock-Free Arena Heap

Replaces `lj_alloc.c` and `lj_mem_*` (lj_gc.h:112–135). The layout follows
Pall's LuaJIT-3.0 GC design (arenas, 16-byte cells, segregated block/mark
bitmaps, differential encoding, huge-block side table); concurrency comes
from strict per-thread arena ownership. Executable model of the bitmap math:
`aux/arena_bitmap_model.c` (run it; it asserts the sweep identities).

## 4.1 Constants & layout

```c
#define LJ_ARENA_SHIFT   16              /* 64 KB arenas, v1 default      */
#define LJ_ARENA_SIZE    (1u<<LJ_ARENA_SHIFT)
#define LJ_ARENA_MASK    (LJ_ARENA_SIZE-1)
#define LJ_CELL_SHIFT    4               /* 16-byte cells                 */
#define LJ_CELL_SIZE     16
#define LJ_ARENA_CELLS   (LJ_ARENA_SIZE>>LJ_CELL_SHIFT)        /* 4096    */
#define LJ_AMETA_CELLS   (LJ_ARENA_CELLS/64)                   /* 64      */
/* first usable cell index: */
#define LJ_AFIRST_CELL   LJ_AMETA_CELLS                        /* 64      */
```
Arena memory is mmap'd `MAP_ANON|MAP_PRIVATE`, size LJ_ARENA_SIZE, aligned
to LJ_ARENA_SIZE (over-map 2x and trim, or MAP_ALIGNED where available).
Alignment gives `arena_of(p) = (GCArena*)((uintptr_t)p & ~LJ_ARENA_MASK)`
and `cell_of(p) = ((uintptr_t)p & LJ_ARENA_MASK) >> LJ_CELL_SHIFT` — the
two operations every barrier/mark/free uses.

Metadata occupies cells 0..63 (1/64 of the arena = 1.56% overhead):

```c
typedef struct GCArena {
  /* --- header (lives in the space of the first bitmap words, which     */
  /*     correspond to cells 0..63 and are never object cells) --- */
  struct GCAhdr {
    uint32_t flags;          /* AF_TRAVERSABLE | AF_NEEDSWEEP | AF_FULL.. */
    uint32_t owner_tid;      /* 0 = orphan/global                        */
    struct GCArena *next;    /* intrusive links for owner / global lists */
    GreyStack *grey;         /* per-arena grey stack ptr (05 §5.6.3 opt) */
    uint32_t sweep_epoch;    /* last GC cycle this arena was swept in    */
    uint32_t live_cells;     /* filled by sweep; pacing input            */
  } hdr;                     /* padded to 128 bytes                      */
  uint64_t block[LJ_ARENA_CELLS/64];   /* 512 B */
  uint64_t mark [LJ_ARENA_CELLS/64];   /* 512 B */
  /* header+bitmaps occupy exactly cells 0..63 (1024B hdr-share + 1024B) */
  /* cells 64..4095: object data */
} GCArena;
```
(Compile-time asserts pin: `offsetof(GCArena, block) == 128`,
`sizeof bitmaps == 1024`, header+bitmaps == 64 cells. The model file does.)

### 4.1.1 Bitmap encoding (per cell index i: bit i of block + bit i of mark)

| block | mark | meaning |
|---|---|---|
| 0 | 0 | block **extent** (continuation cell) |
| 0 | 1 | **free** block start |
| 1 | 0 | **white** block start (unmarked object) |
| 1 | 1 | **black** block start (marked object) |

Allocation of an n-cell object at cell i: set block bit i (its mark bit
encodes color), leave bits i+1..i+n-1 as 00 extents. Marking: set mark bit.
Sweep identities (whole-word parallel, the model file proves them on random
heaps):
```
major:  block' = block & mark ;  mark' = block ^ mark
minor:  block' = block & mark ;  mark' = block | mark
```
After a major sweep, black→white and white/free→free, extents preserved —
without touching object memory. `block|mark` per word also yields the
"any object starts here" mask used by the fit allocator's scavenger.

## 4.2 Size classes & what lives where

- Small (≤ 8 cells = 128 B): GCupval(48B→3 cells incl. nothing wasted? see
  §4.7 sizes), Node-free? (nodes live inside table gens, not standalone),
  GCstr short, GCfunc, GCcdata small, lua_State header.
- Medium (≤ 1024 B): table gens small, buffers, protos.
- Large (≤ LJ_HUGE_THRESHOLD = LJ_ARENA_SIZE/4 = 16 KB): in-arena multi-cell.
- Huge (> 16 KB): whole mmap'd regions, arena-aligned, multiple of 64 KB;
  tracked in the huge side table (§4.5).
Traversable vs non-traversable arenas are segregated by AF_TRAVERSABLE
(strings & non-ref payloads go to non-traversable arenas; markers skip them
wholesale — straight from the Pall design).

## 4.3 Per-thread allocator state (TG.alloc)

```c
typedef struct TGAlloc {
  /* bump frontier per arena-kind: [0]=traversable, [1]=plain */
  struct { GCArena *a; uint32_t cell; uint32_t end; } bump[2];
  /* segregated free runs found by sweep, per size class, owner-local */
  FreeRun *bins[2][LJ_ALLOC_NBINS];      /* plain singly-linked, no atomics */
  GCArena *owned[2];        /* all arenas this thread owns (next links)   */
  GCArena *needsweep[2];    /* owned arenas awaiting lazy sweep           */
  uint8_t  alloc_black;     /* current allocation color (05 §5.5)         */
} TGAlloc;
```
**Owner-sweeps rule**: only the owning thread allocates from or sweeps an
arena ⇒ bump pointers, bins, and the *block* bitmap of an owned arena are
single-writer. The *mark* bitmap is the only concurrently-written word range
(markers fetch_or), and sweep reads it only after the P_SWEEP handshake
(05 §5.8) ⇒ no torn read windows.

## 4.4 Allocation algorithm

```
lj_mem_newgco_t(tg, size, traversable):
  ncells = (size + 15) >> 4
  B = &tg->alloc.bump[traversable]
  if LIKELY(B->cell + ncells <= B->end):
      i = B->cell; B->cell += ncells
      a = B->a
      block_set(a, i)                       # plain store-OR, owner-only
      if tg->alloc.alloc_black: mark_set_rlx(a, i)   # fetch_or (I-3)
      tg->local_total += ncells<<4          # §4.8
      return arena_cell_ptr(a, i)           # gct/payload init by caller
  return alloc_slow(tg, ncells, traversable)

alloc_slow:
  1. take next FreeRun from bins[class(ncells)..] (exact, then split larger)
  2. else lazy-sweep one arena from needsweep[] (§4.6), refill bins, retry
  3. else pop a partially-free arena from the global reuse stack (§4.5),
     adopt (CAS owner_tid 0→tid), sweep it, retry
  4. else mmap fresh arena, adopt, point bump at cell LJ_AFIRST_CELL
  5. on step 3/4 also: pacing check — maybe trigger GC / mark-assist (05 §5.11)
  Steps 1–2 are owner-local (no atomics). 3–4 use the lock-free stacks below.
```
Bump reset: when an owned arena is swept, its largest free run becomes the
new bump window if ≥ LJ_BUMP_MIN (64 cells), else all runs go to bins —
this is the Pall bump↔fit adaptivity with the simplest possible policy;
tune at M9.

## 4.5 Global structures (the only cross-thread allocator state)

```c
struct GCHeapGlobal {
  TaggedPtr arena_reuse[2];   /* Treiber stacks of orphan/partial arenas  */
  TaggedPtr arena_empty;      /* fully-free arenas awaiting unmap/reuse   */
  HugeTab   huge;             /* §4.5.1 */
  _Alignas(64) uint64_t committed_bytes;  /* la_add64 on map/unmap        */
  _Alignas(64) uint64_t live_estimate;    /* written by sweep aggregation */
  uint64_t  gc_trigger;       /* pacing (05 §5.11) */
};
```
TaggedPtr = {ptr, uint64 aba} CAS'd as 16-byte (`la_cas128` — both targets
have it: cmpxchg16b / LSE `casp`; add to lj_atomic). Arenas are never freed
to the OS while any thread might hold a stale pointer obtained from a
GC-object address — they are only unmapped from `arena_empty` after a
handshake epoch passes with the arena unlinked (same grace rule I-4).

### 4.5.1 Huge objects
Open-addressing hash `addr → {size, mark, traversable, finalizer-bit}`,
power-of-two, RCU-resized exactly like the string table vector (06 §6.5.3),
insert/delete by CAS on 16-byte entry pairs; mark bit set by markers with
fetch_or on the entry's flag word. Allocation: mmap, `la_add64`
committed_bytes, CAS-insert. Free: only by GC workers during P_SWEEP.
`arena_of(p)` for a huge pointer lands on its (arena-aligned) base; the
first page carries magic `LJ_HUGE_MAGIC` in the GCAhdr.flags position so
`mark_obj(p)` can branch huge-vs-arena with one compare (05 §5.6.1).

## 4.6 Lazy sweeping (owner side)

At the P_SWEEP handshake every thread moves `owned[*]` → `needsweep[*]` and
resets bump/bins (the swept-state of the old windows is unknown). Sweeping
one arena = apply the §4.1.1 word identities over 64 words, build FreeRun
list from free-run boundaries (word-scan with ctz/clz; model file has the
exact loop), update live_cells, la_add64 into g live accumulator, set
sweep_epoch. GC workers concurrently sweep *unowned* arenas from the global
lists and arenas of dead threads. An arena is "swept for cycle E" exactly
once: claim via CAS sweep_epoch old→E (idempotence guard for the
worker/owner race on orphans).

## 4.7 GCobj header changes (lj_obj.h)

`GCHeader` is today `GCRef nextgc; uint8_t marked; uint8_t gct;`
(lj_obj.h:63–64). In the lockless runtime:
- `nextgc` is **deleted** (no global object list; sweep is bitmaps). The
  8 bytes are kept as `uint64_t gcw` scratch reserved per type:
  GCstr uses it as the intern-chain link (06 §6.5), GCupval drops
  prev/next, lua_State keeps gclist semantics via it, others zero.
  (Keeping the size avoids re-deriving every `sizeof` and dasc offset:
  GCHeader stays 8+1+1 bytes followed by type fields exactly as now.)
- `marked` is renamed `gcflags`: bit0 LJ_GCF_FIXED (strempty etc.),
  bit1 LJ_GCF_FINREG (has finalizer registered), bit2 LJ_GCF_WEAKKEY,
  bit3 LJ_GCF_WEAKVAL (tables cache their mode here as today),
  bit4 LJ_GCF_LEGACYUV (10 §10.4). **Color is not in the object.**
  All `iswhite/isblack/isgray/makewhite/...` macros (lj_gc.h:31–46) are
  retired with the legacy collector; lockless code uses
  `lj_gc2_ismarked(g,o)` = mark-bitmap test.
- Object cell sizes to verify after the change (add static asserts):
  GCupval 32 B (2 cells: hdr 10B+pad, tv 8, v 8, dhash 4 → fits 32),
  GCtab 64 B, GCfunc-L 8+...(per nupvalues), GCstr hdr 24+len.

## 4.8 Accounting, limits, lua_gc API

- `tg->local_total` accumulates; flushed with `la_add64` into
  `g->gc2.alloc_since_trigger` every LJ_ACCT_FLUSH (=32 KB) and at every
  safepoint ack. Pacing reads only the global sums (05 §5.11).
- `collectgarbage("count")` = (live_estimate + Σ flushed)/1024 — documented
  as an estimate under concurrent collection.
- Hard memory limit (sandbox): `committed_bytes > limit` at arena/huge map
  time → throw LUA_ERRMEM on the allocating thread. Granularity = arena.

## 4.9 C API allocator compatibility

`lua_newstate(allocf, ud)`: custom allocf is *only* used as the
arena/huge source (called with osize=LJ_ARENA_SIZE multiples) if the new
`LUAJIT_MEM_HOOK` mode is requested; otherwise ignored with a one-time
warning vmevent. `lua_getallocf` returns the shim. Document in 09 §9.10.

## 4.10 What to delete / keep from lj_alloc.c
Delete the dlmalloc body. Keep: the OS page primitives (mmap probing for
lower 47-bit addresses on GC64 — reuse for arena mapping so GCRef tag bits
stay valid: setgcreft packs itype at bit 47, lj_obj.h:74–76 — arenas MUST
be mapped below 1<<47; reuse `lj_alloc`'s existing MAP_32BIT/probe logic).
