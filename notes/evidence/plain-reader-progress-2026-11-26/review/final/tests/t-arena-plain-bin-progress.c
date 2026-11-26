/* A paused plain writer must not hide another arena's eligible bin run. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"

#if !defined(LJ_ARENA_TEST_HELPERS)
#error "t-arena-plain-bin-progress requires LJ_ARENA_TEST_HELPERS"
#endif

#define RUN_BYTES 64u

typedef struct Publish {
  TGAlloc *alloc;
  void *p;
  int result;
} Publish;

static void *publish_free(void *arg)
{
  Publish *work = (Publish *)arg;
  work->result = lj_arena_remote_free_publish(work->alloc, work->p, RUN_BYTES);
  return NULL;
}

static void pause_at(int committed, int enabled)
{
  if (committed) lj_arena_test_remote_publish_pause(enabled);
  else lj_arena_test_plain_claim_pause(enabled);
}

static int paused_at(int committed)
{
  return committed ? lj_arena_test_remote_publish_paused() :
                     lj_arena_test_plain_claim_paused();
}

/* Consume the real bump extent, without changing allocator cursors or making
** another free run. This leaves only the two deliberately freed bin entries
** eligible for the later allocation. The arena fixture owns all filler bodies
** until lj_arena_alloc_fini(), so no Lua accounting or GC roots are involved. */
static void exhaust_bump(TGAlloc *alloc, PRNGState *rs, GCArena *a)
{
  LJArenaBump *b = &alloc->bump[LJ_ARENAK_PLAIN];
  assert(b->a == a);
  while (b->cell < b->end) {
    size_t bytes = (size_t)(b->end - b->cell) << LJ_CELL_SHIFT;
    void *p;
    if (bytes > LJ_HUGE_THRESHOLD) bytes = LJ_HUGE_THRESHOLD;
    p = lj_arena_alloc(alloc, rs, bytes, 0);
    assert(p && lj_arena_of(p) == a && b->a == a);
  }
}

static int check_bin_progress(PRNGState *rs, int committed)
{
  TGAlloc alloc;
  GCArena *a, *b;
  void *target, *arun, *brun, *reuse;
  LJArenaFreeRun *head;
  Publish work;
  pthread_t writer;
  uint32_t bin, owned_before, owned_during;
  int reused;

  lj_arena_alloc_init(&alloc);
  target = lj_arena_alloc(&alloc, rs, RUN_BYTES, 0);
  arun = lj_arena_alloc(&alloc, rs, RUN_BYTES, 0);
  assert(target && arun && lj_arena_of(target) == lj_arena_of(arun));
  a = lj_arena_of(target);
  memset(target, 0x53, RUN_BYTES);
  exhaust_bump(&alloc, rs, a);
  brun = lj_arena_alloc(&alloc, rs, RUN_BYTES, 0);
  assert(brun && lj_arena_of(brun) != a);
  b = lj_arena_of(brun);
  exhaust_bump(&alloc, rs, b);

  /* Normal owner frees build A -> B in the same bin. The foreign writer only
  ** touches target in A; neither free node is its mutation target. */
  lj_arena_free(&alloc, brun, RUN_BYTES);
  lj_arena_free(&alloc, arun, RUN_BYTES);
  for (bin = 0; bin < LJ_ALLOC_NBINS; bin++)
    if (alloc.bins[LJ_ARENAK_PLAIN][bin] == arun) break;
  assert(bin < LJ_ALLOC_NBINS);
  head = alloc.bins[LJ_ARENAK_PLAIN][bin];
  assert(head->len == lj_arena_ncells(RUN_BYTES));
  assert(head->next == brun && head->next->next == NULL);
  assert(head->next->len == head->len);
  owned_before = lj_arena_alloc_owned_count_acq(&alloc, LJ_ARENAK_PLAIN);
  assert(owned_before == 2u);

  work.alloc = &alloc; work.p = target; work.result = 0;
  pause_at(committed, 1);
  assert(pthread_create(&writer, NULL, publish_free, &work) == 0);
  while (!paused_at(committed)) sched_yield();
  assert(lj_arena_remote_active_acq(a) & LJ_ARENA_REMOTE_SEALED);

  reuse = lj_arena_alloc(&alloc, rs, RUN_BYTES, 0);
  owned_during = lj_arena_alloc_owned_count_acq(&alloc, LJ_ARENAK_PLAIN);
  reused = reuse == brun && owned_during == owned_before;
  assert(paused_at(committed));
  if (reused) {
    assert(lj_arena_bm_get(b->block, lj_arena_cellof(brun)));
    assert(!lj_arena_bm_get(a->block, lj_arena_cellof(arun)));
    memset(reuse, 0xa7, RUN_BYTES);
  }

  /* Resume even on a failed progress check so the baseline failure reports
  ** the unwanted mapping rather than leaving a paused producer behind. */
  pause_at(committed, 0);
  assert(pthread_join(writer, NULL) == 0 && work.result == 1);
  if (!reused)
    fprintf(stderr, "plain bin progress failed: committed=%d reused_B=%d "
            "owned_before=%u owned_during=%u\n", committed, reuse == brun,
            owned_before, owned_during);
  assert(lj_arena_remote_active_acq(a) == 0);
  assert(lj_arena_remote_free_drain(&alloc) == 1u);
  assert(!lj_arena_bm_get(a->block, lj_arena_cellof(target)));
  if (reused)
    assert(((unsigned char *)reuse)[0] == 0xa7 &&
           ((unsigned char *)reuse)[RUN_BYTES - 1u] == 0xa7);
  lj_arena_free(&alloc, reuse, RUN_BYTES);
  lj_arena_alloc_fini(&alloc);
  return reused;
}

int main(void)
{
  PRNGState rs;
  int tentative, committed;
  alarm(60);
  lj_prng_seed_fixed(&rs);
  tentative = check_bin_progress(&rs, 0);
  committed = check_bin_progress(&rs, 1);
  assert(tentative && committed);
  alarm(0);
  puts("t-arena-plain-bin-progress OK: tentative and committed writers");
  return 0;
}
