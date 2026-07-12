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
#define LJ_AF_FLAG_MASK \
  (LJ_AF_TRAVERSABLE|LJ_AF_NEEDSWEEP|LJ_AF_FULL|LJ_AF_REGISTERED| \
   LJ_AF_QUARANTINE|LJ_AF_RECLAIMED|LJ_AF_PREPSWEEP)
#define LJ_AF_HUGE_MAGIC	0x4c4a4800u

/* Arena-local lifetime publication gate. The low bits count admitted
** publishers. CLOSED routes terminal frees to the bit-only late bitmap and
** makes rescues sticky through PENDING. SEALED excludes ordinary intrusive
** publishers and owner transfer while still allowing counted bit/status
** producers whose admission defeats exact commit/open arbitration. */
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
typedef struct LJArenaRemoteFree LJArenaRemoteFree;
typedef struct TGAlloc TGAlloc;

struct HugeTab {
  LJHugeTabHdr *h;
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
  uint8_t pad[32];
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
  uint32_t owner_tid;
  void *owner_tg;
  uint8_t alloc_black;
  uint8_t free_noinsert;
};

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
#define LJ_HUGEF_MASK \
  (LJ_HUGEF_MARK|LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_FINALIZER| \
   LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING| \
   LJ_HUGEF_TICKET|LJ_HUGEF_BUSY|LJ_HUGEF_INTERIOR_CDATA|LJ_HUGEF_READY| \
   LJ_HUGEF_CDATA|LJ_HUGEF_RECOVERY_MASK|LJ_HUGEF_DEFER_FREE)

static LJ_AINLINE uint32_t lj_arena_huge_recovery_state(uint32_t flags)
{
  return (flags & LJ_HUGEF_RECOVERY_MASK) >> LJ_HUGEF_RECOVERY_SHIFT;
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
/* Complete a CLAIMED huge recovery identity. A racing logical free is folded
** into the same full-slot transition and can never leave IDLE|DEFER_FREE.
** SWEEP publishes FREEING|SWEEP_OLD with a fresh-grace sentinel, including
** when no sweep owned the entry at publication time. UNMAP is reserved for a
** future proven-exclusive handoff and is not currently emitted. REQUEUED
** leaves the identity count unchanged and requires another drain. */
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
#endif
LJ_FUNC void lj_arena_alloc_set_registry(TGAlloc *alloc, HugeTab *tab);
LJ_FUNC HugeTab *lj_arena_alloc_registry_acq(const TGAlloc *alloc);
LJ_FUNC int lj_arena_alloc_registry_lookup(const TGAlloc *alloc,
					   const GCArena *a,
					   LJHugeInfo *hi);
LJ_FUNC int lj_arena_alloc_register_existing(TGAlloc *alloc);
LJ_FUNC void lj_arena_alloc_init(TGAlloc *alloc);
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
LJ_FUNC void *lj_arena_alloc(TGAlloc *alloc, PRNGState *rs, size_t size,
			     uint32_t flags);
LJ_FUNC int lj_arena_free_deferred(TGAlloc *alloc, void *p, size_t size);
LJ_FUNC void lj_arena_free(TGAlloc *alloc, void *p, size_t size);
LJ_FUNC int lj_arena_remote_free_publish(TGAlloc *alloc, void *p,
					 size_t size);
LJ_FUNC uint32_t lj_arena_remote_free_drain(TGAlloc *alloc);
LJ_FUNC uint32_t lj_arena_remote_free_drain_sweep(TGAlloc *alloc,
						   GCArena *a);
LJ_FUNC int lj_arena_remote_sweep_busy_acq(const GCArena *a);
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
LJ_STATIC_ASSERT(sizeof(GCAhdr) == 128u);
LJ_STATIC_ASSERT(offsetof(GCAhdr, remote_active) == 56u);
LJ_STATIC_ASSERT((offsetof(GCAhdr, remote_active) & 7u) == 0);
LJ_STATIC_ASSERT(offsetof(GCAhdr, remote_free) == 64u);
LJ_STATIC_ASSERT(offsetof(GCArena, block) == 128u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->block) == 512u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->mark) == 512u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->sweep) == 1024u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->recovery) == 1024u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->late) == 512u);
LJ_STATIC_ASSERT(LJ_AFIRST_CELL == 296u);
LJ_STATIC_ASSERT(sizeof(LJArenaFreeRun) == LJ_CELL_SIZE);
LJ_STATIC_ASSERT(sizeof(LJArenaRemoteFree) == LJ_CELL_SIZE);

#endif
