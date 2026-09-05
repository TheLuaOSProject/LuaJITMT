/* Fair worker queue selection. Setup/cleanup helpers are copied from the
** existing coalescing fixture, excluding unrelated test cases. */
#ifndef LJ_GC2_TEST_HELPERS
#error "t-gc2-sweep-table-coalescing requires LJ_GC2_TEST_HELPERS"
#endif

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lualib.h"
#include "lib/gc2_wide_fixture_helpers.h"

typedef struct Fixture {
  lua_State *L;
  global_State *g;
  TGState *tg;
  GCtab *parent, *child, *grandchild;
  LJGC2TabStamp *stamp;
  size_t huge_size;
} Fixture;

static void drain(Fixture *f)
{
  uint32_t i;
  (void)lj_gc2_flush_ssb(f->g, f->tg);
  for (i = 0; i < 128u && !lj_gc2_test_ssb_empty(f->g); i++) {
    (void)lj_gc2_test_ssb_drain(f->g);
    (void)lj_gc2_flush_ssb(f->g, f->tg);
  }
  assert(lj_gc2_test_ssb_empty(f->g));
  assert(gc2_recovery_items_acq(f->g) == 0);
  assert(gc2_recovery_huge_items_acq(f->g) == 0);
  assert(gc2_table_rescan_pending_acq(f->g) == 0);
}

static GCtab *huge_table(Fixture *f)
{
  GCArena *a;
  GCtab *t;
  f->huge_size = LJ_HUGE_THRESHOLD + 4096u;
  t = (GCtab *)lj_arena_huge_map(&f->tg->prng, f->huge_size,
                                LJ_AF_TRAVERSABLE);
  assert(t);
  memset(t, 0, sizeof(*t));
  la_store8_rel(&t->gct, (uint8_t)~LJ_TTAB);
  newwhite(f->g, t);
  lj_tab_nomm_rel(t, (uint8_t)~0u);
  lj_tab_node_rel(t, &f->g->nilnode);
  lj_tab_freetop_rel(t, &f->g->nilnode);
  a = lj_arena_of(t);
  lj_arena_owner_rel(a, lj_tg_tid_acq(f->tg));
  lj_arena_gc2_tabledesc_rel(a, &f->g->gc2.table_rescan_desc);
  lj_arena_progress_g_rel(a, f->g);
  assert(lj_arena_hugetab_insert(&f->tg->huge, t, f->huge_size,
         LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY) == 1);
  return t;
}

static void enter_sweep(Fixture *f)
{
  LJGC2ActivationSnap mark, weak, sweep;
  assert(gc2_phase_acq(f->g) == LJ_GC2_MARK);
  mark = lj_gc2_activation_snapshot(&f->g->gc2.activation);
  assert(mark.state == LJ_GC2_ACT_MARK);
  assert(lj_gc2_activation_try_transition(&f->g->gc2.activation, &mark,
         mark.mark_epoch, LJ_GC2_ACT_WEAK, &weak) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&f->g->gc2.activation, &weak,
         mark.mark_epoch, LJ_GC2_ACT_SWEEP_OPEN, &sweep) == LJ_GC2_TRANSITION_OK);
  gc2_phase_rel(f->g, LJ_GC2_SWEEP);
  gc2_jit_phase_gate_rel(f->g, 0);
}

static Fixture open_fixture_phase(int huge, int cyclic, int sweep)
{
  Fixture f;
  TValue tv;
  memset(&f, 0, sizeof(f));
  f.L = luaL_newstate();
  assert(f.L);
  lua_gc(f.L, LUA_GCSTOP, 0);
  f.g = G(f.L);
  f.tg = G2TG(f.g);
  lua_createtable(f.L, 2, 4);
  f.parent = huge ? huge_table(&f) : tabV(f.L->top - 1);
  if (!huge) {
    /* Array value, hash value, hash key, and metatable all name this exact
    ** parent. A dirty scan must consume every private self-edge once. */
    copyTVrel(f.L, &lj_tab_array_acq(f.parent)[1], f.L->top - 1);
    lua_pushliteral(f.L, "self");
    lua_pushvalue(f.L, -2);
    lua_rawset(f.L, -3);
    lua_pushvalue(f.L, -1);
    lua_pushinteger(f.L, 1);
    lua_rawset(f.L, -3);
  }
  lj_tab_metatable_rel(f.parent, f.parent);
  lua_createtable(f.L, 2, 0);
  f.child = tabV(f.L->top - 1);
  lua_createtable(f.L, 1, 0);
  f.grandchild = tabV(f.L->top - 1);
  copyTVrel(f.L, &lj_tab_array_acq(f.child)[0], f.L->top - 1);
  if (cyclic) {
    settabV(f.L, &tv, f.parent);
    copyTVrel(f.L, &lj_tab_array_acq(f.child)[1], &tv);
    copyTVrel(f.L, &lj_tab_array_acq(f.grandchild)[0], &tv);
  }
  lj_gc2_mark_begin(f.g);
  /* This C-only fixture grants no native mutator turn. Close the cooperative
  ** MARK allowance before requiring one worker quantum to reach a scan hook. */
  lj_gc2_jit_mark_request_exit(f.g);
  gc2_jit_mark_resume_rel(f.g, 0);
  assert(lj_gc2_markobj(f.g, obj2gco(f.parent)) == 1);
  drain(&f);
  assert(lj_gc2_test_table_scan_current(f.g, f.parent));
  assert(lj_gc2_ismarked(f.g, obj2gco(f.child)) == 0);
  assert(lj_gc2_ismarked(f.g, obj2gco(f.grandchild)) == 0);
  f.stamp = lj_arena_gc2_stamp_acq(f.parent);
  assert(f.stamp);
  if (sweep)
    enter_sweep(&f);
  return f;
}

static void close_fixture(Fixture *f)
{
  lj_gc2_test_table_coalesce_hook(NULL);
  if (!gc2_recovery_failed_acq(f->g))
    drain(f);
  lj_gc2_cycle_to_idle(f->g);
  if (f->huge_size) {
    LJHugeInfo hi;
    assert(lj_arena_hugetab_delete(&f->tg->huge, f->parent, &hi) == 1);
    assert(hi.readers == 0);
    lj_arena_huge_unmap(f->parent, hi.size);
  }
  lua_close(f->L);
}

static void store_child(Fixture *f)
{
  uint64_t state = la_load64_acq(&f->stamp->state);
  if (f->huge_size) {
    lj_tab_metatable_rel(f->parent, f->child);
  } else {
    TValue tv;
    settabV(f->L, &tv, f->child);
    copyTVrel(f->L, &lj_tab_array_acq(f->parent)[0], &tv);
  }
  assert(la_load64_acq(&f->stamp->state) == state);
}

static void assert_graph(Fixture *f)
{
  assert(lj_gc2_ismarked(f->g, obj2gco(f->parent)) == 1);
  assert(lj_gc2_ismarked(f->g, obj2gco(f->child)) == 1);
  assert(lj_gc2_ismarked(f->g, obj2gco(f->grandchild)) == 1);
  assert(lj_gc2_test_table_scan_current(f->g, f->parent));
  assert(!(lj_obj_gcflags(obj2gco(f->parent)) & LJ_GC_NEEDSCAN));
  assert(lj_tab_gc2_rescan_state_acq(f->parent) == LJ_TAB_RESCAN_NONE);
  assert(gc2_smr_readers_acq(f->g) == 0);
  if (f->huge_size) {
    LJHugeInfo hi;
    assert(lj_arena_hugetab_lookup(&f->tg->huge, f->parent, &hi));
    assert(hi.readers == 0);
  } else {
    assert((lj_arena_remote_active_acq(lj_arena_of(f->parent)) &
            LJ_ARENA_REMOTE_COUNT_MASK) == 0);
  }
}

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
  uint32_t quota, sweep, weak, assist, feed, burst, turn, calls = 0, early = 0;
  uint32_t first_service = 0, recovery_before_cleanup;
  uint64_t ssb0, recovery0, seen_ssb, seen_recovery;
  assert(argc == 5);
  assist = !strcmp(argv[4], "assist");
  assert(assist || !strcmp(argv[4], "worker"));
  sweep = !strcmp(argv[1], "sweep");
  weak = !strcmp(argv[1], "weak");
  assert(sweep || weak || !strcmp(argv[1], "mark"));
  assert(!assist || !sweep);
  quota = (uint32_t)strtoul(argv[2], NULL, 10);
  feed = (uint32_t)strtoul(argv[3], NULL, 10);
  assert((quota == 1 || quota == 64) && feed <= 1);
  alarm(20);
  f = open_fixture_phase(0, 0, sweep);
  if (weak) {
    LJGC2ActivationSnap mark = lj_gc2_activation_snapshot(&f.g->gc2.activation);
    LJGC2ActivationSnap next;
    assert(lj_gc2_activation_try_transition(&f.g->gc2.activation, &mark,
           mark.mark_epoch, LJ_GC2_ACT_WEAK, &next) == LJ_GC2_TRANSITION_OK);
    gc2_phase_rel(f.g, LJ_GC2_WEAK);
  }
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
  if (assist) {
    /* Select the real assist entry through its explicit pressure predicate. */
    lj_gc2_alloc_since_store(f.g, 2);
    lj_gc2_hard_store(f.g, 1);
    gc2_assist_shift_store_rlx(f.g, quota == 1 ? 0 : 6);
    assert(lj_gc2_hard_limit_reached(f.g));
  }

  for (burst = 0; burst < (feed ? 4u : 1u) && !early; burst++) {
    if (burst != 0) {
      fill_strings(&f, filler);
      assert(lj_gc2_flush_ssb(f.g, f.tg) == TG_GC2_SSB_SLOTS);
    }
    for (turn = 0; turn < TG_GC2_SSB_SLOTS / quota && !early; turn++) {
      if (assist)
        (void)lj_gc2_assist(f.g, f.tg);
      else
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
      if (assist)
        (void)lj_gc2_assist(f.g, f.tg);
      else
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
  if (assist) {
    assert(gc2_assist_runs_acq(f.g) >= calls);
    assert(!gc2_assist_active_acq(f.g));
    assert(!lj_tg_gc_assist_acq(f.tg));
  }
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
  return early && (!feed || first_service <= 4) ? 0 : 2;
}
