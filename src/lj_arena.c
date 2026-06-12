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
#define LJ_ARENA_MAP_SPAN		((size_t)LJ_ARENA_SIZE * 2u)
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
  return addr >= LJ_ARENA_MMAP_LOWER &&
	 addr <= LJ_ARENA_ADDR_LIMIT - size &&
	 checkptrGC((void *)addr) &&
	 checkptrGC((void *)(addr + size - 1u));
}

static uintptr_t arena_random_hint(PRNGState *rs)
{
  uintptr_t slots = (LJ_ARENA_ADDR_LIMIT - LJ_ARENA_MAP_SPAN) >>
		    LJ_ARENA_SHIFT;
  uintptr_t hint;
  if (!rs)
    return 0;
  hint = (uintptr_t)(lj_prng_u64(rs) % slots) << LJ_ARENA_SHIFT;
  if (hint < LJ_ARENA_MMAP_LOWER)
    hint += LJ_ARENA_SIZE;
  return hint;
}

static GCArena *arena_trim(void *base)
{
  uintptr_t addr = (uintptr_t)base;
  uintptr_t aligned = (addr + LJ_ARENA_MASK) & ~(uintptr_t)LJ_ARENA_MASK;
  size_t lead = (size_t)(aligned - addr);
  size_t trail = LJ_ARENA_MAP_SPAN - lead - LJ_ARENA_SIZE;
  if (lead && munmap(base, lead) != 0) {
    munmap(base, LJ_ARENA_MAP_SPAN);
    return NULL;
  }
  if (trail && munmap((void *)(aligned + LJ_ARENA_SIZE), trail) != 0) {
    munmap((void *)aligned, LJ_ARENA_SIZE + trail);
    return NULL;
  }
  return (GCArena *)aligned;
}

GCArena *lj_arena_map(PRNGState *rs, uint32_t flags)
{
  int olderr = errno;
  uintptr_t hint = 0;
  int retry;
  for (retry = 0; retry < LJ_ARENA_MMAP_PROBE_MAX; retry++) {
    void *p = mmap(hint ? (void *)hint : NULL, LJ_ARENA_MAP_SPAN,
		   PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    uintptr_t addr = (uintptr_t)p;
    if (p != MAP_FAILED) {
      if (arena_addr_ok(addr, LJ_ARENA_MAP_SPAN)) {
	GCArena *a = arena_trim(p);
	if (a) {
	  memset(a, 0, sizeof(*a));
	  a->hdr.flags = flags;
	  errno = olderr;
	  return a;
	}
      } else {
	munmap(p, LJ_ARENA_MAP_SPAN);
      }
    } else if (errno == ENOMEM) {
      errno = olderr;
      return NULL;
    }
    if (hint && retry < LJ_ARENA_MMAP_PROBE_LINEAR) {
      hint += 0x1000000u;
      if (!arena_addr_ok(hint, LJ_ARENA_MAP_SPAN))
	hint = 0;
      continue;
    }
    hint = arena_random_hint(rs);
  }
  errno = olderr;
  return NULL;
}

void lj_arena_unmap(GCArena *a)
{
  int olderr = errno;
  if (a)
    munmap((void *)a, LJ_ARENA_SIZE);
  errno = olderr;
}

static uint32_t arena_kind(uint32_t flags)
{
  return (flags & LJ_AF_TRAVERSABLE) ? LJ_ARENAK_TRAVERSABLE :
				       LJ_ARENAK_PLAIN;
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
  if (size == 0 || size > LJ_HUGE_THRESHOLD)
    return NULL;
  ncells = (uint32_t)((size + LJ_CELL_SIZE-1u) >> LJ_CELL_SHIFT);
  if (ncells > LJ_ARENA_CELLS - LJ_AFIRST_CELL)
    return NULL;
  if (!b->a || b->cell + ncells > b->end) {
    if (!arena_alloc_fresh(alloc, rs, flags))
      return NULL;
  }
  cell = b->cell;
  b->cell += ncells;
  lj_arena_bm_set(b->a->block, cell);
  if (alloc->alloc_black)
    lj_arena_bm_set(b->a->mark, cell);
  return lj_arena_cellptr(b->a, cell);
}
