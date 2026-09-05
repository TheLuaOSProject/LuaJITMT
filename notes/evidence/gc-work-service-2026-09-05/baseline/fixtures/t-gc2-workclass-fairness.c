/* Exercise the production worker's queue selection with an independently
** eligible recovery graph and an ordinary SSB feeder between owned quanta. */
#define main coalescing_fixture_main
#include "t-gc2-sweep-table-coalescing.c"
#undef main

static void fill_strings(Fixture *f, GCstr *filler)
{
  GCRef *end = lj_tg_ssb_end_acq(f->tg);
  uint32_t count = 0;
  while (lj_tg_ssb_next_acq(f->tg) != end) {
    assert(count++ < TG_GC2_SSB_SLOTS);
    assert(lj_gc2_test_ssb_push(f->g, obj2gco(filler)) == 1);
  }
}

int main(int argc, char **argv)
{
  Fixture f;
  GCstr *filler;
  GC2SSBNode *held;
  uint32_t quota, sweep, feed, burst, turn, calls = 0, early = 0;
  uint32_t first_service = 0, recovery_before_cleanup;
  uint64_t ssb0, recovery0, seen_ssb, seen_recovery;
  assert(argc == 4);
  sweep = !strcmp(argv[1], "sweep");
  assert(sweep || !strcmp(argv[1], "mark"));
  quota = (uint32_t)strtoul(argv[2], NULL, 10);
  feed = (uint32_t)strtoul(argv[3], NULL, 10);
  assert((quota == 1 || quota == 64) && feed <= 1);
  alarm(20);
  f = open_fixture_phase(0, 0, sweep);
  assert(gc2_n_workers_acq(f.g) == 0);
  assert(gc2_worker_active_acq(f.g) == 0);
  assert(gc2_cycle_leader_acq(f.g) == 0);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(lj_gc2_test_ssb_empty(f.g));

  lua_pushliteral(f.L, "work-class SSB source");
  filler = strV(f.L->top - 1);
  held = lj_tg_ssb_free_pop(f.tg);
  assert(held && lj_tg_ssb_free_acq(f.tg) == NULL);
  fill_strings(&f, filler);
  store_child(&f);
  lj_gc2_barrier_tab_g(f.g, f.parent);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(f.parent)) ==
         LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(f.g) == 1);
  assert(!lj_gc2_ismarked(f.g, obj2gco(f.child)));
  assert(!lj_gc2_ismarked(f.g, obj2gco(f.grandchild)));
  lj_tg_ssb_free_push(f.tg, held);
  assert(lj_gc2_flush_ssb(f.g, f.tg) == TG_GC2_SSB_SLOTS);
  ssb0 = gc2_ssb_items_drained_acq(f.g);
  recovery0 = gc2_recovery_drained_acq(f.g);

  for (burst = 0; burst < (feed ? 4u : 1u); burst++) {
    if (burst != 0) {
      fill_strings(&f, filler);
      assert(lj_gc2_flush_ssb(f.g, f.tg) == TG_GC2_SSB_SLOTS);
    }
    for (turn = 0; turn < TG_GC2_SSB_SLOTS / quota; turn++) {
      (void)lj_gc2_worker_drain(f.g, quota);
      calls++;
      if (!early && lj_gc2_test_recovery_state(f.g, obj2gco(f.parent)) ==
                    LJ_ARENA_RECOVERY_IDLE) {
        assert(lj_gc2_ismarked(f.g, obj2gco(f.child)));
        early = 1;
        first_service = calls;
      }
    }
  }
  /* The matched quiet control supplies ordinary drain calls after the initial
  ** SSB source is exhausted, without explicitly invoking recovery itself. */
  if (!feed) {
    for (turn = 0; turn < 16 && !early; turn++) {
      (void)lj_gc2_worker_drain(f.g, quota);
      calls++;
      if (lj_gc2_test_recovery_state(f.g, obj2gco(f.parent)) ==
          LJ_ARENA_RECOVERY_IDLE) {
        assert(lj_gc2_ismarked(f.g, obj2gco(f.child)));
        early = 1;
        first_service = calls;
      }
    }
  }
  seen_ssb = gc2_ssb_items_drained_acq(f.g) - ssb0;
  seen_recovery = gc2_recovery_drained_acq(f.g) - recovery0;
  recovery_before_cleanup = lj_gc2_test_recovery_state(f.g, obj2gco(f.parent));
  assert(!lj_gc2_activation_reclaim_veto(f.g));
  assert(gc2_worker_active_acq(f.g) == 0);
  /* Real cleanup follows the measured window. It must still preserve the
  ** complete parent/child/grandchild graph and retire the recovery identity. */
  drain(&f);
  assert_graph(&f);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(f.parent)) ==
         LJ_ARENA_RECOVERY_IDLE);
  close_fixture(&f);
  printf("{\"phase\":\"%s\",\"quota\":%u,\"feed\":%u,\"calls\":%u,"
         "\"early\":%u,\"first_service\":%u,\"ssb_drained\":%llu,"
         "\"recovery_drained\":%llu,\"recovery_before_cleanup\":%u,"
         "\"cleanup_graph\":true}\n",
         argv[1], quota, feed, calls, early, first_service,
         (unsigned long long)seen_ssb, (unsigned long long)seen_recovery,
         recovery_before_cleanup);
  return early ? 0 : 2;
}
