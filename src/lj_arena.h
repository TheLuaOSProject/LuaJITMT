/*
** Arena heap bitmap scaffolding.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_ARENA_H
#define _LJ_ARENA_H

#include "lj_def.h"

#define LJ_ARENA_SHIFT		16
#define LJ_ARENA_SIZE		((uint32_t)1u << LJ_ARENA_SHIFT)
#define LJ_ARENA_MASK		(LJ_ARENA_SIZE - 1u)
#define LJ_CELL_SHIFT		4
#define LJ_CELL_SIZE		((uint32_t)1u << LJ_CELL_SHIFT)
#define LJ_ARENA_CELLS		(LJ_ARENA_SIZE >> LJ_CELL_SHIFT)
#define LJ_ARENA_WORDS		(LJ_ARENA_CELLS >> 6)

#define LJ_HUGE_THRESHOLD	(LJ_ARENA_SIZE >> 2)

#define LJ_AF_TRAVERSABLE	0x00000001u
#define LJ_AF_NEEDSWEEP		0x00000002u
#define LJ_AF_FULL		0x00000004u
#define LJ_AF_HUGE_MAGIC	0x4c4a4855u

typedef struct GCArena GCArena;
typedef struct GreyStack GreyStack;

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

static LJ_AINLINE uint32_t lj_arena_state(const GCArena *a, uint32_t i)
{
  return (lj_arena_bm_get(a->block, i) << 1) | lj_arena_bm_get(a->mark, i);
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

#endif
