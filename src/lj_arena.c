/*
** Arena heap bitmap scaffolding.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_arena_c
#define LUA_CORE

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <string.h>
#include <sys/mman.h>

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_prng.h"

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define LJ_ARENA_MMAP_PROBE_MAX		30
#define LJ_ARENA_MMAP_PROBE_LINEAR	5
#define LJ_ARENA_MMAP_LOWER		((uintptr_t)0x4000)
#define LJ_ARENA_ADDR_LIMIT		((uintptr_t)1 << 47)

/* Apply the 04_allocator.md sweep identities over the arena bitmaps. */
void lj_arena_sweep_words(GCArena *a, int minor)
{
  uint32_t w;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t b = a->block[w];
    uint64_t m = a->mark[w];
    a->block[w] = b & m;
    a->mark[w] = minor ? (b | m) : (b ^ m);
  }
}

void lj_arena_scan_free_runs(const GCArena *a, LJArenaRunCB cb, void *ud)
{
  int32_t run_start = -1;
  uint32_t i = LJ_AFIRST_CELL;
  while (i < LJ_ARENA_CELLS) {
    uint64_t starts = (a->block[i >> 6] | a->mark[i >> 6]) >> (i & 63);
    uint32_t st;
    if (!starts) {
      i = (i | 63u) + 1u;
      continue;
    }
    i += (uint32_t)__builtin_ctzll(starts);
    if (i >= LJ_ARENA_CELLS)
      break;
    st = lj_arena_state(a, i);
    if (st == 1) {
      if (run_start < 0)
	run_start = (int32_t)i;
    } else if (run_start >= 0) {
      cb((uint32_t)run_start, i - (uint32_t)run_start, ud);
      run_start = -1;
    }
    i++;
  }
  if (run_start >= 0)
    cb((uint32_t)run_start, LJ_ARENA_CELLS - (uint32_t)run_start, ud);
}

static void arena_count_run(uint32_t start, uint32_t len, void *ud)
{
  uint32_t *count = (uint32_t *)ud;
  UNUSED(start);
  UNUSED(len);
  (*count)++;
}

uint32_t lj_arena_count_free_runs(const GCArena *a)
{
  uint32_t count = 0;
  lj_arena_scan_free_runs(a, arena_count_run, &count);
  return count;
}

static int arena_addr_ok(uintptr_t addr, size_t size)
{
  if (size > LJ_ARENA_ADDR_LIMIT - LJ_ARENA_MMAP_LOWER)
    return 0;
  return addr >= LJ_ARENA_MMAP_LOWER &&
	 addr <= LJ_ARENA_ADDR_LIMIT - size &&
	 checkptrGC((void *)addr) &&
	 checkptrGC((void *)(addr + size - 1u));
}

static uintptr_t arena_random_hint(PRNGState *rs, size_t span)
{
  uintptr_t slots = (LJ_ARENA_ADDR_LIMIT - span) >> LJ_ARENA_SHIFT;
  uintptr_t hint;
  if (!rs)
    return 0;
  hint = (uintptr_t)(lj_prng_u64(rs) % slots) << LJ_ARENA_SHIFT;
  if (hint < LJ_ARENA_MMAP_LOWER)
    hint += LJ_ARENA_SIZE;
  return hint;
}

static void *arena_trim(void *base, size_t span, size_t keep)
{
  uintptr_t addr = (uintptr_t)base;
  uintptr_t aligned = (addr + LJ_ARENA_MASK) & ~(uintptr_t)LJ_ARENA_MASK;
  size_t lead = (size_t)(aligned - addr);
  size_t trail = span - lead - keep;
  if (lead && munmap(base, lead) != 0) {
    munmap(base, span);
    return NULL;
  }
  if (trail && munmap((void *)(aligned + keep), trail) != 0) {
    munmap((void *)aligned, keep + trail);
    return NULL;
  }
  return (void *)aligned;
}

static void *arena_map_aligned(PRNGState *rs, size_t keep)
{
  int olderr = errno;
  size_t span = keep + LJ_ARENA_SIZE;
  uintptr_t hint = 0;
  int retry;
  if (span < keep || !arena_addr_ok(LJ_ARENA_MMAP_LOWER, span))
    return NULL;
  for (retry = 0; retry < LJ_ARENA_MMAP_PROBE_MAX; retry++) {
    void *p = mmap(hint ? (void *)hint : NULL, span,
		   PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    uintptr_t addr = (uintptr_t)p;
    if (p != MAP_FAILED) {
      if (arena_addr_ok(addr, span)) {
	void *m = arena_trim(p, span, keep);
	if (m) {
	  errno = olderr;
	  return m;
	}
      } else {
	munmap(p, span);
      }
    } else if (errno == ENOMEM) {
      errno = olderr;
      return NULL;
    }
    if (hint && retry < LJ_ARENA_MMAP_PROBE_LINEAR) {
      hint += 0x1000000u;
      if (!arena_addr_ok(hint, span))
	hint = 0;
      continue;
    }
    hint = arena_random_hint(rs, span);
  }
  errno = olderr;
  return NULL;
}

GCArena *lj_arena_map(PRNGState *rs, uint32_t flags)
{
  GCArena *a = (GCArena *)arena_map_aligned(rs, LJ_ARENA_SIZE);
  if (a) {
    memset(a, 0, sizeof(*a));
    a->hdr.flags = flags;
  }
  return a;
}

void lj_arena_unmap(GCArena *a)
{
  int olderr = errno;
  if (a)
    munmap((void *)a, LJ_ARENA_SIZE);
  errno = olderr;
}

size_t lj_arena_huge_mapsize(size_t size)
{
  size_t need = size + sizeof(GCAhdr);
  if (size <= LJ_HUGE_THRESHOLD ||
      need < size || need > ~(size_t)LJ_ARENA_MASK)
    return 0;
  return (need + LJ_ARENA_MASK) & ~(size_t)LJ_ARENA_MASK;
}

void *lj_arena_huge_map(PRNGState *rs, size_t size, uint32_t flags)
{
  size_t mapsize = lj_arena_huge_mapsize(size);
  GCArena *a;
  if (!mapsize)
    return NULL;
  a = (GCArena *)arena_map_aligned(rs, mapsize);
  if (!a)
    return NULL;
  memset(&a->hdr, 0, sizeof(a->hdr));
  a->hdr.flags = LJ_AF_HUGE_MAGIC | flags;
  a->hdr.live_cells = (uint32_t)(mapsize >> LJ_CELL_SHIFT);
  return (void *)((char *)a + sizeof(GCAhdr));
}

void lj_arena_huge_unmap(void *p, size_t size)
{
  int olderr = errno;
  size_t mapsize = lj_arena_huge_mapsize(size);
  if (p && mapsize)
    munmap((void *)lj_arena_of(p), mapsize);
  errno = olderr;
}

static uint32_t arena_kind(uint32_t flags)
{
  return (flags & LJ_AF_TRAVERSABLE) ? LJ_ARENAK_TRAVERSABLE :
				       LJ_ARENAK_PLAIN;
}

static uint32_t arena_bin(uint32_t ncells)
{
  return ncells < LJ_ALLOC_NBINS ? ncells - 1u : LJ_ALLOC_NBINS - 1u;
}

static void arena_set_alloc(GCArena *a, uint32_t cell, int black)
{
  lj_arena_bm_set(a->block, cell);
  if (black)
    lj_arena_bm_set(a->mark, cell);
  else
    lj_arena_bm_clear(a->mark, cell);
}

static void arena_set_extent(GCArena *a, uint32_t cell)
{
  lj_arena_bm_clear(a->block, cell);
  lj_arena_bm_clear(a->mark, cell);
}

static void arena_insert_run(TGAlloc *alloc, GCArena *a, uint32_t start,
			     uint32_t len)
{
  uint32_t k = arena_kind(a->hdr.flags);
  uint32_t b = arena_bin(len);
  LJArenaFreeRun *run = (LJArenaFreeRun *)lj_arena_cellptr(a, start);
  uint32_t i;
  lj_arena_bm_clear(a->block, start);
  lj_arena_bm_set(a->mark, start);
  for (i = 1; i < len; i++)
    arena_set_extent(a, start + i);
  run->start = start;
  run->len = len;
  run->next = alloc->bins[k][b];
  alloc->bins[k][b] = run;
}

static LJArenaFreeRun **arena_find_run(TGAlloc *alloc, uint32_t k,
				       uint32_t ncells)
{
  uint32_t b;
  for (b = arena_bin(ncells); b < LJ_ALLOC_NBINS; b++) {
    LJArenaFreeRun **pp = &alloc->bins[k][b];
    while (*pp) {
      if ((*pp)->len >= ncells)
	return pp;
      pp = &(*pp)->next;
    }
  }
  return NULL;
}

void lj_arena_alloc_init(TGAlloc *alloc)
{
  memset(alloc, 0, sizeof(*alloc));
}

void lj_arena_alloc_fini(TGAlloc *alloc)
{
  uint32_t k;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    GCArena *a = alloc->owned[k];
    while (a) {
      GCArena *next = a->hdr.next;
      lj_arena_unmap(a);
      a = next;
    }
  }
  lj_arena_alloc_init(alloc);
}

static GCArena *arena_alloc_fresh(TGAlloc *alloc, PRNGState *rs,
				  uint32_t flags)
{
  uint32_t k = arena_kind(flags);
  GCArena *a = lj_arena_map(rs, flags);
  if (!a)
    return NULL;
  a->hdr.next = alloc->owned[k];
  alloc->owned[k] = a;
  alloc->bump[k].a = a;
  alloc->bump[k].cell = LJ_AFIRST_CELL;
  alloc->bump[k].end = LJ_ARENA_CELLS;
  return a;
}

void *lj_arena_alloc(TGAlloc *alloc, PRNGState *rs, size_t size,
		     uint32_t flags)
{
  uint32_t k = arena_kind(flags);
  LJArenaBump *b = &alloc->bump[k];
  uint32_t ncells, cell;
  if (size == 0)
    return NULL;
  if (size > LJ_HUGE_THRESHOLD)
    return lj_arena_huge_map(rs, size, flags);
  ncells = lj_arena_ncells(size);
  if (ncells > LJ_ARENA_CELLS - LJ_AFIRST_CELL)
    return NULL;
  {
    LJArenaFreeRun **pp = arena_find_run(alloc, k, ncells);
    if (pp) {
      LJArenaFreeRun *run = *pp;
      GCArena *a = lj_arena_of(run);
      uint32_t start = run->start;
      uint32_t len = run->len;
      *pp = run->next;
      if (len > ncells)
	arena_insert_run(alloc, a, start + ncells, len - ncells);
      arena_set_alloc(a, start, alloc->alloc_black);
      return lj_arena_cellptr(a, start);
    }
  }
  if (!b->a || b->cell + ncells > b->end) {
    if (!arena_alloc_fresh(alloc, rs, flags))
      return NULL;
  }
  cell = b->cell;
  b->cell += ncells;
  arena_set_alloc(b->a, cell, alloc->alloc_black);
  return lj_arena_cellptr(b->a, cell);
}

void lj_arena_free(TGAlloc *alloc, void *p, size_t size)
{
  GCArena *a;
  uint32_t start, ncells;
  if (!p || size == 0)
    return;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    lj_arena_huge_unmap(p, size);
    return;
  }
  if (size > LJ_HUGE_THRESHOLD)
    return;
  start = lj_arena_cellof(p);
  ncells = lj_arena_ncells(size);
  if (start < LJ_AFIRST_CELL || start + ncells > LJ_ARENA_CELLS)
    return;
  arena_insert_run(alloc, a, start, ncells);
}

void *lj_arena_realloc(TGAlloc *alloc, PRNGState *rs, void *p,
		       size_t osize, size_t nsize, uint32_t flags)
{
  void *np;
  GCArena *a;
  int oldhuge;
  if (!p)
    return lj_arena_alloc(alloc, rs, nsize, flags);
  if (nsize == 0) {
    lj_arena_free(alloc, p, osize);
    return NULL;
  }
  a = lj_arena_of(p);
  oldhuge = lj_arena_ishuge(a);
  if (oldhuge && nsize > LJ_HUGE_THRESHOLD &&
      lj_arena_huge_mapsize(osize) == lj_arena_huge_mapsize(nsize))
    return p;
  if (oldhuge || nsize > LJ_HUGE_THRESHOLD) {
    size_t csize = osize < nsize ? osize : nsize;
    np = lj_arena_alloc(alloc, rs, nsize, flags);
    if (!np)
      return NULL;
    memcpy(np, p, csize);
    lj_arena_free(alloc, p, osize);
    return np;
  }
  if (nsize <= osize) {
    uint32_t ocells = lj_arena_ncells(osize);
    uint32_t ncells = lj_arena_ncells(nsize);
    if (ncells < ocells) {
      GCArena *a = lj_arena_of(p);
      arena_insert_run(alloc, a, lj_arena_cellof(p) + ncells,
		       ocells - ncells);
    }
    return p;
  }
  np = lj_arena_alloc(alloc, rs, nsize, flags);
  if (!np)
    return NULL;
  memcpy(np, p, osize);
  lj_arena_free(alloc, p, osize);
  return np;
}
