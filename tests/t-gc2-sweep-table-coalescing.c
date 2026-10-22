/*
** Semantic SWEEP table requests may coalesce only behind a newer complete
** scan. Exercise the production publisher/converter with exact interleavings,
** available SSB slots, reader denial, and cyclic recovery work.
*/
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

static Fixture *watched;
static uint32_t scans, pause_stage, paused, released, cross_phase;
static uint32_t mark_entry, finish_old_scan;
static pthread_t old_scanner;
static void cross_phase_before_publication(Fixture *f);
static void enter_sweep(Fixture *f);

static void wait_value(uint32_t *p, uint32_t value)
{
  while (la_load32_acq(p) != value)
    la_cpu_pause();
}

static void hook(global_State *g, GCtab *t, uint32_t stage)
{
  uint32_t expect;
  if (!watched || g != watched->g || t != watched->parent)
    return;
  if (stage == LJ_GC2_TABLE_COALESCE_TEST_SCAN_START)
    (void)la_add32_rlx(&scans, 1);
  if (stage == LJ_GC2_TABLE_COALESCE_TEST_MARK_ENTER && mark_entry) {
    uint32_t denial = mark_entry;
    mark_entry = 0;
    enter_sweep(watched);
    if (denial > 1u) {
      assert(lj_arena_lifetime_state_cas(lj_arena_of(t), lj_arena_cellof(t),
               LJ_ARENA_LIFETIME_LIVE, denial == 2u ?
               LJ_ARENA_LIFETIME_DESTRUCT : LJ_ARENA_LIFETIME_MUTATING));
    }
  }
  if (stage == LJ_GC2_TABLE_COALESCE_TEST_POST_DIRTY && finish_old_scan) {
    finish_old_scan = 0;
    la_store32_rel(&released, 1);
    assert(pthread_join(old_scanner, NULL) == 0);
    assert(lj_gc2_test_table_scan_current(g, t));
  }
  if (stage == LJ_GC2_TABLE_COALESCE_TEST_PRE_DIRTY && cross_phase) {
    cross_phase = 0;
    cross_phase_before_publication(watched);
  }
  if (stage == LJ_GC2_TABLE_COALESCE_TEST_PRE_DIRTY ||
      stage == LJ_GC2_TABLE_COALESCE_TEST_POST_DIRTY) {
    if (watched->huge_size) {
      LJHugeInfo hi;
      assert(lj_arena_hugetab_lookup(&watched->tg->huge, t, &hi));
      assert(hi.readers != 0);
    } else {
      assert((lj_arena_remote_active_acq(lj_arena_of(t)) &
              LJ_ARENA_REMOTE_COUNT_MASK) != 0);
    }
  }
  expect = stage;
  if (la_cas32(&pause_stage, &expect, 0, LA_ACQ_REL, LA_ACQ)) {
    la_store32_rel(&paused, stage);
    wait_value(&released, 1);
    la_store32_rel(&paused, 0);
  }
}

static void watch(Fixture *f, uint32_t stage)
{
  watched = f;
  scans = paused = released = 0;
  pause_stage = stage;
  lj_gc2_test_table_coalesce_hook(hook);
}

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

static Fixture open_fixture(int huge, int cyclic)
{
  return open_fixture_phase(huge, cyclic, 1);
}

static void close_fixture(Fixture *f)
{
  watched = NULL;
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

static void publish(Fixture *f, int api)
{
  TValue tv;
  LJGC2Lease lease;
  if (api == 0)
    assert(lj_gc2_trace_sweep_root(f->g, obj2gco(f->parent)) == 1);
  else if (api == 1) {
    settabV(f->L, &tv, f->parent);
    lj_gc2_barrier_tv_g(f->g, &tv);
  } else if (api == 2) {
    assert(lj_gc2_obj_lease_acquire(f->g, obj2gco(f->parent),
             (uint32_t)~LJ_TTAB, NULL, &lease) >= 0);
    lj_gc2_lease_release(&lease);
  } else {
    assert(lj_gc2_test_table_expected_status(f->g, f->parent) ==
           LJ_GC2_TV_EDGE_VALID);
  }
}

static void test_duplicates(int huge, int api, int cyclic)
{
  Fixture f = open_fixture(huge, cyclic);
  uint32_t i;
  watch(&f, 0);
  store_child(&f);
  for (i = 0; i < 32u; i++)
    publish(&f, api);
  assert(!lj_gc2_test_table_scan_current(f.g, f.parent));
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(lj_gc2_flush_ssb(f.g, f.tg) == 32u);
  for (i = 0; i < 128u && !lj_gc2_test_ssb_empty(f.g); i++)
    (void)lj_gc2_worker_drain(f.g, 1);
  drain(&f);
  assert_graph(&f);
  if (scans != 1)
    fprintf(stderr, "duplicate scans: huge=%d api=%d cyclic=%d scans=%u\n",
            huge, api, cyclic, scans);
  assert(scans == 1);
  assert(!lj_gc2_activation_reclaim_veto(f.g));
  close_fixture(&f);
}

static void *drain_while_publisher_paused(void *arg)
{
  Fixture *f = (Fixture *)arg;
  uint32_t stage;
  do { stage = la_load32_acq(&paused); } while (!stage);
  /* The old slot is published, the new one is still private to its caller. */
  assert(lj_gc2_worker_drain(f->g, 1) != 0);
  assert(lj_gc2_test_table_scan_current(f->g, f->parent));
  assert(scans == 1);
  la_store32_rel(&released, 1);
  return NULL;
}

static void test_publication_pause(int huge, uint32_t stage)
{
  Fixture f = open_fixture(huge, 1);
  pthread_t worker;
  publish(&f, 0);
  assert(lj_gc2_flush_ssb(f.g, f.tg) == 1);
  store_child(&f);
  watch(&f, stage);
  assert(pthread_create(&worker, NULL, drain_while_publisher_paused, &f) == 0);
  publish(&f, 0);
  assert(pthread_join(worker, NULL) == 0);
  drain(&f);
  assert_graph(&f);
  assert(scans == (stage == LJ_GC2_TABLE_COALESCE_TEST_PRE_DIRTY ? 2u : 1u));
  close_fixture(&f);
}

static void *scan_to_proof(void *arg)
{
  Fixture *f = (Fixture *)arg;
  assert(lj_gc2_worker_drain(f->g, 1) != 0);
  return NULL;
}

static void test_late_write(int huge, int saturated)
{
  Fixture f = open_fixture(huge, 1);
  pthread_t worker;
  publish(&f, 0);
  if (saturated)
    ljt_gc2_wide_seed(f.parent, UINT64_MAX, UINT32_MAX, 0, 1);
  assert(lj_gc2_flush_ssb(f.g, f.tg) == 1);
  watch(&f, LJ_GC2_TABLE_COALESCE_TEST_PRE_PROOF);
  assert(pthread_create(&worker, NULL, scan_to_proof, &f) == 0);
  wait_value(&paused, LJ_GC2_TABLE_COALESCE_TEST_PRE_PROOF);
  assert(lj_gc2_ismarked(f.g, obj2gco(f.child)) == 0);
  store_child(&f);
  publish(&f, 0);
  la_store32_rel(&released, 1);
  assert(pthread_join(worker, NULL) == 0);
  if (saturated) {
    /* The old scanner can republish the absorbing maximum after the write.
    ** The converter must refuse this ambiguous proof even with a sticky veto. */
    assert(lj_gc2_test_table_scan_current(f.g, f.parent));
    assert(lj_gc2_activation_reclaim_veto(f.g));
  } else {
    assert(!lj_gc2_test_table_scan_current(f.g, f.parent));
  }
  drain(&f);
  assert_graph(&f);
  assert(scans >= 2u);
  close_fixture(&f);
}

static void *transient_mark(void *arg)
{
  Fixture *f = (Fixture *)arg;
  GCArena *a = lj_arena_of(f->parent);
  uint32_t cell = lj_arena_cellof(f->parent);
  while (!lj_gc2_test_queue_post_admit_paused())
    la_cpu_pause();
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
                                    LJ_ARENA_LIFETIME_DESTRUCT));
  lj_gc2_test_queue_post_admit_release();
  while (!lj_gc2_test_queue_retry_witness_paused())
    la_cpu_pause();
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_DESTRUCT,
                                    LJ_ARENA_LIFETIME_LIVE));
  lj_gc2_test_queue_retry_witness_release();
  return NULL;
}

static void test_mark_retry(void)
{
  Fixture f = open_fixture(0, 1);
  pthread_t transient;
  watch(&f, 0);
  store_child(&f);
  lj_gc2_test_queue_post_admit_pause(obj2gco(f.parent));
  lj_gc2_test_queue_retry_witness_pause(obj2gco(f.parent));
  assert(pthread_create(&transient, NULL, transient_mark, &f) == 0);
  assert(lj_gc2_test_table_expected_status(f.g, f.parent) ==
         LJ_GC2_TV_EDGE_RETRY);
  assert(pthread_join(transient, NULL) == 0);
  drain(&f);  /* No successful API retry is permitted to conceal a lost mark. */
  assert_graph(&f);
  close_fixture(&f);
}

static void cross_phase_before_publication(Fixture *f)
{
  if (gc2_phase_acq(f->g) == LJ_GC2_IDLE)
    lj_gc2_mark_begin(f->g);
  enter_sweep(f);
  /* An independent existing queue owner completes a real scan after the
  ** semantic mark LP, but before the delayed publisher exposes its request. */
  assert(lj_gc2_worker_drain(f->g, 1) != 0);
  assert(lj_gc2_test_table_scan_current(f->g, f->parent));
  assert(lj_gc2_ismarked(f->g, obj2gco(f->child)) == 0);
  store_child(f);
}

static void test_phase_crossing(int api)
{
  Fixture f = open_fixture(0, 1);
  lj_gc2_cycle_to_idle(f.g);
  if (api == 2)
    gc2_generational_rel(f.g, 1);
  else
    lj_gc2_mark_begin(f.g);
  assert(lj_gc2_test_ssb_push(f.g, obj2gco(f.parent)) == 1);
  assert(lj_gc2_flush_ssb(f.g, f.tg) == 1);
  watch(&f, 0);
  cross_phase = 1;
  if (api == 0)
    assert(lj_gc2_markobj_expected_status(f.g, obj2gco(f.parent),
             (uint32_t)~LJ_TTAB, NULL) == 1);
  else if (api == 1)
    lj_gc2_test_rescan_pending_clear_cycle(f.g, obj2gco(f.parent));
  else
    lj_gc2_remember_root(f.g, obj2gco(f.parent));
  assert(cross_phase == 0);
  drain(&f);
  assert_graph(&f);
  assert(scans == 2);
  close_fixture(&f);
}

static void test_huge_wrong_type(void)
{
  Fixture f = open_fixture(1, 1);
  watch(&f, 0);
  store_child(&f);
  assert(lj_gc2_markobj_expected_status(f.g, obj2gco(f.parent),
           (uint32_t)~LJ_TFUNC, NULL) < 0);
  drain(&f);
  assert_graph(&f);
  assert(scans == 1);
  close_fixture(&f);
}

static void test_huge_overflow(void)
{
  Fixture f = open_fixture(1, 1);
  LJHugeReader *readers;
  uint32_t i;
  uint64_t stamp;
  uint64_t pending, huge_pending;
  GCRef *next;
  int queued;
  long page_size = sysconf(_SC_PAGESIZE);
  void *page;
  assert(page_size > 0);
  page = (void *)((uintptr_t)f.parent & ~((uintptr_t)page_size - 1u));
  store_child(&f);
  stamp = la_load64_acq(&f.stamp->state);
  next = lj_tg_ssb_next_acq(f.tg);
  readers = (LJHugeReader *)calloc(LJ_ARENA_HUGE_READER_MAX, sizeof(*readers));
  assert(readers);
  for (i = 0; i < LJ_ARENA_HUGE_READER_MAX; i++)
    assert(lj_arena_hugetab_reader_acquire(&f.tg->huge, f.parent,
             &readers[i], NULL) == LJ_ARENA_HUGE_READER_ACQUIRED);
  assert(mprotect(page, (size_t)page_size, PROT_NONE) == 0);
  assert(lj_gc2_markobj_expected_status(f.g, obj2gco(f.parent),
           (uint32_t)~LJ_TTAB, NULL) < 0);
  assert(mprotect(page, (size_t)page_size, PROT_READ|PROT_WRITE) == 0);
  assert(la_load64_acq(&f.stamp->state) == stamp);
  queued = lj_tg_ssb_next_acq(f.tg) != next;
  pending = gc2_recovery_items_acq(f.g);
  huge_pending = gc2_recovery_huge_items_acq(f.g);
  assert(!gc2_recovery_failed_acq(f.g));
  for (i = 0; i < LJ_ARENA_HUGE_READER_MAX; i++)
    assert(lj_arena_hugetab_reader_release(&readers[i], NULL) ==
           LJ_ARENA_HUGE_READER_RELEASED);
  free(readers);
  watch(&f, 0);
  if (pending)
    assert(lj_gc2_test_recovery_drain(f.g, 1) == 1);
  drain(&f);
  assert_graph(&f);
  assert(!queued && pending == 1 && huge_pending == 1);
  close_fixture(&f);
}

static void test_registry_denial(void)
{
  Fixture f = open_fixture(1, 0);
  GCRef *next = lj_tg_ssb_next_acq(f.tg);
  long page_size = sysconf(_SC_PAGESIZE);
  void *page;
  assert(page_size > 0);
  page = (void *)((uintptr_t)f.parent & ~((uintptr_t)page_size - 1u));
  assert(gc2_smr_readers_acq(f.g) == 0);
  gc2_smr_reclaiming_rel(f.g, LJ_GC2_SMR_META_EXCLUSIVE);
  assert(mprotect(page, (size_t)page_size, PROT_NONE) == 0);
  assert(lj_gc2_markobj_expected_status(f.g, obj2gco(f.parent),
           (uint32_t)~LJ_TTAB, NULL) < 0);
  assert(lj_tg_ssb_next_acq(f.tg) == next);
  assert(gc2_recovery_failed_acq(f.g));
  assert(lj_gc2_activation_reclaim_veto(f.g));
  assert(mprotect(page, (size_t)page_size, PROT_READ|PROT_WRITE) == 0);
  gc2_smr_reclaiming_rel(f.g, LJ_GC2_SMR_OPEN);
  close_fixture(&f);
}

static void test_mark_barrier_cutover(uint32_t denial)
{
  Fixture f = open_fixture_phase(0, 1, 0);
  uint64_t stamp = la_load64_acq(&f.stamp->state);
  uint64_t after_publication, recovery;
  store_child(&f);
  watch(&f, 0);
  mark_entry = denial;
  lj_gc2_barrier_tab_g(f.g, f.parent);
  assert(mark_entry == 0);
  after_publication = la_load64_acq(&f.stamp->state);
  recovery = gc2_recovery_items_acq(f.g);
  if (denial == 3u) {
    /* An unrelated MUTATING owner supplies no rescuable exact locator. The
    ** request must leave the sticky veto without touching table storage. */
    assert(la_load64_acq(&f.stamp->state) == stamp);
    assert(gc2_recovery_failed_acq(f.g));
    assert(lj_gc2_activation_reclaim_veto(f.g));
    assert(lj_arena_lifetime_state_cas(lj_arena_of(f.parent),
             lj_arena_cellof(f.parent), LJ_ARENA_LIFETIME_MUTATING,
             LJ_ARENA_LIFETIME_LIVE));
  } else {
    /* Complete the synthetic transient owner without publishing another
    ** request. A broken helper must fail on the lost descendant, not merely
    ** because the fixture left its parent in DESTRUCT after the call. */
    if (denial == 2u)
      (void)lj_arena_lifetime_state_cas(lj_arena_of(f.parent),
             lj_arena_cellof(f.parent), LJ_ARENA_LIFETIME_DESTRUCT,
             LJ_ARENA_LIFETIME_LIVE);
    drain(&f);
    assert_graph(&f);
    if (denial == 2u)
      assert(after_publication == stamp && recovery == 1);
    assert(!gc2_recovery_failed_acq(f.g));
  }
  close_fixture(&f);
}

static void test_mark_barrier_saturation(void)
{
  Fixture f = open_fixture_phase(0, 1, 0);
  lj_gc2_barrier_tab_g(f.g, f.parent);
  ljt_gc2_wide_seed(f.parent, UINT64_MAX, UINT32_MAX, 0, 1);
  assert(lj_gc2_flush_ssb(f.g, f.tg) == 1);
  watch(&f, LJ_GC2_TABLE_COALESCE_TEST_PRE_PROOF);
  assert(pthread_create(&old_scanner, NULL, scan_to_proof, &f) == 0);
  wait_value(&paused, LJ_GC2_TABLE_COALESCE_TEST_PRE_PROOF);
  store_child(&f);
  mark_entry = 1;
  finish_old_scan = 1;
  lj_gc2_barrier_tab_g(f.g, f.parent);
  assert(!mark_entry && !finish_old_scan);
  assert(lj_gc2_activation_reclaim_veto(f.g));
  drain(&f);
  assert_graph(&f);
  close_fixture(&f);
}

static int no_op(lua_State *L)
{
  (void)L;
  return 0;
}

static void test_mark_barrier_wrong_type(void)
{
  Fixture f = open_fixture_phase(0, 0, 0);
  GCRef *next;
  GCudata *ud;
  GCfunc *fn;
  LJGC2TabStamp *stamp;
  LJGC2Lease lease;
  uint64_t state, token;
  uint32_t i;
  (void)lua_newuserdata(f.L, sizeof(GCtab));
  ud = udataV(f.L->top - 1);
  next = lj_tg_ssb_next_acq(f.tg);
  lj_gc2_barrier_tab_g(f.g, (GCtab *)(void *)ud);
  assert(lj_tg_ssb_next_acq(f.tg) == next);
  assert(!gc2_recovery_items_acq(f.g));
  assert(!gc2_recovery_failed_acq(f.g));
  /* A function in real traversable storage reaches the exact type check,
  ** unlike the allocator-plain userdata rejection above. Keep its sidecar
  ** and body admitted while verifying neither table field is changed. */
  for (i = 0; i < 8u; i++) lua_pushinteger(f.L, (lua_Integer)i);
  lua_pushcclosure(f.L, no_op, 8);
  fn = funcV(f.L->top - 1);
  assert(lj_arena_flags_acq(lj_arena_of(fn)) & LJ_AF_TRAVERSABLE);
  assert(lj_gc2_tv_lease_acquire(f.g, f.L->top - 1, &lease) ==
         LJ_GC2_TV_EDGE_VALID);
  stamp = lj_arena_gc2_stamp_acq(fn);
  assert(stamp);
  state = la_load64_acq(&stamp->state);
  token = la_load64_acq(&stamp->token.control);
  next = lj_tg_ssb_next_acq(f.tg);
  lj_gc2_barrier_tab_g(f.g, (GCtab *)(void *)fn);
  assert(lj_tg_ssb_next_acq(f.tg) == next);
  assert(la_load64_acq(&stamp->state) == state);
  assert(la_load64_acq(&stamp->token.control) == token);
  assert(!gc2_recovery_items_acq(f.g));
  assert(!gc2_recovery_failed_acq(f.g));
  lj_gc2_lease_release(&lease);
  close_fixture(&f);
}

static uint32_t exact_scan_result;
static void *scan_exact(void *arg)
{
  Fixture *f = (Fixture *)arg;
  la_store32_rel(&exact_scan_result,
    (uint32_t)lj_gc2_test_table_token_scan_one(f->g, f->parent));
  return NULL;
}

static void test_old_scanner(int huge, int wide, int exact)
{
  Fixture f = open_fixture_phase(huge, 1, !exact);
  pthread_t scanner;
  uint64_t ticket = 0;
  ljt_gc2_wide_seed(f.parent, 17u, 1u, 0, wide);
  if (exact) {
    ticket = lj_gc2_test_table_token_request(f.g, f.parent);
    assert(ticket != 0);
  } else {
    assert(lj_gc2_test_ssb_push(f.g, obj2gco(f.parent)) == 1);
    assert(lj_gc2_flush_ssb(f.g, f.tg) == 1);
  }
  watch(&f, LJ_GC2_TABLE_COALESCE_TEST_PRE_PROOF);
  assert(pthread_create(&scanner, NULL,
    exact ? scan_exact : scan_to_proof, &f) == 0);
  wait_value(&paused, LJ_GC2_TABLE_COALESCE_TEST_PRE_PROOF);
  if (wide) {
    ljt_gc2_wide_seed(f.parent, 17u, UINT32_MAX, 0, 1);
  } else {
    /* The new W serial equals the old inline serial: checking low bits
    ** without checking the captured domain would accept a false proof. */
    ljt_gc2_wide_seed(f.parent, 17u, 0u, 0, 0);
    la_store64_rel(&f.stamp->state, UINT32_MAX - 1u);
  }
  store_child(&f);
  if (exact)
    lj_gc2_barrier_tab_g(f.g, f.parent);
  else
    publish(&f, 0);
  assert(!lj_gc2_activation_reclaim_veto(f.g));
  assert((uint32_t)la_load64_acq(&f.stamp->state) == UINT32_MAX);
  assert(ljt_gc2_wide_snapshot(f.parent).hi == (wide ? 18u : 17u));
  assert((uint32_t)ljt_gc2_wide_snapshot(f.parent).lo == 1u);
  la_store32_rel(&released, 1);
  assert(pthread_join(scanner, NULL) == 0);
  if (exact) {
    uint64_t control = la_load64_acq(&f.stamp->token.control);
    assert(exact_scan_result == 0);
    assert(lj_gc2_table_token_generation(control) == ticket);
    assert(lj_gc2_table_token_state(control) == LJ_GC2_TABLE_TOKEN_PENDING);
    assert(lj_gc2_test_table_token_scan_one(f.g, f.parent) == 1);
    assert(lj_gc2_table_token_state(la_load64_acq(&f.stamp->token.control)) ==
           LJ_GC2_TABLE_TOKEN_NONE);
  }
  drain(&f);
  assert_graph(&f);
  assert(scans >= 2);
  close_fixture(&f);
  printf("old scanner huge=%d wide=%d exact=%d rejected\n", huge, wide, exact);
}

static void *peer_promote(void *arg)
{
  Fixture *f = (Fixture *)arg;
  uint32_t stage;
  do { stage = la_load32_acq(&paused); } while (!stage);
  assert((uint32_t)(ljt_gc2_wide_snapshot(f->parent).lo >> 32) == 0);
  assert((uint32_t)la_load64_acq(&f->stamp->state) ==
    (stage == LJ_GC2_TABLE_COALESCE_TEST_PRE_MODE ?
     UINT32_MAX - 1u : UINT32_MAX));
  publish(f, 0);  /* Completes while the first publisher remains paused. */
  assert((uint32_t)la_load64_acq(&f->stamp->state) == UINT32_MAX);
  la_store32_rel(&released, 1);
  return NULL;
}

static void test_mode_pause(int huge, uint32_t stage)
{
  Fixture f = open_fixture(huge, 1);
  pthread_t peer;
  uint32_t cycle = gc2_cycle_acq(f.g);
  ljt_gc2_wide_seed(f.parent, 73u, 5u, cycle, 0);
  /* Simulate a reused cell's old current-cycle W, with a real completed
  ** semantic request waiting in the new incarnation's ordinary queue. */
  store_child(&f);
  publish(&f, 0);
  assert(lj_gc2_flush_ssb(f.g, f.tg) == 1);
  la_store64_rel(&f.stamp->state, UINT32_MAX - 1u);
  watch(&f, stage);
  assert(pthread_create(&peer, NULL, peer_promote, &f) == 0);
  publish(&f, 0);
  assert(pthread_join(peer, NULL) == 0);
  drain(&f);
  assert_graph(&f);
  assert(!lj_gc2_activation_reclaim_veto(f.g));
  close_fixture(&f);
  printf("mode pause huge=%d stage=%u peer completed\n", huge, stage);
}

static void lua_ok(lua_State *L, const char *s)
{
  if (luaL_dostring(L, s)) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    abort();
  }
}

static void test_continued_collection(int huge)
{
  Fixture f;
  uint32_t round;
  memset(&f, 0, sizeof(f));
  f.L = luaL_newstate(); assert(f.L); luaL_openlibs(f.L);
  lua_gc(f.L, LUA_GCSTOP, 0);
  f.g = G(f.L); f.tg = G2TG(f.g);
  lua_newtable(f.L);
  f.parent = huge ? huge_table(&f) : tabV(f.L->top - 1);
  if (huge) settabV(f.L, f.L->top - 1, f.parent);
  lua_setglobal(f.L, "p");
  lua_ok(f.L, "weak = setmetatable({}, {__mode='v'})");
  f.stamp = lj_arena_gc2_stamp_acq(f.parent);
  for (round = 0; round < 12; round++) {
    uint64_t era;
    lua_ok(f.L, "p.keep = {n=2718, child={value=314}}; weak[1] = {}");
    era = ljt_gc2_wide_snapshot(f.parent).hi;
    ljt_gc2_wide_seed(f.parent, era, UINT32_MAX, 0, 1);
    lj_gc2_test_table_dirty_bump(f.g, f.parent);
    assert(ljt_gc2_wide_snapshot(f.parent).hi == era + 1);
    assert(!lj_gc2_activation_reclaim_veto(f.g));
    assert(lua_gc(f.L, LUA_GCCOLLECT, 0) == 0);
    assert(gc2_phase_acq(f.g) == LJ_GC2_IDLE);
    assert(gc2_recovery_items_acq(f.g) == 0);
    assert(!lj_gc2_activation_reclaim_veto(f.g));
    lua_ok(f.L, "assert(weak[1] == nil); assert(p.keep.n == 2718 and p.keep.child.value == 314)");
  }
  lua_ok(f.L, "p=nil; weak=nil");
  lua_settop(f.L, 0);
  close_fixture(&f);
  printf("huge=%d twelve rollover collections reached IDLE\n", huge);
}

#if defined(LJ_TEST_WRAP_CALLOC)
extern void *__real_calloc(size_t, size_t);
static int deny_calloc;
static uint32_t denied_calloc_calls;
void *__wrap_calloc(size_t n, size_t size)
{
  if (deny_calloc) { denied_calloc_calls++; return NULL; }
  return __real_calloc(n, size);
}
#endif

static void test_reserved_failure(void)
{
  Fixture f = open_fixture(0, 0);
  GCArena *plain;
  lj_arena_test_gc2_sidecar_fail_alloc(1);
  assert(lj_arena_map(&f.tg->prng, LJ_AF_TRAVERSABLE) == NULL);
  /* Tail W has no separate sidecar allocation to fail. Map failure and
  ** locator-insertion cleanup are exercised by the mmap64 wrapper fixture. */
  {
    size_t size = LJ_HUGE_THRESHOLD + 4096u;
    void *huge = lj_arena_huge_map(&f.tg->prng, size, LJ_AF_TRAVERSABLE);
    assert(huge && lj_arena_gc2_wide_acq(huge));
    lj_arena_huge_unmap(huge, size);
  }
  plain = lj_arena_map(&f.tg->prng, 0); assert(plain);
  assert(lj_arena_gc2_tabstamp_acq(plain) == NULL);
  lj_arena_unmap(plain);
  lj_arena_test_gc2_sidecar_fail_alloc(0);
  ljt_gc2_wide_seed(f.parent, 0, 0, 0, 0);
  la_store64_rel(&f.stamp->state, UINT32_MAX - 1u);
  store_child(&f);
#if defined(LJ_TEST_WRAP_CALLOC)
  deny_calloc = 1;
#endif
  lj_gc2_test_table_dirty_bump(f.g, f.parent);
#if defined(LJ_TEST_WRAP_CALLOC)
  deny_calloc = 0;
  assert(denied_calloc_calls == 0);
#endif
  assert(ljt_gc2_wide_snapshot(f.parent).lo == 1);
  assert(!lj_gc2_activation_reclaim_veto(f.g));
  publish(&f, 0); drain(&f); assert_graph(&f);
  close_fixture(&f);
  puts("reservation failure remains private; post-store promotion allocates nothing");
}


int main(int argc, char **argv)
{
  int huge, api, cyclic, wide, exact;
  const char *mode = argc > 1 ? argv[1] : "all";
  alarm(60);
  if (!strcmp(mode, "all") || !strcmp(mode, "duplicates"))
    for (huge = 0; huge < 2; huge++)
      for (api = 0; api < 4; api++)
        for (cyclic = 0; cyclic < 2; cyclic++)
          test_duplicates(huge, api, cyclic);
  if (!strcmp(mode, "all") || !strcmp(mode, "pause"))
    for (huge = 0; huge < 2; huge++) {
      test_publication_pause(huge, LJ_GC2_TABLE_COALESCE_TEST_PRE_DIRTY);
      test_publication_pause(huge, LJ_GC2_TABLE_COALESCE_TEST_POST_DIRTY);
      test_late_write(huge, 0);
    }
  if (!strcmp(mode, "all") || !strcmp(mode, "saturation"))
    for (huge = 0; huge < 2; huge++)
      test_late_write(huge, 1);
  if (!strcmp(mode, "all") || !strcmp(mode, "retry"))
    test_mark_retry();
  if (!strcmp(mode, "all") || !strcmp(mode, "phase"))
    for (api = 0; api < 3; api++)
      test_phase_crossing(api);
  if (!strcmp(mode, "phase_expected")) test_phase_crossing(0);
  if (!strcmp(mode, "phase_base")) test_phase_crossing(1);
  if (!strcmp(mode, "phase_remember")) test_phase_crossing(2);
  if (!strcmp(mode, "all") || !strcmp(mode, "wrongtype"))
    test_huge_wrong_type();
  if (!strcmp(mode, "all") || !strcmp(mode, "overflow"))
    test_huge_overflow();
  if (!strcmp(mode, "all") || !strcmp(mode, "registry"))
    test_registry_denial();
  if (!strcmp(mode, "all") || !strcmp(mode, "mark_barrier")) {
    for (api = 1; api <= 3; api++) test_mark_barrier_cutover((uint32_t)api);
    test_mark_barrier_wrong_type();
    test_mark_barrier_saturation();
  }
  if (!strcmp(mode, "mark_barrier_denial")) test_mark_barrier_cutover(2);
  if (!strcmp(mode, "mark_barrier_saturation")) test_mark_barrier_saturation();
  if (!strcmp(mode, "all") || !strcmp(mode, "wide")) {
    for (huge = 0; huge < 2; huge++) {
      for (wide = 0; wide < 2; wide++)
        for (exact = 0; exact < 2; exact++)
          test_old_scanner(huge, wide, exact);
      test_mode_pause(huge, LJ_GC2_TABLE_COALESCE_TEST_PRE_MODE);
      test_mode_pause(huge, LJ_GC2_TABLE_COALESCE_TEST_POST_MODE);
      test_continued_collection(huge);
    }
    test_reserved_failure();
  }
  puts("t-gc2-sweep-table-coalescing OK: admitted requests coalesce behind "
       "complete scans; unresolved and saturated requests remain durable");
  return 0;
}
