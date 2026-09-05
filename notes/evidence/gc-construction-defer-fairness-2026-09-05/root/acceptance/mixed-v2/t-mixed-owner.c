/* Real attached publisher, held constructor, and independent eligible arena. */
#define main retained_fixture_main
#include "../v2/t-owner-defer.c"
#undef main
#include <pthread.h>
#include <stdlib.h>
#include "lj_safepoint.h"

typedef struct Peer {
  lua_State *L;
  TGState *tg;
  GCupval *uv[256];
  unsigned n;
  uint32_t stage;
} Peer;

static void nap(void)
{
  struct timespec ts = { 0, 1000000 };
  while (nanosleep(&ts, &ts) != 0) {}
}

static void wait_stage(Peer *p, uint32_t stage)
{
  unsigned i;
  for (i = 0; i < 1000 && la_load32_acq(&p->stage) != stage; i++)
    nap();
  assert(la_load32_acq(&p->stage) == stage);
}

static void *peer_main(void *arg)
{
  Peer *p = (Peer *)arg;
  TValue nil;
  unsigned i;
  assert(lj_threading_attach(p->L));
  p->tg = L2TG(p->L);
  setnilV(&nil);
  {
    GCupval *last = new_constructing(p->L, &nil);
    GCArena *first = lj_arena_of(last);
    /* Leave one real constructor at the last allocation of an exhausted
     * arena. Each previous constructor is fully published before continuing;
     * at most two private constructors coexist, and no GC plane is written. */
    for (i = 0; i < 2048; i++) {
      GCupval *next = new_constructing(p->L, &nil);
      if (lj_arena_of(next) != first) {
        lj_mem_freegco_unpublished(G(p->L), next, sizeof(GCupval));
        break;
      }
      assert(lj_gc_linkobj_new(G(p->L), obj2gco(last)) == LJ_GC_ROOT_LINKED);
      lj_gc_pubobjroot(p->L, obj2gco(last));
      last = next;
    }
    assert(i < 2048);
    p->uv[0] = last;
    printf("TAIL fillers=%u cell=%u cells=%u size=%zu\n",
           i, lj_arena_cellof(last), LJ_ARENA_CELLS, sizeof(GCupval));
    fflush(stdout);
  }
  lj_native_enter(p->tg);
  la_store32_rel(&p->stage, 1);
  wait_stage(p, 2);
  (void)lj_native_leave(p->L);
  for (i = 0; i < p->n; i++)
    lj_mem_freegco_unpublished(G(p->L), p->uv[i], sizeof(GCupval));
  lj_native_enter(p->tg);
  la_store32_rel(&p->stage, 3);
  wait_stage(p, 4);
  (void)lj_native_leave(p->L);
  lj_threading_detach(p->L, 1);
  return NULL;
}

int main(int argc, char **argv)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  Peer p;
  pthread_t thread;
  GCtab *live;
  GCArena *eligible, *blocked;
  uint32_t cycle, workers = argc > 1 ? (uint32_t)atoi(argv[1]) : 0;
  uint32_t old_epoch;
  uint64_t defer0, parks0, runs0, start, elapsed, events, parks, runs;
  int eligible_done, result;
  unsigned i;
  memset(&p, 0, sizeof(p));
  p.n = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
  assert(p.n == 1 && workers <= 2);
  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  p.L = lua_newthread(L);  /* Main stack retains this actual attached owner. */
  for (i = 0; i < 32; i++) {
    lua_newtable(L);
    lua_pop(L, 1);
  }
  lua_newtable(L);
  lua_pushinteger(L, 9237);
  lua_setfield(L, -2, "eligible_sentinel");
  live = tabV(L->top-1);
  eligible = lj_arena_of(live);
  old_epoch = la_load32_acq(&eligible->hdr.sweep_epoch);
  lj_native_enter(tg);
  assert(pthread_create(&thread, NULL, peer_main, &p) == 0);
  wait_stage(&p, 1);
  (void)lj_native_leave(L);
  assert(p.tg != tg);
  blocked = lj_arena_of(p.uv[0]);
  assert(blocked != eligible);
  for (i = 0; i < p.n; i++)
    held(lj_arena_of(p.uv[i]), lj_arena_cellof(p.uv[i]));
  state("peer-birth", g, p.tg, blocked, lj_arena_cellof(p.uv[0]));
  start = now_ns();
  result = lj_gc2_collect_active(L);
  elapsed = now_ns() - start;
  cycle = gc2_cycle_acq(g);
  state("peer-after-collect", g, p.tg, blocked, lj_arena_cellof(p.uv[0]));
  printf("MIXED initial result=%d ns=%" PRIu64 " cycle=%u old_epoch=%u"
         " eligible=%p epoch=%u flags=%u main_quarantine=%p main_needsweep=%p\n",
    result, elapsed, cycle, old_epoch, (void *)eligible,
    la_load32_acq(&eligible->hdr.sweep_epoch), lj_arena_flags_acq(eligible),
    (void *)tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE],
    (void *)tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE]);
  fflush(stdout);
  assert(result == 0 && elapsed < UINT64_C(1000000000));
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP && old_epoch != cycle);
  assert(blocked->hdr.reclaim_cell == LJ_AFIRST_CELL);
  /* The fair initial pass may already complete the independent arena. */
  if (workers)
    assert(lj_gc2_workers_set(g, workers) == 1);
  defer0 = gc2_deferred_epoch_acq(g);
  parks0 = gc2_worker_parks_acq(g);
  runs0 = gc2_sweep_owner_runs_acq(g);
  start = now_ns();
  if (workers) {
    lj_native_enter(tg);
    for (i = 0; i < 150; i++) nap();
  } else {
    for (i = 0; i < 64; i++)
      (void)lj_gc2_worker_drain(g, 64);
  }
  elapsed = now_ns() - start;
  events = gc2_deferred_epoch_acq(g) - defer0;
  parks = gc2_worker_parks_acq(g) - parks0;
  runs = gc2_sweep_owner_runs_acq(g) - runs0;
  eligible_done = la_load32_acq(&eligible->hdr.sweep_epoch) == cycle;
  for (i = 0; i < p.n; i++)
    held(lj_arena_of(p.uv[i]), lj_arena_cellof(p.uv[i]));
  printf("MIXED held workers=%u objects=%u ns=%" PRIu64
         " events=%" PRIu64 " parks=%" PRIu64 " runs=%" PRIu64
         " eligible_done=%d epoch=%u cycle=%u phase=%u\n",
    workers, p.n, elapsed, events, parks, runs, eligible_done,
    la_load32_acq(&eligible->hdr.sweep_epoch), cycle, gc2_phase_acq(g));
  fflush(stdout);
  assert(elapsed < UINT64_C(1000000000));
  if (workers) {
    assert(events > 0 && parks > 0);
    assert(events < elapsed / UINT64_C(250000) + 32u);
    (void)lj_native_leave(L);
    assert(lj_gc2_workers_set(g, 0) == 1);
  }
  state("peer-before-release", g, p.tg, blocked, lj_arena_cellof(p.uv[0]));
  lj_native_enter(tg);
  la_store32_rel(&p.stage, 2);
  wait_stage(&p, 3);
  (void)lj_native_leave(L);
  result = lj_gc2_collect_active(L);
  printf("MIXED released result=%d phase=%u peer_quarantine=%p eligible_epoch=%u\n",
    result, gc2_phase_acq(g),
    (void *)p.tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE],
    la_load32_acq(&eligible->hdr.sweep_epoch));
  fflush(stdout);
  assert(result == 1 && gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(p.tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE] == NULL);
  lua_getfield(L, -1, "eligible_sentinel");
  assert(lua_tointeger(L, -1) == 9237);
  lua_pop(L, 1);
  lj_native_enter(tg);
  la_store32_rel(&p.stage, 4);
  assert(pthread_join(thread, NULL) == 0);
  (void)lj_native_leave(L);
  lua_settop(L, 0);
  assert(lj_gc2_collect_active(L) == 1);
  lua_close(L);
  assert(eligible_done);  /* Releasing the blocker must not be its precondition. */
  puts("t-mixed-owner OK");
  return 0;
}
