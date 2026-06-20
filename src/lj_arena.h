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
#define LJ_AF_FLAG_MASK \
  (LJ_AF_TRAVERSABLE|LJ_AF_NEEDSWEEP|LJ_AF_FULL)
#define LJ_AF_HUGE_MAGIC	0x4c4a4800u

typedef struct GCArena GCArena;
typedef struct GreyStack GreyStack;
typedef struct LJArenaFreeRun LJArenaFreeRun;
typedef struct LJArenaBump LJArenaBump;
typedef struct LJArenaAllocD LJArenaAllocD;
typedef struct LJHugeTabHdr LJHugeTabHdr;
typedef struct HugeTab HugeTab;
typedef struct LJHugeInfo LJHugeInfo;
typedef struct TGAlloc TGAlloc;

typedef struct GCAhdr {
  uint32_t flags;
  uint32_t owner_tid;
  GCArena *next;
  GreyStack *grey;
  uint32_t sweep_epoch;
  uint32_t live_cells;
  uint8_t pad[96];
} GCAhdr;

struct GCArena {
  GCAhdr hdr;
  uint64_t block[LJ_ARENA_WORDS];
  uint64_t mark[LJ_ARENA_WORDS];
};

static LJ_AINLINE GCArena *lj_arena_next_acq(const GCArena *a)
{
  return (GCArena *)la_loadptr_acq((void *const *)&a->hdr.next);
}

static LJ_AINLINE void lj_arena_next_rel(GCArena *a, GCArena *next)
{
  la_storeptr_rel((void **)&a->hdr.next, next);
}

struct LJArenaFreeRun {
  LJArenaFreeRun *next;
  uint32_t start;
  uint32_t len;
};

struct LJArenaBump {
  GCArena *a;
  uint32_t cell;
  uint32_t end;
};

struct TGAlloc {
  LJArenaBump bump[LJ_ARENA_NKINDS];
  LJArenaFreeRun *bins[LJ_ARENA_NKINDS][LJ_ALLOC_NBINS];
  GCArena *owned[LJ_ARENA_NKINDS];
  GCArena *needsweep[LJ_ARENA_NKINDS];
  uint32_t sweep_epoch;
  uint32_t prepare_epoch;
  uint32_t owner_tid;
  uint8_t alloc_black;
};

struct HugeTab {
  LJHugeTabHdr *h;
};

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
#define LJ_HUGEF_MASK \
  (LJ_HUGEF_MARK|LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_FINALIZER)

/* The header plus full block/mark bitmaps occupy 72 cells. The arena body
** starts after both bitmaps so the model keeps complete 4096-cell coverage. */
#define LJ_ARENA_META_BYTES	((uint32_t)sizeof(GCArena))
#define LJ_AFIRST_CELL \
  ((uint32_t)((sizeof(GCArena) + LJ_CELL_SIZE-1) >> LJ_CELL_SHIFT))

typedef void (*LJArenaRunCB)(uint32_t start, uint32_t len, void *ud);

LJ_FUNC void lj_arena_sweep_words(GCArena *a, int minor);
LJ_FUNC void lj_arena_scan_free_runs(const GCArena *a, LJArenaRunCB cb, void *ud);
LJ_FUNC uint32_t lj_arena_count_free_runs(const GCArena *a);
LJ_FUNC GCArena *lj_arena_map(PRNGState *rs, uint32_t flags);
LJ_FUNC void lj_arena_unmap(GCArena *a);
LJ_FUNC size_t lj_arena_huge_mapsize(size_t size);
LJ_FUNC void *lj_arena_huge_map(PRNGState *rs, size_t size, uint32_t flags);
LJ_FUNC void lj_arena_huge_unmap(void *p, size_t size);
LJ_FUNC int lj_arena_hugetab_init(HugeTab *ht, uint32_t hbits);
LJ_FUNC void lj_arena_hugetab_fini(HugeTab *ht);
LJ_FUNC int lj_arena_hugetab_insert(HugeTab *ht, void *p, size_t size,
				    uint32_t hflags);
LJ_FUNC int lj_arena_hugetab_lookup(HugeTab *ht, const void *p,
				    LJHugeInfo *hi);
LJ_FUNC int lj_arena_hugetab_mark(HugeTab *ht, const void *p,
				  LJHugeInfo *hi);
LJ_FUNC void lj_arena_hugetab_clear_marks(HugeTab *ht);
LJ_FUNC uint64_t lj_arena_hugetab_live_bytes(HugeTab *ht,
					     uint32_t required_flags);
LJ_FUNC int lj_arena_hugetab_transfer(HugeTab *dst, HugeTab *src,
				      uint32_t owner_tid);
LJ_FUNC int lj_arena_hugetab_delete(HugeTab *ht, const void *p,
				    LJHugeInfo *hi);
LJ_FUNC void lj_arena_alloc_init(TGAlloc *alloc);
LJ_FUNC void lj_arena_alloc_fini(TGAlloc *alloc);
LJ_FUNC void lj_arena_alloc_clear_marks(TGAlloc *alloc);
LJ_FUNC void lj_arena_alloc_rebuild_free_kind(TGAlloc *alloc, uint32_t kind);
LJ_FUNC void lj_arena_alloc_rebuild_free(TGAlloc *alloc);
LJ_FUNC void lj_arena_alloc_prepare_sweep_kind(TGAlloc *alloc, uint32_t kind);
LJ_FUNC void lj_arena_alloc_prepare_sweep(TGAlloc *alloc);
LJ_FUNC void lj_arena_alloc_restore_sweep_kind(TGAlloc *alloc, uint32_t kind);
LJ_FUNC void lj_arena_alloc_sweep_kind(TGAlloc *alloc, uint32_t kind,
				       uint32_t epoch, int minor);
LJ_FUNC GCArena *lj_arena_sweep_one(TGAlloc *alloc, uint32_t kind,
				    uint32_t epoch, int minor);
LJ_FUNC uint32_t lj_arena_alloc_transfer(TGAlloc *dst, TGAlloc *src);
LJ_FUNC void *lj_arena_alloc(TGAlloc *alloc, PRNGState *rs, size_t size,
			     uint32_t flags);
LJ_FUNC void lj_arena_free(TGAlloc *alloc, void *p, size_t size);
LJ_FUNC void *lj_arena_realloc(TGAlloc *alloc, PRNGState *rs, void *p,
			       size_t osize, size_t nsize, uint32_t flags);
LJ_FUNC void lj_arena_allocd_init(LJArenaAllocD *ad, TGAlloc *alloc,
				  PRNGState *rs, uint32_t flags);
LJ_FUNC void lj_arena_allocd_sethugetab(LJArenaAllocD *ad, HugeTab *ht);
LJ_FUNC void *lj_arena_allocd_alloc(LJArenaAllocD *ad, size_t size,
				    uint32_t flags);
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
  return (uint32_t)((bm[i >> 6] >> (i & 63)) & 1u);
}

static LJ_AINLINE void lj_arena_bm_set(uint64_t *bm, uint32_t i)
{
  bm[i >> 6] |= (uint64_t)1 << (i & 63);
}

static LJ_AINLINE void lj_arena_bm_clear(uint64_t *bm, uint32_t i)
{
  bm[i >> 6] &= ~((uint64_t)1 << (i & 63));
}

static LJ_AINLINE uint32_t lj_arena_state(const GCArena *a, uint32_t i)
{
  return (lj_arena_bm_get(a->block, i) << 1) | lj_arena_bm_get(a->mark, i);
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
LJ_STATIC_ASSERT(offsetof(GCArena, block) == 128u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->block) == 512u);
LJ_STATIC_ASSERT(sizeof(((GCArena *)0)->mark) == 512u);
LJ_STATIC_ASSERT(LJ_AFIRST_CELL == 72u);
LJ_STATIC_ASSERT(sizeof(LJArenaFreeRun) == LJ_CELL_SIZE);

#endif
