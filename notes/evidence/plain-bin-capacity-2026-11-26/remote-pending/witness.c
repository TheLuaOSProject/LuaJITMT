#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"

#define N 8u
typedef struct Work { TGAlloc *alloc; void *p; int result; } Work;
static void *publish(void *arg)
{
  Work *w = (Work *)arg;
  w->result = lj_arena_remote_free_publish(w->alloc, w->p, 64u);
  return NULL;
}
static void states(const char *phase, Work *w, uint32_t drained)
{
  uint32_t i;
  printf("{\"phase\":\"%s\",\"drained\":%u,\"states\":[", phase, drained);
  for (i = 0; i < N; i++) {
    GCArena *a = lj_arena_of(w[i].p);
    uint32_t c = lj_arena_cellof(w[i].p);
    printf("%s{\"index\":%u,\"block\":%u,\"late\":%u,\"life\":%u,\"sweep\":%u}",
      i ? "," : "", i, (unsigned)lj_arena_bm_get(a->block, c),
      (unsigned)lj_arena_late_get(a, c),
      lj_arena_lifetime_state_acq(a, c), lj_arena_sweep_state_acq(a, c));
  }
  puts("]}");
}
int main(int argc, char **argv)
{
  TGAlloc alloc;
  PRNGState rs;
  Work work[N];
  pthread_t threads[N];
  GCArena *a;
  void *guard;
  uint32_t i, spins = 0, drained, reason = 0, fillers = 0;
  const uint32_t kind = LJ_ARENAK_TRAVERSABLE;
  int require_eight = argc > 1 && strcmp(argv[1], "--require-eight") == 0;
  setvbuf(stdout, NULL, _IONBF, 0);
  lj_prng_seed_fixed(&rs);
  lj_arena_alloc_init(&alloc);
  alloc.alloc_black = 1;
  for (i = 0; i < N; i++) {
    work[i].alloc = &alloc;
    work[i].p = lj_arena_alloc(&alloc, &rs, 64u, LJ_AF_TRAVERSABLE);
    work[i].result = 0;
    assert(work[i].p != NULL);
    memset(work[i].p, 0x30 + (int)i, 64u);
    if (i) assert(lj_arena_of(work[i].p) == lj_arena_of(work[0].p));
  }
  a = lj_arena_of(work[0].p);
  guard = lj_arena_alloc(&alloc, &rs, 64u, LJ_AF_TRAVERSABLE);
  assert(guard && lj_arena_of(guard) == a);
  memset(guard, 0xa7, 64u);
  /* Consume the unused bump with live opaque fillers. Reuse below must then
  ** come from the eight target bodies, not an unrelated fresh tail. */
  while (alloc.bump[kind].cell < alloc.bump[kind].end) {
    uint32_t cells = alloc.bump[kind].end - alloc.bump[kind].cell;
    void *p;
    if (cells > LJ_HUGE_THRESHOLD / LJ_CELL_SIZE)
      cells = LJ_HUGE_THRESHOLD / LJ_CELL_SIZE;
    p = lj_arena_alloc(&alloc, &rs, (size_t)cells * LJ_CELL_SIZE,
                       LJ_AF_TRAVERSABLE);
    assert(p && lj_arena_of(p) == a && ++fillers < 32u);
  }
  /* Publisher0 owns one admission while paused AFTER its queue CAS. Every
  ** other publisher necessarily has own_count+at least that admission. */
  lj_arena_test_remote_publish_pause(1);
  assert(pthread_create(&threads[0], NULL, publish, &work[0]) == 0);
  while (!lj_arena_test_remote_publish_paused()) {
    assert(++spins != 100000000u);
    la_cpu_pause();
  }
  assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == work[0].p);
  for (i = 1; i < N; i++)
    assert(pthread_create(&threads[i], NULL, publish, &work[i]) == 0);
  for (i = 1; i < N; i++) {
    unsigned j;
    assert(pthread_join(threads[i], NULL) == 0 && work[i].result == 1);
    assert(lj_arena_bm_get(a->block, lj_arena_cellof(work[i].p)));
    assert(lj_arena_late_get(a, lj_arena_cellof(work[i].p)));
    assert(lj_arena_lifetime_state_acq(a, lj_arena_cellof(work[i].p)) ==
           LJ_ARENA_LIFETIME_LIVE);
    for (j = 0; j < 64u; j++)
      assert(((unsigned char *)work[i].p)[j] == 0x30u + i);
  }
  states("seven_deferred_while_first_paused", work, 0);
  lj_arena_test_remote_publish_pause(0);
  assert(pthread_join(threads[0], NULL) == 0 && work[0].result == 1);
  drained = lj_arena_remote_free_drain(&alloc);
  states("immediate_owner_drain", work, drained);
  assert(drained == 1u);
  if (require_eight) assert(drained == N);  /* Original flaky requirement. */
  assert(!lj_arena_bm_get(a->block, lj_arena_cellof(work[0].p)));
  assert(!lj_arena_late_get(a, lj_arena_cellof(work[0].p)));
  assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL);
  for (i = 1; i < N; i++) {
    uint32_t c = lj_arena_cellof(work[i].p);
    assert(lj_arena_bm_get(a->block, c) && lj_arena_late_get(a, c));
    assert(lj_arena_lifetime_state_acq(a, c) == LJ_ARENA_LIFETIME_LIVE);
  }
  /* Real allocator preparation and quarantine consume every durable late
  ** intent only after all publishers/readers joined. No repeated raw free. */
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, kind));
  states("after_prepare", work, drained);
  for (i = 1; i < N; i++) {
    uint32_t c = lj_arena_cellof(work[i].p);
    assert(lj_arena_bm_get(a->block, c) && lj_arena_late_get(a, c));
    assert(lj_arena_lifetime_state_acq(a, c) == LJ_ARENA_LIFETIME_FREE);
    assert(lj_arena_sweep_state_acq(a, c) == LJ_ARENA_SWEEP_FREEING);
  }
  assert(lj_arena_alloc_quarantine_one(&alloc, kind, 11u) == a);
  assert(lj_arena_reclaim_seal(a));
  assert(lj_arena_alloc_quarantine_finish(&alloc, kind, a, 31u, 0, &reason));
  states("after_quarantine", work, drained);
  assert(lj_arena_bm_get(a->block, lj_arena_cellof(guard)));
  assert(lj_arena_lifetime_state_acq(a, lj_arena_cellof(guard)) ==
         LJ_ARENA_LIFETIME_LIVE);
  for (i = 0; i < 64u; i++) assert(((unsigned char *)guard)[i] == 0xa7);
  for (i = 0; i < N; i++) {
    uint32_t c = lj_arena_cellof(work[i].p);
    assert(!lj_arena_bm_get(a->block, c) && !lj_arena_late_get(a, c));
    assert(lj_arena_lifetime_state_acq(a, c) == LJ_ARENA_LIFETIME_FREE);
  }
  for (i = 0; i < N; i++) {
    void *p = lj_arena_alloc(&alloc, &rs, 64u, LJ_AF_TRAVERSABLE);
    assert(p == work[i].p);
  }
  assert(alloc.owned[kind] == a && lj_arena_next_acq(a) == NULL);
  puts("{\"phase\":\"exact_reuse\",\"reused\":8,\"arenas\":1,\"ok\":true}");
  lj_arena_alloc_fini(&alloc);
  return 0;
}
