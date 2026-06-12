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
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_prng.h"

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define LJ_ARENA_MMAP_PROBE_MAX		30
#define LJ_ARENA_MMAP_PROBE_LINEAR	5
#define LJ_ARENA_MMAP_LOWER		((uintptr_t)0x4000)
#define LJ_ARENA_ADDR_LIMIT		((uintptr_t)1 << 47)
#define LJ_HUGETAB_MAX_BITS		26
#define LJ_HUGETAB_META_SHIFT		4
#define LJ_HUGETAB_META_MASK \
  (((uint64_t)1 << LJ_HUGETAB_META_SHIFT) - 1u)
#define LJ_HUGETAB_EMPTY		((uint64_t)0)
#define LJ_HUGETAB_TOMBSTONE		((uint64_t)1)

typedef struct LJHugeEnt {
  la_u128 slot;
} LJHugeEnt;

struct LJHugeTabHdr {
  uint32_t hbits;
  uint32_t mask;
  size_t mapsize;
  LJHugeEnt ent[1];
};

LJ_STATIC_ASSERT(sizeof(LJHugeEnt) == 16u);
LJ_STATIC_ASSERT(offsetof(LJHugeTabHdr, ent) == 16u);
LJ_STATIC_ASSERT((offsetof(LJHugeTabHdr, ent) & 15u) == 0);
LJ_STATIC_ASSERT((LJ_AF_HUGE_MAGIC & LJ_AF_FLAG_MASK) == 0);

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

static size_t hugetab_mapsize(uint32_t hbits)
{
  size_t cap, hdr = offsetof(LJHugeTabHdr, ent);
  if (hbits > LJ_HUGETAB_MAX_BITS)
    return 0;
  cap = (size_t)1 << hbits;
  if (cap > (~(size_t)0 - hdr) / sizeof(LJHugeEnt))
    return 0;
  return hdr + cap * sizeof(LJHugeEnt);
}

static uint32_t hugetab_hash(uint64_t addr, uint32_t mask)
{
  uint64_t x = addr >> LJ_CELL_SHIFT;
  x ^= x >> 33;
  x *= U64x(ff51afd7,ed558ccd);
  x ^= x >> 33;
  x *= U64x(c4ceb9fe,1a85ec53);
  x ^= x >> 33;
  return (uint32_t)x & mask;
}

static int hugetab_pack(void *p, size_t size, uint32_t hflags,
			uint64_t *addr, uint64_t *meta)
{
  uintptr_t u = (uintptr_t)p;
  if (!p || u <= LJ_HUGETAB_TOMBSTONE || !lj_arena_huge_mapsize(size) ||
      (hflags & ~LJ_HUGEF_MASK) != 0 ||
      size > (~(uint64_t)0 >> LJ_HUGETAB_META_SHIFT))
    return 0;
  *addr = (uint64_t)u;
  *meta = ((uint64_t)size << LJ_HUGETAB_META_SHIFT) | (uint64_t)hflags;
  return 1;
}

static void hugetab_decode(uint64_t meta, LJHugeInfo *hi)
{
  if (hi) {
    hi->size = (size_t)(meta >> LJ_HUGETAB_META_SHIFT);
    hi->flags = (uint32_t)(meta & LJ_HUGETAB_META_MASK);
  }
}

static int hugetab_search(LJHugeTabHdr *h, uint64_t addr,
			  LJHugeEnt **ep, uint64_t *metap)
{
  uint32_t cap = h->mask + 1u;
  uint32_t i = hugetab_hash(addr, h->mask);
  uint32_t n;
  for (n = 0; n < cap; n++, i = (i + 1u) & h->mask) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t ea = la_load64_acq(&e->slot.lo);  /* 04 §4.5.1 publish edge. */
    if (ea == LJ_HUGETAB_EMPTY)
      return 0;
    if (ea == addr) {
      uint64_t meta = la_load64_acq(&e->slot.hi);  /* 04 §4.5.1 metadata. */
      if (la_load64_acq(&e->slot.lo) == addr) {  /* Stable found snapshot. */
	if (ep)
	  *ep = e;
	if (metap)
	  *metap = meta;
	return 1;
      }
    }
  }
  return 0;
}

int lj_arena_hugetab_init(HugeTab *ht, uint32_t hbits)
{
  int olderr = errno;
  size_t mapsize = hugetab_mapsize(hbits);
  LJHugeTabHdr *h;
  if (!ht || ht->h || !mapsize)
    return 0;
  h = (LJHugeTabHdr *)mmap(NULL, mapsize, PROT_READ|PROT_WRITE,
			   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (h == MAP_FAILED)
    return 0;
  h->hbits = hbits;
  h->mask = (1u << hbits) - 1u;
  h->mapsize = mapsize;
  ht->h = h;
  errno = olderr;
  return 1;
}

void lj_arena_hugetab_fini(HugeTab *ht)
{
  int olderr = errno;
  if (ht && ht->h) {
    LJHugeTabHdr *h = ht->h;
    size_t mapsize = h->mapsize;
    ht->h = NULL;
    munmap((void *)h, mapsize);
  }
  errno = olderr;
}

int lj_arena_hugetab_insert(HugeTab *ht, void *p, size_t size,
			    uint32_t hflags)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  if (!h || !hugetab_pack(p, size, hflags, &addr, &meta))
    return -1;
  for (;;) {
    uint32_t cap = h->mask + 1u;
    uint32_t i = hugetab_hash(addr, h->mask);
    uint32_t n;
    LJHugeEnt *freeent = NULL;
    la_u128 freeval, des;
    for (n = 0; n < cap; n++, i = (i + 1u) & h->mask) {
      LJHugeEnt *e = &h->ent[i];
      uint64_t ea = la_load64_acq(&e->slot.lo);  /* 04 §4.5.1 slot state. */
      if (ea == addr)
	return 0;
      if (ea == LJ_HUGETAB_EMPTY || ea == LJ_HUGETAB_TOMBSTONE) {
	uint64_t emeta = la_load64_acq(&e->slot.hi);  /* 04 §4.5.1 CAS pair. */
	if (!freeent) {
	  freeent = e;
	  freeval.lo = ea;
	  freeval.hi = emeta;
	}
	if (ea == LJ_HUGETAB_EMPTY)
	  break;
      }
    }
    if (!freeent)
      return -1;
    des.lo = addr;
    des.hi = meta;
    if (la_cas128(&freeent->slot, &freeval, des))  /* 04 §4.5.1 publish. */
      return 1;
  }
}

int lj_arena_hugetab_lookup(HugeTab *ht, const void *p, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  if (hugetab_search(h, addr, NULL, &meta)) {
    hugetab_decode(meta, hi);
    return 1;
  }
  return 0;
}

int lj_arena_hugetab_mark(HugeTab *ht, const void *p, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, oldmeta;
  LJHugeEnt *e;
  if (!h || !p)
    return -1;
  addr = (uint64_t)(uintptr_t)p;
  if (!hugetab_search(h, addr, &e, NULL))
    return -1;
  oldmeta = la_or64_rlx(&e->slot.hi, LJ_HUGEF_MARK);  /* 04 §4.5.1 mark. */
  hugetab_decode(oldmeta | LJ_HUGEF_MARK, hi);
  return (oldmeta & LJ_HUGEF_MARK) ? 0 : 1;
}

void lj_arena_hugetab_clear_marks(HugeTab *ht)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap;
  if (!h)
    return;
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);
    if (addr > LJ_HUGETAB_TOMBSTONE)
      la_and64_rlx(&e->slot.hi, ~(uint64_t)LJ_HUGEF_MARK);
  }
}

int lj_arena_hugetab_delete(HugeTab *ht, const void *p, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    LJHugeEnt *e;
    uint64_t meta;
    la_u128 exp, des;
    if (!hugetab_search(h, addr, &e, &meta))
      return 0;
    exp.lo = addr;
    exp.hi = meta;
    des.lo = LJ_HUGETAB_TOMBSTONE;
    des.hi = 0;
    if (la_cas128(&e->slot, &exp, des)) {  /* 04 §4.5.1 delete publish. */
      hugetab_decode(meta, hi);
      return 1;
    }
  }
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

static void arena_set_free_run(GCArena *a, uint32_t start, uint32_t len)
{
  uint32_t i;
  lj_arena_bm_clear(a->block, start);
  lj_arena_bm_set(a->mark, start);
  for (i = 1; i < len; i++)
    arena_set_extent(a, start + i);
}

static void arena_insert_run(TGAlloc *alloc, GCArena *a, uint32_t start,
			     uint32_t len)
{
  uint32_t k = arena_kind(a->hdr.flags);
  uint32_t b = arena_bin(len);
  LJArenaFreeRun *run = (LJArenaFreeRun *)lj_arena_cellptr(a, start);
  arena_set_free_run(a, start, len);
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

static void arena_clear_bins(TGAlloc *alloc, uint32_t k)
{
  memset(alloc->bins[k], 0, sizeof(alloc->bins[k]));
}

static void arena_unmap_list(GCArena *a)
{
  while (a) {
    GCArena *next = a->hdr.next;
    lj_arena_unmap(a);
    a = next;
  }
}

static uint32_t arena_count_live_cells(const GCArena *a)
{
  uint32_t i, n = 0;
  for (i = LJ_AFIRST_CELL; i < LJ_ARENA_CELLS; i++)
    n += lj_arena_bm_get(a->block, i);
  return n;
}

typedef struct ArenaLargestRun {
  uint32_t start;
  uint32_t len;
} ArenaLargestRun;

static void arena_find_largest_run(uint32_t start, uint32_t len, void *ud)
{
  ArenaLargestRun *lr = (ArenaLargestRun *)ud;
  if (len > lr->len) {
    lr->start = start;
    lr->len = len;
  }
}

typedef struct ArenaRebuildRuns {
  TGAlloc *alloc;
  GCArena *a;
  ArenaLargestRun bump;
} ArenaRebuildRuns;

typedef struct ArenaRebuildFree {
  TGAlloc *alloc;
  GCArena *a;
  uint32_t limit;
} ArenaRebuildFree;

static void arena_rebuild_run(uint32_t start, uint32_t len, void *ud)
{
  ArenaRebuildRuns *rr = (ArenaRebuildRuns *)ud;
  if (rr->bump.len >= LJ_BUMP_MIN &&
      start == rr->bump.start && len == rr->bump.len)
    return;
  arena_insert_run(rr->alloc, rr->a, start, len);
}

static void arena_rebuild_free_run(uint32_t start, uint32_t len, void *ud)
{
  ArenaRebuildFree *rf = (ArenaRebuildFree *)ud;
  if (start >= rf->limit)
    return;
  if (len > rf->limit - start)
    len = rf->limit - start;
  arena_insert_run(rf->alloc, rf->a, start, len);
}

void lj_arena_alloc_init(TGAlloc *alloc)
{
  memset(alloc, 0, sizeof(*alloc));
}

void lj_arena_alloc_fini(TGAlloc *alloc)
{
  uint32_t k;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    arena_unmap_list(alloc->owned[k]);
    arena_unmap_list(alloc->needsweep[k]);
  }
  lj_arena_alloc_init(alloc);
}

void lj_arena_alloc_clear_marks(TGAlloc *alloc)
{
  uint32_t k;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    GCArena *a;
    for (a = alloc->owned[k]; a != NULL; a = a->hdr.next) {
      uint32_t w;
      for (w = 0; w < LJ_ARENA_WORDS; w++)
	a->mark[w] &= ~a->block[w];
    }
  }
}

void lj_arena_alloc_rebuild_free_kind(TGAlloc *alloc, uint32_t k)
{
  if (k < LJ_ARENA_NKINDS) {
    GCArena *a;
    arena_clear_bins(alloc, k);
    for (a = alloc->owned[k]; a != NULL; a = a->hdr.next) {
      ArenaRebuildFree rf;
      rf.alloc = alloc;
      rf.a = a;
      rf.limit = alloc->bump[k].a == a ? alloc->bump[k].cell :
					 LJ_ARENA_CELLS;
      lj_arena_scan_free_runs(a, arena_rebuild_free_run, &rf);
    }
  }
}

void lj_arena_alloc_rebuild_free(TGAlloc *alloc)
{
  uint32_t k;
  for (k = 0; k < LJ_ARENA_NKINDS; k++)
    lj_arena_alloc_rebuild_free_kind(alloc, k);
}

void lj_arena_alloc_prepare_sweep_kind(TGAlloc *alloc, uint32_t k)
{
  GCArena *a;
  if (k >= LJ_ARENA_NKINDS)
    return;
  a = alloc->owned[k];
  if (alloc->bump[k].a && alloc->bump[k].cell < alloc->bump[k].end)
    arena_set_free_run(alloc->bump[k].a, alloc->bump[k].cell,
		       alloc->bump[k].end - alloc->bump[k].cell);
  alloc->owned[k] = NULL;
  alloc->bump[k].a = NULL;
  alloc->bump[k].cell = 0;
  alloc->bump[k].end = 0;
  arena_clear_bins(alloc, k);
  while (a) {
    GCArena *next = a->hdr.next;
    a->hdr.flags |= LJ_AF_NEEDSWEEP;
    a->hdr.next = alloc->needsweep[k];
    alloc->needsweep[k] = a;
    a = next;
  }
}

void lj_arena_alloc_prepare_sweep(TGAlloc *alloc)
{
  uint32_t k;
  for (k = 0; k < LJ_ARENA_NKINDS; k++)
    lj_arena_alloc_prepare_sweep_kind(alloc, k);
}

void lj_arena_alloc_sweep_kind(TGAlloc *alloc, uint32_t kind,
			       uint32_t epoch, int minor)
{
  while (lj_arena_sweep_one(alloc, kind, epoch, minor) != NULL)
    ;
}

GCArena *lj_arena_sweep_one(TGAlloc *alloc, uint32_t kind, uint32_t epoch,
			    int minor)
{
  GCArena *a;
  ArenaLargestRun lr = { 0, 0 };
  ArenaRebuildRuns rr;
  if (kind >= LJ_ARENA_NKINDS)
    return NULL;
  a = alloc->needsweep[kind];
  if (!a)
    return NULL;
  alloc->needsweep[kind] = a->hdr.next;
  a->hdr.next = NULL;
  lj_arena_sweep_words(a, minor);
  lj_arena_scan_free_runs(a, arena_find_largest_run, &lr);
  if (lr.len >= LJ_BUMP_MIN)
    arena_set_free_run(a, lr.start, lr.len);
  rr.alloc = alloc;
  rr.a = a;
  rr.bump = lr;
  lj_arena_scan_free_runs(a, arena_rebuild_run, &rr);
  if (lr.len >= LJ_BUMP_MIN) {
    alloc->bump[kind].a = a;
    alloc->bump[kind].cell = lr.start;
    alloc->bump[kind].end = lr.start + lr.len;
  }
  a->hdr.live_cells = arena_count_live_cells(a);
  a->hdr.sweep_epoch = epoch;
  a->hdr.flags &= ~LJ_AF_NEEDSWEEP;
  a->hdr.next = alloc->owned[kind];
  alloc->owned[kind] = a;
  return a;
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

void lj_arena_allocd_init(LJArenaAllocD *ad, TGAlloc *alloc, PRNGState *rs,
			  uint32_t flags)
{
  ad->alloc = alloc;
  ad->prng = rs;
  ad->huge = NULL;
  ad->flags = flags;
}

void lj_arena_allocd_sethugetab(LJArenaAllocD *ad, HugeTab *ht)
{
  ad->huge = ht;
}

static uint32_t arena_allocf_hflags(LJArenaAllocD *ad, uint32_t flags)
{
  uint32_t hflags = 0;
  if (flags & LJ_AF_TRAVERSABLE)
    hflags |= LJ_HUGEF_TRAVERSABLE;
  if (ad->alloc->alloc_black)
    hflags |= LJ_HUGEF_MARK;
  return hflags;
}

static void *arena_allocf_new(LJArenaAllocD *ad, size_t size, uint32_t flags)
{
  void *p;
  if (!ad->huge || size <= LJ_HUGE_THRESHOLD)
    return lj_arena_alloc(ad->alloc, ad->prng, size, flags);
  p = lj_arena_huge_map(ad->prng, size, flags);
  if (p && lj_arena_hugetab_insert(ad->huge, p, size,
				   arena_allocf_hflags(ad, flags)) != 1) {
    lj_arena_huge_unmap(p, size);
    return NULL;
  }
  return p;
}

static void arena_allocf_free(LJArenaAllocD *ad, void *ptr, size_t osize)
{
  if (ad->huge && ptr && lj_arena_ishuge(lj_arena_of(ptr))) {
    LJHugeInfo hi;
    if (lj_arena_hugetab_delete(ad->huge, ptr, &hi) == 1) {
      lj_arena_huge_unmap(ptr, hi.size);
      return;
    }
  }
  lj_arena_free(ad->alloc, ptr, osize);
}

void *lj_arena_allocd_alloc(LJArenaAllocD *ad, size_t size, uint32_t flags)
{
  if (!ad || !ad->alloc || !ad->prng)
    return NULL;
  return arena_allocf_new(ad, size, flags);
}

void *lj_arena_allocf(void *ud, void *ptr, size_t osize, size_t nsize)
{
  LJArenaAllocD *ad = (LJArenaAllocD *)ud;
  if (!ad || !ad->alloc || !ad->prng)
    return NULL;
  if (!ptr)
    return arena_allocf_new(ad, nsize, ad->flags);
  if (nsize == 0) {
    arena_allocf_free(ad, ptr, osize);
    return NULL;
  }
  if (osize == 0)
    return NULL;
  if (ad->huge && (lj_arena_ishuge(lj_arena_of(ptr)) ||
		   nsize > LJ_HUGE_THRESHOLD)) {
    LJHugeInfo hi;
    size_t csize;
    void *np;
    if (lj_arena_ishuge(lj_arena_of(ptr)) &&
	lj_arena_hugetab_lookup(ad->huge, ptr, &hi) == 1)
      osize = hi.size;
    csize = osize < nsize ? osize : nsize;
    np = arena_allocf_new(ad, nsize, ad->flags);
    if (!np)
      return NULL;
    memcpy(np, ptr, csize);
    arena_allocf_free(ad, ptr, osize);
    return np;
  }
  return lj_arena_realloc(ad->alloc, ad->prng, ptr, osize, nsize, ad->flags);
}
