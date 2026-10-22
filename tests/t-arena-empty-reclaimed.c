/* Terminal-empty reclaimed arenas retain reuse without repeating sweep work. */
#ifndef LJ_ARENA_TEST_HELPERS
#error "t-arena-empty-reclaimed requires LJ_ARENA_TEST_HELPERS"
#endif

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"

#define KIND LJ_ARENAK_TRAVERSABLE

static uint32_t count(GCArena *a)
{
  uint32_t n = 0;
  for (; a; a = lj_arena_next_acq(a))
    assert(++n < 16u);
  return n;
}

static void check_counts(TGAlloc *alloc)
{
  assert(lj_arena_alloc_owned_count_acq(alloc, KIND) ==
         count(alloc->owned[KIND]));
  assert(lj_arena_alloc_needsweep_count_acq(alloc, KIND) ==
         count(alloc->needsweep[KIND]));
}

static void check_no_blocks(GCArena *a)
{
  uint32_t w;
  for (w = 0; w < LJ_ARENA_WORDS; w++)
    assert(la_load64_acq(&a->block[w]) == 0);
}

static GCArena *finish_one(TGAlloc *alloc, uint32_t epoch)
{
  uint32_t reason = LJ_ARENA_FINISH_NONE;
  GCArena *a = lj_arena_alloc_quarantine_one(alloc, KIND, 0);
  assert(a && lj_arena_reclaim_seal(a));
  assert(lj_arena_alloc_quarantine_finish(
    alloc, KIND, a, epoch, 1, &reason));
  assert(reason == LJ_ARENA_FINISH_COMMITTED);
  check_counts(alloc);
  return a;
}

/* Allocate and physically free real storage, then use the ordinary standalone
** sweep/quarantine API. No synthetic empty certificate or live count is set. */
static GCArena *make_empty(TGAlloc *alloc, PRNGState *rs, void **oldp)
{
  GCArena *a;
  void *p;
  lj_arena_alloc_init(alloc);
  p = lj_arena_alloc(alloc, rs, 64u, LJ_AF_TRAVERSABLE);
  assert(p);
  a = lj_arena_of(p);
  assert(lj_arena_lifetime_state_acq(a, lj_arena_cellof(p)) ==
         LJ_ARENA_LIFETIME_LIVE);
  memset(p, 0x5a, 64u);
  lj_arena_free(alloc, p, 64u);
  assert(lj_arena_lifetime_state_acq(a, lj_arena_cellof(p)) ==
         LJ_ARENA_LIFETIME_FREE);
  assert(lj_arena_alloc_prepare_sweep_kind(alloc, KIND));
  assert(finish_one(alloc, 7u) == a);
  assert(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED);
  assert(lj_arena_remote_active_acq(a) == LJ_ARENA_REMOTE_CLOSED);
  assert(a->hdr.live_cells == 0);
  check_no_blocks(a);
  if (oldp)
    *oldp = p;
  return a;
}

static void check_kept(TGAlloc *alloc, GCArena *a)
{
  check_counts(alloc);
  assert(alloc->owned[KIND] == NULL && alloc->needsweep[KIND] == NULL);
  assert(alloc->quarantine[KIND] == NULL);
  assert(lj_arena_alloc_reclaimed_head(alloc, KIND) == NULL);
  assert(lj_arena_alloc_empty_reclaimed_head(alloc) == a);
  assert(lj_arena_next_acq(a) == NULL);
  assert(lj_arena_alloc_binmask_acq(alloc, KIND) == 0);
  assert(a->hdr.live_cells == 0 && a->hdr.sweep_epoch == 7u);
  check_no_blocks(a);
}

static void test_repeated_skip_and_black_reuse(PRNGState *rs)
{
  TGAlloc alloc;
  GCArena *a;
  GCArena before;
  void *old, *p;
  uint32_t cell, i;
  a = make_empty(&alloc, rs, &old);
  cell = lj_arena_cellof(old);
  /* Mark and coverage are scratch at FREE/block0. The shortcut preserves
  ** them, while ordinary whole-run adoption must scrub old coverage before
  ** publishing a new active-black allocation. */
  lj_arena_bm_set(a->mark, cell + 1u);
  lj_arena_bm_set(a->cdata, cell + 1u);
  memcpy(&before, a, sizeof(before));
  for (i = 0; i < 8u; i++) {
    assert(lj_arena_alloc_prepare_sweep_kind(&alloc, KIND));
    check_kept(&alloc, a);
    assert(memcmp(&before, a, sizeof(before)) == 0);
  }
  lj_arena_alloc_black_rel(&alloc, 1);
  p = lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE);
  assert(p && lj_arena_of(p) == a);
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  assert(lj_arena_bm_get(a->block, lj_arena_cellof(p)));
  assert(lj_arena_bm_get(a->mark, lj_arena_cellof(p)));
  for (i = 0; i < LJ_ARENA_WORDS; i++)
    assert(la_load64_acq(&a->cdata[i]) == 0);
  memset(p, 0xa5, 64u);
  check_counts(&alloc);
  lj_arena_free(&alloc, p, 64u);
  assert(lj_arena_alloc_fini_try(&alloc));
}

static void test_live_block_and_hint(PRNGState *rs)
{
  TGAlloc alloc;
  GCArena *a;
  void *p;
  uint32_t cell;
  lj_arena_alloc_init(&alloc);
  lj_arena_alloc_black_rel(&alloc, 1);
  p = lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE);
  assert(p);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  memset(p, 0x3c, 64u);
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, KIND));
  assert(finish_one(&alloc, 7u) == a);
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  a->hdr.live_cells = 0;  /* Deliberately wrong diagnostic hint. */
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, KIND));
  assert(alloc.needsweep[KIND] == a);
  assert(lj_arena_bm_get(a->block, cell));
  assert(*(uint64_t *)p == UINT64_C(0x3c3c3c3c3c3c3c3c));
  check_counts(&alloc);
  assert(lj_arena_alloc_restore_sweep_kind(&alloc, KIND));
  lj_arena_free(&alloc, p, 64u);
  assert(lj_arena_alloc_fini_try(&alloc));
}

static void test_held_reader(PRNGState *rs)
{
  TGAlloc alloc;
  GCArena *a = make_empty(&alloc, rs, NULL);
  assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_FULL);
  assert(!lj_arena_alloc_prepare_sweep_kind(&alloc, KIND));
  assert(alloc.needsweep[KIND] == a);
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  assert(lj_arena_remote_sweep_busy_acq(a));
  check_no_blocks(a);
  check_counts(&alloc);
  lj_arena_rescue_leave(a);
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, KIND));
  assert(finish_one(&alloc, 7u) == a);
  assert(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED);
  assert(lj_arena_alloc_fini_try(&alloc));
}

typedef struct Race {
  TGAlloc *alloc;
  PRNGState *rs;
  void *p;
  void *allocated;
  int result;
} Race;

typedef struct FinishRace {
  TGAlloc *alloc;
  GCArena *a;
  uint32_t reason;
  int result;
} FinishRace;

static void wait_seen(uint32_t (*seen)(void))
{
  uint32_t i;
  for (i = 0; i < 1000000u && !seen(); i++)
    sched_yield();
  assert(i < 1000000u);
}

static void *prepare_thread(void *arg)
{
  Race *r = (Race *)arg;
  r->result = lj_arena_alloc_prepare_sweep_kind(r->alloc, KIND);
  return NULL;
}

static void *late_thread(void *arg)
{
  Race *r = (Race *)arg;
  r->result = lj_arena_remote_free_publish(r->alloc, r->p, 64u);
  return NULL;
}

static void *adopt_thread(void *arg)
{
  Race *r = (Race *)arg;
  r->allocated = lj_arena_alloc(r->alloc, r->rs, 64u, LJ_AF_TRAVERSABLE);
  return NULL;
}

static void *finish_thread(void *arg)
{
  FinishRace *r = (FinishRace *)arg;
  assert(lj_arena_reclaim_seal(r->a));
  r->result = lj_arena_alloc_quarantine_finish(
    r->alloc, KIND, r->a, 7u, 1, &r->reason);
  return NULL;
}

static void test_late_after_shortcut_claim(PRNGState *rs)
{
  TGAlloc alloc;
  GCArena *a;
  void *old;
  Race owner, late;
  pthread_t owner_thread, publisher_thread;
  a = make_empty(&alloc, rs, &old);
  memset(&owner, 0, sizeof(owner));
  memset(&late, 0, sizeof(late));
  owner.alloc = late.alloc = &alloc;
  late.p = old;
  lj_arena_test_empty_reclaimed_pause(a, 1);
  assert(pthread_create(&owner_thread, NULL, prepare_thread, &owner) == 0);
  wait_seen(lj_arena_test_empty_reclaimed_paused);
  assert(lj_arena_remote_active_acq(a) == LJ_ARENA_REMOTE_SEALED);
  lj_arena_test_plain_late_pause(1);
  assert(pthread_create(&publisher_thread, NULL, late_thread, &late) == 0);
  wait_seen(lj_arena_test_plain_late_paused);
  assert(lj_arena_remote_active_acq(a) ==
         (LJ_ARENA_REMOTE_SEALED|LJ_ARENA_REMOTE_PENDING|1u));
  lj_arena_test_empty_reclaimed_pause(NULL, 0);
  assert(pthread_join(owner_thread, NULL) == 0 && owner.result);
  check_kept(&alloc, a);
  assert(lj_arena_remote_active_acq(a) ==
         (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_PENDING|1u));
  lj_arena_test_plain_late_pause(0);
  assert(pthread_join(publisher_thread, NULL) == 0 && late.result);
  /* Stale FREE/block0 rejects the bit/body, but the real producer's PENDING
  ** evidence must survive its leave and cannot be erased by the shortcut. */
  assert(lj_arena_remote_active_acq(a) ==
         (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_PENDING));
  check_no_blocks(a);
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, KIND));
  assert(alloc.needsweep[KIND] == a);
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  assert(finish_one(&alloc, 7u) == a);
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, KIND));
  check_kept(&alloc, a);
  assert(lj_arena_alloc_fini_try(&alloc));
}

static void test_failed_adoption(PRNGState *rs, int after_staging)
{
  TGAlloc alloc;
  GCArena *a = make_empty(&alloc, rs, NULL);
  Race r;
  pthread_t thread;
  memset(&r, 0, sizeof(r));
  r.alloc = &alloc;
  r.rs = rs;
  if (after_staging) {
    lj_arena_test_open_sealed_pause(a, 1);
    assert(pthread_create(&thread, NULL, adopt_thread, &r) == 0);
    wait_seen(lj_arena_test_open_sealed_paused);
    assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
    assert(lj_arena_alloc_owned_count_acq(&alloc, KIND) == 0);
    assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_COMMITTED);
    lj_arena_test_open_sealed_pause(NULL, 0);
    assert(pthread_join(thread, NULL) == 0);
  } else {
    assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_FULL);
    (void)adopt_thread(&r);
  }
  assert(r.allocated && lj_arena_of(r.allocated) != a);
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  assert(lj_arena_alloc_reclaimed_head(&alloc, KIND) == a);
  check_counts(&alloc);
  lj_arena_rescue_leave(a);
  lj_arena_free(&alloc, r.allocated, 64u);
  assert(lj_arena_alloc_terminal_reconcile(&alloc));
  assert(lj_arena_alloc_fini_try(&alloc));
}

static void test_reader_after_shortcut_claim(PRNGState *rs)
{
  TGAlloc alloc;
  GCArena *a;
  void *old, *held[8];
  Race owner;
  pthread_t thread;
  uint32_t n = 0, i;
  a = make_empty(&alloc, rs, &old);
  memset(&owner, 0, sizeof(owner));
  owner.alloc = &alloc;
  lj_arena_test_empty_reclaimed_pause(a, 1);
  assert(pthread_create(&thread, NULL, prepare_thread, &owner) == 0);
  wait_seen(lj_arena_test_empty_reclaimed_paused);
  assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_COMMITTED);
  assert(lj_arena_lifetime_state_acq(a, lj_arena_cellof(old)) ==
         LJ_ARENA_LIFETIME_FREE);
  assert(!lj_arena_bm_get(a->block, lj_arena_cellof(old)));
  lj_arena_test_empty_reclaimed_pause(NULL, 0);
  assert(pthread_join(thread, NULL) == 0 && owner.result);
  assert(lj_arena_remote_active_acq(a) == (LJ_ARENA_REMOTE_CLOSED|1u));
  check_kept(&alloc, a);
  held[n] = lj_arena_alloc(&alloc, rs, LJ_HUGE_THRESHOLD, LJ_AF_TRAVERSABLE);
  assert(held[n] && lj_arena_of(held[n++]) != a);
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  check_no_blocks(a);
  lj_arena_rescue_leave(a);
  /* Exhaust the independent fresh arena. The formerly admitted empty spare
  ** must become reusable after the reader leaves, without a new collection. */
  do {
    assert(n < 8u);
    held[n] = lj_arena_alloc(&alloc, rs, LJ_HUGE_THRESHOLD,
                            LJ_AF_TRAVERSABLE);
    assert(held[n]);
    n++;
  } while (lj_arena_of(held[n-1u]) != a);
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  check_counts(&alloc);
  for (i = 0; i < n; i++)
    lj_arena_free(&alloc, held[i], LJ_HUGE_THRESHOLD);
  assert(lj_arena_alloc_fini_try(&alloc));
}

static void test_late_recovery_owner(PRNGState *rs)
{
  TGAlloc alloc;
  GCArena *a;
  void *p;
  uint32_t cell, reason = LJ_ARENA_FINISH_NONE;
  lj_arena_alloc_init(&alloc);
  p = lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE);
  assert(p);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  /* The standalone fixture owns these real reciprocal lifetime/recovery
  ** lanes; no global reservation is needed for this private allocator. */
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
                                   LJ_ARENA_LIFETIME_RECOVERY));
  assert(lj_arena_recovery_state_cas(a, cell, LJ_ARENA_RECOVERY_IDLE,
                                   LJ_ARENA_RECOVERY_PENDING));
  assert(lj_arena_remote_free_publish(&alloc, p, 64u));
  assert(lj_arena_late_get(a, cell));
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, KIND));
  assert(lj_arena_alloc_quarantine_one(&alloc, KIND, 0) == a);
  assert(lj_arena_reclaim_seal(a));
  assert(!lj_arena_alloc_quarantine_finish(&alloc, KIND, a, 7u, 1, &reason));
  assert(reason == LJ_ARENA_FINISH_ACTIONABLE);
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  assert(lj_arena_bm_get(a->block, cell) && lj_arena_late_get(a, cell));
  lj_arena_reclaim_unseal(a, 1);
  assert(lj_arena_recovery_state_cas(a, cell, LJ_ARENA_RECOVERY_PENDING,
                                   LJ_ARENA_RECOVERY_IDLE));
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_RECOVERY,
                                   LJ_ARENA_LIFETIME_LIVE));
  assert(lj_arena_reclaim_seal(a));
  assert(lj_arena_alloc_quarantine_finish(&alloc, KIND, a, 7u, 1, &reason));
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  assert(lj_arena_bm_get(a->block, cell) && lj_arena_late_get(a, cell));
  /* The released reciprocal owner permits the next ordinary PREP to consume
  ** the durable late intent and only then certify an empty incarnation. */
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, KIND));
  assert(finish_one(&alloc, 7u) == a);
  assert(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED);
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, KIND));
  check_kept(&alloc, a);
  assert(lj_arena_alloc_fini_try(&alloc));
}

static void test_late_destruct_owner(PRNGState *rs)
{
  TGAlloc alloc;
  GCArena *a;
  void *p;
  uint32_t cell;
  lj_arena_alloc_init(&alloc);
  p = lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE);
  assert(p);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  /* Own a tentative DESTRUCT on a real block, then publish a durable free
  ** through the ordinary remote API. WHITE cannot transfer this ownership. */
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
                                   LJ_ARENA_LIFETIME_DESTRUCT));
  assert(lj_arena_remote_free_publish(&alloc, p, 64u));
  assert(lj_arena_late_get(a, cell));
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, KIND));
  assert(finish_one(&alloc, 7u) == a);
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  assert(lj_arena_bm_get(a->block, cell) && lj_arena_late_get(a, cell));
  assert(lj_arena_lifetime_state_acq(a, cell) ==
         LJ_ARENA_LIFETIME_DESTRUCT);
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_DESTRUCT,
                                   LJ_ARENA_LIFETIME_LIVE));
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, KIND));
  assert(finish_one(&alloc, 7u) == a);
  assert(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED);
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, KIND));
  check_kept(&alloc, a);
  assert(lj_arena_alloc_fini_try(&alloc));
}

static void test_descriptor_and_terminal_invalidation(PRNGState *rs)
{
  TGAlloc src, dst;
  GCArena *a = make_empty(&src, rs, NULL);
  LJGC2TableDesc desc;
  LJGC2TableDescTicket ticket;
  LJGC2TableDescSnap snap;
  lj_gc2_tabledesc_init_unpublished(&desc, 0);
  lj_arena_gc2_tabledesc_rel(a, &desc);
  /* The shared descriptor may change independently of the arena's cached
  ** proof. An exact mapping owner must force ordinary handling at use. */
  assert(lj_gc2_tabledesc_try_publish(&desc,
    lj_arena_cellptr(a, LJ_AFIRST_CELL), &ticket, &snap) ==
    LJ_GC2_TABLEDESC_RESULT_OK);
  assert(lj_arena_alloc_prepare_sweep_kind(&src, KIND));
  assert(src.needsweep[KIND] == a);
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  assert(lj_gc2_tabledesc_finish_help(&desc, &ticket, &snap) ==
    LJ_GC2_TABLEDESC_RESULT_OK);
  assert(finish_one(&src, 7u) == a);
  assert(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED);
  assert(lj_arena_terminal_reconcile(a));
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  assert(lj_arena_alloc_prepare_sweep_kind(&src, KIND));
  assert(finish_one(&src, 7u) == a);
  lj_arena_alloc_init(&dst);
  lj_arena_alloc_owner_rel(&dst, 7u);
  assert(lj_arena_alloc_transfer(&dst, &src) == 1u);
  assert(lj_arena_alloc_empty_reclaimed_head(&src) == NULL);
  assert(lj_arena_alloc_reclaimed_head(&dst, KIND) == a);
  assert(lj_arena_owner_acq(a) == 7u);
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  check_counts(&src);
  check_counts(&dst);
  assert(lj_arena_alloc_prepare_sweep_kind(&dst, KIND));
  assert(finish_one(&dst, 7u) == a);
  assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_FULL);
  assert(!lj_arena_alloc_fini_try(&dst));
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  assert(lj_arena_alloc_empty_reclaimed_head(&dst) == a);
  lj_arena_rescue_leave(a);
  assert(lj_arena_alloc_terminal_reconcile(&dst));
  assert(lj_arena_alloc_fini_try(&dst));
  assert(lj_arena_alloc_fini_try(&src));
}

typedef struct FullArena {
  GCArena *a;
  void *parts[4];
  size_t sizes[4];
  uint32_t n;
} FullArena;

static FullArena fill_raw_arena(TGAlloc *alloc, PRNGState *rs)
{
  FullArena full;
  size_t left = (LJ_ARENA_CELLS-LJ_AFIRST_CELL) * LJ_CELL_SIZE;
  memset(&full, 0, sizeof(full));
  while (left) {
    size_t size = left > LJ_HUGE_THRESHOLD ? LJ_HUGE_THRESHOLD : left;
    void *p = lj_arena_alloc(alloc, rs, size, LJ_AF_TRAVERSABLE);
    assert(p && full.n < 4u);
    if (!full.a)
      full.a = lj_arena_of(p);
    assert(lj_arena_of(p) == full.a);
    memset(p, 0x3c, size);
    full.parts[full.n] = p;
    full.sizes[full.n++] = size;
    left -= size;
  }
  assert(alloc->bump[KIND].cell == alloc->bump[KIND].end);
  return full;
}

static void cycle_all(TGAlloc *alloc)
{
  assert(lj_arena_alloc_prepare_sweep_kind(alloc, KIND));
  while (alloc->needsweep[KIND])
    assert(lj_arena_alloc_quarantine_one(alloc, KIND, 0));
  while (alloc->quarantine[KIND]) {
    GCArena *a = alloc->quarantine[KIND];
    uint32_t reason = LJ_ARENA_FINISH_NONE;
    assert(lj_arena_reclaim_seal(a));
    assert(lj_arena_alloc_quarantine_finish(alloc, KIND, a, 7u, 1, &reason));
    assert(reason == LJ_ARENA_FINISH_COMMITTED);
  }
  check_counts(alloc);
}

static void test_quarantine_producer_during_adoption(PRNGState *rs,
                                                    int lose_open)
{
  TGAlloc alloc;
  FullArena full[2];
  GCArena *a, *published;
  Race owner;
  FinishRace producer;
  pthread_t thread, producer_thread;
  uint32_t i, j, reason = LJ_ARENA_FINISH_NONE;
  lj_arena_alloc_init(&alloc);
  for (i = 0; i < 2u; i++)
    full[i] = fill_raw_arena(&alloc, rs);
  for (i = 0; i < 2u; i++)
    for (j = 0; j < full[i].n; j++)
      lj_arena_free(&alloc, full[i].parts[j], full[i].sizes[j]);
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, KIND));
  while (alloc.needsweep[KIND])
    assert(lj_arena_alloc_quarantine_one(&alloc, KIND, 0));
  a = alloc.quarantine[KIND];
  assert(a && lj_arena_reclaim_seal(a));
  assert(lj_arena_alloc_quarantine_finish(&alloc, KIND, a, 7u, 1, &reason));
  assert(lj_arena_alloc_empty_reclaimed_head(&alloc) == a);
  published = alloc.quarantine[KIND];
  assert(published && published != a);

  memset(&producer, 0, sizeof(producer));
  producer.alloc = &alloc;
  producer.a = published;
  lj_arena_test_reclaimed_publish_pause(published, 1);
  assert(pthread_create(&producer_thread, NULL, finish_thread, &producer) == 0);
  wait_seen(lj_arena_test_reclaimed_publish_paused);
  assert(lj_arena_alloc_empty_reclaimed_head(&alloc) == a);
  memset(&owner, 0, sizeof(owner));
  owner.alloc = &alloc;
  owner.rs = rs;
  lj_arena_test_open_sealed_pause(a, 1);
  assert(pthread_create(&thread, NULL, adopt_thread, &owner) == 0);
  wait_seen(lj_arena_test_open_sealed_paused);
  assert(lj_arena_alloc_empty_reclaimed_head(&alloc) == NULL);
  if (lose_open)
    assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_COMMITTED);
  lj_arena_test_open_sealed_pause(NULL, 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(owner.allocated);
  /* The producer's saved expected head has now been popped and relinked by
  ** the owner. Its CAS must retry without writing through that stale node. */
  assert(lj_arena_alloc_empty_reclaimed_head(&alloc) == NULL);
  lj_arena_test_reclaimed_publish_pause(NULL, 0);
  assert(pthread_join(producer_thread, NULL) == 0 && producer.result);
  assert(producer.reason == LJ_ARENA_FINISH_COMMITTED);
  assert(lj_arena_alloc_empty_reclaimed_head(&alloc) == published);
  assert(lj_arena_next_acq(published) == NULL);
  if (lose_open) {
    assert(lj_arena_of(owner.allocated) != a);
    assert(lj_arena_alloc_reclaimed_head(&alloc, KIND) == a);
    assert(lj_arena_next_acq(a) == NULL);
    assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
    lj_arena_rescue_leave(a);
  } else {
    assert(lj_arena_of(owner.allocated) == a);
    assert(lj_arena_alloc_reclaimed_head(&alloc, KIND) == NULL);
  }
  check_counts(&alloc);
  lj_arena_free(&alloc, owner.allocated, 64u);
  assert(lj_arena_alloc_terminal_reconcile(&alloc));
  assert(lj_arena_alloc_fini_try(&alloc));
}

static void test_register_empty_spare(PRNGState *rs)
{
  TGAlloc alloc;
  HugeTab registry = { 0 };
  LJHugeInfo info;
  GCArena *a = make_empty(&alloc, rs, NULL);
  assert(!(lj_arena_flags_acq(a) & LJ_AF_REGISTERED));
  assert(lj_arena_hugetab_init(&registry, 2));
  lj_arena_alloc_set_registry(&alloc, &registry);
  assert(lj_arena_alloc_register_existing(&alloc));
  assert(lj_arena_flags_acq(a) & LJ_AF_REGISTERED);
  assert(lj_arena_alloc_registry_lookup(&alloc, a, &info));
  assert(lj_arena_alloc_fini_try(&alloc));
  assert(!lj_arena_hugetab_lookup(&registry, a, &info));
  lj_arena_hugetab_fini(&registry);
}

static void test_plain_reclaimed_kind(PRNGState *rs)
{
  TGAlloc alloc;
  GCArena *a;
  void *p;
  uint32_t kind = LJ_ARENAK_PLAIN, reason = LJ_ARENA_FINISH_NONE;
  lj_arena_alloc_init(&alloc);
  p = lj_arena_alloc(&alloc, rs, 64u, 0);
  assert(p);
  a = lj_arena_of(p);
  lj_arena_free(&alloc, p, 64u);
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, kind));
  assert(lj_arena_alloc_quarantine_one(&alloc, kind, 0) == a);
  assert(lj_arena_reclaim_seal(a));
  assert(lj_arena_alloc_quarantine_finish(&alloc, kind, a, 7u, 1, &reason));
  assert(reason == LJ_ARENA_FINISH_COMMITTED);
  assert(!(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED));
  assert(lj_arena_alloc_empty_reclaimed_head(&alloc) == NULL);
  assert(lj_arena_alloc_reclaimed_head(&alloc, kind) == a);
  p = lj_arena_alloc(&alloc, rs, 64u, 0);
  assert(p && lj_arena_of(p) == a);
  lj_arena_free(&alloc, p, 64u);
  assert(lj_arena_alloc_fini_try(&alloc));
}

static void abandon_private_dtor(GCArena *a, uint32_t cell, uint32_t kind)
{
  uint32_t plane;
  assert(!lj_arena_bm_get(a->block, cell));
  for (plane = 0; plane < LJ_ARENA_DTOR_PLANES; plane++)
    if (kind & ((uint32_t)1u << plane))
      lj_arena_bm_clear(a->dtor[plane], cell);
  assert(lj_arena_lifetime_state_cas(a, cell,
    LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_FREE));
}

/* No list/flags/count injection: two real full retained arenas and two freed
** spares establish the geometry. Repeated ordinary cycles must not hide those
** spares behind unsuitable retained arenas and grow new mappings forever. */
static void test_retained_capacity(PRNGState *rs, int pair_reservation)
{
  TGAlloc alloc;
  FullArena full[4];
  uint32_t i, j, round;
  lj_arena_alloc_init(&alloc);
  for (i = 0; i < 4u; i++)
    full[i] = fill_raw_arena(&alloc, rs);
  for (i = 2u; i < 4u; i++)
    for (j = 0; j < full[i].n; j++)
      lj_arena_free(&alloc, full[i].parts[j], full[i].sizes[j]);
  cycle_all(&alloc);
  cycle_all(&alloc);  /* The old shortcut now puts full arenas above spares. */
  for (round = 0; round < 32u; round++) {
    GCArena *a;
    if (pair_reservation) {
      uint32_t cell;
      assert(lj_arena_reserve_bump_dtor_pair(&alloc, rs, LJ_AF_TRAVERSABLE,
        4u, 2u, LJ_ARENA_DTOR_LFUNC1, LJ_ARENA_DTOR_CLOSED_UV, &a, &cell));
      assert(a == full[2].a || a == full[3].a);
      /* The caller owns this unpublished pair. No semantic body was built. */
      abandon_private_dtor(a, cell, LJ_ARENA_DTOR_LFUNC1);
      abandon_private_dtor(a, cell + 2u, LJ_ARENA_DTOR_CLOSED_UV);
    } else {
      void *p = lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE);
      assert(p);
      a = lj_arena_of(p);
      assert(a == full[2].a || a == full[3].a);
      memset(p, 0xa5, 64u);
      lj_arena_free(&alloc, p, 64u);
    }
    cycle_all(&alloc);
    assert(count(lj_arena_alloc_reclaimed_head(&alloc, KIND)) +
           count(lj_arena_alloc_empty_reclaimed_head(&alloc)) == 4u);
    for (i = 0; i < 2u; i++)
      for (j = 0; j < full[i].n; j++) {
        const unsigned char *p = (const unsigned char *)full[i].parts[j];
        assert(lj_arena_bm_get(full[i].a->block, lj_arena_cellof(p)));
        assert(p[0] == 0x3c && p[full[i].sizes[j]-1u] == 0x3c);
      }
  }
  assert(lj_arena_alloc_fini_try(&alloc));
}

int main(int argc, char **argv)
{
  PRNGState rs;
  lj_prng_seed_fixed(&rs);
  if (argc == 2) {
    assert(strcmp(argv[1], "reuse-generic") == 0 ||
           strcmp(argv[1], "reuse-pair") == 0);
    test_retained_capacity(&rs, strcmp(argv[1], "reuse-pair") == 0);
    puts("arena empty reclaimed capacity test passed");
    return 0;
  }
  test_repeated_skip_and_black_reuse(&rs);
  test_live_block_and_hint(&rs);
  test_held_reader(&rs);
  test_late_after_shortcut_claim(&rs);
  test_reader_after_shortcut_claim(&rs);
  test_failed_adoption(&rs, 0);
  test_failed_adoption(&rs, 1);
  test_late_recovery_owner(&rs);
  test_late_destruct_owner(&rs);
  test_descriptor_and_terminal_invalidation(&rs);
  test_quarantine_producer_during_adoption(&rs, 0);
  test_quarantine_producer_during_adoption(&rs, 1);
  test_register_empty_spare(&rs);
  test_plain_reclaimed_kind(&rs);
  test_retained_capacity(&rs, 0);
  test_retained_capacity(&rs, 1);
  puts("arena empty reclaimed tests passed");
  return 0;
}
