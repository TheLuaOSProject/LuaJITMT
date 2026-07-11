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
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_prng.h"
#include "lj_tg.h"
#include "lj_thr.h"

#if !defined(_WIN32)
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

#define LJ_ARENA_MMAP_PROBE_MAX		30
#define LJ_ARENA_MMAP_PROBE_LINEAR	5
#define LJ_ARENA_MMAP_LOWER		((uintptr_t)0x4000)
#define LJ_ARENA_ADDR_LIMIT		((uintptr_t)1 << 47)
#define LJ_HUGETAB_MAX_BITS		26
#define LJ_HUGETAB_META_SHIFT		8
#define LJ_HUGETAB_META_MASK \
  (((uint64_t)1 << LJ_HUGETAB_META_SHIFT) - 1u)
#define LJ_HUGETAB_EMPTY		((uint64_t)0)
#define LJ_HUGETAB_TOMBSTONE		((uint64_t)1)

static LJ_AINLINE uint64_t arena_remote_count(uint64_t active)
{
  return active & LJ_ARENA_REMOTE_COUNT_MASK;
}

static LJ_NORET void arena_remote_overflow(void)
{
  /* Every live admission requires a distinct executing context and nonzero
  ** address-space-backed state. Supported x64 processes cannot host 2^61 of
  ** them. Treat a violated platform invariant as fatal instead of dropping a
  ** lifetime intent or inventing an uncoordinated poison state. */
  abort();
}

static void arena_progress_wake(GCArena *a)
{
  global_State *g = a ? (global_State *)lj_arena_progress_g_acq(a) : NULL;
  if (g)
    lj_gc2_sweep_publish_wake(g);
}

static void arena_late_clear_committed_free(GCArena *a)
{
  uint32_t w;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t block = la_load64_acq(&a->block[w]);
    (void)la_and64_rlx(&a->late[w], block);
  }
}

static int arena_remote_enter(GCArena *a)
{
  uint64_t active;
  if (!a)
    return 0;
  active = la_load64_acq(&a->hdr.remote_active);
  for (;;) {
    uint64_t expect = active;
    if (active & LJ_ARENA_REMOTE_STATE_MASK)
      return 0;
    if (arena_remote_count(active) == LJ_ARENA_REMOTE_COUNT_MASK) {
      arena_remote_overflow();
    }
    if (la_cas64(&a->hdr.remote_active, &expect, active + 1u,
		 LA_ACQ_REL, LA_ACQ))
      return 1;
    active = expect;
  }
}

/* Admit a bit-only late publisher after terminal close or while SEALED. The
** admission CAS publishes PENDING before the late bit, so an owner can never
** clear/commit past an intent whose producer was preempted before the bit. */
static int arena_remote_late_enter(GCArena *a)
{
  uint64_t active;
  if (!a)
    return 0;
  active = la_load64_acq(&a->hdr.remote_active);
  for (;;) {
    uint64_t expect = active;
    uint64_t next;
    if (!(active & (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED)))
      return 0;  /* Gate reopened: caller must use the ordinary route. */
    if (arena_remote_count(active) == LJ_ARENA_REMOTE_COUNT_MASK) {
      arena_remote_overflow();
    }
    next = (active + 1u) | LJ_ARENA_REMOTE_PENDING;
    if (la_cas64(&a->hdr.remote_active, &expect, next,
		 LA_ACQ_REL, LA_ACQ))
      return 1;
    active = expect;
  }
}

static void arena_publish_leave(GCArena *a)
{
  uint64_t old = la_sub64_acqrel(&a->hdr.remote_active, 1);
  lj_assertX(arena_remote_count(old) != 0,
	     "arena publisher leave without admission");
  if (arena_remote_count(old) == 1u) {
    uint32_t flags = lj_arena_flags_acq(a);
    uint64_t now = old - 1u;
    int wake = (now & LJ_ARENA_REMOTE_STATE_MASK) != 0 ||
	(flags & (LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|
		  LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) != 0;
    /* Adoption/restore publishes all bitmap and bin state before clearing the
    ** lifecycle flags and dropping SEALED to CLOSED. The last bit-only
    ** publisher can then complete the exact clean gate transition to OPEN. */
    if (!(flags & (LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|
		   LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) &&
	(now & LJ_ARENA_REMOTE_CLOSED) &&
	!(now & LJ_ARENA_REMOTE_SEALED)) {
      uint64_t expect = now;
      arena_late_clear_committed_free(a);
      (void)la_cas64(&a->hdr.remote_active, &expect, 0,
		     LA_REL, LA_ACQ);
    }
    /* OPEN 1->0 is the ordinary mark fast path and has no waiter. Lifecycle
    ** owners need the edge only after they publish a terminal gate/flag. */
    if (wake)
      arena_progress_wake(a);
  }
}

static void arena_remote_leave(GCArena *a)
{
  arena_publish_leave(a);
}

int lj_arena_remote_sweep_busy_acq(const GCArena *a)
{
  uint64_t active;
  if (!a)
    return 0;
  active = lj_arena_remote_active_acq(a);
  return arena_remote_count(active) != 0;
}

int lj_arena_reclaim_seal(GCArena *a)
{
  uint64_t active;
  if (!a)
    return 0;
  active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t state = active & LJ_ARENA_REMOTE_STATE_MASK;
    uint64_t expect = active;
    uint64_t next;
    if (arena_remote_count(active) != 0)
      return 0;
    if (state & LJ_ARENA_REMOTE_SEALED)
      return 0;
    if (state != 0 && state != LJ_ARENA_REMOTE_CLOSED &&
	state != (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_PENDING))
      return 0;
    next = (state & LJ_ARENA_REMOTE_PENDING) |
	   LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED;
    if (la_cas64(&a->hdr.remote_active, &expect, next,
		 LA_ACQ_REL, LA_ACQ))
      return 1;
    active = expect;
  }
}

/* Final clean-state arbitration. A bit-only publisher changes this exact word
** before publishing its intent, so the owner cannot enter mutation/commit
** ownership past a preempted producer. */
static int arena_reclaim_commit_sealed(GCArena *a)
{
  uint64_t expect = LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED;
  return a && la_cas64(&a->hdr.remote_active, &expect,
			LJ_ARENA_REMOTE_SEALED, LA_ACQ_REL, LA_ACQ);
}

int lj_arena_reclaim_clear_pending(GCArena *a)
{
  uint64_t active;
  const uint64_t clean = LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED;
  if (!a)
    return 0;
  active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t expect = active;
    if (active == clean)
      return 1;
    if (active != (clean|LJ_ARENA_REMOTE_PENDING))
      return 0;
    if (la_cas64(&a->hdr.remote_active, &expect, clean,
		 LA_ACQ_REL, LA_ACQ))
      return 1;
    active = expect;
  }
}

void lj_arena_reclaim_unseal(GCArena *a, int keep_pending)
{
  uint64_t active;
  if (!a)
    return;
  active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t expect = active;
    uint64_t next;
    lj_assertX((active & LJ_ARENA_REMOTE_SEALED) != 0,
	       "arena unseal without exclusive ownership");
    if (!(active & LJ_ARENA_REMOTE_SEALED))
      return;
    next = arena_remote_count(active) | LJ_ARENA_REMOTE_CLOSED |
	((keep_pending || arena_remote_count(active) != 0) &&
	 (active & LJ_ARENA_REMOTE_PENDING) ?
	 LJ_ARENA_REMOTE_PENDING : 0u);
    if (la_cas64(&a->hdr.remote_active, &expect, next,
		 LA_REL, LA_ACQ))
      return;
    active = expect;
  }
}

static int arena_remote_open_sealed(GCArena *a)
{
  uint64_t active;
  if (!a)
    return 0;
  active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t expect = active;
    if (active != LJ_ARENA_REMOTE_SEALED)
      return 0;
    if (la_cas64(&a->hdr.remote_active, &expect, 0,
		 LA_REL, LA_ACQ))
      return 1;
    active = expect;
  }
}

int lj_arena_rescue_enter(GCArena *a)
{
  uint64_t active;
  if (!a)
    return 0;
  active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t expect = active;
    uint64_t next;
    int committed;
    if (arena_remote_count(active) == LJ_ARENA_REMOTE_COUNT_MASK) {
      arena_remote_overflow();
    }
    committed = (active & LJ_ARENA_REMOTE_SEALED) &&
		!(active & LJ_ARENA_REMOTE_CLOSED);
    next = active + 1u;
    if (!committed &&
	(active & (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED)))
      next |= LJ_ARENA_REMOTE_PENDING;
    if (la_cas64(&a->hdr.remote_active, &expect, next,
		 LA_ACQ_REL, LA_ACQ))
      return committed ? LJ_ARENA_RESCUE_COMMITTED :
	((active & LJ_ARENA_REMOTE_SEALED) ?
	 LJ_ARENA_RESCUE_BIT_ONLY : LJ_ARENA_RESCUE_FULL);
    active = expect;
  }
}

void lj_arena_rescue_leave(GCArena *a)
{
  arena_publish_leave(a);
}

static void arena_remote_late_leave(GCArena *a)
{
  arena_publish_leave(a);
}

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
void lj_arena_sweep_words(GCArena *a, int preserve_marks)
{
  uint32_t w;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t b = la_load64_acq(&a->block[w]);
    uint64_t m = la_load64_acq(&a->mark[w]);
    la_store64_rel(&a->block[w], b & m);
    la_store64_rel(&a->mark[w], preserve_marks ? (b | m) : (b ^ m));
  }
}

void lj_arena_scan_free_runs(const GCArena *a, LJArenaRunCB cb, void *ud)
{
  int32_t run_start = -1;
  uint32_t i = LJ_AFIRST_CELL;
  while (i < LJ_ARENA_CELLS) {
    uint64_t starts = (la_load64_rlx(&a->block[i >> 6]) |
		       la_load64_rlx(&a->mark[i >> 6])) >> (i & 63);
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

#if defined(_WIN32)
static void *arena_map_aligned(PRNGState *rs, size_t keep)
{
  int olderr = errno;
  uintptr_t hint = 0;
  int retry;
  if (!arena_addr_ok(LJ_ARENA_MMAP_LOWER, keep))
    return NULL;
  for (retry = 0; retry < LJ_ARENA_MMAP_PROBE_MAX; retry++) {
    void *p = VirtualAlloc(hint ? (void *)hint : NULL, keep,
			   MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    uintptr_t addr = (uintptr_t)p;
    if (p) {
      if ((addr & LJ_ARENA_MASK) == 0 && arena_addr_ok(addr, keep)) {
	errno = olderr;
	return p;
      }
      VirtualFree(p, 0, MEM_RELEASE);
    }
    if (hint && retry < LJ_ARENA_MMAP_PROBE_LINEAR) {
      hint += 0x1000000u;
      if (!arena_addr_ok(hint, keep))
	hint = 0;
      continue;
    }
    hint = arena_random_hint(rs, keep);
  }
  errno = olderr;
  return NULL;
}

static void arena_unmap_aligned(void *p, size_t size)
{
  UNUSED(size);
  VirtualFree(p, 0, MEM_RELEASE);
}

static void *arena_os_map(size_t size)
{
  return VirtualAlloc(NULL, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
}

static void arena_os_unmap(void *p, size_t size)
{
  UNUSED(size);
  VirtualFree(p, 0, MEM_RELEASE);
}
#else
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

static void arena_unmap_aligned(void *p, size_t size)
{
  munmap(p, size);
}

static void *arena_os_map(size_t size)
{
  void *p = mmap(NULL, size, PROT_READ|PROT_WRITE,
		 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}

static void arena_os_unmap(void *p, size_t size)
{
  munmap(p, size);
}
#endif

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
  if (a) {
    free(lj_arena_gc2_tabstamp_acq(a));
    arena_unmap_aligned((void *)a, LJ_ARENA_SIZE);
  }
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
    arena_unmap_aligned((void *)lj_arena_of(p), mapsize);
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

/* Metadata transitions which confer mapping/header ownership must also prove
** that the address half of the open-addressed slot is unchanged. A 64-bit
** metadata CAS after hugetab_search() can otherwise mutate a reused slot, or
** race a 128-bit delete which has already made the mapping unaddressable. */
static int hugetab_cas_meta(LJHugeEnt *e, uint64_t addr, uint64_t oldmeta,
			    uint64_t newmeta)
{
  la_u128 exp, des;
  exp.lo = addr;
  exp.hi = oldmeta;
  des.lo = addr;
  des.hi = newmeta;
  return la_cas128(&e->slot, &exp, des);
}

int lj_arena_hugetab_init(HugeTab *ht, uint32_t hbits)
{
  int olderr = errno;
  size_t mapsize = hugetab_mapsize(hbits);
  LJHugeTabHdr *h;
  if (!ht || ht->h || !mapsize)
    return 0;
  h = (LJHugeTabHdr *)arena_os_map(mapsize);
  if (!h)
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
    arena_os_unmap((void *)h, mapsize);
  }
  errno = olderr;
}

uint32_t lj_arena_hugetab_fini_all(HugeTab *ht)
{
  int olderr = errno;
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap, unmapped = 0;
  if (!h)
    return 0;
  /* Terminal single-owner destruction. Ordinary fini deliberately releases
  ** only the side table because live mappings may already have transferred to
  ** another allocator. Here every slot is detached first and a mapping is
  ** unmapped exactly once. Dead-allocator transfer transactionally tombstones
  ** each source slot before changing the mapping header owner, so a later
  ** destination-capacity failure leaves every moved prefix destination-only
  ** and every unmoved suffix source-only. Thus this table is authoritative and
  ** no possibly stale mapping header need be sampled here. BUSY/FREEING/
  ** RETIRED are runtime arbitration states; after freeall and the terminal
  ** registry grace there is no actor left to complete them, so the full-slot
  ** CAS supersedes all of them. */
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    for (;;) {
      uint64_t addr = la_load64_acq(&e->slot.lo);
      uint64_t meta;
      la_u128 exp, des;
      if (addr <= LJ_HUGETAB_TOMBSTONE)
	break;
      meta = la_load64_acq(&e->slot.hi);
      if (la_load64_acq(&e->slot.lo) != addr)
	continue;
      exp.lo = addr;
      exp.hi = meta;
      des.lo = LJ_HUGETAB_TOMBSTONE;
      des.hi = 0;
      if (!la_cas128(&e->slot, &exp, des))
	continue;
      {
	size_t size = (size_t)(meta >> LJ_HUGETAB_META_SHIFT);
	lj_arena_huge_unmap((void *)(uintptr_t)addr, size);
	unmapped++;
      }
      break;
    }
  }
  lj_arena_hugetab_fini(ht);
  errno = olderr;
  return unmapped;
}

int lj_arena_hugetab_forget_terminal(HugeTab *ht, const void *p,
				      LJHugeInfo *hi)
{
  int olderr = errno;
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    la_u128 exp, des;
    if (!hugetab_search(h, addr, &e, &meta)) {
      errno = olderr;
      return 0;
    }
    exp.lo = addr;
    exp.hi = meta;
    des.lo = LJ_HUGETAB_TOMBSTONE;
    des.hi = 0;
    if (la_cas128(&e->slot, &exp, des)) {
      hugetab_decode(meta, hi);
      errno = olderr;
      return 1;
    }
  }
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
    if (hi)
      hugetab_decode(meta, hi);
    return 1;
  }
  return 0;
}

int lj_arena_hugetab_range_lookup(HugeTab *ht, const void *p, void **basep,
				  LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t target;
  uint32_t i, cap;
  if (!h || !p)
    return 0;
  target = (uint64_t)(uintptr_t)p;
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);  /* 04 §4.5.1 slot state. */
    if (addr > LJ_HUGETAB_TOMBSTONE) {
      uint64_t meta = la_load64_acq(&e->slot.hi);  /* 04 §4.5.1 metadata. */
      if (la_load64_acq(&e->slot.lo) == addr) {  /* Stable snapshot. */
	size_t size = (size_t)(meta >> LJ_HUGETAB_META_SHIFT);
	if (target >= addr && target - addr < (uint64_t)size) {
	  if (basep)
	    *basep = (void *)(uintptr_t)addr;
	  hugetab_decode(meta, hi);
	  return 1;
	}
      }
    }
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
  for (;;) {
    uint64_t newmeta;
    if (!hugetab_search(h, addr, &e, &oldmeta))
      return -1;
    if (oldmeta & LJ_HUGEF_FREEING)
      return -1;  /* Destructor ownership has crossed its grace LP. */
    newmeta = (oldmeta | LJ_HUGEF_MARK) & ~(uint64_t)LJ_HUGEF_RETIRED;
    if (hugetab_cas_meta(e, addr, oldmeta, newmeta)) {
      hugetab_decode(newmeta, hi);
      if (oldmeta & LJ_HUGEF_RETIRED)
	return 2;  /* Exact detached GC header must be reanchored after grace. */
      return (oldmeta & LJ_HUGEF_MARK) ? 0 : 1;
    }
  }
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
    while (addr > LJ_HUGETAB_TOMBSTONE &&
	   la_load64_acq(&e->slot.lo) == addr) {
      uint64_t meta = la_load64_acq(&e->slot.hi);
      if (!(meta & LJ_HUGEF_MARK) ||
	  hugetab_cas_meta(e, addr, meta,
			   meta & ~(uint64_t)LJ_HUGEF_MARK))
	break;
    }
  }
}

void lj_arena_hugetab_prepare_sweep(HugeTab *ht)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap;
  if (!h)
    return;
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);
    if (addr > LJ_HUGETAB_TOMBSTONE) {
      uint64_t meta = la_load64_acq(&e->slot.hi);
      while ((meta & LJ_HUGEF_TRAVERSABLE) != 0 &&
	     !(meta & (LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING|
		       LJ_HUGEF_TICKET|LJ_HUGEF_BUSY))) {
	uint64_t next = meta | LJ_HUGEF_SWEEP_OLD;
	if (hugetab_cas_meta(e, addr, meta, next))
	  break;
	if (la_load64_acq(&e->slot.lo) != addr)
	  break;
	meta = la_load64_acq(&e->slot.hi);
      }
    }
  }
}

void lj_arena_hugetab_abort_sweep(HugeTab *ht)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap;
  if (!h)
    return;
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);
    if (addr > LJ_HUGETAB_TOMBSTONE) {
      uint64_t meta = la_load64_acq(&e->slot.hi);
      while (la_load64_acq(&e->slot.lo) == addr &&
	     (meta & LJ_HUGEF_SWEEP_OLD) &&
	     !(meta & (LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING|
		       LJ_HUGEF_TICKET|LJ_HUGEF_BUSY))) {
	uint64_t next = (meta | LJ_HUGEF_MARK) &
			~(uint64_t)LJ_HUGEF_SWEEP_OLD;
	if (hugetab_cas_meta(e, addr, meta, next))
	  break;
	if (la_load64_acq(&e->slot.lo) != addr)
	  break;
	meta = la_load64_acq(&e->slot.hi);
      }
    }
  }
}

void lj_arena_hugetab_finish_sweep(HugeTab *ht, int preserve_marks)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap;
  if (!h)
    return;
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);
    if (addr > LJ_HUGETAB_TOMBSTONE) {
      uint64_t meta = la_load64_acq(&e->slot.hi);
      for (;;) {
	uint64_t next;
	if (la_load64_acq(&e->slot.lo) != addr ||
	    !(meta & LJ_HUGEF_SWEEP_OLD) ||
	    !(meta & LJ_HUGEF_MARK) ||
	    (meta & (LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING|
		     LJ_HUGEF_TICKET|LJ_HUGEF_BUSY)) ||
	    la_loadptr_acq((void *const *)&lj_arena_of(
	      (void *)(uintptr_t)addr)->hdr.retire_obj) != NULL)
	  break;
	next = meta & ~(uint64_t)LJ_HUGEF_SWEEP_OLD;
	if (!preserve_marks)
	  next &= ~(uint64_t)LJ_HUGEF_MARK;
	if (hugetab_cas_meta(e, addr, meta, next)) {
	  GCArena *a = lj_arena_of((void *)(uintptr_t)addr);
	  la_store64_rel(&a->hdr.retire_epoch, 0);
	  la_storeptr_rel(&a->hdr.retire_obj, NULL);
	  break;
	}
	if (la_load64_acq(&e->slot.lo) != addr)
	  break;
	meta = la_load64_acq(&e->slot.hi);
      }
    }
  }
}

int lj_arena_hugetab_sweep_next(HugeTab *ht, uint32_t *cursor,
				 void **pp, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap;
  if (pp)
    *pp = NULL;
  if (!h || !cursor)
    return 0;
  cap = h->mask + 1u;
  for (i = *cursor; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);
    *cursor = i + 1u;
    if (addr > LJ_HUGETAB_TOMBSTONE) {
      uint64_t meta = la_load64_acq(&e->slot.hi);
      if (la_load64_acq(&e->slot.lo) == addr &&
	  (meta & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_TRAVERSABLE)) ==
	    (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_TRAVERSABLE)) {
	if (pp)
	  *pp = (void *)(uintptr_t)addr;
	hugetab_decode(meta, hi);
	return 1;
      }
    }
  }
  return 0;
}

int lj_arena_hugetab_has_sweep_old(HugeTab *ht)
{
  uint32_t cursor = 0;
  void *p;
  return lj_arena_hugetab_sweep_next(ht, &cursor, &p, NULL);
}

int lj_arena_hugetab_retire(HugeTab *ht, const void *p, const void *obj,
			    uint64_t retire_epoch, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p || !obj)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  /* BUSY pins the mapping before either header field is touched. This matters
  ** even for a losing retirement attempt: an external free may otherwise win
  ** deletion between search and these stores, or its fresh-grace sentinel may
  ** be overwritten by a retire attempt which cannot publish TICKET. */
  for (;;) {
    uint64_t busy, next;
    if (!hugetab_search(h, addr, &e, &meta))
      return 0;
    if (!(meta & LJ_HUGEF_SWEEP_OLD))
      return 0;
    if (meta & LJ_HUGEF_TICKET) {
      hugetab_decode(meta, hi);
      return 1;
    }
    if (meta & LJ_HUGEF_BUSY)
      return 0;
    busy = meta | LJ_HUGEF_BUSY;
    if (!hugetab_cas_meta(e, addr, meta, busy))
      continue;
    {
      GCArena *a = lj_arena_of(p);
      la_storeptr_rel(&a->hdr.retire_obj, (void *)obj);
      /* FREEING already carries the external publisher's fresh-grace
      ** sentinel. Root detachment may still add its exact TICKET afterward,
      ** but must not weaken that later physical-free epoch. */
      if (!(busy & LJ_HUGEF_FREEING))
	la_store64_rel(&a->hdr.retire_epoch, retire_epoch);
    }
    /* MARK may be added while BUSY is held. Preserve it and publish TICKET
    ** only after the exact header fields are release-visible. */
    for (;;) {
      next = (busy | LJ_HUGEF_TICKET) & ~(uint64_t)LJ_HUGEF_BUSY;
      if (!(busy & (LJ_HUGEF_MARK|LJ_HUGEF_FREEING)))
	next |= LJ_HUGEF_RETIRED;
      else
	next &= ~(uint64_t)LJ_HUGEF_RETIRED;
      if (hugetab_cas_meta(e, addr, busy, next)) {
	hugetab_decode(next, hi);
	return 1;
      }
      if (la_load64_acq(&e->slot.lo) != addr)
	return 0;
      busy = la_load64_acq(&e->slot.hi);
      if (!(busy & LJ_HUGEF_BUSY))
	return 0;
    }
  }
}

int lj_arena_hugetab_claim_freeing(HugeTab *ht, const void *p,
					    LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta))
      return 0;
    if (!(meta & LJ_HUGEF_RETIRED) ||
	(meta & (LJ_HUGEF_MARK|LJ_HUGEF_FREEING|LJ_HUGEF_BUSY)))
      return 0;
    next = (meta & ~(uint64_t)LJ_HUGEF_RETIRED) | LJ_HUGEF_FREEING;
    if (hugetab_cas_meta(e, addr, meta, next)) {
      hugetab_decode(next, hi);
      return 1;
    }
  }
}

int lj_arena_hugetab_claim_live_ticket(HugeTab *ht, const void *p,
					       LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta))
      return 0;
    if ((meta & (LJ_HUGEF_MARK|LJ_HUGEF_TICKET)) !=
	(LJ_HUGEF_MARK|LJ_HUGEF_TICKET) ||
	(meta & (LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING|LJ_HUGEF_BUSY)))
      return 0;
    /* BUSY is a transient ownership claim. TICKET stays set until the exact
    ** header is linked and retire_obj has been cleared. */
    next = meta | LJ_HUGEF_BUSY;
    if (hugetab_cas_meta(e, addr, meta, next)) {
      hugetab_decode(next, hi);
      return 1;
    }
  }
}

int lj_arena_hugetab_finish_live_ticket(HugeTab *ht, const void *p,
						LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta))
      return 0;
    if ((meta & (LJ_HUGEF_MARK|LJ_HUGEF_TICKET|LJ_HUGEF_BUSY)) !=
	(LJ_HUGEF_MARK|LJ_HUGEF_TICKET|LJ_HUGEF_BUSY) ||
	(meta & (LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING)))
      return 0;
    next = meta & ~(uint64_t)(LJ_HUGEF_TICKET|LJ_HUGEF_BUSY);
    if (hugetab_cas_meta(e, addr, meta, next)) {
      hugetab_decode(next, hi);
      return 1;
    }
  }
}

enum {
  LJ_HUGE_EXT_MISSING = 0,
  LJ_HUGE_EXT_CLAIMED = 1,
  LJ_HUGE_EXT_OWNED = 2,
  LJ_HUGE_EXT_CONTENDED = 3
};

/* Atomically choose the external-free side of prepare-vs-free. If PREPARE has
** not published SWEEP_OLD, BUSY makes this caller the terminal table deleter.
** If PREPARE won, the same CAS pins the mapping until finish hands it to the
** sole sweep deleter. No header access precedes this ownership transition. */
static int hugetab_claim_external_free(HugeTab *ht, const void *p,
					int require_sweep, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return LJ_HUGE_EXT_MISSING;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta))
      return LJ_HUGE_EXT_MISSING;
    if (require_sweep && !(meta & LJ_HUGEF_SWEEP_OLD))
      return LJ_HUGE_EXT_MISSING;
    if (meta & LJ_HUGEF_FREEING) {
      hugetab_decode(meta, hi);
      return LJ_HUGE_EXT_OWNED;
    }
    if (meta & LJ_HUGEF_BUSY) {
      hugetab_decode(meta, hi);
      return LJ_HUGE_EXT_CONTENDED;  /* Another operation won the racy LP. */
    }
    next = (meta & ~(uint64_t)(LJ_HUGEF_MARK|LJ_HUGEF_RETIRED)) |
	   LJ_HUGEF_FREEING|LJ_HUGEF_BUSY;
    if (hugetab_cas_meta(e, addr, meta, next)) {
      if (next & LJ_HUGEF_SWEEP_OLD)
	la_store64_rel(&lj_arena_of(p)->hdr.retire_epoch, ~(uint64_t)0);
      hugetab_decode(next, hi);
      return LJ_HUGE_EXT_CLAIMED;
    }
  }
}

int lj_arena_hugetab_claim_external_free(HugeTab *ht, const void *p,
					   LJHugeInfo *hi)
{
  return hugetab_claim_external_free(ht, p, 0, hi) ==
	 LJ_HUGE_EXT_CLAIMED;
}

/* A realloc pin is nonterminal: it excludes prepare/free/header teardown while
** preserving the old allocation if replacement allocation fails. The claim
** itself is the realloc-vs-free LP; a competing external free which observes
** BUSY loses that racy operation without ever touching the mapping. */
static int hugetab_claim_realloc(HugeTab *ht, const void *p, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta) ||
	(meta & (LJ_HUGEF_BUSY|LJ_HUGEF_FREEING|
		 LJ_HUGEF_RETIRED|LJ_HUGEF_TICKET)))
      return 0;
    next = meta | LJ_HUGEF_BUSY;
    if (hugetab_cas_meta(e, addr, meta, next)) {
      hugetab_decode(next, hi);
      return 1;
    }
  }
}

static int hugetab_finish_realloc_keep(HugeTab *ht, const void *p,
					size_t nsize, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t packed_addr, next;
    uint32_t flags;
    if (!hugetab_search(h, addr, &e, &meta) ||
	!(meta & LJ_HUGEF_BUSY) || (meta & LJ_HUGEF_FREEING))
      return 0;
    flags = (uint32_t)meta & LJ_HUGEF_MASK;
    flags &= ~LJ_HUGEF_BUSY;
    if (!hugetab_pack((void *)(uintptr_t)addr, nsize, flags,
		      &packed_addr, &next) || packed_addr != addr)
      return 0;
    if (hugetab_cas_meta(e, addr, meta, next)) {
      hugetab_decode(next, hi);
      return 1;
    }
  }
}

static int hugetab_release_realloc(HugeTab *ht, const void *p,
				    LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta) ||
	!(meta & LJ_HUGEF_BUSY) || (meta & LJ_HUGEF_FREEING))
      return 0;
    next = meta & ~(uint64_t)LJ_HUGEF_BUSY;
    if (hugetab_cas_meta(e, addr, meta, next)) {
      hugetab_decode(next, hi);
      return 1;
    }
  }
}

static int hugetab_realloc_to_external_free(HugeTab *ht, const void *p,
					      LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta) ||
	!(meta & LJ_HUGEF_BUSY) || (meta & LJ_HUGEF_FREEING))
      return 0;
    next = (meta & ~(uint64_t)(LJ_HUGEF_MARK|LJ_HUGEF_RETIRED)) |
	   LJ_HUGEF_FREEING;  /* Retain BUSY continuously through the copy. */
    if (hugetab_cas_meta(e, addr, meta, next)) {
      if (next & LJ_HUGEF_SWEEP_OLD)
	la_store64_rel(&lj_arena_of(p)->hdr.retire_epoch, ~(uint64_t)0);
      hugetab_decode(next, hi);
      return 1;
    }
  }
}

int lj_arena_hugetab_finish_external_free(HugeTab *ht, const void *p,
					    LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return LJ_ARENA_HUGE_FINISH_LOST;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    la_u128 exp, des;
    if (!hugetab_search(h, addr, &e, &meta) ||
	(meta & (LJ_HUGEF_FREEING|LJ_HUGEF_BUSY)) !=
	  (LJ_HUGEF_FREEING|LJ_HUGEF_BUSY))
      return LJ_ARENA_HUGE_FINISH_LOST;
    exp.lo = addr;
    exp.hi = meta;
    if (meta & LJ_HUGEF_SWEEP_OLD) {
      uint64_t next = meta & ~(uint64_t)LJ_HUGEF_BUSY;
      /* This is the final header store while BUSY excludes both reanchor and
      ** retirement publication. Its release edge precedes exposing FREEING
      ** to the sweep owner, which must complete a fresh grace before unmap. */
      la_store64_rel(&lj_arena_of(p)->hdr.retire_epoch, ~(uint64_t)0);
      des.lo = addr;
      des.hi = next;
      if (la_cas128(&e->slot, &exp, des)) {
	hugetab_decode(next, hi);
	return LJ_ARENA_HUGE_FINISH_DEFERRED;
      }
    } else {
      des.lo = LJ_HUGETAB_TOMBSTONE;
      des.hi = 0;
      if (la_cas128(&e->slot, &exp, des)) {
	hugetab_decode(meta, hi);
	return LJ_ARENA_HUGE_FINISH_UNMAP;
      }
    }
  }
}

int lj_arena_hugetab_defer_external_free(HugeTab *ht, const void *p,
					   LJHugeInfo *hi)
{
  LJHugeInfo snap;
  int claim = hugetab_claim_external_free(ht, p, 1, &snap);
  if (claim == LJ_HUGE_EXT_MISSING || claim == LJ_HUGE_EXT_CONTENDED)
    return 0;
  if (claim == LJ_HUGE_EXT_OWNED) {
    if (hi)
      *hi = snap;
    return 1;  /* A nonwaiting duplicate never touches the mapping header. */
  }
  if (lj_arena_hugetab_finish_external_free(ht, p, &snap) !=
      LJ_ARENA_HUGE_FINISH_DEFERRED)
    return 0;
  if (hi)
    *hi = snap;
  return 1;
}

int lj_arena_hugetab_revert_retired(HugeTab *ht, const void *p)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta))
      return 0;
    if (!(meta & LJ_HUGEF_FREEING) || (meta & LJ_HUGEF_BUSY))
      return 0;
    next = (meta & ~(uint64_t)LJ_HUGEF_FREEING) | LJ_HUGEF_RETIRED;
    if (hugetab_cas_meta(e, addr, meta, next))
      return 1;
  }
}

uint64_t lj_arena_hugetab_live_bytes(HugeTab *ht, uint32_t required_flags)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t bytes = 0;
  uint32_t i, cap;
  if (!h)
    return 0;
  required_flags &= LJ_HUGEF_MASK;
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);  /* 04 §4.5.1 slot state. */
    if (addr > LJ_HUGETAB_TOMBSTONE) {
      uint64_t meta = la_load64_acq(&e->slot.hi);  /* 04 §4.5.1 metadata. */
      if (la_load64_acq(&e->slot.lo) == addr) {  /* Stable snapshot. */
	uint32_t hflags = (uint32_t)(meta & LJ_HUGETAB_META_MASK);
	if ((hflags & required_flags) == required_flags) {
	  size_t size = (size_t)(meta >> LJ_HUGETAB_META_SHIFT);
	  if (bytes > ~(uint64_t)0 - (uint64_t)size)
	    bytes = ~(uint64_t)0;
	  else
	    bytes += (uint64_t)size;
	}
      }
    }
  }
  return bytes;
}

int lj_arena_hugetab_transfer(HugeTab *dst, HugeTab *src, uint32_t owner_tid)
{
  LJHugeTabHdr *h = src ? src->h : NULL;
  uint32_t i, cap;
  if (!h)
    return 1;
  if (dst == src)
    return 1;
  if (!dst || !dst->h)
    return 0;
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);
    if (addr > LJ_HUGETAB_TOMBSTONE) {
      uint64_t meta = la_load64_acq(&e->slot.hi);
      if (la_load64_acq(&e->slot.lo) == addr) {
	void *p = (void *)(uintptr_t)addr;
	size_t size = (size_t)(meta >> LJ_HUGETAB_META_SHIFT);
	uint32_t hflags = (uint32_t)(meta & LJ_HUGETAB_META_MASK);
	int inserted = lj_arena_hugetab_insert(dst, p, size, hflags);
	if (inserted < 0)
	  return 0;
	if (inserted == 0) {
	  LJHugeInfo existing;
	  if (lj_arena_hugetab_lookup(dst, p, &existing) != 1 ||
	      existing.size != size || existing.flags != hflags)
	    return 0;
	}
	/* The source owner is dead and the surrounding TG writer gate has proved
	** quiescence. Make each entry transfer transactional even if an abandoned
	** BUSY/FREEING state would make the ordinary delete refuse: destination
	** insert/confirm, exact source tombstone, then and only then publish the new
	** header owner. A later capacity failure can never leave a stale source
	** duplicate pointing at a mapping which the destination may unmap. */
	if (!lj_arena_hugetab_forget_terminal(src, p, NULL)) {
	  if (inserted > 0 &&
	      !lj_arena_hugetab_forget_terminal(dst, p, NULL))
	    abort();  /* Never return while leaving a new duplicate behind. */
	  return 0;
	}
	lj_arena_owner_rel(lj_arena_of(p), owner_tid);
      }
    }
  }
  return 1;
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
    if (meta & LJ_HUGEF_BUSY)
      return 0;  /* Header publication/reanchor still owns the mapping. */
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

static uint32_t arena_registry_hflags(uint32_t flags)
{
  uint32_t hflags = 0;
  if (flags & LJ_AF_TRAVERSABLE)
    hflags |= LJ_HUGEF_TRAVERSABLE;
  return hflags;
}

static void arena_registered_set(GCArena *a)
{
  uint32_t old;
  if (!a)
    return;
  old = lj_arena_flags_acq(a);
  while (!(old & LJ_AF_REGISTERED)) {
    uint32_t expect = old;
    if (la_cas32(&a->hdr.flags, &expect, old | LJ_AF_REGISTERED,
		 LA_ACQ_REL, LA_ACQ))
      return;
    old = expect;
  }
}

static void arena_registered_clear(GCArena *a)
{
  uint32_t old;
  if (!a)
    return;
  old = lj_arena_flags_acq(a);
  while (old & LJ_AF_REGISTERED) {
    uint32_t expect = old;
    if (la_cas32(&a->hdr.flags, &expect,
		 old & (uint32_t)~LJ_AF_REGISTERED, LA_ACQ_REL, LA_ACQ))
      return;
    old = expect;
  }
}

static uint32_t arena_bin(uint32_t ncells)
{
  return lj_arena_bin_from_ncells(ncells);
}

#define LJ_ARENA_BIN_WALK_LIMIT 8192u

static void arena_set_extent(GCArena *a, uint32_t cell)
{
  lj_arena_block_clear(a, cell);
  lj_arena_bm_clear(a->mark, cell);
}

static void arena_set_alloc(GCArena *a, uint32_t cell, uint32_t ncells,
			    int black)
{
  uint32_t i;
  lj_assertX(ncells != 0 && cell >= LJ_AFIRST_CELL &&
	     ncells <= LJ_ARENA_CELLS - cell,
	     "arena allocation extent out of range");
  /* Rebuilt free runs coalesce adjacent state-1 boundaries. Consuming such a
  ** run must erase every old interior boundary before publishing its new
  ** allocation start; otherwise a later rebuild can relink a suffix of this
  ** live allocation as reusable storage. */
  for (i = 1; i < ncells; i++)
    arena_set_extent(a, cell + i);
  if (black)
    lj_arena_bm_set(a->mark, cell);
  else
    lj_arena_bm_clear(a->mark, cell);
  /* Release-publish block[] only after the initial mark is durable. */
  lj_arena_block_set(a, cell);
}

static void arena_set_free_run(GCArena *a, uint32_t start, uint32_t len)
{
  uint32_t i;
  lj_arena_block_clear(a, start);
  lj_arena_bm_set(a->mark, start);
  for (i = 1; i < len; i++)
    arena_set_extent(a, start + i);
}

static void arena_link_run_head(TGAlloc *alloc, GCArena *a, uint32_t start,
				uint32_t len)
{
  uint32_t k = arena_kind(a->hdr.flags);
  uint32_t b = arena_bin(len);
  LJArenaFreeRun *run = (LJArenaFreeRun *)lj_arena_cellptr(a, start);
  run->start = start;
  run->len = len;
  run->next = alloc->bins[k][b];
  alloc->bins[k][b] = run;
  alloc->binmask[k] |= (uint32_t)1u << b;
}

static void arena_insert_run_head(TGAlloc *alloc, GCArena *a, uint32_t start,
				  uint32_t len)
{
  arena_set_free_run(a, start, len);
  arena_link_run_head(alloc, a, start, len);
}

static LJ_AINLINE int arena_free_run_ptr_ok(const LJArenaFreeRun *run)
{
  uintptr_t addr = (uintptr_t)run;
  return checkptrGC(run) && (addr & (LJ_CELL_SIZE-1u)) == 0 &&
	 (addr & LJ_ARENA_MASK) >=
	 ((uintptr_t)LJ_AFIRST_CELL << LJ_CELL_SHIFT);
}

static LJ_AINLINE int arena_free_run_valid_knownptr(const LJArenaFreeRun *run,
						    uint32_t *lenp)
{
  GCArena *a;
  uint32_t start, len;
  a = lj_arena_of(run);
  start = run->start;
  len = run->len;
  /*
  ** Free-run bin nodes live in the first cell of the free run they describe.
  ** Allocating from a bin can leave old payload bytes in cells that later sit
  ** behind a defensive bin pointer. Keep the exact-address duplicate scrub,
  ** but also drop any node whose bitmap state no longer says "free run start".
  */
  if (start < LJ_AFIRST_CELL || start >= LJ_ARENA_CELLS || len == 0 ||
      len > LJ_ARENA_CELLS - start ||
      lj_arena_cellptr(a, start) != (void *)run ||
      lj_arena_state(a, start) != 1)
    return 0;
  if (lenp) *lenp = len;
  return 1;
}

static void arena_insert_run(TGAlloc *alloc, GCArena *a, uint32_t start,
					     uint32_t len)
{
  uint32_t k = arena_kind(a->hdr.flags);
  uint32_t b = arena_bin(len);
  LJArenaFreeRun *run = (LJArenaFreeRun *)lj_arena_cellptr(a, start);
  LJArenaFreeRun **pp = &alloc->bins[k][b];
  uint32_t steps = 0;
  int scrub_head = 1;
  while (*pp) {
    LJArenaFreeRun *cur = *pp;
    LJArenaFreeRun *next;
    if (!arena_free_run_ptr_ok(cur)) {
      *pp = NULL;
      break;
    }
    next = cur->next;
    if (cur == run) {
      *pp = next == cur ? NULL : next;
      continue;
    }
    if (next == cur || ++steps > LJ_ARENA_BIN_WALK_LIMIT) {
      *pp = NULL;
      break;
    }
    /*
    ** Scrub stale leading nodes before publishing a new run, but leave full
    ** per-node validation to arena_find_run(), which must validate before
    ** reuse anyway. This keeps insertion from turning long valid bins into a
    ** bitmap-walking hot path.
    */
    if (scrub_head && !arena_free_run_valid_knownptr(cur, NULL)) {
      *pp = next;
      continue;
    }
    scrub_head = 0;
    pp = &cur->next;
  }
  arena_insert_run_head(alloc, a, start, len);
}

static LJ_AINLINE uint32_t arena_remote_meta(uint32_t start, uint32_t len)
{
  lj_assertX(start < (1u << 12) && len < (1u << 12),
	     "arena remote-free metadata overflow");
  return (start & 0xffu) | ((start & 0xf00u) << 8) | (len << 20);
}

static LJ_AINLINE uint32_t arena_remote_start(const LJArenaRemoteFree *node)
{
  uint32_t meta = la_load32_acq(&node->meta);
  return (meta & 0xffu) | ((meta >> 8) & 0xf00u);
}

static LJ_AINLINE uint32_t arena_remote_len(const LJArenaRemoteFree *node)
{
  return la_load32_acq(&node->meta) >> 20;
}

static LJ_AINLINE void arena_remote_set_meta(LJArenaRemoteFree *node,
					      uint32_t start, uint32_t len)
{
  node->reserved = 0;
  la_store32_rel(&node->meta, arena_remote_meta(start, len));
}

/* A terminal/grace-late free needs no intrusive size record. The allocation
** bitmap already identifies its exact start and extent, while this atomic bit
** pins that allocation until a later PREPSWEEP and grace consume it. */
static int arena_late_pin(GCArena *a, const void *p, size_t size)
{
  uint32_t start, ncells;
  if (!a || !p || size == 0)
    return -1;
  start = lj_arena_cellof(p);
  ncells = lj_arena_ncells(size);
  if (start < LJ_AFIRST_CELL || start >= LJ_ARENA_CELLS ||
      ncells > LJ_ARENA_CELLS - start ||
      lj_arena_cellptr(a, start) != p)
    return -1;
  /* Admission count precedes this atomic classification. An adoption cannot
  ** open/reuse the cell until leave; a terminal apply racing block 1->0 makes
  ** this at worst a duplicate bit which stable adoption removes. */
  if (!((la_load64_acq(&a->block[start >> 6]) >> (start & 63)) & 1u))
    return -1;
  (void)la_bit_test_and_set64(&a->late[start >> 6], start & 63);
  arena_progress_wake(a);
  return 1;
}

static int arena_remote_late_publish(GCArena *a, void *p, size_t size)
{
  int entered = arena_remote_late_enter(a);
  int published;
  if (entered <= 0)
    return entered;  /* Zero means reopened/retry; negative means retain. */
  published = arena_late_pin(a, p, size);
  arena_remote_late_leave(a);
  return published;
}

int lj_arena_quarantine_owns_body(const void *p, size_t size)
{
  GCArena *a;
  uint32_t cell, ncells, state, flags;
  if (!p || size == 0 || size > LJ_HUGE_THRESHOLD)
    return 0;
  cell = lj_arena_cellof(p);
  ncells = lj_arena_ncells(size);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
      ncells > LJ_ARENA_CELLS - cell)
    return 0;
retry_open:
  a = lj_arena_of(p);
  if (lj_arena_cellptr(a, cell) != p)
    return 0;
  flags = lj_arena_flags_acq(a);
  /* The sweep owner changes RETIRED to FREEING before invoking a type-specific
  ** destructor. Its eventual lj_mem_freegco_defer() re-enters this helper while
  ** the arena gate is intentionally CLOSED. Recognize that already-owned
  ** terminal state before the admission path; otherwise the owner queues its
  ** own body as a post-grace late free and terminal commit can never drain it.
  ** Atomic sweep state is the exact ownership proof, so peers observing the
  ** same terminal state may also discard duplicate physical-free requests.
  */
  if ((flags & (LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE)) &&
      lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_FREEING)
    return 1;
  /* A closed terminal/reclaimed gate owns bitmap publication. Publish only a
  ** sidecar late bit: the body remains intact and allocated until a later
  ** complete sweep/grace consumes the pin. */
  if (!arena_remote_enter(a)) {
    int late = arena_remote_late_publish(a, (void *)p, size);
    if (late == 0)
      goto retry_open;  /* Lock-free retry without growing the C stack. */
    return 1;
  }
  flags = lj_arena_flags_acq(a);
  if (!(flags & (LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE))) {
    if (flags & (LJ_AF_PREPSWEEP|LJ_AF_RECLAIMED)) {
      (void)arena_late_pin(a, p, size);
      arena_remote_leave(a);
      return 1;
    }
    arena_remote_leave(a);
    return 0;
  }
  /* This helper is called only at the physical-free boundary. Transfer the
  ** bitmap mutation to the sweep owner before returning to the destructor.
  ** A trace may publish FREEING just before its final gct=0 release store; the
  ** quarantine's trace-specific completion check closes that short window. */
  for (state = lj_arena_sweep_state_acq(a, cell);;) {
    if (state == LJ_ARENA_SWEEP_FREEING) {
      arena_remote_leave(a);
      return 1;
    }
    if (lj_arena_sweep_state_cas(a, cell, state,
					 LJ_ARENA_SWEEP_FREEING)) {
      if (state == LJ_ARENA_SWEEP_RETIRED) {
	uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	lj_assertX(old != 0, "arena quarantine deferred underflow");
	UNUSED(old);
      } else {
	  /* A physical free of a previously LIVE/raw cell can occur after the
	  ** arena's earlier grace. Force the owner to take a fresh epoch before
	  ** converting this cell into reusable bitmap space. */
	  la_store64_rel(&a->hdr.retire_epoch, ~(uint64_t)0);
      }
      arena_remote_leave(a);
      return 1;
    }
    state = lj_arena_sweep_state_acq(a, cell);
  }
}

int lj_arena_remote_free_publish(TGAlloc *alloc, void *p, size_t size)
{
  GCArena *a;
  LJArenaRemoteFree *node, *head;
  uint32_t start, ncells, oldstate;
  if (!alloc || !p || size == 0 || size > LJ_HUGE_THRESHOLD)
    return 0;
retry_open:
  a = lj_arena_of(p);
  if (lj_arena_alloc_free_noinsert_acq(alloc))
    return 1;
  if (!arena_remote_enter(a)) {
    int late = arena_remote_late_publish(a, p, size);
    if (late == 0)
      goto retry_open;
    return 1;
  }
  if (lj_arena_quarantine_owns_body(p, size)) {
    arena_remote_leave(a);
    return 1;
  }
  start = lj_arena_cellof(p);
  ncells = lj_arena_ncells(size);
  if (start < LJ_AFIRST_CELL || start + ncells > LJ_ARENA_CELLS ||
      !lj_arena_bm_get(a->block, start)) {
    arena_remote_leave(a);
    return 0;
  }
  /* A CLOSED late record for a committed-live cell remains intrusive after
  ** adoption. Its late bit is published before its queue link and is stable
  ** throughout the owned generation. Do not let a duplicate ordinary free
  ** change WHITE->FREEING and then rewrite that same node's next pointer into
  ** a self-cycle. The next PREPSWEEP drain is the sole consumer of the bit. */
  if (la_load64_acq(&a->late[start >> 6]) &
      ((uint64_t)1 << (start & 63))) {
    arena_remote_leave(a);
    return 1;
  }
  /* Deduplicate cross-owner destructor completion without touching the
  ** owner-local block bitmap. Allocation-state words are otherwise advisory
  ** outside sweep and are reset when the owner consumes this record. */
  oldstate = lj_arena_sweep_state_acq(a, start);
  for (;;) {
    if (oldstate == LJ_ARENA_SWEEP_FREEING) {
      arena_remote_leave(a);
      return 1;
    }
    if (lj_arena_sweep_state_cas(a, start, oldstate,
					 LJ_ARENA_SWEEP_FREEING))
      break;
    oldstate = lj_arena_sweep_state_acq(a, start);
  }
  node = (LJArenaRemoteFree *)p;
  arena_remote_set_meta(node, start, ncells);
  head = (LJArenaRemoteFree *)la_loadptr_acq(
    (void *const *)&a->hdr.remote_free);
  do {
    la_storeptr_rlx((void **)&node->next, head);
  } while (!la_casptr((void **)&a->hdr.remote_free, (void **)&head, node,
		       LA_REL, LA_ACQ));
  arena_remote_leave(a);
  return 1;
}

static uint32_t arena_remote_free_drain_one(TGAlloc *alloc, GCArena *a)
{
  LJArenaRemoteFree *node;
  uint32_t n = 0;
  uint32_t flags;
  if (!alloc || !a ||
      ((flags = lj_arena_flags_acq(a)) &
       (LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE|LJ_AF_PREPSWEEP)))
    return 0;
  node = (LJArenaRemoteFree *)la_xchgptr_acqrel(
    (void **)&a->hdr.remote_free, NULL);
  while (node) {
    LJArenaRemoteFree *next = (LJArenaRemoteFree *)la_loadptr_acq(
      (void *const *)&node->next);
    uint32_t start = arena_remote_start(node);
    uint32_t len = arena_remote_len(node);
    int valid = lj_arena_of(node) == a && start >= LJ_AFIRST_CELL &&
	start < LJ_ARENA_CELLS && len != 0 &&
	len <= LJ_ARENA_CELLS - start &&
	lj_arena_cellptr(a, start) == (void *)node;
    if (valid && lj_arena_bm_get(a->block, start)) {
      if (flags & LJ_AF_RECLAIMED)
	arena_set_free_run(a, start, len);
      else
	arena_insert_run(alloc, a, start, len);
      (void)lj_arena_sweep_state_cas(a, start,
	LJ_ARENA_SWEEP_FREEING, LJ_ARENA_SWEEP_WHITE);
      n++;
    }
    node = next;
  }
  return n;
}

uint32_t lj_arena_remote_free_drain_sweep(TGAlloc *alloc, GCArena *a)
{
  LJArenaRemoteFree *node;
  uint32_t n = 0;
  if (!alloc || !a ||
      !(lj_arena_flags_acq(a) &
	(LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE)))
    return 0;
  node = (LJArenaRemoteFree *)la_xchgptr_acqrel(
    (void **)&a->hdr.remote_free, NULL);
  while (node) {
    LJArenaRemoteFree *next = (LJArenaRemoteFree *)la_loadptr_acq(
      (void *const *)&node->next);
    uint32_t start = arena_remote_start(node);
    uint32_t len = arena_remote_len(node);
    if (lj_arena_of(node) == a && start >= LJ_AFIRST_CELL &&
	start < LJ_ARENA_CELLS && len != 0 &&
	len <= LJ_ARENA_CELLS - start &&
	lj_arena_cellptr(a, start) == (void *)node &&
	lj_arena_bm_get(a->block, start)) {
      uint32_t state = lj_arena_sweep_state_acq(a, start);
      while (state != LJ_ARENA_SWEEP_FREEING) {
	if (lj_arena_sweep_state_cas(a, start, state,
					   LJ_ARENA_SWEEP_FREEING)) {
	  if (state == LJ_ARENA_SWEEP_RETIRED) {
	    uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	    lj_assertX(old != 0, "arena remote-free deferred underflow");
	    UNUSED(old);
	  }
	  break;
	}
	state = lj_arena_sweep_state_acq(a, start);
      }
      n++;
    }
    node = next;
  }
  return n;
}

uint32_t lj_arena_remote_free_drain(TGAlloc *alloc)
{
  uint32_t k, n = 0;
  if (!alloc)
    return 0;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    GCArena *a;
    for (a = alloc->owned[k]; a != NULL; a = lj_arena_next_acq(a))
      n += arena_remote_free_drain_one(alloc, a);
  }
  return n;
}

static void arena_publish_bump_run(TGAlloc *alloc, uint32_t k)
{
  LJArenaBump *b;
  if (!alloc || k >= LJ_ARENA_NKINDS)
    return;
  b = &alloc->bump[k];
  if (!b->a || b->cell >= b->end)
    return;
  /*
  ** The active bump window is absent from the reusable free-run bins. Publish
  ** its unused tail before the window is replaced, otherwise lazy sweeping can
  ** strand one large free run per swept arena and force fresh arena mapping.
  */
  arena_insert_run_head(alloc, b->a, b->cell, b->end - b->cell);
  b->a = NULL;
  b->cell = 0;
  b->end = 0;
}

static void arena_refresh_binmask(TGAlloc *alloc, uint32_t k, uint32_t b)
{
  if (alloc->bins[k][b])
    alloc->binmask[k] |= (uint32_t)1u << b;
  else
    alloc->binmask[k] &= ~((uint32_t)1u << b);
}

static LJArenaFreeRun **arena_find_run(TGAlloc *alloc, uint32_t k,
				       uint32_t ncells, uint32_t *binp)
{
  uint32_t mask = alloc->binmask[k] & lj_arena_binmask_from_ncells(ncells);
  while (mask) {
    uint32_t b = lj_ffs(mask);
    LJArenaFreeRun **pp = &alloc->bins[k][b];
    uint32_t steps = 0;
    mask &= mask - 1u;
    while (*pp) {
      LJArenaFreeRun *run = *pp;
      LJArenaFreeRun *next;
      uint32_t len;
      if (++steps > LJ_ARENA_BIN_WALK_LIMIT) {
	alloc->bins[k][b] = NULL;
	arena_refresh_binmask(alloc, k, b);
	break;
      }
      if (!arena_free_run_ptr_ok(run)) {
	*pp = NULL;
	arena_refresh_binmask(alloc, k, b);
	break;
      }
      next = run->next;
      if (next == run) {
	*pp = NULL;
	arena_refresh_binmask(alloc, k, b);
	break;
      }
      if (!arena_free_run_valid_knownptr(run, &len)) {
	*pp = next;
	arena_refresh_binmask(alloc, k, b);
	continue;
      }
      if (len >= ncells) {
	*binp = b;
	return pp;
      }
      pp = &run->next;
    }
    arena_refresh_binmask(alloc, k, b);
  }
  return NULL;
}

static void arena_clear_bins(TGAlloc *alloc, uint32_t k)
{
  memset(alloc->bins[k], 0, sizeof(alloc->bins[k]));
  alloc->binmask[k] = 0;
}

static GCArena *arena_reclaimed_acq(const TGAlloc *alloc, uint32_t k)
{
  return (GCArena *)la_loadptr_acq((void *const *)&alloc->reclaimed[k]);
}

static int arena_reclaimed_cas(TGAlloc *alloc, uint32_t k,
				GCArena **oldp, GCArena *a)
{
  return la_casptr((void **)&alloc->reclaimed[k], (void **)oldp, a,
		   LA_ACQ_REL, LA_ACQ);
}

static void arena_sweep_state_reset(GCArena *a)
{
  uint32_t w;
  for (w = 0; w < LJ_ARENA_SWEEP_WORDS; w++)
    la_store64_rel(&a->sweep[w], 0);
}

static void arena_sweep_state_prepare(GCArena *a)
{
  uint32_t w;
  /* WHITE means "not detached/classified" during NEEDSWEEP. The mark bitmap
  ** still distinguishes live raw/fixed allocations. LIVE is reserved for an
  ** exact old GC header detached from the ownership spine (or rescued after
  ** retirement), so the post-grace pass can reanchor it exactly once. Keep an
  ** allocated FREEING start terminal throughout PREP: an intrusive remote-free
  ** record may already own and overwrite that body, and no committed reader may
  ** observe a transient WHITE gap before the stable queue drain. */
  for (w = 0; w < LJ_ARENA_SWEEP_WORDS; w++) {
    uint64_t old = la_load64_acq(&a->sweep[w]);
    uint64_t block = la_load64_acq(&a->block[w >> 1]);
    uint32_t starts = (uint32_t)(w & 1u ? block >> 32 : block);
    uint64_t next = 0;
    uint32_t j;
    for (j = 0; j < 32u; j++) {
      uint32_t cell = (w << 5) + j;
      if (cell >= LJ_AFIRST_CELL && (starts & ((uint32_t)1u << j)) &&
	  ((old >> (j << 1)) & 3u) == LJ_ARENA_SWEEP_FREEING)
	next |= (uint64_t)LJ_ARENA_SWEEP_FREEING << (j << 1);
    }
    la_store64_rel(&a->sweep[w], next);
  }
  a->hdr.reclaim_cell = LJ_AFIRST_CELL;
  a->hdr.reclaim_deferred = 0;
}

static void arena_prepare_bump_tail(GCArena *a)
{
  uint32_t cell, end, i;
  if (!a)
    return;
  cell = la_load32_acq(&a->hdr.prep_bump_cell);
  end = la_load32_acq(&a->hdr.prep_bump_end);
  if (cell >= LJ_AFIRST_CELL && cell < end && end <= LJ_ARENA_CELLS) {
    /* Committed PREP readers may mark another live cell in the same word.
    ** Structural publication must therefore update only its own bits with
    ** atomic RMWs instead of losing an unrelated concurrent mark OR. */
    (void)la_and64_rlx(&a->block[cell >> 6],
		       ~((uint64_t)1 << (cell & 63)));
    (void)la_or64_rlx(&a->mark[cell >> 6],
		      (uint64_t)1 << (cell & 63));
    for (i = cell + 1u; i < end; i++) {
      (void)la_and64_rlx(&a->block[i >> 6],
			 ~((uint64_t)1 << (i & 63)));
      (void)la_and64_rlx(&a->mark[i >> 6],
			 ~((uint64_t)1 << (i & 63)));
    }
  }
  la_store32_rel(&a->hdr.prep_bump_cell, 0);
  la_store32_rel(&a->hdr.prep_bump_end, 0);
}

/* Consume only pins which were visible before this PREPSWEEP generation.
** SEALED admission makes a concurrent publisher set PENDING first. A bit
** published after this exchange simply remains pinned for one extra cycle. */
static uint32_t arena_late_prepare_consume(GCArena *a)
{
  uint32_t w, n = 0;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t bits = la_load64_acq(&a->late[w]);
    while (bits) {
      uint32_t j = lj_ffs64(bits);
      uint32_t cell = (w << 6) + j;
      uint32_t state;
      bits &= bits - 1u;
      if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
	  !lj_arena_bm_get(a->block, cell)) {
	(void)la_and64_rlx(&a->late[w], ~((uint64_t)1 << j));
	continue;
      }
      state = lj_arena_sweep_state_acq(a, cell);
      while (state != LJ_ARENA_SWEEP_FREEING) {
	if (lj_arena_sweep_state_cas(a, cell, state,
					 LJ_ARENA_SWEEP_FREEING)) {
	  n++;
	  break;
	}
	state = lj_arena_sweep_state_acq(a, cell);
      }
      /* Publish terminal state before clearing the pin. A marker which misses
      ** the bit must observe/recheck FREEING before reading the body. */
      (void)la_and64_rlx(&a->late[w], ~((uint64_t)1 << j));
    }
  }
  return n;
}

/* A current-generation late pin represents an already-completed physical
** destructor. Keep the allocation body, never reconstruct its header, and
** settle transient detached accounting before the terminal bitmap scan. */
static uint32_t arena_quarantine_settle_late(GCArena *a)
{
  uint32_t w, n = 0;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t bits = la_load64_acq(&a->late[w]) &
		    la_load64_acq(&a->block[w]);
    while (bits) {
      uint32_t j = lj_ffs64(bits);
      uint32_t cell = (w << 6) + j;
      uint32_t state = lj_arena_sweep_state_acq(a, cell);
      bits &= bits - 1u;
      while (state != LJ_ARENA_SWEEP_WHITE) {
	if (lj_arena_sweep_state_cas(a, cell, state,
					 LJ_ARENA_SWEEP_WHITE)) {
	  if (state == LJ_ARENA_SWEEP_RETIRED) {
	    uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	    lj_assertX(old != 0, "arena late-pin deferred underflow");
	    UNUSED(old);
	  }
	  n++;
	  break;
	}
	state = lj_arena_sweep_state_acq(a, cell);
      }
    }
  }
  return n;
}

void lj_arena_alloc_set_registry(TGAlloc *alloc, HugeTab *tab)
{
  if (alloc)
    la_storeptr_rel((void **)&alloc->smalltab, tab);
}

HugeTab *lj_arena_alloc_registry_acq(const TGAlloc *alloc)
{
  return alloc ? (HugeTab *)la_loadptr_acq((void *const *)&alloc->smalltab) :
		 NULL;
}

int lj_arena_alloc_registry_lookup(const TGAlloc *alloc, const GCArena *a,
				   LJHugeInfo *hi)
{
  HugeTab *tab = lj_arena_alloc_registry_acq(alloc);
  return tab && a ? lj_arena_hugetab_lookup(tab, a, hi) : 0;
}

static int arena_registry_insert_fresh(TGAlloc *alloc, GCArena *a,
				       uint32_t flags)
{
  HugeTab *tab = lj_arena_alloc_registry_acq(alloc);
  if (!tab)
    return 1;
  if (lj_arena_hugetab_insert(tab, a, LJ_ARENA_SIZE,
			      arena_registry_hflags(flags)) != 1)
    return 0;
  arena_registered_set(a);
  return 1;
}

static int arena_registry_insert_existing(TGAlloc *alloc, GCArena *a,
					  uint32_t flags)
{
  int ok;
  HugeTab *tab = lj_arena_alloc_registry_acq(alloc);
  if (!tab)
    return 1;
  ok = lj_arena_hugetab_insert(tab, a, LJ_ARENA_SIZE,
			       arena_registry_hflags(flags));
  if (ok >= 0)
    arena_registered_set(a);
  return ok >= 0;
}

static void arena_registry_delete(TGAlloc *alloc, GCArena *a)
{
  HugeTab *tab = lj_arena_alloc_registry_acq(alloc);
  if (tab && a) {
    (void)lj_arena_hugetab_delete(tab, a, NULL);
    arena_registered_clear(a);
  }
}

static void arena_unmap_list(TGAlloc *alloc, GCArena *a)
{
  while (a) {
    GCArena *next = lj_arena_next_acq(a);
    arena_registry_delete(alloc, a);
    lj_arena_unmap(a);
    a = next;
  }
}

static int arena_register_list(TGAlloc *alloc, GCArena *a)
{
  for (; a != NULL; a = lj_arena_next_acq(a))
    if (!arena_registry_insert_existing(alloc, a, a->hdr.flags))
      return 0;
  return 1;
}

int lj_arena_alloc_register_existing(TGAlloc *alloc)
{
  uint32_t k;
  if (!alloc || !lj_arena_alloc_registry_acq(alloc))
    return 1;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    if (!arena_register_list(alloc, alloc->owned[k]) ||
	!arena_register_list(alloc, alloc->needsweep[k]) ||
	!arena_register_list(alloc, alloc->quarantine[k]) ||
	!arena_register_list(alloc, arena_reclaimed_acq(alloc, k)))
      return 0;
  }
  return 1;
}

static uint32_t arena_count_live_cells(const GCArena *a)
{
  uint32_t i = LJ_AFIRST_CELL, n = 0;
  while (i < LJ_ARENA_CELLS) {
    uint32_t st = lj_arena_state(a, i);
    uint32_t j = i + 1u;
    if (st == 0) {
      i++;
      continue;
    }
    while (j < LJ_ARENA_CELLS && lj_arena_state(a, j) == 0)
      j++;
    if (st & 2u)
      n += j - i;
    i = j;
  }
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
  arena_link_run_head(rr->alloc, rr->a, start, len);
}

static void arena_rebuild_free_run(uint32_t start, uint32_t len, void *ud)
{
  ArenaRebuildFree *rf = (ArenaRebuildFree *)ud;
  if (start >= rf->limit)
    return;
  if (len > rf->limit - start)
    len = rf->limit - start;
  arena_link_run_head(rf->alloc, rf->a, start, len);
}

static int arena_adopt_reclaimed_one(TGAlloc *alloc, uint32_t k)
{
  GCArena *a, *next, *old_owned;
  ArenaRebuildFree rf;
  TGAlloc staged;
  LJArenaFreeRun *old_bins[LJ_ALLOC_NBINS];
  uint32_t old_binmask, b;
  if (!alloc || k >= LJ_ARENA_NKINDS)
    return 0;
  a = arena_reclaimed_acq(alloc, k);
  for (;;) {
    if (!a)
      return 0;
    next = lj_arena_next_acq(a);
    if (arena_reclaimed_cas(alloc, k, &a, next))
      break;
  }
  la_store32_rel(&a->hdr.flags,
		 lj_arena_flags_acq(a) | LJ_AF_PREPSWEEP);
  if (!lj_arena_reclaim_seal(a))
    goto retry_reclaimed;
  /* Rebuild into private staging heads while CLOSED|SEALED. If a bit-only
  ** publisher dirties the gate, no reusable run has escaped into owner bins. */
  (void)arena_remote_free_drain_one(alloc, a);
  arena_late_clear_committed_free(a);
  if (!lj_arena_reclaim_clear_pending(a) ||
      !arena_reclaim_commit_sealed(a))
    goto retry_unseal;
  memset(&staged, 0, sizeof(staged));
  memcpy(old_bins, alloc->bins[k], sizeof(old_bins));
  old_binmask = alloc->binmask[k];
  old_owned = alloc->owned[k];
  rf.alloc = &staged;
  rf.a = a;
  rf.limit = LJ_ARENA_CELLS;
  lj_arena_scan_free_runs(a, arena_rebuild_free_run, &rf);
  for (b = 0; b < LJ_ALLOC_NBINS; b++) {
    LJArenaFreeRun *head = staged.bins[k][b];
    if (head) {
      LJArenaFreeRun *tail = head;
      while (tail->next)
	tail = tail->next;
      tail->next = alloc->bins[k][b];
      alloc->bins[k][b] = head;
    }
  }
  alloc->binmask[k] |= staged.binmask[k];
  la_store32_rel(&a->hdr.flags,
		 lj_arena_flags_acq(a) & ~LJ_AF_RECLAIMED);
  lj_arena_next_rel(a, old_owned);
  alloc->owned[k] = a;
  if (arena_remote_open_sealed(a)) {
    la_store32_rel(&a->hdr.flags,
		   lj_arena_flags_acq(a) & ~LJ_AF_PREPSWEEP);
    return 1;
  }
  /* A publisher won after the clean generation CAS. Roll back owner-visible
  ** staging without touching its bit intent, then retry from CLOSED later. */
  alloc->owned[k] = old_owned;
  memcpy(alloc->bins[k], old_bins, sizeof(old_bins));
  alloc->binmask[k] = old_binmask;
  la_store32_rel(&a->hdr.flags,
		 lj_arena_flags_acq(a) | LJ_AF_RECLAIMED);

retry_unseal:
  lj_arena_reclaim_unseal(a, 1);
retry_reclaimed:
  {
    GCArena *head = arena_reclaimed_acq(alloc, k);
    do {
      lj_arena_next_rel(a, head);
    } while (!arena_reclaimed_cas(alloc, k, &head, a));
  }
  return 0;
}

void lj_arena_alloc_init(TGAlloc *alloc)
{
  memset(alloc, 0, sizeof(*alloc));
}

void lj_arena_alloc_fini(TGAlloc *alloc)
{
  uint32_t k;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    arena_unmap_list(alloc, alloc->owned[k]);
    arena_unmap_list(alloc, alloc->needsweep[k]);
    arena_unmap_list(alloc, alloc->quarantine[k]);
    arena_unmap_list(alloc, arena_reclaimed_acq(alloc, k));
  }
  lj_arena_alloc_init(alloc);
}

static void arena_clear_marks_list(GCArena *a)
{
  for (; a != NULL; a = lj_arena_next_acq(a)) {
    uint32_t w;
    for (w = 0; w < LJ_ARENA_WORDS; w++) {
      uint64_t block = la_load64_acq(&a->block[w]);
      (void)la_and64_rlx(&a->mark[w], ~block);
    }
  }
}

void lj_arena_alloc_clear_marks(TGAlloc *alloc)
{
  uint32_t k;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    arena_clear_marks_list(alloc->owned[k]);
    arena_clear_marks_list(alloc->needsweep[k]);
    /* A quarantine belongs to the still-open sweep cycle. Its LIVE state and
    ** mark bit are a late-publication rescue proof and must survive until that
    ** arena either finishes or the cycle is explicitly restored. */
  }
}

void lj_arena_alloc_rebuild_free_kind(TGAlloc *alloc, uint32_t k)
{
  if (k < LJ_ARENA_NKINDS) {
    GCArena *a;
    arena_clear_bins(alloc, k);
    for (a = alloc->owned[k]; a != NULL; a = lj_arena_next_acq(a)) {
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

int lj_arena_alloc_prepare_sweep_kind(TGAlloc *alloc, uint32_t k)
{
  GCArena *a, *reclaimed, *work, *prepared = NULL;
  int complete = 1;
  if (k >= LJ_ARENA_NKINDS)
    return 0;
  /* A completed sweep leaves arenas on the CLOSED reclaimed stack until the
  ** owner needs allocation space. A later collection must not depend on such
  ** an allocation. First detach every owner allocation cursor and bin at this
  ** owner safepoint. A gate collision can then leave an arena in PREPSWEEP
  ** without returning it to allocator-visible state; the mutator may allocate
  ** a fresh arena while the last publisher wakes a later RESET_ALLOC retry.
  */
  if (alloc->bump[k].a) {
    if (alloc->bump[k].cell < alloc->bump[k].end) {
      la_store32_rel(&alloc->bump[k].a->hdr.prep_bump_cell,
		     alloc->bump[k].cell);
      la_store32_rel(&alloc->bump[k].a->hdr.prep_bump_end,
		     alloc->bump[k].end);
    } else {
      la_store32_rel(&alloc->bump[k].a->hdr.prep_bump_cell, 0);
      la_store32_rel(&alloc->bump[k].a->hdr.prep_bump_end, 0);
    }
  }
  a = alloc->owned[k];
  alloc->owned[k] = NULL;
  alloc->bump[k].a = NULL;
  alloc->bump[k].cell = 0;
  alloc->bump[k].end = 0;
  arena_clear_bins(alloc, k);
  reclaimed = (GCArena *)la_xchgptr_acqrel(
    (void **)&alloc->reclaimed[k], NULL);
  work = alloc->needsweep[k];  /* PREPSWEEP entries from an earlier retry. */
  alloc->needsweep[k] = NULL;

  /* Publish every newly detached source as PREPSWEEP before it becomes part of
  ** the retry list. Remote physical frees then use nonintrusive late pins even
  ** if their OPEN admission raced this owner-side detachment. */
  while (a || reclaimed) {
    GCArena *next;
    if (!a) {
      a = reclaimed;
      reclaimed = NULL;
    }
    next = lj_arena_next_acq(a);
    if (next == a ||
	(next && (lj_arena_flags_acq(next) & LJ_AF_NEEDSWEEP)))
      next = NULL;
    la_store32_rel(&a->hdr.flags,
		   (lj_arena_flags_acq(a) &
		    ~(LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) |
		   LJ_AF_PREPSWEEP);
    lj_arena_next_rel(a, work);
    work = a;
    a = next;
  }

  while (work) {
    GCArena *next = lj_arena_next_acq(work);
    uint32_t flags = lj_arena_flags_acq(work);
    if (next == work)
      next = NULL;
    /* A prior retry may already have completed this arena. */
    if ((flags & (LJ_AF_NEEDSWEEP|LJ_AF_PREPSWEEP)) ==
	LJ_AF_NEEDSWEEP)
      goto prepared_one;
    la_store32_rel(&work->hdr.flags,
		   (flags &
		    ~(LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) |
		   LJ_AF_PREPSWEEP);
    if (!lj_arena_reclaim_seal(work)) {
      complete = 0;
      goto prepared_one;
    }
    /* SEALED still admits counted intent producers. PREPSWEEP is visible
    ** before the exact zero-count generation LP, so a post-LP reader returns
    ** DEAD without touching header bytes while sweep/late state is rebuilt. */
    if (!lj_arena_reclaim_clear_pending(work) ||
	!arena_reclaim_commit_sealed(work)) {
      lj_arena_reclaim_unseal(work, 1);
      complete = 0;
      goto prepared_one;
    }
    arena_prepare_bump_tail(work);
    arena_sweep_state_prepare(work);
    /* Ordinary OPEN records are stable under SEALED. Grace-late frees are
    ** bit-only and become FREEING only in this later generation, before its
    ** grace. A publication racing this exchange remains pinned for one extra
    ** generation and sets PENDING before the bit. */
    (void)lj_arena_remote_free_drain_sweep(alloc, work);
    (void)arena_late_prepare_consume(work);
    lj_arena_reclaim_unseal(work, 1);
    la_store32_rel(&work->hdr.flags,
		   (lj_arena_flags_acq(work) &
		    ~(LJ_AF_PREPSWEEP|LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) |
		   LJ_AF_NEEDSWEEP);

prepared_one:
    lj_arena_next_rel(work, prepared);
    prepared = work;
    work = next;
  }
  alloc->needsweep[k] = prepared;
  return complete;
}

GCArena *lj_arena_alloc_quarantine_one(TGAlloc *alloc, uint32_t kind,
					       uint64_t retire_epoch)
{
  GCArena *a, *next;
  if (!alloc || kind >= LJ_ARENA_NKINDS)
    return NULL;
  a = alloc->needsweep[kind];
  if (!a)
    return NULL;
  next = lj_arena_next_acq(a);
  if (next == a || (next && !(lj_arena_flags_acq(next) & LJ_AF_NEEDSWEEP)))
    next = NULL;
  alloc->needsweep[kind] = next;
  a->hdr.retire_epoch = retire_epoch;
  a->hdr.reclaim_cell = LJ_AFIRST_CELL;
  la_store32_rel(&a->hdr.flags,
		 (lj_arena_flags_acq(a) &
		  ~(LJ_AF_NEEDSWEEP|LJ_AF_RECLAIMED|LJ_AF_PREPSWEEP)) |
		 LJ_AF_QUARANTINE);
  lj_arena_next_rel(a, alloc->quarantine[kind]);
  alloc->quarantine[kind] = a;
  return a;
}

GCArena *lj_arena_alloc_quarantine_head(const TGAlloc *alloc, uint32_t kind)
{
  return alloc && kind < LJ_ARENA_NKINDS ? alloc->quarantine[kind] : NULL;
}

GCArena *lj_arena_alloc_reclaimed_head(const TGAlloc *alloc, uint32_t kind)
{
  return alloc && kind < LJ_ARENA_NKINDS ?
    arena_reclaimed_acq(alloc, kind) : NULL;
}

static int arena_quarantine_bitmap_ready(GCArena *a, uint32_t *retry_cell)
{
  uint32_t w;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t b = la_load64_acq(&a->block[w]);
    uint64_t late = la_load64_acq(&a->late[w]);
    uint64_t marks = la_load64_acq(&a->mark[w]);
    uint32_t j;
    for (j = 0; j < 64u; j++) {
      uint32_t state;
      if (!(b & ((uint64_t)1 << j)))
	continue;
      if (late & ((uint64_t)1 << j))
	continue;
      state = lj_arena_sweep_state_acq(a, (w << 6) + j);
      if (state == LJ_ARENA_SWEEP_LIVE ||
	  state == LJ_ARENA_SWEEP_RETIRED) {
	if (retry_cell)
	  *retry_cell = (w << 6) + j;
	return 0;  /* First actionable detached root or pending destructor. */
      }
      if (state == LJ_ARENA_SWEEP_WHITE &&
	  !(marks & ((uint64_t)1 << j))) {
	/* The ownership-spine pass has completed, so a remaining WHITE start is
	** raw/opaque storage. Resolve it here while SEALED instead of returning
	** an owner-only reason with no publisher wake. */
	(void)la_bit_test_and_set64(&a->mark[w], j);
      }
    }
  }
  return 1;
}

static void arena_quarantine_apply_bitmap(GCArena *a, int preserve_marks)
{
  uint32_t w;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t b = la_load64_acq(&a->block[w]);
    uint64_t m = la_load64_acq(&a->mark[w]);
    uint64_t late = la_load64_acq(&a->late[w]);
    uint64_t live = 0, freeing = 0;
    uint32_t j;
    for (j = 0; j < 64u; j++) {
      uint32_t cell = (w << 6) + j;
      uint32_t state;
      if (!(b & ((uint64_t)1 << j)))
	continue;
      if (late & ((uint64_t)1 << j)) {
	live |= (uint64_t)1 << j;
	continue;
      }
      state = lj_arena_sweep_state_acq(a, cell);
      if (state == LJ_ARENA_SWEEP_WHITE)
	live |= (uint64_t)1 << j;
      else
	freeing |= (uint64_t)1 << j;
    }
    la_store64_rel(&a->block[w], live);
    la_store64_rel(&a->mark[w], ((~b) & m) | freeing |
		   (preserve_marks ? live : (uint64_t)0));
  }
}

int lj_arena_alloc_quarantine_finish(TGAlloc *alloc, uint32_t kind,
				      GCArena *a, uint32_t sweep_epoch,
				      int preserve_marks, uint32_t *reasonp)
{
  GCArena *head, *next, *reclaimed;
  uint32_t retry_cell = LJ_ARENA_CELLS;
  uint32_t reason = LJ_ARENA_FINISH_NONE;
  if (reasonp)
    *reasonp = LJ_ARENA_FINISH_NONE;
  if (!alloc || kind >= LJ_ARENA_NKINDS || !a)
    return 0;
  head = alloc->quarantine[kind];
  if (head != a)
    return 0;
  la_store32_rel(&a->hdr.flags,
		 (lj_arena_flags_acq(a) & ~LJ_AF_RECLAIMED) |
		 LJ_AF_QUARANTINE|LJ_AF_PREPSWEEP);
  if ((lj_arena_remote_active_acq(a) &
       (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED)) !=
      (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED)) {
    reason = LJ_ARENA_FINISH_PUBLISHER;
    goto blocked;
  }
  (void)arena_quarantine_settle_late(a);
  if (la_loadptr_acq((void *const *)&a->hdr.remote_free) != NULL) {
    reason = LJ_ARENA_FINISH_PUBLISHER;
    goto blocked;
  }
  if (la_load64_acq(&a->hdr.retire_epoch) == ~(uint64_t)0) {
    reason = LJ_ARENA_FINISH_EPOCH;
    goto blocked;
  }
  if (lj_arena_reclaim_deferred_acq(a) != 0) {
    reason = LJ_ARENA_FINISH_ACTIONABLE;
    goto blocked;
  }
  if (!arena_quarantine_bitmap_ready(a, &retry_cell)) {
    reason = retry_cell < LJ_ARENA_CELLS ?
      LJ_ARENA_FINISH_ACTIONABLE : LJ_ARENA_FINISH_UNCLASSIFIED;
    goto blocked;
  }
  /* Clear only an intent whose producer has left, then switch generations by
  ** exact clean CAS. Any rescue admitted after validation dirties the word and
  ** defeats this LP. After it, rescue loses without touching mark/state. */
  if (!lj_arena_reclaim_clear_pending(a) ||
      !arena_reclaim_commit_sealed(a)) {
    reason = LJ_ARENA_FINISH_PUBLISHER;
    goto blocked;
  }
  arena_quarantine_apply_bitmap(a, preserve_marks);
  /* Publish every terminal block decision before WHITE is reused as the
  ** post-commit sidecar value. Committed readers sample state then block, so
  ** a dead cell can never combine reset-WHITE with its old block bit. */
  arena_sweep_state_reset(a);
  next = lj_arena_next_acq(a);
  if (next == a || (next && !(lj_arena_flags_acq(next) & LJ_AF_QUARANTINE)))
    next = NULL;
  alloc->quarantine[kind] = next;
  lj_arena_next_rel(a, NULL);
  a->hdr.live_cells = arena_count_live_cells(a);
  a->hdr.sweep_epoch = sweep_epoch;
  a->hdr.retire_epoch = 0;
  a->hdr.reclaim_cell = LJ_AFIRST_CELL;
  a->hdr.reclaim_deferred = 0;
  la_store32_rel(&a->hdr.flags,
		 (lj_arena_flags_acq(a) &
		  ~(LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE|LJ_AF_PREPSWEEP)) |
		 LJ_AF_RECLAIMED);
  reclaimed = arena_reclaimed_acq(alloc, kind);
  do {
    lj_arena_next_rel(a, reclaimed);
  } while (!arena_reclaimed_cas(alloc, kind, &reclaimed, a));
  lj_arena_reclaim_unseal(a, 1);  /* Queue-only until stable adoption. */
  if (reasonp)
    *reasonp = LJ_ARENA_FINISH_COMMITTED;
  return 1;

blocked:
  if (retry_cell < a->hdr.reclaim_cell)
    a->hdr.reclaim_cell = retry_cell;
  la_store32_rel(&a->hdr.flags,
		 lj_arena_flags_acq(a) & ~LJ_AF_PREPSWEEP);
  if (reasonp)
    *reasonp = reason;
  return 0;
}

void lj_arena_alloc_prepare_sweep(TGAlloc *alloc)
{
  uint32_t k;
  for (k = 0; k < LJ_ARENA_NKINDS; k++)
    lj_arena_alloc_prepare_sweep_kind(alloc, k);
}

int lj_arena_alloc_restore_sweep_kind(TGAlloc *alloc, uint32_t k)
{
  GCArena *a;
  if (k >= LJ_ARENA_NKINDS)
    return 0;
  while ((a = alloc->needsweep[k]) != NULL) {
    GCArena *next = lj_arena_next_acq(a);
    GCArena *old_owned;
    TGAlloc staged;
    LJArenaFreeRun *old_bins[LJ_ALLOC_NBINS];
    uint32_t old_binmask, old_flags, b;
    ArenaRebuildFree rf;
    uint32_t i;
    if (next == a ||
	(next && !(lj_arena_flags_acq(next) &
		  (LJ_AF_NEEDSWEEP|LJ_AF_PREPSWEEP))))
      next = NULL;
    la_store32_rel(&a->hdr.flags,
		   lj_arena_flags_acq(a) | LJ_AF_PREPSWEEP);
    if (!lj_arena_reclaim_seal(a))
      return 0;
    (void)lj_arena_remote_free_drain_sweep(alloc, a);
    /* The legal abort path precedes root detachment, so PREPSWEEP may have
    ** produced only WHITE and destructor-complete FREEING starts. Refuse an
    ** unexpected actionable generation before taking the irreversible LP. */
    for (i = LJ_AFIRST_CELL; i < LJ_ARENA_CELLS; i++) {
      uint32_t state;
      if (!lj_arena_bm_get(a->block, i))
	continue;
      state = lj_arena_sweep_state_acq(a, i);
      if (state == LJ_ARENA_SWEEP_LIVE ||
	  state == LJ_ARENA_SWEEP_RETIRED) {
	lj_arena_reclaim_unseal(a, 1);
	return 0;
      }
    }
    /* Exact C|S->S precedes every bitmap/sidecar mutation. A producer admitted
    ** before this point dirties PENDING and defeats the CAS; a later producer
    ** is counted in the committed generation and defeats exact OPEN. */
    if (!lj_arena_reclaim_clear_pending(a) ||
	!arena_reclaim_commit_sealed(a)) {
      lj_arena_reclaim_unseal(a, 1);
      return 0;
    }
    arena_prepare_bump_tail(a);
    /* PREPSWEEP already consumed late[] before changing those starts to
    ** FREEING, so absence of a late bit is not provenance. Re-pin every
    ** FREEING allocation and retain its block bit through a fresh full grace.
    ** This conservatively delays ordinary remote frees by one cycle too. */
    for (i = LJ_AFIRST_CELL; i < LJ_ARENA_CELLS; i++) {
      if (lj_arena_bm_get(a->block, i) &&
	  lj_arena_sweep_state_acq(a, i) == LJ_ARENA_SWEEP_FREEING) {
	(void)la_bit_test_and_set64(&a->late[i >> 6], i & 63);
	(void)lj_arena_sweep_state_cas(a, i, LJ_ARENA_SWEEP_FREEING,
					 LJ_ARENA_SWEEP_WHITE);
      }
    }
    arena_sweep_state_reset(a);
    a->hdr.reclaim_cell = LJ_AFIRST_CELL;
    a->hdr.reclaim_deferred = 0;
    arena_late_clear_committed_free(a);
    memset(&staged, 0, sizeof(staged));
    rf.alloc = &staged;
    rf.a = a;
    rf.limit = LJ_ARENA_CELLS;
    lj_arena_scan_free_runs(a, arena_rebuild_free_run, &rf);
    memcpy(old_bins, alloc->bins[k], sizeof(old_bins));
    old_binmask = alloc->binmask[k];
    old_owned = alloc->owned[k];
    old_flags = lj_arena_flags_acq(a);
    for (b = 0; b < LJ_ALLOC_NBINS; b++) {
      LJArenaFreeRun *head = staged.bins[k][b];
      if (head) {
	LJArenaFreeRun *tail = head;
	while (tail->next)
	  tail = tail->next;
	tail->next = alloc->bins[k][b];
	alloc->bins[k][b] = head;
      }
    }
    alloc->binmask[k] |= staged.binmask[k];
    la_store32_rel(&a->hdr.flags,
		   lj_arena_flags_acq(a) & ~LJ_AF_NEEDSWEEP);
    lj_arena_next_rel(a, old_owned);
    alloc->owned[k] = a;
    if (!arena_remote_open_sealed(a)) {
      alloc->owned[k] = old_owned;
      memcpy(alloc->bins[k], old_bins, sizeof(old_bins));
      alloc->binmask[k] = old_binmask;
      la_store32_rel(&a->hdr.flags, old_flags);
      lj_arena_next_rel(a, next);
      lj_arena_reclaim_unseal(a, 1);
      return 0;
    }
    la_store32_rel(&a->hdr.flags,
		   lj_arena_flags_acq(a) & ~LJ_AF_PREPSWEEP);
    alloc->needsweep[k] = next;
  }
  return 1;
}

void lj_arena_alloc_sweep_kind(TGAlloc *alloc, uint32_t kind,
				       uint32_t epoch, int preserve_marks)
{
  while (lj_arena_sweep_one(alloc, kind, epoch, preserve_marks) != NULL)
    ;
}

static void arena_unlink_owned_duplicate(TGAlloc *alloc, uint32_t kind,
					 GCArena *target)
{
  GCArena *prev = NULL, *a;
  if (!alloc || kind >= LJ_ARENA_NKINDS || !target)
    return;
  for (a = alloc->owned[kind]; a != NULL;) {
    GCArena *next = lj_arena_next_acq(a);
    if (a == target) {
      if (next == a || (next && (next->hdr.flags & LJ_AF_NEEDSWEEP)))
	next = NULL;
      if (prev)
	lj_arena_next_rel(prev, next);
      else
	alloc->owned[kind] = next;
      return;
    }
    if (next == a)
      return;
    prev = a;
    a = next;
  }
}

GCArena *lj_arena_sweep_one(TGAlloc *alloc, uint32_t kind, uint32_t epoch,
			    int preserve_marks)
{
  GCArena *a;
  ArenaLargestRun lr = { 0, 0 };
  ArenaRebuildRuns rr;
  if (kind >= LJ_ARENA_NKINDS)
    return NULL;
  a = alloc->needsweep[kind];
  if (!a)
    return NULL;
  arena_unlink_owned_duplicate(alloc, kind, a);
  {
    GCArena *next = lj_arena_next_acq(a);
    if (next == a || (next && !(next->hdr.flags & LJ_AF_NEEDSWEEP)))
      next = NULL;
    alloc->needsweep[kind] = next;
  }
  lj_arena_next_rel(a, NULL);
  lj_arena_sweep_words(a, preserve_marks);
  lj_arena_scan_free_runs(a, arena_find_largest_run, &lr);
  if (lr.len >= LJ_BUMP_MIN)
    arena_set_free_run(a, lr.start, lr.len);
  rr.alloc = alloc;
  rr.a = a;
  rr.bump = lr;
  lj_arena_scan_free_runs(a, arena_rebuild_run, &rr);
  if (lr.len >= LJ_BUMP_MIN) {
    arena_publish_bump_run(alloc, kind);
    alloc->bump[kind].a = a;
    alloc->bump[kind].cell = lr.start;
    alloc->bump[kind].end = lr.start + lr.len;
  }
  a->hdr.live_cells = arena_count_live_cells(a);
  a->hdr.sweep_epoch = epoch;
  a->hdr.flags &= ~LJ_AF_NEEDSWEEP;
  lj_arena_next_rel(a, alloc->owned[kind]);
  alloc->owned[kind] = a;
  return a;
}

static uint32_t arena_transfer_list(GCArena **dstp, GCArena *a,
				    uint32_t owner_tid, global_State *progress_g)
{
  uint32_t n = 0;
  while (a) {
    GCArena *next = lj_arena_next_acq(a);
    lj_arena_owner_rel(a, owner_tid);
    lj_arena_progress_g_rel(a, progress_g);
    lj_arena_next_rel(a, *dstp);
    *dstp = a;
    a = next;
    n++;
  }
  return n;
}

uint32_t lj_arena_alloc_transfer(TGAlloc *dst, TGAlloc *src)
{
  uint32_t k, n = 0;
  uint32_t owner_tid;
  TGState *owner_tg;
  global_State *progress_g;
  if (!dst || !src || dst == src)
    return 0;
  /* The source owner is dead/quiescent. Its arena-local Treiber queues remain
  ** routable across owner_tid changes, but draining now avoids carrying dead
  ** payload records through list rebuild. */
  (void)lj_arena_remote_free_drain(src);
  owner_tid = lj_arena_alloc_owner_acq(dst);
  owner_tg = (TGState *)lj_arena_alloc_owner_tg_acq(dst);
  progress_g = owner_tg ? owner_tg->gl : NULL;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    LJArenaBump *b = &src->bump[k];
    if (b->a && b->cell < b->end)
      arena_set_free_run(b->a, b->cell, b->end - b->cell);
    src->bump[k].a = NULL;
    src->bump[k].cell = 0;
    src->bump[k].end = 0;
    arena_clear_bins(src, k);
    n += arena_transfer_list(&dst->owned[k], src->owned[k], owner_tid,
			     progress_g);
    src->owned[k] = NULL;
    n += arena_transfer_list(&dst->needsweep[k], src->needsweep[k],
			     owner_tid, progress_g);
    src->needsweep[k] = NULL;
    n += arena_transfer_list(&dst->quarantine[k], src->quarantine[k],
			     owner_tid, progress_g);
    src->quarantine[k] = NULL;
    {
      GCArena *a = (GCArena *)la_xchgptr_acqrel(
	(void **)&src->reclaimed[k], NULL);
      while (a) {
	GCArena *next = lj_arena_next_acq(a);
	GCArena *head = arena_reclaimed_acq(dst, k);
	lj_arena_owner_rel(a, owner_tid);
	lj_arena_progress_g_rel(a, progress_g);
	do {
	  lj_arena_next_rel(a, head);
	} while (!arena_reclaimed_cas(dst, k, &head, a));
	a = next;
	n++;
      }
    }
    lj_arena_alloc_rebuild_free_kind(dst, k);
  }
  lj_arena_alloc_set_registry(src, NULL);
  lj_arena_alloc_owner_rel(src, 0);
  lj_arena_alloc_owner_tg_rel(src, NULL);
  lj_arena_alloc_black_rel(src, 0);
  return n;
}

static GCArena *arena_alloc_fresh(TGAlloc *alloc, PRNGState *rs,
				  uint32_t flags)
{
  uint32_t k = arena_kind(flags);
  GCArena *a = lj_arena_map(rs, flags);
  if (!a)
    return NULL;
  lj_arena_owner_rel(a, lj_arena_alloc_owner_acq(alloc));
  {
    TGState *owner_tg = (TGState *)lj_arena_alloc_owner_tg_acq(alloc);
    lj_arena_progress_g_rel(a, owner_tg ? owner_tg->gl : NULL);
  }
  if (!arena_registry_insert_fresh(alloc, a, flags)) {
    lj_arena_unmap(a);
    return NULL;
  }
  lj_arena_next_rel(a, alloc->owned[k]);
  alloc->owned[k] = a;
  alloc->bump[k].a = a;
  alloc->bump[k].cell = LJ_AFIRST_CELL;
  alloc->bump[k].end = LJ_ARENA_CELLS;
  return a;
}

int lj_arena_reserve_bump(TGAlloc *alloc, PRNGState *rs, uint32_t flags,
			  uint32_t ncells, GCArena **ap, uint32_t *cellp)
{
  uint32_t k = arena_kind(flags);
  LJArenaBump *b;
  uint32_t cell;
  if (!alloc || !ap || !cellp || ncells == 0 ||
      ncells > LJ_ARENA_CELLS - LJ_AFIRST_CELL)
    return 0;
  b = &alloc->bump[k];
  if (!b->a || b->cell + ncells > b->end) {
    uint32_t bin = 0;
    LJArenaFreeRun **pp;
    arena_publish_bump_run(alloc, k);
    pp = arena_find_run(alloc, k, ncells, &bin);
    if ((!pp || !*pp) && arena_adopt_reclaimed_one(alloc, k))
      pp = arena_find_run(alloc, k, ncells, &bin);
    if (!pp || !*pp) {
      (void)lj_arena_remote_free_drain(alloc);
      pp = arena_find_run(alloc, k, ncells, &bin);
    }
    if (pp && *pp) {
      LJArenaFreeRun *run = *pp;
      GCArena *a = lj_arena_of(run);
      uint32_t start = run->start;
      uint32_t len = run->len;
      uint32_t i;
      *pp = run->next;
      arena_refresh_binmask(alloc, k, bin);
      /* Rebuild may have coalesced several adjacent state-1 boundaries into
      ** this private bump window. Erase the complete old boundary map now;
      ** specialized C/VM/JIT bump publishers subsequently install only their
      ** real object starts. */
      for (i = 0; i < len; i++)
	arena_set_extent(a, start + i);
      b->a = NULL;
      b->cell = 0;
      b->end = 0;
      /*
      ** Specialized bump callers do not need generic free-run reuse order.
      ** Reserve the first cells for the caller and keep the remaining run as
      ** the unpublished bump window, matching sweep's largest-run protocol.
      */
      if (len > ncells) {
	b->a = a;
	b->cell = start + ncells;
	b->end = start + len;
      }
      *ap = a;
      *cellp = start;
      return 1;
    }
    if (!arena_alloc_fresh(alloc, rs, flags))
      return 0;
  }
  cell = b->cell;
  b->cell = cell + ncells;
  *ap = b->a;
  *cellp = cell;
  return 1;
}

void *lj_arena_alloc(TGAlloc *alloc, PRNGState *rs, size_t size,
		     uint32_t flags)
{
  uint32_t k = arena_kind(flags);
  LJArenaBump *b = &alloc->bump[k];
  uint32_t ncells, cell;
  if (size == 0)
    return NULL;
  if (size > LJ_HUGE_THRESHOLD) {
    void *p = lj_arena_huge_map(rs, size, flags);
    if (p)
      lj_arena_owner_rel(lj_arena_of(p), lj_arena_alloc_owner_acq(alloc));
    return p;
  }
  ncells = lj_arena_ncells(size);
  if (ncells > LJ_ARENA_CELLS - LJ_AFIRST_CELL)
    return NULL;
  {
    uint32_t bin = 0;
    LJArenaFreeRun **pp = arena_find_run(alloc, k, ncells, &bin);
    if ((!pp || !*pp) && arena_adopt_reclaimed_one(alloc, k))
      pp = arena_find_run(alloc, k, ncells, &bin);
    if (!pp || !*pp) {
      (void)lj_arena_remote_free_drain(alloc);
      pp = arena_find_run(alloc, k, ncells, &bin);
    }
    if (pp && *pp) {
      LJArenaFreeRun *run = *pp;
      GCArena *a = lj_arena_of(run);
      uint32_t start = run->start;
      uint32_t len = run->len;
      *pp = run->next;
      arena_refresh_binmask(alloc, k, bin);
      if (len > ncells)
	arena_insert_run(alloc, a, start + ncells, len - ncells);
      arena_set_alloc(a, start, ncells,
		      lj_arena_alloc_black_acq(alloc));
      return lj_arena_cellptr(a, start);
    }
  }
  if (!b->a || b->cell + ncells > b->end) {
    arena_publish_bump_run(alloc, k);
    if (!arena_alloc_fresh(alloc, rs, flags))
      return NULL;
  }
  cell = b->cell;
  b->cell += ncells;
  arena_set_alloc(b->a, cell, ncells, lj_arena_alloc_black_acq(alloc));
  return lj_arena_cellptr(b->a, cell);
}

void lj_arena_free(TGAlloc *alloc, void *p, size_t size)
{
  GCArena *a;
  uint32_t start, ncells;
  if (!p || size == 0)
    return;
  /* lua_Alloc's old size is the lifetime-safe class discriminator. Never read
  ** an arena header merely to decide whether a possibly stale huge address is
  ** mapped. Direct huge allocations have no side table and retain the normal
  ** single-owner free contract; arena_allocf uses terminal table ownership. */
  if (size > LJ_HUGE_THRESHOLD) {
    lj_arena_huge_unmap(p, size);
    return;
  }
  a = lj_arena_of(p);
  if (lj_arena_alloc_free_noinsert_acq(alloc))
    return;
  start = lj_arena_cellof(p);
  ncells = lj_arena_ncells(size);
  if (start < LJ_AFIRST_CELL || start + ncells > LJ_ARENA_CELLS)
    return;
  arena_insert_run(alloc, a, start, ncells);
}

int lj_arena_free_deferred(TGAlloc *alloc, void *p, size_t size)
{
  GCArena *a;
  uint32_t start, ncells, flags;
  int published;
  if (!alloc)
    return 0;
  if (!p || size == 0)
    return 0;
  if (size > LJ_HUGE_THRESHOLD)
    return 0;
retry_open:
  a = lj_arena_of(p);
  if (lj_arena_quarantine_owns_body(p, size))
    return 1;
  start = lj_arena_cellof(p);
  ncells = lj_arena_ncells(size);
  if (start < LJ_AFIRST_CELL || start + ncells > LJ_ARENA_CELLS)
    return 0;
  if (!arena_remote_enter(a)) {
    int late = arena_remote_late_publish(a, p, size);
    if (late == 0)
      goto retry_open;
    return 1;  /* Published, duplicate, or conservatively retained. */
  }
  flags = lj_arena_flags_acq(a);
  if (flags & (LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE|
	       LJ_AF_RECLAIMED)) {
    arena_remote_leave(a);
    return lj_arena_quarantine_owns_body(p, size);
  }
  if (!lj_arena_bm_get(a->block, start)) {
    arena_remote_leave(a);
    return 1;  /* Exact body was already committed free. */
  }
  /* A completed GC destructor must not make its body reusable before a later
  ** sweep/grace. Retain the allocation bit, clear its liveness mark, and pin
  ** the exact start without overwriting the still-SMR-visible header. The next
  ** PREPSWEEP converts the bit to FREEING before arming a fresh grace. */
  (void)la_and64_rlx(&a->mark[start >> 6],
		     ~((uint64_t)1 << (start & 63)));
  published = arena_late_pin(a, p, size);
  arena_remote_leave(a);
  return published != 0;
}

void *lj_arena_realloc(TGAlloc *alloc, PRNGState *rs, void *p,
		       size_t osize, size_t nsize, uint32_t flags)
{
  void *np;
  int oldhuge;
  if (!p)
    return lj_arena_alloc(alloc, rs, nsize, flags);
  if (nsize == 0) {
    lj_arena_free(alloc, p, osize);
    return NULL;
  }
  /* osize, not an allocation-header probe, is valid after a competing huge
  ** table owner has unmapped p. The direct API still assumes the caller owns
  ** p while copying; arena_allocf adds a BUSY pin for shared huge mappings. */
  oldhuge = osize > LJ_HUGE_THRESHOLD;
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
  if (lj_arena_alloc_black_acq(ad->alloc))
    hflags |= LJ_HUGEF_MARK;
  return hflags;
}

static void *arena_allocf_new(LJArenaAllocD *ad, size_t size, uint32_t flags)
{
  void *p;
  if (!ad->huge || size <= LJ_HUGE_THRESHOLD)
    return lj_arena_alloc(ad->alloc, ad->prng, size, flags);
  p = lj_arena_huge_map(ad->prng, size, flags);
  if (p)
    lj_arena_owner_rel(lj_arena_of(p),
		       lj_arena_alloc_owner_acq(ad->alloc));
  if (p && lj_arena_hugetab_insert(ad->huge, p, size,
				   arena_allocf_hflags(ad, flags)) != 1) {
    lj_arena_huge_unmap(p, size);
    return NULL;
  }
  return p;
}

static void arena_allocf_free(LJArenaAllocD *ad, void *ptr, size_t osize)
{
  if (!ptr || osize == 0)
    return;
  if (osize > LJ_HUGE_THRESHOLD) {
    /* The table claim is both duplicate suppression and the linearization
    ** point against prepare_sweep(). A missing entry is stale/already owned;
    ** in either case no mapping header or payload may be touched. */
    if (ad->huge) {
      int finish;
      LJHugeInfo hi;
      if (!lj_arena_hugetab_claim_external_free(ad->huge, ptr, &hi))
	return;
      finish = lj_arena_hugetab_finish_external_free(ad->huge, ptr, &hi);
      lj_assertX(finish != LJ_ARENA_HUGE_FINISH_LOST,
		 "huge external-free ownership lost");
      if (finish == LJ_ARENA_HUGE_FINISH_UNMAP)
	lj_arena_huge_unmap(ptr, hi.size);
      return;
    }
    lj_arena_free(ad->alloc, ptr, osize);
    return;
  }
  {
    void *owner_tg = lj_arena_alloc_owner_tg_acq(ad->alloc);
    if (lj_arena_quarantine_owns_body(ptr, osize))
      return;  /* Quarantine exclusively converts the body bitmap to free. */
    if (owner_tg != NULL && (void *)lj_thr_get_tg() != owner_tg) {
      int published = lj_arena_remote_free_publish(ad->alloc, ptr, osize);
      /* A valid owner-routed arena body always has one of two destinations:
      ** the per-arena queue or sweep ownership. Never fall through to another
      ** TG's owner-local bins. In release builds an impossible validation
      ** failure conservatively retains the body instead of corrupting them. */
      lj_assertX(published, "arena remote-free publication failed");
      UNUSED(published);
      return;
    }
  }
  lj_arena_free(ad->alloc, ptr, osize);
}

static void *arena_allocf_realloc_huge(LJArenaAllocD *ad, void *ptr,
					 size_t nsize)
{
  LJHugeInfo hi;
  size_t csize;
  void *np;
  int finish;
  /* Claim before allocation: this both rejects a stale table address before
  ** the OS can reuse it for np and pins the old payload throughout allocation
  ** and copy. A failed replacement releases the nonterminal pin unchanged. */
  if (!hugetab_claim_realloc(ad->huge, ptr, &hi))
    return NULL;
  if (nsize > LJ_HUGE_THRESHOLD &&
      lj_arena_huge_mapsize(hi.size) == lj_arena_huge_mapsize(nsize)) {
    /* Same mapping extent: update authoritative logical size and retain the
    ** stock O(1) realloc fast path without a free/sweep observation window. */
    if (hugetab_finish_realloc_keep(ad->huge, ptr, nsize, &hi))
      return ptr;
    (void)hugetab_release_realloc(ad->huge, ptr, NULL);
    return NULL;
  }
  np = arena_allocf_new(ad, nsize, ad->flags);
  if (!np) {
    int released = hugetab_release_realloc(ad->huge, ptr, &hi);
    lj_assertX(released, "huge realloc pin lost on allocation failure");
    UNUSED(released);
    return NULL;
  }
  csize = hi.size < nsize ? hi.size : nsize;
  memcpy(np, ptr, csize);
  if (!hugetab_realloc_to_external_free(ad->huge, ptr, &hi)) {
    (void)hugetab_release_realloc(ad->huge, ptr, NULL);
    arena_allocf_free(ad, np, nsize);
    return NULL;
  }
  finish = lj_arena_hugetab_finish_external_free(ad->huge, ptr, &hi);
  lj_assertX(finish != LJ_ARENA_HUGE_FINISH_LOST,
	     "huge realloc ownership lost");
  if (finish == LJ_ARENA_HUGE_FINISH_UNMAP) {
    lj_arena_huge_unmap(ptr, hi.size);
  } else if (finish == LJ_ARENA_HUGE_FINISH_LOST) {
    arena_allocf_free(ad, np, nsize);
    return NULL;
  }
  return np;
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
  int oldhuge;
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
  oldhuge = osize > LJ_HUGE_THRESHOLD;
  if (ad->huge && oldhuge)
    return arena_allocf_realloc_huge(ad, ptr, nsize);
  if (ad->huge && nsize > LJ_HUGE_THRESHOLD) {
    size_t csize;
    void *np;
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
