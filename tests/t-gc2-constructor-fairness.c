/* Multiple real tail constructors; observe quota-one TG continuation. */
#define main retained_fixture_main
#include "t-gc2-constructor-defer.c"
#undef main
#include <pthread.h>
#include <stdlib.h>
#include "lj_safepoint.h"

typedef struct Peer {
  lua_State *L;
  TGState *tg;
  GCupval *uv;
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
    p->uv = last;
    printf("TAIL tid=%u fillers=%u cell=%u cells=%u size=%zu\n",
      lj_tg_tid_acq(p->tg), i, lj_arena_cellof(last),
      LJ_ARENA_CELLS, sizeof(GCupval));
    fflush(stdout);
  }
  lj_native_enter(p->tg);
  la_store32_rel(&p->stage, 1);
  wait_stage(p, 2);
  (void)lj_native_leave(p->L);
  lj_mem_freegco_unpublished(G(p->L), p->uv, sizeof(GCupval));
  lj_native_enter(p->tg);
  la_store32_rel(&p->stage, 3);
  wait_stage(p, 4);
  (void)lj_native_leave(p->L);
  lj_threading_detach(p->L, 1);
  return NULL;
}

static int current_tid_present(global_State *g, uint32_t tid)
{
  TGState *tg;
  unsigned visits = 0;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    assert(++visits < 32);
    if (lj_tg_tid_acq(tg) == tid)
      return 1;
    if (lj_tg_next_acq(tg) == tg)
      break;
  }
  return 0;
}

int main(int argc, char **argv)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  Peer p[3];
  pthread_t threads[3];
  GCArena *blocked[3], *eligible;
  uint32_t tids[3], cycles, old_epoch, main_tid = lj_tg_tid_acq(tg);
  uint32_t workers = argc > 1 ? (uint32_t)atoi(argv[1]) : 0;
  unsigned count = argc > 2 ? (unsigned)atoi(argv[2]) : 3;
  uint32_t quota = argc > 3 ? (uint32_t)atoi(argv[3]) : 1;
  int detach_hint = argc > 4 && strcmp(argv[4], "detach") == 0;
  uint64_t epoch0, parks0, runs0, start, elapsed, events, parks;
  unsigned i, j, owner_calls = 0, eligible_turns = 0, first_turn = 0;
  unsigned last_turn = 0, max_gap = 0, reset[3] = { 0, 0, 0 };
  unsigned first_done_call = 0, first_done_turn = 0;
  int result, eligible_done, fairness_ok = 1;
  uint32_t saved_hint = 0;
  memset(p, 0, sizeof(p));
  assert(count >= 1 && count <= 3 && workers <= 2);
  assert(quota == 1 || quota == 64);
  assert(!detach_hint || workers == 0);
  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  for (i = 0; i < count; i++) p[i].L = lua_newthread(L);
  for (i = 0; i < 32; i++) { lua_newtable(L); lua_pop(L, 1); }
  lua_newtable(L);
  lua_pushinteger(L, 9237);
  lua_setfield(L, -2, "eligible_sentinel");
  eligible = lj_arena_of(tabV(L->top-1));
  old_epoch = la_load32_acq(&eligible->hdr.sweep_epoch);
  lj_native_enter(tg);
  /* Sequential admission fixes the initial list order, while every held
   * constructor is owned by an actual live, native-parked peer. */
  for (i = 0; i < count; i++) {
    assert(pthread_create(&threads[i], NULL, peer_main, &p[i]) == 0);
    wait_stage(&p[i], 1);
    tids[i] = lj_tg_tid_acq(p[i].tg);
    blocked[i] = lj_arena_of(p[i].uv);
    assert(blocked[i] != eligible);
  }
  (void)lj_native_leave(L);
  start = now_ns();
  result = lj_gc2_collect_active(L);
  elapsed = now_ns() - start;
  cycles = gc2_cycle_acq(g);
  printf("INITIAL result=%d ns=%" PRIu64 " count=%u quota=%u workers=%u"
    " cycle=%u main_tid=%u hint=%u eligible_epoch=%u cursor=%u\n",
    result, elapsed, count, quota, workers, cycles, main_tid,
    gc2_sweep_owner_next_tid_acq(g),
    la_load32_acq(&eligible->hdr.sweep_epoch), eligible->hdr.reclaim_cell);
  assert(result == 0 && elapsed < UINT64_C(1000000000));
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP && old_epoch != cycles);
  for (i = 0; i < count; i++) {
    held(blocked[i], lj_arena_cellof(p[i].uv));
    state("initial-blocker", g, p[i].tg, blocked[i], lj_arena_cellof(p[i].uv));
  }
  if (workers) assert(lj_gc2_workers_set(g, workers) == 1);
  epoch0 = gc2_deferred_epoch_acq(g);
  parks0 = gc2_worker_parks_acq(g);
  runs0 = gc2_sweep_owner_runs_acq(g);
  start = now_ns();
  if (workers) {
    lj_native_enter(tg);
    for (i = 0; i < 150; i++) nap();
  } else {
    /* This new quota-one case has a fixed 256-call observation window.
     * It does not replace the unchanged 64-call quota64 mixed fixture. */
    for (i = 0; i < 256; i++) {
      uint32_t hint0 = gc2_sweep_owner_next_tid_acq(g);
      uint32_t cursor0 = eligible->hdr.reclaim_cell;
      uint32_t sweep0 = la_load32_acq(&eligible->hdr.sweep_epoch);
      uint32_t cursors[3], hint1, work;
      uint64_t run0 = gc2_sweep_owner_runs_acq(g), delta;
      uint64_t event0 = gc2_deferred_epoch_acq(g);
      for (j = 0; j < count; j++) cursors[j] = blocked[j]->hdr.reclaim_cell;
      work = lj_gc2_worker_drain(g, quota);
      delta = gc2_sweep_owner_runs_acq(g) - run0;
      hint1 = gc2_sweep_owner_next_tid_acq(g);
      if (delta) owner_calls++;
      if (quota == 1) assert(delta <= 1u);
      if ((cursor0 != eligible->hdr.reclaim_cell ||
           sweep0 != la_load32_acq(&eligible->hdr.sweep_epoch)) &&
          sweep0 != cycles) {
        unsigned gap = owner_calls - last_turn;
        eligible_turns++;
        if (!first_turn) first_turn = owner_calls;
        if (gap > max_gap) max_gap = gap;
        if (quota == 1 && gap > count + 1u) fairness_ok = 0;
        last_turn = owner_calls;
      }
      for (j = 0; j < count; j++) {
        if (cursors[j] > LJ_AFIRST_CELL &&
            blocked[j]->hdr.reclaim_cell == LJ_AFIRST_CELL)
          reset[j]++;
      }
      if (!first_done_call && la_load32_acq(&eligible->hdr.sweep_epoch) == cycles) {
        first_done_call = i + 1u;
        first_done_turn = owner_calls;
      }
      printf("TURN call=%u quota=%u hint=%u->%u work=%u owners=%" PRIu64
        " owner_calls=%u event=%" PRIu64 " main_cursor=%u->%u"
        " main_epoch=%u->%u phase=%u\n", i + 1u, quota, hint0, hint1,
        work, delta, owner_calls, gc2_deferred_epoch_acq(g) - event0,
        cursor0, eligible->hdr.reclaim_cell, sweep0,
        la_load32_acq(&eligible->hdr.sweep_epoch), gc2_phase_acq(g));
      if (detach_hint && first_done_call && hint1 != main_tid &&
          current_tid_present(g, hint1)) {
        saved_hint = hint1;
        break;
      }
    }
  }
  elapsed = now_ns() - start;
  events = gc2_deferred_epoch_acq(g) - epoch0;
  parks = gc2_worker_parks_acq(g) - parks0;
  eligible_done = la_load32_acq(&eligible->hdr.sweep_epoch) == cycles;
  printf("HELD count=%u quota=%u workers=%u ns=%" PRIu64
    " events=%" PRIu64 " parks=%" PRIu64 " runs=%" PRIu64
    " eligible_done=%d first_done_call=%u first_done_owner_call=%u"
    " eligible_turns=%u first_turn=%u max_gap=%u fairness=%d"
    " reset=%u,%u,%u hint=%u detach_saved=%u\n",
    count, quota, workers, elapsed, events, parks,
    gc2_sweep_owner_runs_acq(g) - runs0, eligible_done,
    first_done_call, first_done_turn, eligible_turns, first_turn, max_gap,
    fairness_ok, reset[0], reset[1], reset[2],
    gc2_sweep_owner_next_tid_acq(g), saved_hint);
  fflush(stdout);
  assert(elapsed < UINT64_C(1000000000));
  if (workers) {
    assert(events > 0 && parks > 0);
    assert(events < elapsed / UINT64_C(250000) + 32u);
    (void)lj_native_leave(L);
    assert(lj_gc2_workers_set(g, 0) == 1);
  }
  for (i = 0; i < count; i++) {
    held(blocked[i], lj_arena_cellof(p[i].uv));
    state("held-blocker", g, p[i].tg, blocked[i], lj_arena_cellof(p[i].uv));
  }
  lj_native_enter(tg);
  for (i = 0; i < count; i++) la_store32_rel(&p[i].stage, 2);
  for (i = 0; i < count; i++) wait_stage(&p[i], 3);
  (void)lj_native_leave(L);
  if (detach_hint) {
    uint32_t reclaimed;
    assert(saved_hint != 0 && saved_hint != main_tid);
    assert(gc2_sweep_owner_next_tid_acq(g) == saved_hint);
    lj_native_enter(tg);
    for (i = 0; i < count; i++) la_store32_rel(&p[i].stage, 4);
    for (i = 0; i < count; i++) assert(pthread_join(threads[i], NULL) == 0);
    (void)lj_native_leave(L);
    reclaimed = lj_tg_reclaim_dead(g);  /* Existing gated runtime transfer. */
    printf("DETACHED saved=%u hint=%u reclaimed=%u threads=%u present=%d\n",
      saved_hint, gc2_sweep_owner_next_tid_acq(g), reclaimed,
      gc2_n_threads_acq(g), current_tid_present(g, saved_hint));
    fflush(stdout);
    assert(gc2_sweep_owner_next_tid_acq(g) == saved_hint);
    assert(!current_tid_present(g, saved_hint));
    for (i = 0; i < count; i++) assert(!current_tid_present(g, tids[i]));
    /* Do not dereference any saved peer TG or arena after the real transfer. */
  }
  start = now_ns();
  result = lj_gc2_collect_active(L);
  elapsed = now_ns() - start;
  printf("RELEASED result=%d phase=%u ns=%" PRIu64
    " hint=%u main_tid=%u\n", result, gc2_phase_acq(g), elapsed,
    gc2_sweep_owner_next_tid_acq(g), main_tid);
  fflush(stdout);
  assert(result == 1 && gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(elapsed < UINT64_C(1000000000));
  if (detach_hint) assert(gc2_sweep_owner_next_tid_acq(g) == main_tid);
  else for (i = 0; i < count; i++)
    assert(p[i].tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE] == NULL);
  lua_getfield(L, -1, "eligible_sentinel");
  assert(lua_tointeger(L, -1) == 9237);
  lua_pop(L, 1);
  if (!detach_hint) {
    lj_native_enter(tg);
    for (i = 0; i < count; i++) la_store32_rel(&p[i].stage, 4);
    for (i = 0; i < count; i++) assert(pthread_join(threads[i], NULL) == 0);
    (void)lj_native_leave(L);
  }
  lua_settop(L, 0);
  assert(lj_gc2_collect_active(L) == 1);
  lua_close(L);
  assert(eligible_done && fairness_ok);
  puts("t-fair-owner OK");
  return 0;
}
