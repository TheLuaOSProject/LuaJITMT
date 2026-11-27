/* Real free capacity must not depend on a fixed bin-walk cutoff. Ordinary
** cases use only allocations, frees and owner drains: no rebuild, transfer,
** cursor edits or synthetic links. Corruption has its own named case. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"

#define CAPACITY_RUNS 9000u
#define SEARCH_PREFIX 8192u
#define BLOCK_BYTES 1024u
#define GUARD_BYTES 16u
#define FIT_BYTES (2u * BLOCK_BYTES)
#define CENSUS_LIMIT (2u * CAPACITY_RUNS + 1024u)

enum FreeSource { FREE_LOCAL, FREE_REMOTE, FREE_LATE };
typedef struct Workload {
  TGAlloc alloc;
  PRNGState prng;
  uint32_t flags, kind, count;
  unsigned char **blocks, **guards;
  uintptr_t *sorted;
} Workload;
typedef struct Census {
  uint32_t nodes, targets, fit_position;
  uint64_t cells;
} Census;
typedef struct Publish { Workload *work; int duplicate; } Publish;

static int compare_address(const void *ap, const void *bp)
{
  uintptr_t a = *(const uintptr_t *)ap, b = *(const uintptr_t *)bp;
  return a < b ? -1 : a != b;
}
static unsigned char pattern(uint32_t i)
{
  return (unsigned char)(1u + i % 251u);
}
static void check_bytes(const unsigned char *p, size_t len, unsigned char value)
{
  size_t i;
  for (i = 0; i < len; i++) assert(p[i] == value);
}
static void check_guards(const Workload *w)
{
  uint32_t i;
  for (i = 0; i < w->count; i++)
    check_bytes(w->guards[i], GUARD_BYTES, pattern(i + 79u));
}
static int owns_arena(const Workload *w, const GCArena *wanted)
{
  GCArena *a;
  for (a = w->alloc.owned[w->kind]; a; a = lj_arena_next_acq(a))
    if (a == wanted) return 1;
  return 0;
}

/* Owner-only census with every publisher joined and every mapping retained.
** This test's physical upper bound detects cycles, not production bin length. */
static Census census(const Workload *w, const void *fit)
{
  Census c = { 0, 0, 0, 0 };
  uintptr_t *seen = (uintptr_t *)calloc(CENSUS_LIMIT, sizeof(*seen));
  uint32_t b, i;
  assert(seen != NULL);
  for (b = 0; b < LJ_ALLOC_NBINS; b++) {
    const LJArenaFreeRun *run;
    uint32_t position = 0;
    for (run = w->alloc.bins[w->kind][b]; run; run = run->next) {
      GCArena *a = lj_arena_of(run);
      uintptr_t key = (uintptr_t)run;
      assert(c.nodes < CENSUS_LIMIT && owns_arena(w, a));
      assert(run->start >= LJ_AFIRST_CELL && run->start < LJ_ARENA_CELLS);
      assert(lj_arena_cellptr(a, run->start) == (const void *)run);
      assert(run->len && run->len <= LJ_ARENA_CELLS - run->start);
      assert(lj_arena_bin_from_ncells(run->len) == b);
      assert(lj_arena_state(a, run->start) == 1u);
      if (w->flags & LJ_AF_TRAVERSABLE)
        assert(lj_arena_lifetime_state_acq(a, run->start) == LJ_ARENA_LIFETIME_FREE);
      seen[c.nodes++] = key;
      c.cells += run->len;
      position++;
      if (bsearch(&key, w->sorted, w->count, sizeof(key), compare_address)) {
        assert(run->len == lj_arena_ncells(BLOCK_BYTES));
        c.targets++;
      }
      if (run == fit) c.fit_position = position;
      if (fit && !c.fit_position)
        assert(run->len < lj_arena_ncells(FIT_BYTES));
    }
  }
  qsort(seen, c.nodes, sizeof(*seen), compare_address);
  for (i = 1; i < c.nodes; i++) assert(seen[i - 1u] != seen[i]);
  free(seen);
  return c;
}

/* Consume unpublished bump space using actual live allocations. */
static void exhaust_bump(Workload *w)
{
  LJArenaBump *b = &w->alloc.bump[w->kind];
  uint32_t before = lj_arena_alloc_owned_count_acq(&w->alloc, w->kind);
  while (b->a && b->cell < b->end) {
    size_t bytes = (size_t)(b->end - b->cell) * LJ_CELL_SIZE;
    void *p;
    if (bytes > LJ_HUGE_THRESHOLD) bytes = LJ_HUGE_THRESHOLD;
    p = lj_arena_alloc(&w->alloc, &w->prng, bytes, w->flags);
    assert(p != NULL);
    memset(p, 0x71, bytes);
  }
  assert(lj_arena_alloc_owned_count_acq(&w->alloc, w->kind) == before);
}
static void init_workload(Workload *w, uint32_t flags, uint32_t count)
{
  memset(w, 0, sizeof(*w));
  w->flags = flags;
  w->kind = flags & LJ_AF_TRAVERSABLE ? LJ_ARENAK_TRAVERSABLE : LJ_ARENAK_PLAIN;
  w->count = count;
  w->blocks = (unsigned char **)calloc(count ? count : 1u, sizeof(*w->blocks));
  w->guards = (unsigned char **)calloc(count ? count : 1u, sizeof(*w->guards));
  w->sorted = (uintptr_t *)calloc(count ? count : 1u, sizeof(*w->sorted));
  assert(w->blocks && w->guards && w->sorted);
  lj_prng_seed_fixed(&w->prng);
  lj_arena_alloc_init(&w->alloc);
}
static void allocate_blocks(Workload *w)
{
  uint32_t i;
  for (i = 0; i < w->count; i++) {
    w->blocks[i] = (unsigned char *)lj_arena_alloc(
      &w->alloc, &w->prng, BLOCK_BYTES, w->flags);
    w->guards[i] = (unsigned char *)lj_arena_alloc(
      &w->alloc, &w->prng, GUARD_BYTES, w->flags);
    assert(w->blocks[i] && w->guards[i]);
    memset(w->blocks[i], pattern(i), BLOCK_BYTES);
    memset(w->guards[i], pattern(i + 79u), GUARD_BYTES);
    w->sorted[i] = (uintptr_t)w->blocks[i];
  }
  qsort(w->sorted, w->count, sizeof(*w->sorted), compare_address);
  exhaust_bump(w);
}
static void fini_workload(Workload *w)
{
  check_guards(w);
  lj_arena_alloc_fini(&w->alloc);
  free(w->sorted);
  free(w->guards);
  free(w->blocks);
}
static void *publish_blocks(void *ud)
{
  Publish *p = (Publish *)ud;
  Workload *w = p->work;
  uint32_t i;
  for (i = 0; i < w->count; i++) {
    assert(lj_arena_remote_free_publish(&w->alloc, w->blocks[i], BLOCK_BYTES));
    if (p->duplicate)
      assert(lj_arena_remote_free_publish(&w->alloc, w->blocks[i], BLOCK_BYTES));
  }
  return NULL;
}
enum FragmentCase { FRAGMENT_LOCAL, FRAGMENT_REMOTE, FRAGMENT_READER,
                    FRAGMENT_RESERVED };

static int test_fragmented_plain(enum FragmentCase mode)
{
  TGAlloc alloc;
  PRNGState rs;
  GCArena *target = NULL, *guard_arena, *used;
  uint32_t start = 0, i, block_cells = lj_arena_ncells(BLOCK_BYTES);
  uint32_t slots = (LJ_ARENA_CELLS - LJ_AFIRST_CELL) / block_cells;
  uint32_t targets = slots - (mode == FRAGMENT_RESERVED);
  uint32_t large = 4u * BLOCK_BYTES;
  unsigned char **blocks = calloc(targets + slots, sizeof(*blocks));
  unsigned char *value;
  assert(blocks);
  lj_prng_seed_fixed(&rs);
  lj_arena_alloc_init(&alloc);
  if (mode == FRAGMENT_RESERVED) {
    assert(lj_arena_reserve_bump(&alloc, &rs, 0, block_cells, &target, &start));
    memset(lj_arena_cellptr(target, start), 0xcd, BLOCK_BYTES);
  }
  for (i = 0; i < targets + slots; i++) {
    blocks[i] = lj_arena_alloc(&alloc, &rs, BLOCK_BYTES, 0);
    assert(blocks[i]);
    memset(blocks[i], pattern(i), BLOCK_BYTES);
  }
  if (target) assert(target == lj_arena_of(blocks[0]));
  target = lj_arena_of(blocks[0]);
  guard_arena = lj_arena_of(blocks[targets]);
  assert(target != guard_arena);
  assert(lj_arena_alloc_owned_count_acq(&alloc, LJ_ARENAK_PLAIN) == 2);
  for (i = 0; i < targets; i++) {
    assert(lj_arena_of(blocks[i]) == target);
    if (mode == FRAGMENT_REMOTE)
      assert(lj_arena_remote_free_publish(&alloc, blocks[i], BLOCK_BYTES));
    else
      lj_arena_free(&alloc, blocks[i], BLOCK_BYTES);
  }
  if (mode == FRAGMENT_REMOTE)
    assert(lj_arena_remote_free_drain(&alloc) == targets);
  if (mode == FRAGMENT_READER)
    assert(lj_arena_rescue_enter(target) == LJ_ARENA_RESCUE_FULL);
  value = lj_arena_alloc(&alloc, &rs, large, 0);
  assert(value);
  used = lj_arena_of(value);
  memset(value, 0xab, large);
  if (mode == FRAGMENT_READER || mode == FRAGMENT_RESERVED) {
    assert(used != target && used != guard_arena);
    if (mode == FRAGMENT_READER)
      lj_arena_rescue_leave(target);
    else {
      check_bytes(lj_arena_cellptr(target, start), BLOCK_BYTES, 0xcd);
      lj_arena_block_set(target, start);  /* Finish the earlier reservation. */
      lj_arena_free(&alloc, lj_arena_cellptr(target, start), BLOCK_BYTES);
    }
    while (alloc.bump[LJ_ARENAK_PLAIN].cell + lj_arena_ncells(large) <=
           alloc.bump[LJ_ARENAK_PLAIN].end) {
      void *p = lj_arena_alloc(&alloc, &rs, large, 0);
      assert(p && lj_arena_of(p) == used);
    }
    check_bytes(value, large, 0xab);
    value = lj_arena_alloc(&alloc, &rs, large, 0);
    assert(value);
    used = lj_arena_of(value);
    assert(lj_arena_alloc_owned_count_acq(&alloc, LJ_ARENAK_PLAIN) == 3);
  } else {
    assert(lj_arena_alloc_owned_count_acq(&alloc, LJ_ARENAK_PLAIN) == 2);
  }
  assert(used == target);
  for (i = targets; i < targets + slots; i++) {
    assert(lj_arena_of(blocks[i]) == guard_arena);
    check_bytes(blocks[i], BLOCK_BYTES, pattern(i));
  }
  lj_arena_alloc_fini(&alloc);
  free(blocks);
  return 1;
}

static void free_blocks(Workload *w, enum FreeSource source)
{
  uint32_t i;
  GCArena *a;
  pthread_t producer;
  Publish publish = { w, (w->flags & LJ_AF_TRAVERSABLE) != 0 };
  for (i = 0; i < w->count; i++)
    check_bytes(w->blocks[i], BLOCK_BYTES, pattern(i));
  if (source == FREE_LATE) {
    assert(w->kind == LJ_ARENAK_PLAIN);
    for (a = w->alloc.owned[w->kind]; a; a = lj_arena_next_acq(a))
      assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_FULL);
  }
  if (source != FREE_REMOTE) {
    for (i = 0; i < w->count; i++) {
      lj_arena_free(&w->alloc, w->blocks[i], BLOCK_BYTES);
      if (source == FREE_LATE)
        assert(lj_arena_free_deferred(&w->alloc, w->blocks[i], BLOCK_BYTES));
    }
  }
  if (source != FREE_LOCAL) {
    assert(pthread_create(&producer, NULL, publish_blocks, &publish) == 0);
    assert(pthread_join(producer, NULL) == 0);
    if (source == FREE_LATE) {
      assert(lj_arena_remote_free_drain(&w->alloc) == 0u);
      assert(lj_arena_remote_pending_acq(&w->alloc));
      for (i = 0; i < w->count; i++) {
        a = lj_arena_of(w->blocks[i]);
        assert(lj_arena_bm_get(a->block, lj_arena_cellof(w->blocks[i])));
        assert(lj_arena_late_get(a, lj_arena_cellof(w->blocks[i])));
        check_bytes(w->blocks[i], BLOCK_BYTES, pattern(i));
      }
      for (a = w->alloc.owned[w->kind]; a; a = lj_arena_next_acq(a)) {
        assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL);
        lj_arena_rescue_leave(a);
      }
    }
    assert(lj_arena_remote_free_drain(&w->alloc) == w->count);
    assert(lj_arena_remote_free_drain(&w->alloc) == 0u);
  }
  /* Repeated terminal frees before reuse must not publish duplicate nodes.
  ** PLAIN queue publication itself keeps its single-owner free contract. */
  for (i = 0; i < w->count; i++) {
    a = lj_arena_of(w->blocks[i]);
    assert(!lj_arena_bm_get(a->block, lj_arena_cellof(w->blocks[i])));
    assert(!lj_arena_late_get(a, lj_arena_cellof(w->blocks[i])));
    lj_arena_free(&w->alloc, w->blocks[i], BLOCK_BYTES);
  }
}

static int test_capacity(uint32_t flags, enum FreeSource source)
{
  static const char *sources[] = { "local", "remote", "late" };
  Workload w;
  Census before, freed;
  uint32_t arenas, after, i, reused = 0;
  unsigned char *seen, **again;
  int ok;
  init_workload(&w, flags, CAPACITY_RUNS);
  allocate_blocks(&w);
  before = census(&w, NULL);
  arenas = lj_arena_alloc_owned_count_acq(&w.alloc, w.kind);
  free_blocks(&w, source);
  freed = census(&w, NULL);
  seen = (unsigned char *)calloc(w.count, 1);
  again = (unsigned char **)calloc(w.count, sizeof(*again));
  assert(seen && again);
  for (i = 0; i < w.count; i++) {
    uintptr_t key, *found;
    again[i] = (unsigned char *)lj_arena_alloc(
      &w.alloc, &w.prng, BLOCK_BYTES, flags);
    assert(again[i]);
    key = (uintptr_t)again[i];
    found = (uintptr_t *)bsearch(&key, w.sorted, w.count,
                                sizeof(key), compare_address);
    if (found) {
      size_t index = (size_t)(found - w.sorted);
      assert(!seen[index]);
      seen[index] = 1;
      reused++;
    }
    memset(again[i], pattern(i + 103u), BLOCK_BYTES);
  }
  after = lj_arena_alloc_owned_count_acq(&w.alloc, w.kind);
  for (i = 0; i < w.count; i++)
    check_bytes(again[i], BLOCK_BYTES, pattern(i + 103u));
  ok = freed.targets == w.count && reused == w.count && after == arenas &&
       freed.cells == before.cells + (uint64_t)w.count *
                                      lj_arena_ncells(BLOCK_BYTES);
  printf("capacity kind=%s source=%s targets=%u/%u bins=%u cells=%llu/%llu "
         "reused=%u/%u arenas=%u->%u %s\n",
         flags ? "traversable" : "plain", sources[source], freed.targets,
         w.count, freed.nodes, (unsigned long long)freed.cells,
         (unsigned long long)(before.cells + (uint64_t)w.count *
                                            lj_arena_ncells(BLOCK_BYTES)),
         reused, w.count, arenas, after, ok ? "PASS" : "FAIL");
  free(again);
  free(seen);
  fini_workload(&w);
  return ok;
}
static int test_search(uint32_t flags)
{
  Workload w;
  Census before, after;
  unsigned char *fit, *guard;
  void *found, *miss;
  uint32_t i, arenas, after_fit;
  int ok;
  init_workload(&w, flags, SEARCH_PREFIX);
  fit = (unsigned char *)lj_arena_alloc(&w.alloc, &w.prng, FIT_BYTES, flags);
  guard = (unsigned char *)lj_arena_alloc(&w.alloc, &w.prng, GUARD_BYTES, flags);
  assert(fit && guard);
  memset(fit, 0x45, FIT_BYTES);
  memset(guard, 0x92, GUARD_BYTES);
  allocate_blocks(&w);
  arenas = lj_arena_alloc_owned_count_acq(&w.alloc, w.kind);
  lj_arena_free(&w.alloc, fit, FIT_BYTES);
  for (i = 0; i < w.count; i++)
    lj_arena_free(&w.alloc, w.blocks[i], BLOCK_BYTES);
  before = census(&w, fit);
  /* Old insertion truncates only after 8192 existing nodes, then prepends.
  ** The fitting node survives at position 8193, isolating search without a
  ** rebuild even when older arena-tail nodes behind it were truncated. */
  assert(before.fit_position == SEARCH_PREFIX + 1u);
  assert(before.targets == w.count);
  found = lj_arena_alloc(&w.alloc, &w.prng, FIT_BYTES, flags);
  assert(found);
  after_fit = lj_arena_alloc_owned_count_acq(&w.alloc, w.kind);
  after = census(&w, NULL);
  ok = found == fit && after_fit == arenas && after.targets == w.count &&
       after.cells + lj_arena_ncells(FIT_BYTES) == before.cells;
  /* An unsuccessful complete search must also retain every nonfitting run.
  ** This second fitting allocation may legitimately require a fresh arena. */
  if (ok) {
    miss = lj_arena_alloc(&w.alloc, &w.prng, FIT_BYTES, flags);
    assert(miss && miss != fit);
    after = census(&w, NULL);
    ok = after.targets == w.count &&
         after.cells + lj_arena_ncells(FIT_BYTES) == before.cells;
  }
  check_bytes(guard, GUARD_BYTES, 0x92);
  printf("search kind=%s fit_position=%u returned_fit=%d remaining=%u/%u "
         "arenas=%u->%u %s\n", flags ? "traversable" : "plain",
         before.fit_position, found == fit, after.targets, w.count,
         arenas, after_fit, ok ? "PASS" : "FAIL");
  fini_workload(&w);
  return ok;
}

/* Deliberately corrupted owner-private links, separate from real-capacity
** witnesses. Require bounded completion and safe fallback, not full recovery. */
static int test_defensive_cycles(void)
{
  Workload w;
  LJArenaFreeRun *a, *b;
  void *third, *found;
  uint32_t bin = lj_arena_bin_from_ncells(lj_arena_ncells(BLOCK_BYTES));
  init_workload(&w, 0, 0);
  a = (LJArenaFreeRun *)lj_arena_alloc(&w.alloc, &w.prng, BLOCK_BYTES, 0);
  b = (LJArenaFreeRun *)lj_arena_alloc(&w.alloc, &w.prng, BLOCK_BYTES, 0);
  third = lj_arena_alloc(&w.alloc, &w.prng, BLOCK_BYTES, 0);
  assert(a && b && third);
  exhaust_bump(&w);
  /* Allocated bodies make these forged leading nodes stale. */
  a->start = lj_arena_cellof(a); a->len = lj_arena_ncells(BLOCK_BYTES);
  b->start = lj_arena_cellof(b); b->len = lj_arena_ncells(BLOCK_BYTES);
  a->next = b; b->next = a;
  w.alloc.bins[w.kind][bin] = a;
  la_store32_rel(&w.alloc.binmask[w.kind], (uint32_t)1u << bin);
  lj_arena_free(&w.alloc, third, BLOCK_BYTES);
  assert(lj_arena_alloc(&w.alloc, &w.prng, BLOCK_BYTES, 0) == third);
  lj_arena_alloc_fini(&w.alloc);
  lj_arena_alloc_init(&w.alloc);
  a = (LJArenaFreeRun *)lj_arena_alloc(&w.alloc, &w.prng, BLOCK_BYTES, 0);
  b = (LJArenaFreeRun *)lj_arena_alloc(&w.alloc, &w.prng, BLOCK_BYTES, 0);
  assert(a && b);
  exhaust_bump(&w);
  lj_arena_free(&w.alloc, a, BLOCK_BYTES);
  lj_arena_free(&w.alloc, b, BLOCK_BYTES);
  /* Real free nodes with a deliberately corrupt back-edge. */
  assert(w.alloc.bins[w.kind][bin] == b && b->next == a);
  a->next = b;
  found = lj_arena_alloc(&w.alloc, &w.prng, FIT_BYTES, 0);
  assert(found && found != (void *)a && found != (void *)b);
  fini_workload(&w);
  puts("defensive-cycles stale-insertion=bounded nonfit-search=bounded PASS");
  return 1;
}

int main(int argc, char **argv)
{
  int ok = 1, ran = 0;
  const char *selected = argc > 1 ? argv[1] : "all";
  assert(argc <= 2);
  setbuf(stdout, NULL);
  alarm(120);
#define RUN(name, expression) \
  do { if (!strcmp(selected, "all") || !strcmp(selected, name)) { \
    ran = 1; if (!(expression)) ok = 0; \
  } } while (0)
  RUN("plain-local", test_capacity(0, FREE_LOCAL));
  RUN("traversable-local", test_capacity(LJ_AF_TRAVERSABLE, FREE_LOCAL));
  RUN("plain-remote", test_capacity(0, FREE_REMOTE));
  RUN("traversable-remote", test_capacity(LJ_AF_TRAVERSABLE, FREE_REMOTE));
  RUN("plain-late", test_capacity(0, FREE_LATE));
  RUN("fragment-local", test_fragmented_plain(FRAGMENT_LOCAL));
  RUN("fragment-remote", test_fragmented_plain(FRAGMENT_REMOTE));
  RUN("fragment-reader", test_fragmented_plain(FRAGMENT_READER));
  RUN("fragment-reserved", test_fragmented_plain(FRAGMENT_RESERVED));
  RUN("plain-search", test_search(0));
  RUN("traversable-search", test_search(LJ_AF_TRAVERSABLE));
  RUN("defensive-cycles", test_defensive_cycles());
#undef RUN
  if (!ran) {
    fprintf(stderr, "unknown bin-capacity case: %s\n", selected);
    return 2;
  }
  alarm(0);
  if (ok) puts("t-arena-bin-capacity OK");
  return ok ? 0 : 1;
}
