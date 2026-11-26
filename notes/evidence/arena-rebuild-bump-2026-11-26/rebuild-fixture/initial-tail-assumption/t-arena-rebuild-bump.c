/* Rebuild must exclude only the private bump interval, including when that
** interval is interior to an arena or to one coalesced scanner callback.
** All layouts arise from real allocations/frees. No cursor or bin-link edits. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"

#define MAX_SPANS 64u
#define MAX_ARENAS 8u

typedef struct Span {
  unsigned char *p;
  size_t bytes;
  uint32_t active, value, state, ready, life, root, recovery;
} Span;
typedef struct Work {
  TGAlloc alloc;
  PRNGState prng;
  uint32_t flags, kind, count;
  Span spans[MAX_SPANS];
} Work;
typedef struct Coverage {
  uint32_t available, expected, nodes;
} Coverage;
typedef struct BumpSnapshot {
  LJArenaBump bump;
  unsigned char *body;
  uint32_t *planes;
} BumpSnapshot;

static void init(Work *w, uint32_t flags)
{
  memset(w, 0, sizeof(*w));
  w->flags = flags;
  w->kind = flags & LJ_AF_TRAVERSABLE ? LJ_ARENAK_TRAVERSABLE : LJ_ARENAK_PLAIN;
  lj_arena_alloc_init(&w->alloc);
  lj_prng_seed_fixed(&w->prng);
}

static Span *take(Work *w, size_t bytes)
{
  Span *s;
  GCArena *a;
  uint32_t cell;
  assert(bytes && bytes <= LJ_HUGE_THRESHOLD && bytes % LJ_CELL_SIZE == 0);
  assert(w->count < MAX_SPANS);
  s = &w->spans[w->count++];
  s->p = (unsigned char *)lj_arena_alloc(&w->alloc, &w->prng, bytes, w->flags);
  assert(s->p != NULL);
  s->bytes = bytes; s->active = 1; s->value = 0x30u + w->count;
  memset(s->p, (int)s->value, bytes);
  a = lj_arena_of(s->p); cell = lj_arena_cellof(s->p);
  s->state = lj_arena_state(a, cell);
  s->ready = lj_arena_ready_get(a, cell);
  s->life = lj_arena_lifetime_state_acq(a, cell);
  s->root = lj_arena_root_state_acq(a, cell);
  s->recovery = lj_arena_recovery_state_acq(a, cell);
  return s;
}

static void drop(Work *w, Span *s)
{
  assert(s->active);
  lj_arena_free(&w->alloc, s->p, s->bytes);
  assert(!lj_arena_bm_get(lj_arena_of(s->p)->block, lj_arena_cellof(s->p)));
  s->active = 0;
}

static void check_live(const Work *w)
{
  uint32_t i;
  for (i = 0; i < w->count; i++) {
    const Span *s = &w->spans[i];
    GCArena *a;
    uint32_t cell;
    size_t j;
    if (!s->active) continue;
    a = lj_arena_of(s->p); cell = lj_arena_cellof(s->p);
    assert(lj_arena_state(a, cell) == s->state);
    assert(lj_arena_ready_get(a, cell) == s->ready);
    assert(lj_arena_lifetime_state_acq(a, cell) == s->life);
    assert(lj_arena_root_state_acq(a, cell) == s->root);
    assert(lj_arena_recovery_state_acq(a, cell) == s->recovery);
    for (j = 0; j < s->bytes; j++) assert(s->p[j] == s->value);
  }
}

static void protect_live(Span *root, Span *recovery)
{
  assert(lj_arena_root_state_cas(lj_arena_of(root->p), lj_arena_cellof(root->p),
    LJ_ARENA_ROOT_NONE, LJ_ARENA_ROOT_MEMBER));
  root->root = LJ_ARENA_ROOT_MEMBER;
  assert(lj_arena_recovery_state_cas(lj_arena_of(recovery->p),
    lj_arena_cellof(recovery->p), LJ_ARENA_RECOVERY_IDLE,
    LJ_ARENA_RECOVERY_PENDING));
  recovery->recovery = LJ_ARENA_RECOVERY_PENDING;
}

static void finish(Work *w)
{
  uint32_t i;
  check_live(w);
  for (i = 0; i < w->count; i++) {
    Span *s = &w->spans[i];
    GCArena *a;
    uint32_t cell;
    if (!s->active) continue;
    a = lj_arena_of(s->p); cell = lj_arena_cellof(s->p);
    if (s->root == LJ_ARENA_ROOT_MEMBER)
      assert(lj_arena_root_state_cas(a, cell, s->root, LJ_ARENA_ROOT_NONE));
    if (s->recovery == LJ_ARENA_RECOVERY_PENDING) {
      assert(lj_arena_recovery_state_cas(a, cell, s->recovery,
                                       LJ_ARENA_RECOVERY_CLAIMED));
      assert(lj_arena_recovery_state_cas(a, cell, LJ_ARENA_RECOVERY_CLAIMED,
                                       LJ_ARENA_RECOVERY_IDLE));
    }
  }
  assert(lj_arena_alloc_fini_try(&w->alloc));
}

static void exhaust(Work *w)
{
  LJArenaBump *b = &w->alloc.bump[w->kind];
  uint32_t arenas = lj_arena_alloc_owned_count_acq(&w->alloc, w->kind);
  while (b->a && b->cell < b->end) {
    size_t bytes = (size_t)(b->end - b->cell) * LJ_CELL_SIZE;
    if (bytes > LJ_HUGE_THRESHOLD) bytes = LJ_HUGE_THRESHOLD;
    (void)take(w, bytes);
  }
  assert(lj_arena_alloc_owned_count_acq(&w->alloc, w->kind) == arenas);
}

static uint32_t planes(GCArena *a, uint32_t cell)
{
  return lj_arena_state(a, cell) | (lj_arena_ready_get(a, cell) << 2) |
    (lj_arena_late_get(a, cell) << 3) | (lj_arena_cdata_get(a, cell) << 4) |
    (lj_arena_lifetime_state_acq(a, cell) << 8) |
    (lj_arena_root_state_acq(a, cell) << 12) |
    (lj_arena_recovery_state_acq(a, cell) << 16);
}

static BumpSnapshot snapshot(const Work *w)
{
  BumpSnapshot s;
  uint32_t i, n;
  s.bump = w->alloc.bump[w->kind]; s.body = NULL; s.planes = NULL;
  n = s.bump.a ? s.bump.end - s.bump.cell : 0;
  if (n) {
    s.body = (unsigned char *)malloc((size_t)n * LJ_CELL_SIZE);
    s.planes = (uint32_t *)malloc((size_t)n * sizeof(*s.planes));
    assert(s.body && s.planes);
    memcpy(s.body, lj_arena_cellptr(s.bump.a, s.bump.cell), (size_t)n * LJ_CELL_SIZE);
    for (i = 0; i < n; i++) s.planes[i] = planes(s.bump.a, s.bump.cell + i);
  }
  return s;
}

static void unchanged(const Work *w, BumpSnapshot *s)
{
  const LJArenaBump *b = &w->alloc.bump[w->kind];
  uint32_t i, n = s->bump.a ? s->bump.end - s->bump.cell : 0;
  assert(b->a == s->bump.a && b->cell == s->bump.cell && b->end == s->bump.end);
  if (n) {
    assert(!memcmp(s->body, lj_arena_cellptr(b->a, b->cell), (size_t)n * LJ_CELL_SIZE));
    for (i = 0; i < n; i++) assert(s->planes[i] == planes(b->a, b->cell + i));
  }
  free(s->body); free(s->planes);
}

/* Independently account for every usable cell using allocation history and
** the private interval. Published runs may coalesce, but must cover each
** remaining cell once, without touching a live allocation or private cell. */
static Coverage coverage(const Work *w)
{
  GCArena *arenas[MAX_ARENAS], *a;
  unsigned char map[MAX_ARENAS][LJ_ARENA_CELLS];
  Coverage c = { 0, 0, 0 };
  uint32_t count = 0, i, j, k, bin;
  memset(map, 0, sizeof(map));
  for (a = w->alloc.owned[w->kind]; a; a = lj_arena_next_acq(a)) {
    assert(count < MAX_ARENAS); arenas[count++] = a;
  }
  assert(count == lj_arena_alloc_owned_count_acq(&w->alloc, w->kind));
  for (i = 0; i < w->count; i++) {
    const Span *s = &w->spans[i];
    uint32_t start, len;
    if (!s->active) continue;
    for (k = 0; k < count && arenas[k] != lj_arena_of(s->p); k++) {}
    assert(k < count); start = lj_arena_cellof(s->p); len = lj_arena_ncells(s->bytes);
    for (j = start; j < start + len; j++) {
      assert(!map[k][j]); map[k][j] = 1;
    }
  }
  if (w->alloc.bump[w->kind].a) {
    const LJArenaBump *b = &w->alloc.bump[w->kind];
    for (k = 0; k < count && arenas[k] != b->a; k++) {}
    assert(k < count && b->cell <= b->end && b->end <= LJ_ARENA_CELLS);
    for (j = b->cell; j < b->end; j++) {
      assert(!map[k][j]); map[k][j] = 2;
    }
  }
  for (k = 0; k < count; k++)
    for (j = LJ_AFIRST_CELL; j < LJ_ARENA_CELLS; j++)
      if (!map[k][j]) c.expected++;
  for (bin = 0; bin < LJ_ALLOC_NBINS; bin++) {
    const LJArenaFreeRun *run;
    for (run = w->alloc.bins[w->kind][bin]; run; run = run->next) {
      assert(++c.nodes <= MAX_ARENAS * LJ_ARENA_CELLS);
      for (k = 0; k < count && arenas[k] != lj_arena_of(run); k++) {}
      assert(k < count);
      assert(run->start >= LJ_AFIRST_CELL && run->start < LJ_ARENA_CELLS);
      assert(lj_arena_cellptr(arenas[k], run->start) == (const void *)run);
      assert(run->len && run->len <= LJ_ARENA_CELLS - run->start);
      assert(lj_arena_bin_from_ncells(run->len) == bin);
      for (j = run->start; j < run->start + run->len; j++) {
        assert(!map[k][j]); map[k][j] = 3; c.available++;
      }
    }
  }
  check_live(w);
  return c;
}

static Coverage rebuild(Work *w)
{
  BumpSnapshot b = snapshot(w);
  lj_arena_alloc_rebuild_free_kind(&w->alloc, w->kind);
  unchanged(w, &b);
  return coverage(w);
}

static int test_interior(uint32_t flags, int exhausted)
{
  Work w;
  Span *p, *q, *g1, *g2, *half, *found;
  Coverage c;
  uint32_t arenas, i;
  int ok = 1;
  init(&w, flags);
  p = take(&w, 1024); g1 = take(&w, 16);
  q = take(&w, 1024); g2 = take(&w, 16); exhaust(&w);
  arenas = lj_arena_alloc_owned_count_acq(&w.alloc, w.kind);
  drop(&w, q); drop(&w, p); half = take(&w, 512); assert(half->p == p->p);
  protect_live(g1, g2);
  if (exhausted) assert(take(&w, 512)->p == p->p + 512);
  for (i = 0; i < 3; i++) {
    c = rebuild(&w); ok &= c.available == c.expected;
  }
  if (!exhausted) assert(take(&w, 512)->p == p->p + 512);
  found = take(&w, 1024);
  ok &= found->p == q->p && lj_arena_alloc_owned_count_acq(&w.alloc, w.kind) == arenas;
  printf("interior kind=%s exhausted=%d free_cells=%u/%u exact_suffix=%d "
         "arenas=%u->%u %s\n", flags ? "traversable" : "plain", exhausted,
         c.available, c.expected, found->p == q->p, arenas,
         lj_arena_alloc_owned_count_acq(&w.alloc, w.kind), ok ? "PASS" : "FAIL");
  finish(&w); return ok;
}

typedef struct Crossing { uint32_t cell, end, found; } Crossing;
static void find_crossing(uint32_t start, uint32_t len, void *ud)
{
  Crossing *c = (Crossing *)ud;
  if (start < c->cell && start + len > c->end) c->found++;
}

static int test_crossing(uint32_t flags)
{
  Work w;
  Span *before, *middle, *right, *later, *half;
  Coverage c;
  Crossing crossing;
  uint32_t i, found = 0, arenas;
  int ok = 1;
  init(&w, flags);
  before = take(&w, 1024); (void)take(&w, 16);
  middle = take(&w, 1024); right = take(&w, 1024); (void)take(&w, 16);
  later = take(&w, 1024); (void)take(&w, 16); exhaust(&w);
  arenas = lj_arena_alloc_owned_count_acq(&w.alloc, w.kind);
  drop(&w, before); drop(&w, later); drop(&w, right); drop(&w, middle);
  half = take(&w, 512); assert(half->p == middle->p); drop(&w, half);
  crossing.cell = w.alloc.bump[w.kind].cell;
  crossing.end = w.alloc.bump[w.kind].end; crossing.found = 0;
  lj_arena_scan_free_runs(lj_arena_of(middle->p), find_crossing, &crossing);
  assert(crossing.found == 1u);
  c = coverage(&w); assert(c.available == c.expected);
  for (i = 0; i < 3; i++) {
    c = rebuild(&w); ok &= c.available == c.expected;
  }
  assert(take(&w, 512)->p == middle->p + 512);
  for (i = 0; i < 3; i++) {
    Span *s = take(&w, 1024);
    if (s->p == before->p) { assert(!(found & 1u)); found |= 1u; }
    if (s->p == right->p) { assert(!(found & 2u)); found |= 2u; }
    if (s->p == later->p) { assert(!(found & 4u)); found |= 4u; }
  }
  ok &= take(&w, 512)->p == middle->p && found == 7u &&
        lj_arena_alloc_owned_count_acq(&w.alloc, w.kind) == arenas;
  printf("crossing kind=%s scan_crossings=%u free_cells=%u/%u reused_mask=%u "
         "arenas=%u->%u %s\n", flags ? "traversable" : "plain", crossing.found,
         c.available, c.expected, found, arenas,
         lj_arena_alloc_owned_count_acq(&w.alloc, w.kind), ok ? "PASS" : "FAIL");
  finish(&w); return ok;
}

static int test_tail(uint32_t flags)
{
  Work w;
  Span *p, *q, *a, *b;
  Coverage c;
  uint32_t arenas, i;
  int ok = 1;
  init(&w, flags);
  p = take(&w, 1024); (void)take(&w, 16); q = take(&w, 1024);
  assert(w.alloc.bump[w.kind].end == LJ_ARENA_CELLS);
  drop(&w, p); drop(&w, q);
  arenas = lj_arena_alloc_owned_count_acq(&w.alloc, w.kind);
  for (i = 0; i < 3; i++) {
    c = rebuild(&w); ok &= c.available == c.expected;
  }
  exhaust(&w); c = rebuild(&w); ok &= c.available == c.expected;
  a = take(&w, 1024); b = take(&w, 1024);
  ok &= ((a->p == p->p && b->p == q->p) || (a->p == q->p && b->p == p->p)) &&
        lj_arena_alloc_owned_count_acq(&w.alloc, w.kind) == arenas;
  /* Exact bin consumption leaves no active bump. That case excludes nothing. */
  assert(w.alloc.bump[w.kind].a == NULL);
  c = rebuild(&w); ok &= c.available == c.expected;
  printf("tail kind=%s free_cells=%u/%u arenas=%u->%u %s\n",
         flags ? "traversable" : "plain", c.available, c.expected, arenas,
         lj_arena_alloc_owned_count_acq(&w.alloc, w.kind), ok ? "PASS" : "FAIL");
  finish(&w); return ok;
}

static int test_transfer(uint32_t flags)
{
  Work dst, src;
  Span *p, *q, *source, *half, *suffix;
  BumpSnapshot saved;
  Coverage c;
  GCArena *source_arena;
  unsigned char *source_tail;
  size_t tail_bytes, offset;
  uint32_t i, arenas;
  int ok;
  init(&dst, flags); init(&src, flags);
  p = take(&dst, 1024); (void)take(&dst, 16);
  q = take(&dst, 1024); (void)take(&dst, 16); exhaust(&dst);
  drop(&dst, q); drop(&dst, p); half = take(&dst, 512); assert(half->p == p->p);
  source = take(&src, 1024); (void)take(&src, 16); drop(&src, source);
  source_arena = src.alloc.bump[src.kind].a;
  source_tail = (unsigned char *)lj_arena_cellptr(source_arena, src.alloc.bump[src.kind].cell);
  tail_bytes = (size_t)(src.alloc.bump[src.kind].end - src.alloc.bump[src.kind].cell) * LJ_CELL_SIZE;
  saved = snapshot(&dst);
  assert(lj_arena_alloc_transfer(&dst.alloc, &src.alloc) == 1u);
  unchanged(&dst, &saved);
  assert(src.alloc.owned[src.kind] == NULL && src.alloc.bump[src.kind].a == NULL);
  assert(src.alloc.bump[src.kind].cell == 0 && src.alloc.bump[src.kind].end == 0);
  assert(lj_arena_alloc_owned_count_acq(&src.alloc, src.kind) == 0u);
  for (i = 0; i < LJ_ALLOC_NBINS; i++) assert(src.alloc.bins[src.kind][i] == NULL);
  assert(lj_arena_state(source_arena, lj_arena_cellof(source_tail)) == 1u);
  assert(dst.count + src.count <= MAX_SPANS);
  memcpy(&dst.spans[dst.count], src.spans, (size_t)src.count * sizeof(Span));
  dst.count += src.count;
  arenas = lj_arena_alloc_owned_count_acq(&dst.alloc, dst.kind);
  assert(arenas == 2u);
  c = coverage(&dst); ok = c.available == c.expected;
  assert(take(&dst, 512)->p == p->p + 512);
  suffix = take(&dst, 1024); ok &= suffix->p == q->p;
  /* Drain the transferred original tail by real allocations, then require
  ** the exact source free block. No new mapping is needed at either end. */
  for (offset = 0; offset < tail_bytes;) {
    size_t bytes = tail_bytes - offset;
    Span *s;
    if (bytes > LJ_HUGE_THRESHOLD) bytes = LJ_HUGE_THRESHOLD;
    s = take(&dst, bytes); ok &= s->p == source_tail + offset; offset += bytes;
  }
  ok &= take(&dst, 1024)->p == source->p &&
        lj_arena_alloc_owned_count_acq(&dst.alloc, dst.kind) == arenas;
  printf("transfer kind=%s free_cells=%u/%u destination_suffix=%d "
         "arenas=%u->%u %s\n", flags ? "traversable" : "plain",
         c.available, c.expected, suffix->p == q->p, arenas,
         lj_arena_alloc_owned_count_acq(&dst.alloc, dst.kind), ok ? "PASS" : "FAIL");
  lj_arena_alloc_fini(&src.alloc); finish(&dst); return ok;
}

int main(int argc, char **argv)
{
  const char *selected = argc > 1 ? argv[1] : "all";
  int ok = 1, ran = 0;
  assert(argc <= 2); setbuf(stdout, NULL); alarm(60);
#define RUN(name, expression) \
  do { if (!strcmp(selected, "all") || !strcmp(selected, name)) { \
    ran = 1; if (!(expression)) ok = 0; \
  } } while (0)
  RUN("plain-interior", test_interior(0, 0));
  RUN("traversable-interior", test_interior(LJ_AF_TRAVERSABLE, 0));
  RUN("plain-exhausted", test_interior(0, 1));
  RUN("traversable-exhausted", test_interior(LJ_AF_TRAVERSABLE, 1));
  RUN("plain-crossing", test_crossing(0));
  RUN("traversable-crossing", test_crossing(LJ_AF_TRAVERSABLE));
  RUN("plain-tail", test_tail(0));
  RUN("traversable-tail", test_tail(LJ_AF_TRAVERSABLE));
  RUN("plain-transfer", test_transfer(0));
  RUN("traversable-transfer", test_transfer(LJ_AF_TRAVERSABLE));
#undef RUN
  if (!ran) { fprintf(stderr, "unknown rebuild-bump case: %s\n", selected); return 2; }
  alarm(0);
  if (ok) puts("t-arena-rebuild-bump OK");
  return ok ? 0 : 1;
}
