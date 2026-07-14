/*
** Arena heap bitmap scaffolding.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_ARENA_H
#define _LJ_ARENA_H

#include "lj_def.h"
#include "lj_atomic.h"

#define LJ_ARENA_SHIFT		16
#define LJ_ARENA_SIZE		((uint32_t)1u << LJ_ARENA_SHIFT)
#define LJ_ARENA_MASK		(LJ_ARENA_SIZE - 1u)
#define LJ_CELL_SHIFT		4
#define LJ_CELL_SIZE		((uint32_t)1u << LJ_CELL_SHIFT)
#define LJ_ARENA_CELLS		(LJ_ARENA_SIZE >> LJ_CELL_SHIFT)
#define LJ_ARENA_WORDS		(LJ_ARENA_CELLS >> 6)

#define LJ_HUGE_THRESHOLD	(LJ_ARENA_SIZE >> 2)
#define LJ_ALLOC_NBINS		32
#define LJ_BUMP_MIN		64

#define LJ_ARENAK_TRAVERSABLE	0
#define LJ_ARENAK_PLAIN		1
#define LJ_ARENA_NKINDS		2

#define LJ_AF_TRAVERSABLE	0x00000001u
#define LJ_AF_NEEDSWEEP		0x00000002u
#define LJ_AF_FULL		0x00000004u
#define LJ_AF_REGISTERED	0x00000008u
#define LJ_AF_QUARANTINE	0x00000010u
#define LJ_AF_RECLAIMED		0x00000020u
#define LJ_AF_PREPSWEEP		0x00000040u
/* Allocation-request modifier. This bit is consumed by the allocator and is
** never persisted in GCAhdr.flags or ordinary HugeTab metadata. */
#define LJ_AF_DTOR_CONSTRUCT	0x40000000u
#define LJ_AF_ROOT_CONSTRUCT	0x80000000u
#define LJ_AF_FLAG_MASK \
  (LJ_AF_TRAVERSABLE|LJ_AF_NEEDSWEEP|LJ_AF_FULL|LJ_AF_REGISTERED| \
   LJ_AF_QUARANTINE|LJ_AF_RECLAIMED|LJ_AF_PREPSWEEP)
#define LJ_AF_HUGE_MAGIC	0x4c4a4800u

/* Arena-local lifetime publication gate. The low bits count admitted
** publishers. CLOSED routes terminal frees to the bit-only late bitmap and
** makes rescues sticky through PENDING. SEALED excludes ordinary intrusive
** publishers and owner transfer while still allowing counted bit/status
** producers whose admission defeats exact commit/open arbitration. For plain
** arenas, SEALED|PENDING is also the exact short-lived body-writer token;
** readers reject it, while bit-only late publishers increment the same count
** and keep the eventual bin generation CLOSED until the last leave. */
#define LJ_ARENA_REMOTE_CLOSED		UINT64_C(0x8000000000000000)
#define LJ_ARENA_REMOTE_SEALED		UINT64_C(0x4000000000000000)
#define LJ_ARENA_REMOTE_PENDING		UINT64_C(0x2000000000000000)
#define LJ_ARENA_REMOTE_COUNT_MASK	UINT64_C(0x1fffffffffffffff)
#define LJ_ARENA_REMOTE_STATE_MASK \
  (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED| \
   LJ_ARENA_REMOTE_PENDING)

static LJ_AINLINE uint32_t lj_arena_bin_from_ncells(uint32_t ncells)
{
  if (ncells == 0)
    return LJ_ALLOC_NBINS;
  return ncells < LJ_ALLOC_NBINS ? ncells - 1u : LJ_ALLOC_NBINS - 1u;
}

static LJ_AINLINE uint32_t lj_arena_binmask_from_bin(uint32_t bin)
{
  return bin < LJ_ALLOC_NBINS ? (~(uint32_t)0 << bin) : 0;
}

static LJ_AINLINE uint32_t lj_arena_binmask_from_ncells(uint32_t ncells)
{
  return lj_arena_binmask_from_bin(lj_arena_bin_from_ncells(ncells));
}

typedef struct GCArena GCArena;
typedef struct GreyStack GreyStack;
typedef struct LJArenaFreeRun LJArenaFreeRun;
typedef struct LJArenaBump LJArenaBump;
typedef struct LJArenaAllocD LJArenaAllocD;
typedef struct LJHugeTabHdr LJHugeTabHdr;
typedef struct HugeTab HugeTab;
typedef struct LJHugeInfo LJHugeInfo;
typedef struct LJHugeReader LJHugeReader;
typedef struct LJArenaRemoteFree LJArenaRemoteFree;
typedef struct TGAlloc TGAlloc;

struct HugeTab {
  LJHugeTabHdr *h;
};

/* Counted HugeTab body admission. The stable table header and exact allocation
** base are sufficient for release; the possibly TG-embedded HugeTab wrapper
** may retire immediately after acquire and is never dereferenced by release.
** Tokens must be zero-initialized before acquire and may be released more than
** once. They are single-caller objects (concurrent release of one token is not
** supported). The stable table header outlives every admitted token, while the
** wrapper need not; fini/fini_all and every slot-removal operation mechanically
** refuse a nonzero reader count. */
struct LJHugeReader {
  LJHugeTabHdr *h;
  void *base;
  uint32_t size;
};

typedef struct GCAhdr {
  uint32_t flags;
  uint32_t owner_tid;
  GCArena *next;
  GreyStack *grey;
  uint32_t sweep_epoch;
  uint32_t live_cells;
  void *gc2_tabstamp;
  uint64_t retire_epoch;
  uint32_t reclaim_cell;
  uint32_t reclaim_deferred;
  uint64_t remote_active;
  LJArenaRemoteFree *remote_free;
  void *retire_obj;  /* Exact GC header for variable-offset huge bodies. */
  void *progress_g;  /* Immutable global_State used for progress wakes. */
  uint32_t prep_bump_cell;  /* Detached bump tail pending PREP commit. */
  uint32_t prep_bump_end;
  /* Terminal THREAD destructors transfer semantic preparation outside the
  ** exclusive GC2 writer after lifetime FREE. This count is the explicit
  ** destructor-incomplete pin which prevents quarantine bitmap commit while
  ** such a body is still queue-owned. */
  uint32_t gcprep_pending;
  /* Terminal unmap closure. Published before the remote_active terminal CAS;
  ** rescue admission rechecks it after any losing CAS retry. */
  uint32_t terminal_closed;
  uint8_t pad[24];
} GCAhdr;

/*
** Two-bit per-cell state used only after an arena enters NEEDSWEEP. The
** WHITE->RETIRED CAS is itself the collector claim/retire LP, so a distinct
** persistent CLAIM state is unnecessary. FREEING plus the arena bitmap is the
** terminal state until rebuild; no persistent DEAD value is needed.
*/
#define LJ_ARENA_SWEEP_CELLS_PER_WORD	32u
#define LJ_ARENA_SWEEP_WORDS \
  (LJ_ARENA_CELLS / LJ_ARENA_SWEEP_CELLS_PER_WORD)

/* Allocation-free fallback work uses an independent two-bit state per arena
** cell. Keeping this separate from sweep[] makes recovery durable across
** MARK/WEAK/SWEEP transitions and lets a producer redirty an object while a
** worker owns its current traversal. Only allocation starts may leave IDLE. */
#define LJ_ARENA_RECOVERY_CELLS_PER_WORD	32u
#define LJ_ARENA_RECOVERY_WORDS \
  (LJ_ARENA_CELLS / LJ_ARENA_RECOVERY_CELLS_PER_WORD)

/* Persistent intrusive-root membership. This is independent of sweep and
** recovery ownership and survives every collector phase. The bit encoding is
** intentional: x64 fresh-object publishers can claim/commit by setting the
** low/high bit respectively, while generic paths use the masked CAS helper. */
#define LJ_ARENA_ROOT_CELLS_PER_WORD	32u
#define LJ_ARENA_ROOT_WORDS \
  (LJ_ARENA_CELLS / LJ_ARENA_ROOT_CELLS_PER_WORD)

/* Per-start allocation lifetime arbitration for traversable small objects.
** Interior cells and every plain-arena cell remain FREE. Four bits provide an
** exact same-word cancel point between semantic rescue and physical free:
** RECOVERY names the counted reserve-before-locator owner, DESTRUCT is a
** tentative free, RESCUE is readable semantic cancellation, and MUTATING is
** non-coalescible non-destructive body/layout/root ownership (including a
** drain whose durable identity is already in the recovery plane). */
#define LJ_ARENA_LIFETIME_CELLS_PER_WORD	16u
#define LJ_ARENA_LIFETIME_WORDS \
  (LJ_ARENA_CELLS / LJ_ARENA_LIFETIME_CELLS_PER_WORD)

/* Authoritative allocation/destructor class for traversable starts. The
** binary encoding is bit-sliced into four ordinary bitmap planes: this keeps
** dynamic x64 publication to one BTS for the hot one-hot classes while still
** leaving fifteen nonzero classes for the complete GC object family. A kind
** is immutable from constructor publication until physical destruction has
** completed and the allocation boundary is removed; object body bytes are
** validation only and never select a destructor.
**
** The first tranche deliberately assigns one-hot codes to the three hot
** closure shapes. The remaining encodings are reserved for generic GC
** destructor classes, not available for type-local marker reuse. */
#define LJ_ARENA_DTOR_PLANES	4u
#define LJ_ARENA_DTOR_NONE	0u
#define LJ_ARENA_DTOR_LFUNC1	1u
#define LJ_ARENA_DTOR_CLOSED_UV	2u
#define LJ_ARENA_DTOR_LFUNC0	4u
#define LJ_ARENA_DTOR_MAX	15u

enum {
  LJ_ARENA_LIFETIME_FREE = 0,
  LJ_ARENA_LIFETIME_LIVE = 1,
  LJ_ARENA_LIFETIME_CONSTRUCT = 2,
  /* Keep encoding 3 for the x64 constructor/recovery crossover classifier. */
  LJ_ARENA_LIFETIME_RECOVERY = 3,
  LJ_ARENA_LIFETIME_DESTRUCT = 4,
  LJ_ARENA_LIFETIME_RESCUE = 5,
  /* Non-destructive ownership without an implied initial reservation. */
  LJ_ARENA_LIFETIME_MUTATING = 6
};

enum {
  LJ_ARENA_ROOT_NONE = 0,
  LJ_ARENA_ROOT_LINKING = 1,
  LJ_ARENA_ROOT_UNLINKING = 2,
  LJ_ARENA_ROOT_MEMBER = 3
};

enum {
  LJ_ARENA_RECOVERY_IDLE = 0,
  LJ_ARENA_RECOVERY_PENDING = 1,
  LJ_ARENA_RECOVERY_CLAIMED = 2,
  LJ_ARENA_RECOVERY_REDIRTY = 3
};

enum {
  LJ_ARENA_SWEEP_WHITE = 0,
  LJ_ARENA_SWEEP_LIVE = 1,
  LJ_ARENA_SWEEP_RETIRED = 2,
  LJ_ARENA_SWEEP_FREEING = 3
};

enum {
  LJ_ARENA_FINISH_NONE = 0,
  LJ_ARENA_FINISH_COMMITTED,
  LJ_ARENA_FINISH_ACTIONABLE,
  LJ_ARENA_FINISH_UNCLASSIFIED,
  LJ_ARENA_FINISH_EPOCH,
  LJ_ARENA_FINISH_PUBLISHER
};

enum {
  LJ_ARENA_RESCUE_RETRY = 0,
  LJ_ARENA_RESCUE_FULL = 1,
  LJ_ARENA_RESCUE_BIT_ONLY = 2,
  LJ_ARENA_RESCUE_COMMITTED = 3
};

struct GCArena {
  GCAhdr hdr;
  uint64_t block[LJ_ARENA_WORDS];
  uint64_t mark[LJ_ARENA_WORDS];
  uint64_t sweep[LJ_ARENA_SWEEP_WORDS];
  /* Durable GC2 work identity. This plane is never allocator scratch and may
  ** only be cleared by the recovery owner through the state CAS protocol. */
  uint64_t recovery[LJ_ARENA_RECOVERY_WORDS];
  /* Allocation-lifetime ownership-spine state. Only allocation starts may be
  ** non-NONE; allocator free/reuse must never erase a live/transient claim. */
  uint64_t root[LJ_ARENA_ROOT_WORDS];
  /* Physical allocation-lifetime ownership. Only traversable allocation
  ** starts may be non-FREE; unlike root[], nonzero never implies root
  ** membership or semantic liveness. */
  uint64_t lifetime[LJ_ARENA_LIFETIME_WORDS];
  /* Bit-sliced authoritative destructor kind. Only allocation starts may be
  ** nonzero. The later block release-publishes a completely installed kind;
  ** lifetime owns the live incarnation, then sealed structural ownership and
  ** block removal prevent reuse while the terminal planes are cleared. */
  uint64_t dtor[LJ_ARENA_DTOR_PLANES][LJ_ARENA_WORDS];
  /* Closed-window remote-free deduplication. Unlike sweep[], this bitmap may
  ** be published after terminal bitmap commit. A bit naming committed live
  ** storage persists across adoption and is consumed by the next sweep. */
  uint64_t late[LJ_ARENA_WORDS];
  /* Per-cell allocation coverage for every fixed/interior cdata body. READY
  ** remains start-only and is published after the complete coverage range and
  ** descriptor. Boundaries plus coverage and GCcdata's byte-tail field give an
  ** exact extent without taxing ordinary Lua allocation hot paths. */
  uint64_t cdata[LJ_ARENA_WORDS];
  /* Header-discovery publication for traversable allocation starts. block=1
  ** and ready=0 is a constructor-owned pending allocation: sweep pins it as
  ** opaque storage and typed readers must not inspect payload bytes. */
  uint64_t ready[LJ_ARENA_WORDS];
};

static LJ_AINLINE uint32_t lj_arena_owner_acq(const GCArena *a)
{
  return la_load32_acq(&a->hdr.owner_tid);  /* 04 section 4.6 owner route. */
}

static LJ_AINLINE uint32_t lj_arena_flags_acq(const GCArena *a)
{
  return la_load32_acq(&a->hdr.flags);
}

static LJ_AINLINE void lj_arena_owner_rel(GCArena *a, uint32_t owner_tid)
{
  la_store32_rel(&a->hdr.owner_tid, owner_tid);  /* 04 section 4.6. */
}

static LJ_AINLINE void *lj_arena_progress_g_acq(const GCArena *a)
{
  return la_loadptr_acq((void *const *)&a->hdr.progress_g);
}

static LJ_AINLINE void lj_arena_progress_g_rel(GCArena *a, void *g)
{
  la_storeptr_rel((void **)&a->hdr.progress_g, g);
}

static LJ_AINLINE GCArena *lj_arena_next_acq(const GCArena *a)
{
  return (GCArena *)la_loadptr_acq((void *const *)&a->hdr.next);
}

static LJ_AINLINE void lj_arena_next_rel(GCArena *a, GCArena *next)
{
  la_storeptr_rel((void **)&a->hdr.next, next);
}

static LJ_AINLINE void *lj_arena_gc2_tabstamp_acq(const GCArena *a)
{
  return la_loadptr_acq((void *const *)&a->hdr.gc2_tabstamp);
}

static LJ_AINLINE int lj_arena_gc2_tabstamp_cas(GCArena *a, void **oldp,
						void *tabstamp)
{
  return la_casptr((void **)&a->hdr.gc2_tabstamp, oldp, tabstamp,
		   LA_ACQ_REL, LA_ACQ);
}

struct LJArenaFreeRun {
  LJArenaFreeRun *next;
  uint32_t start;
  uint32_t len;
};

/* Intrusive remote-free record stored in the first dead allocation cell. */
struct LJArenaRemoteFree {
  LJArenaRemoteFree *next;
  /* Packed 12-bit start/length metadata. Byte 9 is deliberately zero, which
  ** overlaps GChead.gct when this intrusive tombstone occupies a dead object
  ** body. Stale type probes therefore fail closed instead of interpreting a
  ** cell-index byte as a live GC type. */
  uint32_t meta;
  uint32_t reserved;
};

struct LJArenaBump {
  GCArena *a;
  uint32_t cell;
  uint32_t end;
};

struct TGAlloc {
  LJArenaBump bump[LJ_ARENA_NKINDS];
  LJArenaFreeRun *bins[LJ_ARENA_NKINDS][LJ_ALLOC_NBINS];
  uint32_t binmask[LJ_ARENA_NKINDS];
  GCArena *owned[LJ_ARENA_NKINDS];
  GCArena *needsweep[LJ_ARENA_NKINDS];
  GCArena *quarantine[LJ_ARENA_NKINDS];
  GCArena *reclaimed[LJ_ARENA_NKINDS];
  HugeTab *smalltab;
  uint32_t sweep_epoch;
  uint32_t prepare_epoch;
  uint32_t huge_retire_cursor;
  uint32_t huge_reclaim_cursor;
  uint8_t huge_retire_done;
  /* Advisory allocator-wide wake for intrusive remote-free queues. A remote
  ** producer release-publishes this only after its arena-local queue CAS.
  ** Opportunistic drains consume it before walking owned[], while transfer,
  ** sweep and terminal owners deliberately bypass it. */
  uint32_t remote_pending;
  uint32_t owner_tid;
  void *owner_tg;
  uint8_t alloc_black;
  uint8_t free_noinsert;
};

static LJ_AINLINE uint32_t lj_arena_remote_pending_acq(
  const TGAlloc *alloc)
{
  return la_load32_acq(&alloc->remote_pending);
}

static LJ_AINLINE void lj_arena_remote_pending_rel(TGAlloc *alloc,
						    uint32_t pending)
{
  la_store32_rel(&alloc->remote_pending, pending);
}

static LJ_AINLINE uint32_t lj_arena_remote_pending_xchg_acqrel(
  TGAlloc *alloc, uint32_t pending)
{
  return la_xchg32_acqrel(&alloc->remote_pending, pending);
}

static LJ_AINLINE uint32_t lj_arena_alloc_owner_acq(const TGAlloc *alloc)
{
  return la_load32_acq(&alloc->owner_tid);  /* 04 section 4.6 owner route. */
}

static LJ_AINLINE void lj_arena_alloc_owner_rel(TGAlloc *alloc,
						uint32_t owner_tid)
{
  la_store32_rel(&alloc->owner_tid, owner_tid);  /* 04 section 4.6. */
}

static LJ_AINLINE void lj_arena_alloc_owner_tg_rel(TGAlloc *alloc,
						    void *owner_tg)
{
  la_storeptr_rel((void **)&alloc->owner_tg, owner_tg);
}

static LJ_AINLINE void *lj_arena_alloc_owner_tg_acq(const TGAlloc *alloc)
{
  return la_loadptr_acq((void *const *)&alloc->owner_tg);
}

static LJ_AINLINE uint8_t lj_arena_alloc_black_acq(const TGAlloc *alloc)
{
  return la_load8_acq(&alloc->alloc_black);  /* 05 section 5.5 alloc color. */
}

static LJ_AINLINE void lj_arena_alloc_black_rel(TGAlloc *alloc,
						uint8_t alloc_black)
{
  la_store8_rel(&alloc->alloc_black, alloc_black);  /* 05 section 5.5. */
}

static LJ_AINLINE uint8_t lj_arena_alloc_free_noinsert_acq(
  const TGAlloc *alloc)
{
  return la_load8_acq(&alloc->free_noinsert);
}

static LJ_AINLINE void lj_arena_alloc_free_noinsert_rel(TGAlloc *alloc,
							uint8_t enabled)
{
  la_store8_rel(&alloc->free_noinsert, enabled);
}

static LJ_AINLINE uint32_t lj_arena_alloc_binmask(const TGAlloc *alloc,
						  uint32_t kind)
{
  return kind < LJ_ARENA_NKINDS ? alloc->binmask[kind] : 0;
}

static LJ_AINLINE int lj_arena_alloc_has_run_ge(const TGAlloc *alloc,
						uint32_t kind, uint32_t ncells)
{
  return (lj_arena_alloc_binmask(alloc, kind) &
	  lj_arena_binmask_from_ncells(ncells)) != 0;
}

struct LJHugeInfo {
  size_t size;
  uint32_t flags;
  uint32_t readers;
};

struct LJArenaAllocD {
  TGAlloc *alloc;
  PRNGState *prng;
  HugeTab *huge;
  uint32_t flags;
};

#define LJ_HUGEF_MARK		0x01u
#define LJ_HUGEF_TRAVERSABLE	0x02u
#define LJ_HUGEF_FINALIZER	0x04u
#define LJ_HUGEF_SWEEP_OLD	0x08u
#define LJ_HUGEF_RETIRED	0x10u
#define LJ_HUGEF_FREEING	0x20u
#define LJ_HUGEF_TICKET		0x40u
#define LJ_HUGEF_BUSY		0x80u
#define LJ_HUGEF_INTERIOR_CDATA	0x100u
#define LJ_HUGEF_READY		0x200u
#define LJ_HUGEF_CDATA		0x400u
#define LJ_HUGEF_RECOVERY_SHIFT	11u
#define LJ_HUGEF_RECOVERY_MASK	(0x03u << LJ_HUGEF_RECOVERY_SHIFT)
#define LJ_HUGEF_DEFER_FREE	0x2000u
#define LJ_HUGEF_ROOT_SHIFT	14u
#define LJ_HUGEF_ROOT_MASK	(0x03u << LJ_HUGEF_ROOT_SHIFT)
#define LJ_HUGEF_MASK \
  (LJ_HUGEF_MARK|LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_FINALIZER| \
   LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING| \
   LJ_HUGEF_TICKET|LJ_HUGEF_BUSY|LJ_HUGEF_INTERIOR_CDATA|LJ_HUGEF_READY| \
   LJ_HUGEF_CDATA|LJ_HUGEF_RECOVERY_MASK|LJ_HUGEF_DEFER_FREE| \
   LJ_HUGEF_ROOT_MASK)

static LJ_AINLINE uint32_t lj_arena_huge_recovery_state(uint32_t flags)
{
  return (flags & LJ_HUGEF_RECOVERY_MASK) >> LJ_HUGEF_RECOVERY_SHIFT;
}

static LJ_AINLINE uint32_t lj_arena_huge_root_state(uint32_t flags)
{
  return (flags & LJ_HUGEF_ROOT_MASK) >> LJ_HUGEF_ROOT_SHIFT;
}
/* A containing-object marker published MARK while the retire owner held BUSY,
** but deliberately did not inspect or return the still-unpublished header.
** The unique retire owner preserves MARK in its TICKET publication and must
** arrange one later traversal of retire_obj. basep remains NULL for this result. */
#define LJ_ARENA_HUGE_MARK_INTENT	3
#define LJ_ARENA_REGISTRY_BITS	16u

/* The header, block/mark bitmaps, two-bit sweep sidecar, closed-window
** remote-free bitmap, interior-cdata identity and ready-publication planes
** precede allocation cells. */
#define LJ_ARENA_META_BYTES	((uint32_t)sizeof(GCArena))
#define LJ_AFIRST_CELL \
  ((uint32_t)((sizeof(GCArena) + LJ_CELL_SIZE-1) >> LJ_CELL_SHIFT))

typedef void (*LJArenaRunCB)(uint32_t start, uint32_t len, void *ud);

LJ_FUNC void lj_arena_sweep_words(GCArena *a, int preserve_marks);
LJ_FUNC void lj_arena_scan_free_runs(const GCArena *a, LJArenaRunCB cb, void *ud);
LJ_FUNC uint32_t lj_arena_count_free_runs(const GCArena *a);
LJ_FUNC GCArena *lj_arena_map(PRNGState *rs, uint32_t flags);
LJ_FUNC void lj_arena_unmap(GCArena *a);
LJ_FUNC size_t lj_arena_huge_mapsize(size_t size);
LJ_FUNC void *lj_arena_huge_map(PRNGState *rs, size_t size, uint32_t flags);
LJ_FUNC void lj_arena_huge_unmap(void *p, size_t size);
LJ_FUNC int lj_arena_hugetab_init(HugeTab *ht, uint32_t hbits);
LJ_FUNC void lj_arena_hugetab_fini(HugeTab *ht);
LJ_FUNC uint32_t lj_arena_hugetab_fini_all(HugeTab *ht);
LJ_FUNC int lj_arena_hugetab_forget_terminal(HugeTab *ht, const void *p,
					      LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_insert(HugeTab *ht, void *p, size_t size,
				    uint32_t hflags);
LJ_FUNC int lj_arena_hugetab_lookup(HugeTab *ht, const void *p,
				    LJHugeInfo *hi);
/* Recovery lookup returns -1 for a missing mapping or one of the four
** LJ_ARENA_RECOVERY_* states. CAS preserves every non-recovery metadata bit.
** IDLE->PENDING callers must already own the ordinary mapping-lifetime
** admission. The iterator returns only stable non-IDLE entries; that state
** prevents delete/free/realloc ownership until a recovery owner clears it. */
LJ_FUNC int lj_arena_hugetab_recovery_state_acq(HugeTab *ht,
						 const void *p,
						 LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_recovery_state_cas(HugeTab *ht,
						 const void *p,
						 uint32_t from,
						 uint32_t to,
						 LJHugeInfo *hi);
/* Root-membership transitions preserve every other HugeTab metadata bit. */
LJ_FUNC int lj_arena_hugetab_root_state_acq(HugeTab *ht, const void *p,
					     LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_root_state_cas(HugeTab *ht, const void *p,
					     uint32_t from, uint32_t to,
					     LJHugeInfo *hi);
/* Complete a root-state transition without dropping a racing logical free.
** An ordinary transition returns LIVE. Any non-NONE->NONE completion folds a
** racing DEFER_FREE into a fresh-grace sweep handoff and returns SWEEP (this
** includes LINKING rollback). LOST retains the mapping and is safe to retry.
** A zero retire_epoch requests the all-ones fresh sentinel. */
#define LJ_ARENA_HUGE_ROOT_COMPLETE_LOST	0
#define LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE	1
#define LJ_ARENA_HUGE_ROOT_COMPLETE_SWEEP	2
LJ_FUNC int lj_arena_hugetab_root_complete(HugeTab *ht, const void *p,
					    uint32_t from, uint32_t to,
					    uint64_t retire_epoch,
					    LJHugeInfo *hi);
/* Root-construction insertion publishes LINKING in the initial full-slot CAS.
** Commit advances LINKING->MEMBER. Abandon folds a racing deferred free just
** like root_complete and therefore returns one of the ROOT_COMPLETE results. */
LJ_FUNC int lj_arena_hugetab_root_construct_commit(HugeTab *ht,
						     const void *p,
						     LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_root_construct_abandon(HugeTab *ht,
						      const void *p,
						      uint64_t retire_epoch,
						      LJHugeInfo *hi);
/* Complete a CLAIMED huge recovery identity. A racing logical free is folded
** into the same full-slot transition when this is the final owner. If counted
** readers remain, IDLE|DEFER_FREE intentionally transfers that responsibility
** to the last reader. SWEEP publishes FREEING|SWEEP_OLD with a fresh-grace
** sentinel. UNMAP is reserved for a future proven-exclusive handoff and is
** not currently emitted. REQUEUED leaves the identity count unchanged and
** requires another drain. */
#define LJ_ARENA_HUGE_RECOVERY_COMPLETE_LOST	0
#define LJ_ARENA_HUGE_RECOVERY_COMPLETE_LIVE	1
#define LJ_ARENA_HUGE_RECOVERY_COMPLETE_SWEEP	2
#define LJ_ARENA_HUGE_RECOVERY_COMPLETE_UNMAP	3
#define LJ_ARENA_HUGE_RECOVERY_COMPLETE_REQUEUED	4
LJ_FUNC int lj_arena_hugetab_recovery_complete(HugeTab *ht,
						 const void *p,
						 LJHugeInfo *hi);
/* Terminal-only recovery reconciliation, after every mutator/worker has
** joined. A normal identity is atomically cleared for later table teardown;
** DEFER_FREE is atomically tombstoned and transfers unmap ownership to the
** caller. Each success consumes exactly one external recovery count. */
#define LJ_ARENA_HUGE_RECOVERY_TERMINAL_LOST	0
#define LJ_ARENA_HUGE_RECOVERY_TERMINAL_CLEARED	1
#define LJ_ARENA_HUGE_RECOVERY_TERMINAL_UNMAP	2
LJ_FUNC int lj_arena_hugetab_recovery_discard_terminal(HugeTab *ht,
							 const void *p,
							 LJHugeInfo *hi);
/* Iterate every live table entry. Each return is a no-op full-slot-CAS
** snapshot; the caller must still validate semantic flags/lifetime before
** reading the named mapping. */
LJ_FUNC int lj_arena_hugetab_next(HugeTab *ht, uint32_t *cursor,
				   void **pp, LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_recovery_next(HugeTab *ht, uint32_t *cursor,
					    void **pp, LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_range_lookup(HugeTab *ht, const void *p,
					  void **basep, LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_mark(HugeTab *ht, const void *p,
					  LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_mark_range(HugeTab *ht, const void *p,
					void **basep, LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_cdata_range_lookup(HugeTab *ht, const void *p,
						 void **basep, LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_mark_cdata_range(HugeTab *ht, const void *p,
					       void **basep, LJHugeInfo *hi);
/* Counted body admissions use one full-slot CAS to validate address, 32-bit
** authoritative size, flags and reader count. Therefore a successful return
** closes the old lookup-to-header deletion window: base remains mapped and no
** destructive BUSY/FREEING/tombstone/transfer can succeed until release.
**
** The exact/range reader APIs do not change MARK. The cdata variants require
** a published traversable cdata identity; the generic range variant accepts
** any allocation containing p. Mark-reader variants additionally perform the
** corresponding semantic MARK transition and return the same 0/1/2 results
** as the metadata-only mark APIs. MARK_INTENT records liveness behind a
** pre-ticket retire owner but deliberately returns no reader token or base.
** MARK_SATURATED likewise publishes MARK but returns no token when the bounded
** reader field is full; the caller must arrange a later traversal/retry and
** must not inspect body bytes from that result.
**
** Reader acquire returns ACQUIRED, MISSING, or OVERFLOW. Mark-reader acquire
** returns -1 for rejection/missing, MARK_SATURATED for a full bounded counter,
** MARK_INTENT, or the ordinary mark result. A failed acquire leaves a
** zero-initialized token untouched. DEFER_FREE rejects every new admission.
** Release clears the token before its CAS and is idempotent. The last reader
** either drops the count or atomically hands an irrevocable DEFER_FREE to
** FREEING|SWEEP_OLD; root/recovery/BUSY owners may instead retain the durable
** bit for their own completion CAS. No API waits or yields. */
#define LJ_ARENA_HUGE_READER_OVERFLOW	(-2)
#define LJ_ARENA_HUGE_READER_MISSING	0
#define LJ_ARENA_HUGE_READER_ACQUIRED	1
#define LJ_ARENA_HUGE_READER_RELEASE_LOST	0
#define LJ_ARENA_HUGE_READER_RELEASED	1
#define LJ_ARENA_HUGE_READER_HANDOFF	2
#define LJ_ARENA_HUGE_MARK_SATURATED	4
LJ_FUNC int lj_arena_hugetab_reader_acquire(HugeTab *ht, const void *p,
					      LJHugeReader *reader,
					      LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_reader_range_acquire(HugeTab *ht,
						    const void *p,
						    void **basep,
						    LJHugeReader *reader,
						    LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_reader_cdata_range_acquire(HugeTab *ht,
						          const void *p,
						          void **basep,
						          LJHugeReader *reader,
						          LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_mark_reader_acquire(HugeTab *ht,
						   const void *p,
						   LJHugeReader *reader,
						   LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_mark_range_reader_acquire(HugeTab *ht,
						         const void *p,
						         void **basep,
						         LJHugeReader *reader,
						         LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_mark_cdata_range_reader_acquire(
  HugeTab *ht, const void *p, void **basep, LJHugeReader *reader,
  LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_reader_release(LJHugeReader *reader,
					      LJHugeInfo *hi);
/* Pure token geometry: no HugeTab wrapper, slot, mapping header or payload is
** dereferenced. covers_range accepts a zero-length range at base+size. */
LJ_FUNC int lj_arena_hugetab_reader_covers(const LJHugeReader *reader,
					     const void *p);
LJ_FUNC int lj_arena_hugetab_reader_covers_range(const LJHugeReader *reader,
						   const void *p,
						   size_t size);
LJ_FUNC int lj_arena_hugetab_publish_gco(HugeTab *ht, const void *p);
LJ_FUNC int lj_arena_hugetab_publish_cdata(HugeTab *ht, const void *p,
					    int interior);
LJ_FUNC int lj_arena_hugetab_publish_interior_cdata(HugeTab *ht,
						     const void *p);
LJ_FUNC void lj_arena_hugetab_clear_marks(HugeTab *ht);
LJ_FUNC void lj_arena_hugetab_prepare_sweep(HugeTab *ht);
LJ_FUNC void lj_arena_hugetab_abort_sweep(HugeTab *ht);
LJ_FUNC void lj_arena_hugetab_finish_sweep(HugeTab *ht,
					    int preserve_marks);
LJ_FUNC int lj_arena_hugetab_sweep_next(HugeTab *ht, uint32_t *cursor,
					void **pp, LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_has_sweep_old(HugeTab *ht);
/* Retire returns 0 on failure, 1 after publishing/reusing an ordinary TICKET,
** or 2 whenever this unique retire owner's final TICKET contains MARK. MARK
** provenance is not encoded: it may predate BUSY or arrive as BUSY-window
** intent. Return 2 always requires a semantic traversal from exact retire_obj;
** a pre-BUSY mark may therefore cause a conservative duplicate walk. */
LJ_FUNC int lj_arena_hugetab_retire(HugeTab *ht, const void *p,
				    const void *obj, uint64_t retire_epoch,
				    LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_claim_freeing(HugeTab *ht, const void *p,
					    LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_claim_live_ticket(HugeTab *ht, const void *p,
					       LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_finish_live_ticket(HugeTab *ht, const void *p,
						LJHugeInfo *hi);
/* External free/realloc owns the mapping between claim and finish. Finish
** returns UNMAP only when the caller atomically removed a pre-sweep entry;
** DEFERRED leaves a sweep-old entry for the sole sweep deleter. */
#define LJ_ARENA_HUGE_FINISH_LOST	0
#define LJ_ARENA_HUGE_FINISH_DEFERRED	1
#define LJ_ARENA_HUGE_FINISH_UNMAP	2
LJ_FUNC int lj_arena_hugetab_claim_external_free(HugeTab *ht,
						   const void *p,
						   LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_finish_external_free(HugeTab *ht,
						    const void *p,
						    LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_defer_external_free(HugeTab *ht,
						  const void *p,
						  LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_revert_retired(HugeTab *ht, const void *p);
LJ_FUNC uint64_t lj_arena_hugetab_live_bytes(HugeTab *ht,
					     uint32_t required_flags);
LJ_FUNC int lj_arena_hugetab_transfer(HugeTab *dst, HugeTab *src,
				      uint32_t owner_tid);
LJ_FUNC int lj_arena_hugetab_delete(HugeTab *ht, const void *p,
				    LJHugeInfo *hi);
#if defined(LJ_ARENA_TEST_HELPERS)
LJ_FUNC void lj_arena_hugetab_test_retire_pause(int enabled);
LJ_FUNC uint32_t lj_arena_hugetab_test_retire_paused(void);
LJ_FUNC void lj_arena_test_plain_late_pause(int enabled);
LJ_FUNC uint32_t lj_arena_test_plain_late_paused(void);
LJ_FUNC void lj_arena_test_registry_pause(int enabled);
LJ_FUNC uint32_t lj_arena_test_registry_paused(void);
LJ_FUNC void lj_arena_test_plain_claim_pause(int enabled);
LJ_FUNC uint32_t lj_arena_test_plain_claim_paused(void);
LJ_FUNC void lj_arena_test_plain_admit_pause(int enabled);
LJ_FUNC uint32_t lj_arena_test_plain_admit_paused(void);
LJ_FUNC void lj_arena_test_remote_publish_pause(int enabled);
LJ_FUNC uint32_t lj_arena_test_remote_publish_paused(void);
LJ_FUNC void lj_arena_test_remote_drain_pause(int enabled);
LJ_FUNC uint32_t lj_arena_test_remote_drain_paused(void);
LJ_FUNC void lj_arena_test_remote_stats_reset(void);
LJ_FUNC uint64_t lj_arena_test_remote_fast_skips(void);
LJ_FUNC uint64_t lj_arena_test_remote_arena_probes(void);
LJ_FUNC uint64_t lj_arena_test_adopt_whole_count(void);
LJ_FUNC int lj_arena_test_set_free_run(GCArena *a, uint32_t start,
					uint32_t len);
LJ_FUNC int lj_arena_test_terminal_freeing_word(const GCArena *a,
						 uint32_t word);
LJ_FUNC int lj_arena_test_quarantine_apply_bitmap(GCArena *a,
						   int preserve_marks);
#endif
#if defined(LJ_ARENA_TEST_HELPERS) || defined(LJ_GC2_TEST_HELPERS)
LJ_FUNC void lj_arena_test_lifetime_pause(int enabled);
LJ_FUNC uint32_t lj_arena_test_lifetime_paused(void);
#endif
LJ_FUNC void lj_arena_alloc_set_registry(TGAlloc *alloc, HugeTab *tab);
LJ_FUNC HugeTab *lj_arena_alloc_registry_acq(const TGAlloc *alloc);
LJ_FUNC int lj_arena_alloc_registry_lookup(const TGAlloc *alloc,
					   const GCArena *a,
					   LJHugeInfo *hi);
/* Bridge a counted registry-slot reader into the arena's remote admission.
** No arena/header byte is read before the HugeTab reader CAS succeeds. The
** registry count remains held until rescue_enter has modified remote_active,
** so terminal registry deletion and arena unmap cannot both miss the reader. */
LJ_FUNC int lj_arena_hugetab_rescue_enter(HugeTab *registry, GCArena *a,
					    LJHugeInfo *hi);
LJ_FUNC int lj_arena_alloc_register_existing(TGAlloc *alloc);
LJ_FUNC void lj_arena_alloc_init(TGAlloc *alloc);
/* Exact-arena form for the immediate pre-destructor check after an earlier
** terminal destructor recreated a conservative gate intent. The caller owns
** joined-world terminal authority. It accepts OPEN/CLOSED, CASes only exact
** count-zero CLOSED|PENDING to CLOSED, and otherwise fails without inspecting
** or changing any allocation side plane. */
LJ_FUNC int lj_arena_terminal_reconcile(GCArena *a);
/* Quiescent terminal PRE pass. Visits every allocator list without detaching
** it and CASes only exact count-zero CLOSED|PENDING to CLOSED. OPEN/CLOSED are
** already quiet; any admission, SEALED writer, terminal-unmap claim or other
** bit pattern fails the pass. No allocation side plane is inspected or
** changed, so late/root/recovery/lifetime ownership remains authoritative. */
LJ_FUNC int lj_arena_alloc_terminal_reconcile(TGAlloc *alloc);
LJ_FUNC void lj_arena_alloc_fini(TGAlloc *alloc);
LJ_FUNC void lj_arena_alloc_clear_marks(TGAlloc *alloc);
LJ_FUNC void lj_arena_alloc_rebuild_free_kind(TGAlloc *alloc, uint32_t kind);
LJ_FUNC void lj_arena_alloc_rebuild_free(TGAlloc *alloc);
LJ_FUNC int lj_arena_alloc_prepare_sweep_kind(TGAlloc *alloc, uint32_t kind);
LJ_FUNC void lj_arena_alloc_prepare_sweep(TGAlloc *alloc);
LJ_FUNC int lj_arena_alloc_restore_sweep_kind(TGAlloc *alloc, uint32_t kind);
LJ_FUNC GCArena *lj_arena_alloc_quarantine_one(TGAlloc *alloc, uint32_t kind,
						uint64_t retire_epoch);
LJ_FUNC GCArena *lj_arena_alloc_quarantine_head(const TGAlloc *alloc,
						 uint32_t kind);
LJ_FUNC GCArena *lj_arena_alloc_reclaimed_head(const TGAlloc *alloc,
					      uint32_t kind);
LJ_FUNC int lj_arena_alloc_quarantine_finish(TGAlloc *alloc, uint32_t kind,
					      GCArena *a, uint32_t sweep_epoch,
					      int preserve_marks,
					      uint32_t *reasonp);
LJ_FUNC void lj_arena_alloc_sweep_kind(TGAlloc *alloc, uint32_t kind,
					    uint32_t epoch, int preserve_marks);
LJ_FUNC GCArena *lj_arena_sweep_one(TGAlloc *alloc, uint32_t kind,
				    uint32_t epoch, int preserve_marks);
LJ_FUNC uint32_t lj_arena_alloc_transfer(TGAlloc *dst, TGAlloc *src);
LJ_FUNC int lj_arena_reserve_bump(TGAlloc *alloc, PRNGState *rs,
				  uint32_t flags, uint32_t ncells,
				  GCArena **ap, uint32_t *cellp);
LJ_FUNC int lj_arena_reserve_bump_pair(TGAlloc *alloc, PRNGState *rs,
				       uint32_t flags, uint32_t ncells,
				       uint32_t second_offset,
				       GCArena **ap, uint32_t *cellp);
LJ_FUNC int lj_arena_reserve_bump_dtor(TGAlloc *alloc, PRNGState *rs,
				       uint32_t flags, uint32_t ncells,
				       uint32_t dtor_kind,
				       GCArena **ap, uint32_t *cellp);
LJ_FUNC int lj_arena_reserve_bump_dtor_pair(TGAlloc *alloc, PRNGState *rs,
					    uint32_t flags,
					    uint32_t ncells,
					    uint32_t second_offset,
					    uint32_t first_kind,
					    uint32_t second_kind,
					    GCArena **ap,
					    uint32_t *cellp);
LJ_FUNC void *lj_arena_alloc(TGAlloc *alloc, PRNGState *rs, size_t size,
			     uint32_t flags);
LJ_FUNC int lj_arena_free_deferred(TGAlloc *alloc, void *p, size_t size);
LJ_FUNC void lj_arena_free(TGAlloc *alloc, void *p, size_t size);
LJ_FUNC int lj_arena_remote_free_publish(TGAlloc *alloc, void *p,
					 size_t size);
/* Joined-world/ownership-transfer drain. Unlike the opportunistic allocator
** form, this always scans owned[] even when the advisory wake is clear. */
LJ_FUNC uint32_t lj_arena_remote_free_drain_force(TGAlloc *alloc);
LJ_FUNC uint32_t lj_arena_remote_free_drain(TGAlloc *alloc);
LJ_FUNC uint32_t lj_arena_remote_free_drain_sweep(TGAlloc *alloc,
						   GCArena *a);
LJ_FUNC int lj_arena_remote_sweep_busy_acq(const GCArena *a);
#define LJ_ARENA_DESTRUCT_LOST		0
#define LJ_ARENA_DESTRUCT_ACQUIRED	1
#define LJ_ARENA_DESTRUCT_OWNED		2
/* Pre-destructor physical ownership. Only ACQUIRED authorizes one semantic
** destructor invocation; OWNED requires the exact FREE+FREEING terminal pair.
** LOST includes mere late intent or a semantic RESCUE/non-destructive owner,
** and no body byte may be touched. A plain-arena ACQUIRED result retains its
** SEALED writer token across the destructor; the same owner must finish with
** lj_arena_free(), which commits storage and reopens admissions. */
LJ_FUNC int lj_arena_destruct_acquire(const void *p, size_t size);
/* Fresh root construction owns both descriptor planes until commit/abandon.
** Normally commit publishes LIVE before MEMBER and abandon clears LINKING
** before LIVE. If recovery temporarily owns CONSTRUCT->RECOVERY, either helper
** may finish only the root lane; recovery must restore LIVE when it observes
** MEMBER/NONE, or restore CONSTRUCT only while root remains LINKING. */
LJ_FUNC int lj_arena_root_construct_claim(GCArena *a, uint32_t cell);
LJ_FUNC int lj_arena_root_construct_commit(GCArena *a, uint32_t cell);
LJ_FUNC int lj_arena_root_construct_commit_pair(GCArena *a, uint32_t first,
					  uint32_t second);
LJ_FUNC int lj_arena_root_construct_abandon(GCArena *a, uint32_t cell);
/* Rootless typed constructors reserve lifetime CONSTRUCT while root[] remains
** NONE. READY/block publication makes their immutable dtor class discoverable;
** commit then moves CONSTRUCT to LIVE. Recovery may transiently own RECOVERY
** and is required to restore a rootless constructor directly to LIVE. */
LJ_FUNC int lj_arena_dtor_construct_commit(GCArena *a, uint32_t cell);
LJ_FUNC int lj_arena_dtor_construct_commit_pair(GCArena *a, uint32_t first,
						 uint32_t second);
LJ_FUNC int lj_arena_lifetime_empty(const GCArena *a);
/* Quiescent terminal cleanup only. Returns the state it replaced with FREE.
** The caller owns all semantic destruction; this only discards the locator
** lane after no runtime actor can observe or resume the allocation. */
LJ_FUNC uint32_t lj_arena_lifetime_clear_terminal(GCArena *a,
						    uint32_t cell);
LJ_FUNC int lj_arena_reclaim_seal(GCArena *a);
LJ_FUNC int lj_arena_reclaim_clear_pending(GCArena *a);
LJ_FUNC void lj_arena_reclaim_unseal(GCArena *a, int keep_pending);
LJ_FUNC int lj_arena_rescue_enter(GCArena *a);
LJ_FUNC void lj_arena_rescue_leave(GCArena *a);
LJ_FUNC int lj_arena_quarantine_owns_body(const void *p, size_t size);
LJ_FUNC void *lj_arena_realloc(TGAlloc *alloc, PRNGState *rs, void *p,
			       size_t osize, size_t nsize, uint32_t flags);
LJ_FUNC void lj_arena_allocd_init(LJArenaAllocD *ad, TGAlloc *alloc,
				  PRNGState *rs, uint32_t flags);
LJ_FUNC void lj_arena_allocd_sethugetab(LJArenaAllocD *ad, HugeTab *ht);
LJ_FUNC void *lj_arena_allocd_alloc(LJArenaAllocD *ad, size_t size,
				    uint32_t flags);
/* Publish READY for an exact fixed-layout small-arena allocation start. The
** constructor's CONSTRUCT/LIVE/RESCUE lane pins the mapping and incarnation,
** so this local operation deliberately needs no allocator registry reader. */
LJ_FUNC int lj_arena_publish_gco_at(void *p);
LJ_FUNC int lj_arena_allocd_publish_gco(LJArenaAllocD *ad, void *p);
LJ_FUNC int lj_arena_allocd_publish_cdata(LJArenaAllocD *ad, void *p,
					   size_t size, int interior);
LJ_FUNC int lj_arena_allocd_publish_interior_cdata(LJArenaAllocD *ad,
						    void *p, size_t size);
LJ_FUNC void *lj_arena_allocf(void *ud, void *ptr, size_t osize,
			      size_t nsize);

static LJ_AINLINE GCArena *lj_arena_of(const void *p)
{
  return (GCArena *)((uintptr_t)p & ~(uintptr_t)LJ_ARENA_MASK);
}

static LJ_AINLINE uint32_t lj_arena_cellof(const void *p)
{
  return (uint32_t)(((uintptr_t)p & (uintptr_t)LJ_ARENA_MASK) >> LJ_CELL_SHIFT);
}

static LJ_AINLINE void *lj_arena_cellptr(GCArena *a, uint32_t cell)
{
  return (void *)((char *)a + ((uintptr_t)cell << LJ_CELL_SHIFT));
}

static LJ_AINLINE uint32_t lj_arena_bm_get(const uint64_t *bm, uint32_t i)
{
  /* block[] is also the allocation-discovery publication. Acquire is free on
  ** x86-64 and makes a positive observation order later header reads. */
  return (uint32_t)((la_load64_acq(&bm[i >> 6]) >> (i & 63)) & 1u);
}

static LJ_AINLINE uint32_t lj_arena_cdata_get(const GCArena *a, uint32_t i)
{
  return (uint32_t)((la_load64_acq(&a->cdata[i >> 6]) >> (i & 63)) & 1u);
}

static LJ_AINLINE uint32_t lj_arena_ready_get(const GCArena *a, uint32_t i)
{
  return (uint32_t)((la_load64_acq(&a->ready[i >> 6]) >> (i & 63)) & 1u);
}

static LJ_AINLINE uint32_t lj_arena_dtor_kind_acq(const GCArena *a,
						   uint32_t i)
{
  uint32_t plane, kind = LJ_ARENA_DTOR_NONE;
  if (!a || i >= LJ_ARENA_CELLS)
    return LJ_ARENA_DTOR_NONE;
  for (plane = 0; plane < LJ_ARENA_DTOR_PLANES; plane++)
    kind |= (uint32_t)
      ((la_load64_acq(&a->dtor[plane][i >> 6]) >> (i & 63u)) & 1u)
      << plane;
  return kind;
}

static LJ_AINLINE uint32_t lj_arena_late_get(const GCArena *a, uint32_t i)
{
  return (uint32_t)((la_load64_acq(&a->late[i >> 6]) >> (i & 63)) & 1u);
}

static LJ_AINLINE uint32_t lj_arena_sweep_state_acq(const GCArena *a,
						     uint32_t i)
{
  uint32_t shift = (i & (LJ_ARENA_SWEEP_CELLS_PER_WORD-1u)) << 1;
  uint64_t word = la_load64_acq(&a->sweep[i / LJ_ARENA_SWEEP_CELLS_PER_WORD]);
  return (uint32_t)((word >> shift) & 0x03u);
}

static LJ_AINLINE uint32_t lj_arena_recovery_state_acq(const GCArena *a,
							uint32_t i)
{
  uint32_t shift;
  uint64_t word;
  if (!a || i >= LJ_ARENA_CELLS)
    return LJ_ARENA_RECOVERY_IDLE;
  shift = (i & (LJ_ARENA_RECOVERY_CELLS_PER_WORD-1u)) << 1;
  word = la_load64_acq(
    &a->recovery[i / LJ_ARENA_RECOVERY_CELLS_PER_WORD]);
  return (uint32_t)((word >> shift) & 0x03u);
}

static LJ_AINLINE uint32_t lj_arena_lifetime_state_acq(const GCArena *a,
							uint32_t i)
{
  uint32_t shift;
  uint64_t word;
  if (!a || i >= LJ_ARENA_CELLS)
    return LJ_ARENA_LIFETIME_FREE;
  shift = (i & (LJ_ARENA_LIFETIME_CELLS_PER_WORD-1u)) << 2;
  word = la_load64_acq(
    &a->lifetime[i / LJ_ARENA_LIFETIME_CELLS_PER_WORD]);
  return (uint32_t)((word >> shift) & 0x0fu);
}

static LJ_AINLINE int lj_arena_lifetime_state_cas(GCArena *a, uint32_t i,
						    uint32_t from,
						    uint32_t to)
{
  uint32_t wi, shift;
  uint64_t mask, old;
  if (!a || i >= LJ_ARENA_CELLS || from > LJ_ARENA_LIFETIME_MUTATING ||
      to > LJ_ARENA_LIFETIME_MUTATING)
    return 0;
  wi = i / LJ_ARENA_LIFETIME_CELLS_PER_WORD;
  shift = (i & (LJ_ARENA_LIFETIME_CELLS_PER_WORD-1u)) << 2;
  mask = (uint64_t)0x0fu << shift;
  old = la_load64_acq(&a->lifetime[wi]);
  for (;;) {
    uint64_t next;
    if (((old & mask) >> shift) != from)
      return 0;
    next = (old & ~mask) | ((uint64_t)to << shift);
    if (la_cas64(&a->lifetime[wi], &old, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }
}

static LJ_AINLINE int lj_arena_packed_state_cas_pair(uint64_t *plane,
	uint32_t cells_per_word, uint32_t lane_bits, uint64_t lane_mask,
	uint32_t first, uint32_t second, uint32_t from, uint32_t to)
{
  uint32_t wi, shift1, shift2;
  uint64_t mask1, mask2, old;
  if (first == second ||
      first / cells_per_word != second / cells_per_word)
    return 0;
  wi = first / cells_per_word;
  shift1 = (first & (cells_per_word-1u)) * lane_bits;
  shift2 = (second & (cells_per_word-1u)) * lane_bits;
  mask1 = lane_mask << shift1;
  mask2 = lane_mask << shift2;
  old = la_load64_acq(&plane[wi]);
  for (;;) {
    uint64_t next;
    if (((old & mask1) >> shift1) != from ||
	((old & mask2) >> shift2) != from)
      return 0;
    next = (old & ~(mask1 | mask2)) |
	((uint64_t)to << shift1) | ((uint64_t)to << shift2);
    if (la_cas64(&plane[wi], &old, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }
}

static LJ_AINLINE int lj_arena_lifetime_state_cas_pair(GCArena *a,
	uint32_t first, uint32_t second, uint32_t from, uint32_t to)
{
  if (!a || first >= LJ_ARENA_CELLS || second >= LJ_ARENA_CELLS ||
      from > LJ_ARENA_LIFETIME_MUTATING ||
      to > LJ_ARENA_LIFETIME_MUTATING)
    return 0;
  return lj_arena_packed_state_cas_pair(a->lifetime,
	LJ_ARENA_LIFETIME_CELLS_PER_WORD, 4u, 0x0fu,
	first, second, from, to);
}

static LJ_AINLINE int lj_arena_recovery_state_cas(GCArena *a, uint32_t i,
						   uint32_t from,
						   uint32_t to)
{
  uint32_t wi, shift;
  uint64_t mask, old;
  if (!a || i >= LJ_ARENA_CELLS || from > LJ_ARENA_RECOVERY_REDIRTY ||
      to > LJ_ARENA_RECOVERY_REDIRTY)
    return 0;
  wi = i / LJ_ARENA_RECOVERY_CELLS_PER_WORD;
  shift = (i & (LJ_ARENA_RECOVERY_CELLS_PER_WORD-1u)) << 1;
  mask = (uint64_t)0x03u << shift;
  old = la_load64_acq(&a->recovery[wi]);
  for (;;) {
    uint64_t next;
    if (((old & mask) >> shift) != from)
      return 0;
    next = (old & ~mask) | ((uint64_t)to << shift);
    if (la_cas64(&a->recovery[wi], &old, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }
}

static LJ_AINLINE uint32_t lj_arena_root_state_acq(const GCArena *a,
						    uint32_t i)
{
  uint32_t shift;
  uint64_t word;
  if (!a || i >= LJ_ARENA_CELLS)
    return LJ_ARENA_ROOT_NONE;
  shift = (i & (LJ_ARENA_ROOT_CELLS_PER_WORD-1u)) << 1;
  word = la_load64_acq(&a->root[i / LJ_ARENA_ROOT_CELLS_PER_WORD]);
  return (uint32_t)((word >> shift) & 0x03u);
}

static LJ_AINLINE int lj_arena_root_state_cas(GCArena *a, uint32_t i,
					       uint32_t from, uint32_t to)
{
  uint32_t wi, shift;
  uint64_t mask, old;
  if (!a || i >= LJ_ARENA_CELLS || from > LJ_ARENA_ROOT_MEMBER ||
      to > LJ_ARENA_ROOT_MEMBER)
    return 0;
  wi = i / LJ_ARENA_ROOT_CELLS_PER_WORD;
  shift = (i & (LJ_ARENA_ROOT_CELLS_PER_WORD-1u)) << 1;
  mask = (uint64_t)0x03u << shift;
  old = la_load64_acq(&a->root[wi]);
  for (;;) {
    uint64_t next;
    if (((old & mask) >> shift) != from)
      return 0;
    next = (old & ~mask) | ((uint64_t)to << shift);
    if (la_cas64(&a->root[wi], &old, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }
}

static LJ_AINLINE int lj_arena_root_state_cas_pair(GCArena *a,
	uint32_t first, uint32_t second, uint32_t from, uint32_t to)
{
  if (!a || first >= LJ_ARENA_CELLS || second >= LJ_ARENA_CELLS ||
      from > LJ_ARENA_ROOT_MEMBER || to > LJ_ARENA_ROOT_MEMBER)
    return 0;
  return lj_arena_packed_state_cas_pair(a->root,
	LJ_ARENA_ROOT_CELLS_PER_WORD, 2u, 0x03u,
	first, second, from, to);
}

LJ_FUNC int lj_arena_recovery_empty(const GCArena *a);
/* Recovery completion may expose a previously recorded late free or unblock a
** sweep retry. This allocation-free wake never mutates recovery ownership. */
LJ_FUNC void lj_arena_recovery_complete_wake(GCArena *a);

static LJ_AINLINE int lj_arena_sweep_state_cas(GCArena *a, uint32_t i,
						uint32_t from, uint32_t to)
{
  uint32_t wi = i / LJ_ARENA_SWEEP_CELLS_PER_WORD;
  uint32_t shift = (i & (LJ_ARENA_SWEEP_CELLS_PER_WORD-1u)) << 1;
  uint64_t mask = (uint64_t)0x03u << shift;
  uint64_t old = la_load64_acq(&a->sweep[wi]);
  for (;;) {
    uint64_t next;
    if (((old & mask) >> shift) != from)
      return 0;
    next = (old & ~mask) | ((uint64_t)to << shift);
    if (la_cas64(&a->sweep[wi], &old, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }
}

static LJ_AINLINE int lj_arena_sweep_state_cas_pair(GCArena *a,
	uint32_t first, uint32_t second, uint32_t from, uint32_t to)
{
  if (!a || first >= LJ_ARENA_CELLS || second >= LJ_ARENA_CELLS ||
      from > LJ_ARENA_SWEEP_FREEING || to > LJ_ARENA_SWEEP_FREEING)
    return 0;
  return lj_arena_packed_state_cas_pair(a->sweep,
	LJ_ARENA_SWEEP_CELLS_PER_WORD, 2u, 0x03u,
	first, second, from, to);
}

static LJ_AINLINE uint32_t lj_arena_reclaim_deferred_acq(const GCArena *a)
{
  return la_load32_acq(&a->hdr.reclaim_deferred);
}

static LJ_AINLINE uint32_t lj_arena_reclaim_deferred_add(GCArena *a,
						  uint32_t n)
{
  return la_add32_rlx(&a->hdr.reclaim_deferred, n);
}

static LJ_AINLINE uint32_t lj_arena_reclaim_deferred_sub(GCArena *a,
						  uint32_t n)
{
  return la_sub32_rlx(&a->hdr.reclaim_deferred, n);
}

static LJ_AINLINE uint32_t lj_arena_gcprep_pending_acq(const GCArena *a)
{
  return la_load32_acq(&a->hdr.gcprep_pending);
}

static LJ_AINLINE uint32_t lj_arena_gcprep_pending_add(GCArena *a,
						uint32_t n)
{
  return la_add32_acqrel(&a->hdr.gcprep_pending, n);
}

static LJ_AINLINE uint32_t lj_arena_gcprep_pending_sub(GCArena *a,
						uint32_t n)
{
  return la_sub32_acqrel(&a->hdr.gcprep_pending, n);
}

static LJ_AINLINE uint64_t lj_arena_remote_active_acq(const GCArena *a)
{
  return la_load64_acq(&a->hdr.remote_active);
}

static LJ_AINLINE void lj_arena_bm_set(uint64_t *bm, uint32_t i)
{
  (void)la_or64_rlx(&bm[i >> 6], (uint64_t)1 << (i & 63));
}

static LJ_AINLINE void lj_arena_bm_clear(uint64_t *bm, uint32_t i)
{
  (void)la_and64_rlx(&bm[i >> 6], ~((uint64_t)1 << (i & 63)));
}

/* Allocation starts are single-writer/multi-reader. The TG allocator owner is
** the only structural writer; GC and remote free paths only sample block[] or
** publish into mark/late/sweep side state. Atomic load/store
** keeps those samples data-race-free without putting a locked RMW on the hot
** allocation path. The x64 VM/JIT block BTS fast paths require the same sole-
** writer predicate. */
static LJ_AINLINE void lj_arena_block_set(GCArena *a, uint32_t i)
{
  uint64_t *word = &a->block[i >> 6];
  uint64_t value = la_load64_rlx(word);
  /* Publish the allocation start only after its mark state is initialized. */
  la_store64_rel(word, value | ((uint64_t)1 << (i & 63)));
}

static LJ_AINLINE void lj_arena_block_clear(GCArena *a, uint32_t i)
{
  uint64_t *word = &a->block[i >> 6];
  uint64_t value = la_load64_rlx(word);
  la_store64_rlx(word, value & ~((uint64_t)1 << (i & 63)));
}

/* Specialized bump paths initialize the complete header while block=0. They
** can publish READY with their sole-writer store immediately before the block
** release, avoiding the generic post-allocation CAS. */
static LJ_AINLINE void lj_arena_ready_set_unpublished(GCArena *a, uint32_t i)
{
  uint64_t *word = &a->ready[i >> 6];
  uint64_t value = la_load64_rlx(word);
  la_store64_rlx(word, value | ((uint64_t)1 << (i & 63)));
}

/* Generic constructors normally need a CAS because block=1 already exposes
** the pending allocation to GC. Before MT/worker activation there is no remote
** bitmap writer, so the main TG may finish READY with one release store. */
static LJ_AINLINE void lj_arena_ready_set_exclusive(GCArena *a, uint32_t i)
{
  uint64_t *word = &a->ready[i >> 6];
  uint64_t value = la_load64_rlx(word);
  la_store64_rel(word, value | ((uint64_t)1 << (i & 63)));
}

static LJ_AINLINE uint32_t lj_arena_state(const GCArena *a, uint32_t i)
{
  uint32_t block = lj_arena_bm_get(a->block, i);
  uint32_t mark = lj_arena_bm_get(a->mark, i);
  return (block << 1) | mark;
}

static LJ_AINLINE int lj_arena_ishuge(const GCArena *a)
{
  return (a->hdr.flags & LJ_AF_HUGE_MAGIC) == LJ_AF_HUGE_MAGIC;
}

static LJ_AINLINE uint32_t lj_arena_ncells(size_t size)
{
  return (uint32_t)((size + LJ_CELL_SIZE-1u) >> LJ_CELL_SHIFT);
}

LJ_STATIC_ASSERT(LJ_ARENA_SIZE == 65536u);
LJ_STATIC_ASSERT(LJ_ARENA_CELLS == 4096u);
LJ_STATIC_ASSERT(LJ_ARENA_WORDS == 64u);
LJ_STATIC_ASSERT(LJ_CELL_SIZE == 16u);
/* vm_x64 and traced FNEW classify these exact packed nibble encodings. */
LJ_STATIC_ASSERT(LJ_ARENA_LIFETIME_FREE == 0);
LJ_STATIC_ASSERT(LJ_ARENA_LIFETIME_LIVE == 1);
LJ_STATIC_ASSERT(LJ_ARENA_LIFETIME_CONSTRUCT == 2);
LJ_STATIC_ASSERT(LJ_ARENA_LIFETIME_RECOVERY == 3);
LJ_STATIC_ASSERT(LJ_ARENA_LIFETIME_MUTATING <= 15);
LJ_STATIC_ASSERT((LJ_AF_ROOT_CONSTRUCT & LJ_AF_FLAG_MASK) == 0);
LJ_STATIC_ASSERT((LJ_AF_DTOR_CONSTRUCT & LJ_AF_FLAG_MASK) == 0);
LJ_STATIC_ASSERT(sizeof(GCAhdr) == 128u);
LJ_STATIC_ASSERT(offsetof(GCAhdr, remote_active) == 56u);
LJ_STATIC_ASSERT((offsetof(GCAhdr, remote_active) & 7u) == 0);
LJ_STATIC_ASSERT(offsetof(GCAhdr, remote_free) == 64u);
LJ_STATIC_ASSERT(offsetof(GCArena, block) == 128u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->block) == 512u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->mark) == 512u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->sweep) == 1024u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->recovery) == 1024u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->root) == 1024u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->lifetime) == 2048u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->dtor) == 2048u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->late) == 512u);
LJ_STATIC_ASSERT(LJ_AFIRST_CELL == 616u);
LJ_STATIC_ASSERT(sizeof(LJArenaFreeRun) == LJ_CELL_SIZE);
LJ_STATIC_ASSERT(sizeof(LJArenaRemoteFree) == LJ_CELL_SIZE);

#endif
