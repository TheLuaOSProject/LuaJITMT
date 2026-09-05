/* Owner-published allocator diagnostics: exact settled counts, no remote walk. */
#ifndef LJ_ARENA_TEST_HELPERS
#error "t-gc2-stats-arenas requires LJ_ARENA_TEST_HELPERS"
#endif

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_prng.h"
#include "lj_tg.h"
#include "lj_dispatch.h"

static uint32_t owner_count(GCArena *a)
{
  uint32_t n = 0;
  for (; a; a = lj_arena_next_acq(a))
    assert(++n < 64u);
  return n;
}

static void check_allocator(TGAlloc *alloc)
{
  uint32_t k, b;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    uint32_t mask = 0;
    assert(lj_arena_alloc_owned_count_acq(alloc, k) ==
           owner_count(alloc->owned[k]));
    assert(lj_arena_alloc_needsweep_count_acq(alloc, k) ==
           owner_count(alloc->needsweep[k]));
    for (b = 0; b < LJ_ALLOC_NBINS; b++)
      if (alloc->bins[k][b])
        mask |= (uint32_t)1u << b;
    assert(lj_arena_alloc_binmask_acq(alloc, k) == mask);
  }
}

static void fill_arenas(TGAlloc *alloc, PRNGState *rs, uint32_t k,
                        uint32_t count)
{
  uint32_t n;
  uint32_t flags = k == LJ_ARENAK_TRAVERSABLE ? LJ_AF_TRAVERSABLE : 0;
  lj_arena_alloc_black_rel(alloc, 1);
  for (n = 0; owner_count(alloc->owned[k]) < count; n++) {
    assert(n < 4096u);
    assert(lj_arena_alloc(alloc, rs, LJ_HUGE_THRESHOLD, flags));
  }
  check_allocator(alloc);
}

static GCArena *finish_one(TGAlloc *alloc, uint32_t k)
{
  uint32_t reason = LJ_ARENA_FINISH_NONE;
  GCArena *a = lj_arena_alloc_quarantine_one(alloc, k, 0);
  assert(a && lj_arena_reclaim_seal(a));
  assert(lj_arena_alloc_quarantine_finish(alloc, k, a, 1u, 1, &reason));
  assert(reason == LJ_ARENA_FINISH_COMMITTED);
  check_allocator(alloc);
  return a;
}

static void test_transitions(void)
{
  TGAlloc src, dst;
  PRNGState rs;
  GCArena *held;
  uint32_t k, moved = 0;
  lj_prng_seed_fixed(&rs);
  lj_arena_alloc_init(&src);
  lj_arena_alloc_init(&dst);
  check_allocator(&src);
  fill_arenas(&src, &rs, LJ_ARENAK_TRAVERSABLE, 3u);
  fill_arenas(&src, &rs, LJ_ARENAK_PLAIN, 2u);

  held = src.owned[LJ_ARENAK_TRAVERSABLE];
  assert(lj_arena_rescue_enter(held) == LJ_ARENA_RESCUE_FULL);
  assert(!lj_arena_alloc_prepare_sweep_kind(&src, LJ_ARENAK_TRAVERSABLE));
  check_allocator(&src);
  assert(lj_arena_alloc_owned_count_acq(&src, LJ_ARENAK_TRAVERSABLE) == 0);
  assert(lj_arena_alloc_needsweep_count_acq(&src, LJ_ARENAK_TRAVERSABLE) == 3);
  lj_arena_rescue_leave(held);
  assert(lj_arena_alloc_prepare_sweep_kind(&src, LJ_ARENAK_TRAVERSABLE));
  assert(lj_arena_alloc_restore_sweep_kind(&src, LJ_ARENAK_TRAVERSABLE));
  check_allocator(&src);

  assert(lj_arena_alloc_prepare_sweep_kind(&src, LJ_ARENAK_PLAIN));
  while (lj_arena_sweep_one(&src, LJ_ARENAK_PLAIN, 1u, 1))
    check_allocator(&src);
  check_allocator(&src);

  assert(lj_arena_alloc_prepare_sweep_kind(&src, LJ_ARENAK_TRAVERSABLE));
  (void)finish_one(&src, LJ_ARENAK_TRAVERSABLE);
  assert(lj_arena_alloc(&src, &rs, 32u, LJ_AF_TRAVERSABLE));
  assert(lj_arena_alloc_owned_count_acq(&src, LJ_ARENAK_TRAVERSABLE) == 1);
  check_allocator(&src);  /* Successful reclaimed adoption. */
  (void)finish_one(&src, LJ_ARENAK_TRAVERSABLE);
  assert(lj_arena_alloc_quarantine_one(&src, LJ_ARENAK_TRAVERSABLE, 0));
  check_allocator(&src);

  /* Carry every populated list class while the destination owns arenas. */
  assert(lj_arena_alloc_prepare_sweep_kind(&src, LJ_ARENAK_PLAIN));
  assert(owner_count(src.owned[LJ_ARENAK_TRAVERSABLE]) == 1u);
  assert(owner_count(src.needsweep[LJ_ARENAK_PLAIN]) == 2u);
  assert(owner_count(src.quarantine[LJ_ARENAK_TRAVERSABLE]) == 1u);
  assert(owner_count(lj_arena_alloc_reclaimed_head(
    &src, LJ_ARENAK_TRAVERSABLE)) +
    owner_count(lj_arena_alloc_empty_reclaimed_head(&src)) == 1u);
  fill_arenas(&dst, &rs, LJ_ARENAK_TRAVERSABLE, 1u);
  fill_arenas(&dst, &rs, LJ_ARENAK_PLAIN, 1u);
  assert(lj_arena_alloc_prepare_sweep_kind(&dst, LJ_ARENAK_PLAIN));
  fill_arenas(&dst, &rs, LJ_ARENAK_PLAIN, 1u);
  for (k = 0; k < LJ_ARENA_NKINDS; k++)
    moved += owner_count(src.owned[k]) + owner_count(src.needsweep[k]) +
      owner_count(src.quarantine[k]) +
      owner_count(lj_arena_alloc_reclaimed_head(&src, k));
  moved += owner_count(lj_arena_alloc_empty_reclaimed_head(&src));
  assert(lj_arena_alloc_transfer(&dst, &src) == moved);
  check_allocator(&src);
  check_allocator(&dst);
  assert(lj_arena_alloc_restore_sweep_kind(&dst, LJ_ARENAK_PLAIN));
  check_allocator(&dst);
  assert(lj_arena_alloc_fini_try(&src));
  assert(lj_arena_alloc_fini_try(&dst));
  check_allocator(&src);
  check_allocator(&dst);
}

static void test_partial_fini(void)
{
  TGAlloc alloc;
  PRNGState rs;
  GCArena *owned, *needsweep;
  lj_prng_seed_fixed(&rs);
  lj_arena_alloc_init(&alloc);
  fill_arenas(&alloc, &rs, LJ_ARENAK_TRAVERSABLE, 3u);
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, LJ_ARENAK_TRAVERSABLE));
  fill_arenas(&alloc, &rs, LJ_ARENAK_TRAVERSABLE, 2u);
  owned = alloc.owned[LJ_ARENAK_TRAVERSABLE];
  needsweep = alloc.needsweep[LJ_ARENAK_TRAVERSABLE];
  assert(lj_arena_rescue_enter(owned) != LJ_ARENA_RESCUE_RETRY);
  assert(lj_arena_rescue_enter(needsweep) != LJ_ARENA_RESCUE_RETRY);
  assert(!lj_arena_alloc_fini_try(&alloc));
  check_allocator(&alloc);
  assert(lj_arena_alloc_owned_count_acq(&alloc, LJ_ARENAK_TRAVERSABLE) == 1);
  assert(lj_arena_alloc_needsweep_count_acq(&alloc, LJ_ARENAK_TRAVERSABLE) == 1);
  lj_arena_rescue_leave(owned);
  lj_arena_rescue_leave(needsweep);
  assert(lj_arena_alloc_terminal_reconcile(&alloc));
  assert(lj_arena_alloc_fini_try(&alloc));
  check_allocator(&alloc);
}

typedef struct OpenRace {
  TGAlloc alloc;
  PRNGState rs;
  int adoption;
  int restored;
  void *allocated;
} OpenRace;

static void *run_open_race(void *arg)
{
  OpenRace *r = (OpenRace *)arg;
  if (r->adoption)
    r->allocated = lj_arena_alloc(&r->alloc, &r->rs, 32u, LJ_AF_TRAVERSABLE);
  else
    r->restored = lj_arena_alloc_restore_sweep_kind(
      &r->alloc, LJ_ARENAK_TRAVERSABLE);
  return NULL;
}

static void test_failed_open(int adoption)
{
  OpenRace r;
  GCArena *a;
  pthread_t owner;
  uint32_t i;
  memset(&r, 0, sizeof(r));
  r.adoption = adoption;
  lj_prng_seed_fixed(&r.rs);
  lj_arena_alloc_init(&r.alloc);
  fill_arenas(&r.alloc, &r.rs, LJ_ARENAK_TRAVERSABLE, 1u);
  assert(lj_arena_alloc_prepare_sweep_kind(&r.alloc, LJ_ARENAK_TRAVERSABLE));
  a = adoption ? finish_one(&r.alloc, LJ_ARENAK_TRAVERSABLE) :
    r.alloc.needsweep[LJ_ARENAK_TRAVERSABLE];
  lj_arena_test_open_sealed_pause(a, 1);
  assert(pthread_create(&owner, NULL, run_open_race, &r) == 0);
  for (i = 0; i < 1000000u && !lj_arena_test_open_sealed_paused(); i++)
    sched_yield();
  assert(i < 1000000u);
  /* Staged owner heads already contain a, but no successful OPEN has occurred.
  ** The last committed scalar remains available while that owner is paused. */
  assert(lj_arena_alloc_owned_count_acq(&r.alloc, LJ_ARENAK_TRAVERSABLE) == 0);
  assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_COMMITTED);
  lj_arena_test_open_sealed_pause(NULL, 0);
  assert(pthread_join(owner, NULL) == 0);
  if (adoption) {
    assert(r.allocated && lj_arena_of(r.allocated) != a);
    assert(lj_arena_alloc_owned_count_acq(&r.alloc, LJ_ARENAK_TRAVERSABLE) == 1);
  } else {
    assert(!r.restored);
    assert(lj_arena_alloc_owned_count_acq(&r.alloc, LJ_ARENAK_TRAVERSABLE) == 0);
    assert(lj_arena_alloc_needsweep_count_acq(&r.alloc, LJ_ARENAK_TRAVERSABLE) == 1);
  }
  check_allocator(&r.alloc);
  lj_arena_rescue_leave(a);
  assert(lj_arena_alloc_prepare_sweep_kind(&r.alloc, LJ_ARENAK_TRAVERSABLE));
  assert(lj_arena_alloc_restore_sweep_kind(&r.alloc, LJ_ARENAK_TRAVERSABLE));
  check_allocator(&r.alloc);
  assert(lj_arena_alloc_fini_try(&r.alloc));
}

typedef struct Reader {
  global_State *g;
  uint32_t ready;
  uint32_t done;
  uint32_t calls;
  uint32_t expected_owned;
  uint32_t expected_needsweep;
  uint32_t expected_binmask;
  int exact;
} Reader;

static void *snapshot_reader(void *arg)
{
  Reader *r = (Reader *)arg;
  uint32_t n = 0;
  la_store32_rel(&r->ready, 1);
  do {
    GC2StatsSnapshot s;
    lj_gc2_stats_snapshot(r->g, &s);
    if (r->exact) {
      assert(s.arena_traversable_owned == r->expected_owned);
      assert(s.arena_traversable_needsweep == r->expected_needsweep);
      assert(s.arena_traversable_binmask == r->expected_binmask);
    } else {
      assert(s.arena_traversable_owned <= 3u);
      assert(s.arena_traversable_needsweep <= 3u);
    }
    n++;
  } while (!la_load32_acq(&r->done));
  r->calls = n;
  return NULL;
}

static void test_remote_snapshot(int protect)
{
  /* A root-empty synthetic universe isolates the production stats reader from
  ** unrelated Lua graph traversal. The embedded main allocator is real and
  ** uses the same allocation/prepare/restore operations as runtime owners. */
  GG_State *GG = (GG_State *)calloc(1, sizeof(*GG));
  TGAlloc *alloc;
  PRNGState rs;
  Reader r;
  pthread_t reader;
  GCArena *arena[8];
  uint32_t n = 0, i;
  assert(GG);
  GG->g.main_tg = &GG->main_tg;
  alloc = &GG->main_tg.alloc;
  lj_arena_alloc_init(alloc);
  lj_prng_seed_fixed(&rs);
  fill_arenas(alloc, &rs, LJ_ARENAK_TRAVERSABLE, 3u);
  memset(&r, 0, sizeof(r));
  r.g = &GG->g;
  r.exact = protect;
  if (protect) {
    GCArena *a;
    assert(lj_arena_alloc_prepare_sweep_kind(alloc, LJ_ARENAK_TRAVERSABLE));
    fill_arenas(alloc, &rs, LJ_ARENAK_TRAVERSABLE, 2u);
    r.expected_owned = owner_count(alloc->owned[LJ_ARENAK_TRAVERSABLE]);
    r.expected_needsweep = owner_count(alloc->needsweep[LJ_ARENAK_TRAVERSABLE]);
    r.expected_binmask = alloc->binmask[LJ_ARENAK_TRAVERSABLE];
    for (a = alloc->owned[LJ_ARENAK_TRAVERSABLE]; a; a = lj_arena_next_acq(a))
      arena[n++] = a;
    for (a = alloc->needsweep[LJ_ARENAK_TRAVERSABLE]; a; a = lj_arena_next_acq(a))
      arena[n++] = a;
    assert(n == 5u);
    for (i = 0; i < n; i++)
      assert(mprotect(arena[i], LJ_ARENA_SIZE, PROT_NONE) == 0);
    la_store32_rel(&r.done, 1);
  }
  assert(pthread_create(&reader, NULL, snapshot_reader, &r) == 0);
  if (!protect) {
    while (!la_load32_acq(&r.ready))
      sched_yield();
    for (i = 0; i < 128u; i++) {
      assert(lj_arena_alloc_prepare_sweep_kind(alloc, LJ_ARENAK_TRAVERSABLE));
      assert(lj_arena_alloc_restore_sweep_kind(alloc, LJ_ARENAK_TRAVERSABLE));
      check_allocator(alloc);
    }
    la_store32_rel(&r.done, 1);
  }
  assert(pthread_join(reader, NULL) == 0);
  assert(r.calls != 0);
  for (i = 0; i < n; i++)
    assert(mprotect(arena[i], LJ_ARENA_SIZE, PROT_READ|PROT_WRITE) == 0);
  check_allocator(alloc);
  /* Synthetic count injection exercises the legacy independent caps without
  ** allocating a million arenas. No list-changing operation follows except
  ** terminal fini, whose existing walk recounts its retained arenas. */
  la_store32_rel(&alloc->owned_count[LJ_ARENAK_TRAVERSABLE],
                 LJ_GC2_ROOT_SCAN_LIMIT + 1u);
  la_store32_rel(&alloc->needsweep_count[LJ_ARENAK_TRAVERSABLE],
                 LJ_GC2_ROOT_SCAN_LIMIT + 1u);
  {
    GC2StatsSnapshot s;
    lj_gc2_stats_snapshot(&GG->g, &s);
    assert(s.arena_traversable_owned == LJ_GC2_ROOT_SCAN_LIMIT);
    assert(s.arena_traversable_needsweep == LJ_GC2_ROOT_SCAN_LIMIT);
  }
  /* Fini computes its surviving counts in the existing unmap pass. */
  assert(lj_arena_alloc_fini_try(alloc));
  free(GG);
}

static void test_bootstrap(void)
{
  lua_State *L = luaL_newstate();
  GC2StatsSnapshot s;
  assert(L);
  lua_gc(L, LUA_GCSTOP, 0);
  check_allocator(&G(L)->main_tg->alloc);
  lj_gc2_stats_snapshot(G(L), &s);
  assert(s.arena_traversable_owned ==
         owner_count(G(L)->main_tg->alloc.owned[LJ_ARENAK_TRAVERSABLE]));
  assert(s.arena_traversable_needsweep ==
         owner_count(G(L)->main_tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE]));
  lua_close(L);
}

int main(int argc, char **argv)
{
  if (argc == 2 && strcmp(argv[1], "protected") == 0) {
    test_remote_snapshot(1);
  } else if (argc == 2 && strcmp(argv[1], "concurrent") == 0) {
    test_remote_snapshot(0);
  } else {
    test_transitions();
    test_partial_fini();
    test_failed_open(0);
    test_failed_open(1);
    test_remote_snapshot(1);
    test_remote_snapshot(0);
    test_bootstrap();
  }
  puts("t-gc2-stats-arenas OK: scalar snapshots and allocator transitions");
  return 0;
}
