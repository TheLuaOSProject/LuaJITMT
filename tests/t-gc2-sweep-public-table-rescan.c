/*
** Public SWEEP table rescans must survive the ordinary SSB converter.
** A current scan stamp covers the old payload, not a later raw store followed
** by a public parent barrier. Recovery saturation must not be required to
** make that publication effective.
*/

#ifndef LJ_GC2_TEST_HELPERS
#error "t-gc2-sweep-public-table-rescan requires LJ_GC2_TEST_HELPERS"
#endif

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_ctype.h"

#if LJ_HASFFI
/* Inject a transient membership result at each distinct production consumer.
** The direct predicate cases below separately exercise real closed-SMR and
** allocation-lifetime denials. No GC traversal or queue operation is wrapped. */
static GCtab *retry_target;
static uint32_t retry_at, membership_calls, membership_retries;

extern int __real_lj_ctype_fin_istab(global_State *g, GCtab *t);
int __wrap_lj_ctype_fin_istab(global_State *g, GCtab *t)
{
  if (t == retry_target && ++membership_calls >= retry_at) {
    membership_retries++;
    return LJ_CTYPE_FIN_RETRY;
  }
  return __real_lj_ctype_fin_istab(g, t);
}
#endif

enum {
  PUBLIC_ROOT,
  PUBLIC_TABLE_G,
  PUBLIC_TABLE_L,
  PUBLIC_TV_G,
  PUBLIC_TV_ROOT
};

static void drain_all(global_State *g, TGState *tg)
{
  uint32_t turn;
  (void)lj_gc2_flush_ssb(g, tg);
  for (turn = 0; turn < 64u && !lj_gc2_test_ssb_empty(g); turn++)
    (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
}

/* Isolate the SWEEP publication contract without running destructive sweep
** work or a broad root handshake which could independently mark the child. */
static LJGC2ActivationSnap enter_sweep(global_State *g)
{
  LJGC2ActivationSnap mark, weak, sweep;
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  mark = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(mark.state == LJ_GC2_ACT_MARK);
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &mark,
           mark.mark_epoch, LJ_GC2_ACT_WEAK, &weak) ==
         LJ_GC2_TRANSITION_OK);
  gc2_phase_rel(g, LJ_GC2_WEAK);
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &weak,
           mark.mark_epoch, LJ_GC2_ACT_SWEEP_OPEN, &sweep) ==
         LJ_GC2_TRANSITION_OK);
  gc2_phase_rel(g, LJ_GC2_SWEEP);
  gc2_jit_phase_gate_rel(g, 0);
  assert(!lj_gc2_activation_reclaim_veto(g));
  return sweep;
}

static void leave_sweep(global_State *g, const LJGC2ActivationSnap *sweep)
{
  LJGC2ActivationSnap idle;
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  gc2_phase_rel(g, LJ_GC2_IDLE);
  assert(lj_gc2_activation_try_abandon_sweep_open(
           &g->gc2.activation, sweep, &idle) == LJ_GC2_TRANSITION_OK);
  assert(idle.state == LJ_GC2_ACT_IDLE);
}

static void test_public_rescan(int publication, uint32_t requests, int cyclic)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtab *parent, *child, *grandchild;
  LJGC2TabStamp *stamp;
  LJGC2ActivationSnap sweep;
  GCRef *next;
  uint64_t before_raw;
  uint64_t drained0;
  uint32_t request;
  TValue value;
  int child_marked;

  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  lua_createtable(L, 2, 0);
  parent = tabV(L->top - 1);
  if (cyclic) {
    assert(lj_tab_asize_acq(parent) > 1);
    copyTVrel(L, &lj_tab_array_acq(parent)[1], L->top - 1);
  }
  lua_createtable(L, 1, 0);
  child = tabV(L->top - 1);
  lua_newtable(L);
  grandchild = tabV(L->top - 1);
  /* Before the synthetic cycle, build this private edge without creating an
  ** IDLE table-store rescan that would independently mark child at startup. */
  assert(lj_tab_asize_acq(child) > 0);
  copyTVrel(L, &lj_tab_array_acq(child)[0], L->top - 1);
  lua_pop(L, 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  drain_all(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  assert(lj_gc2_test_table_scan_current(g, parent));
  assert(!(lj_obj_gcflags(obj2gco(parent)) & LJ_GC_NEEDSCAN));
  assert(lj_tab_gc2_rescan_state_acq(parent) == LJ_TAB_RESCAN_NONE);
  assert(gc2_table_rescan_pending_acq(g) == 0);
  stamp = lj_arena_gc2_stamp_acq(parent);
  assert(stamp != NULL);
  assert(lj_gc2_table_token_state(la_load64_acq(&stamp->token.control)) ==
         LJ_GC2_TABLE_TOKEN_NONE);

  sweep = enter_sweep(g);
  before_raw = la_load64_acq(&stamp->state);
  assert(lj_tab_asize_acq(parent) > 0);
  settabV(L, &value, child);
  copyTVrel(L, &lj_tab_array_acq(parent)[0], &value);
  assert(la_load64_acq(&stamp->state) == before_raw);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  /* An available active slot forces the ordinary SSB path. No full-buffer
  ** recovery lane may rescue the request and conceal a converter drop. */
  next = lj_tg_ssb_next_acq(tg);
  assert(next != NULL && lj_tg_ssb_end_acq(tg) - next >= requests);
  assert(lj_tg_ssb_free_acq(tg) != NULL);
  assert(gc2_recovery_items_acq(g) == 0);
  drained0 = gc2_ssb_items_drained_acq(g);
  for (request = 0; request < requests; request++) {
    if (publication == PUBLIC_ROOT)
      assert(lj_gc2_trace_sweep_root(g, obj2gco(parent)) == 1u);
    else if (publication == PUBLIC_TABLE_G)
      lj_gc2_barrier_tab_g(g, parent);
    else if (publication == PUBLIC_TABLE_L)
      lj_gc2_barrier_tab(L, parent);
    else if (publication == PUBLIC_TV_G)
      lj_gc2_barrier_tv_g(g, L->base);
    else
      lj_gc_pubroot(L, L->base);
    assert(lj_tg_ssb_next_acq(tg) == next + request + 1u);
    assert(gcref(next[request]) == obj2gco(parent));
  }
  assert(gc2_recovery_items_acq(g) == 0);
  assert(lj_gc2_flush_ssb(g, tg) == requests);
  assert(!lj_gc2_test_ssb_empty(g));
  drain_all(g, tg);
  assert(gc2_ssb_items_drained_acq(g) >= drained0 + requests);
  assert(gc2_recovery_items_acq(g) == 0);
  assert(gc2_recovery_huge_items_acq(g) == 0);
  assert(gc2_recovery_failed_acq(g) == 0);
  child_marked = lj_gc2_ismarked(g, obj2gco(child));
  if (child_marked != 1)
    fprintf(stderr, "public SWEEP table rescan dropped: publication=%d "
            "requests=%u cyclic=%d "
            "child_marked=%d ssb_drained=%llu scan_current=%d "
            "needscan=%u rescan_state=%u pending=%u\n",
            publication, requests, cyclic, child_marked,
            (unsigned long long)(gc2_ssb_items_drained_acq(g) - drained0),
            lj_gc2_test_table_scan_current(g, parent),
            (unsigned)(lj_obj_gcflags(obj2gco(parent)) & LJ_GC_NEEDSCAN),
            (unsigned)lj_tab_gc2_rescan_state_acq(parent),
            (unsigned)gc2_table_rescan_pending_acq(g));
  assert(child_marked == 1);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 1);
  assert(lj_gc2_test_table_scan_current(g, parent));
  assert(!(lj_obj_gcflags(obj2gco(parent)) & LJ_GC_NEEDSCAN));
  assert(lj_tab_gc2_rescan_state_acq(parent) == LJ_TAB_RESCAN_NONE);
  assert(gc2_table_rescan_pending_acq(g) == 0);
  assert(lj_gc2_table_token_state(la_load64_acq(&stamp->token.control)) ==
         LJ_GC2_TABLE_TOKEN_NONE);
  assert(!lj_gc2_activation_reclaim_veto(g));
  leave_sweep(g, &sweep);
  lua_close(L);
}

#if LJ_HASFFI
static GCtab *make_finreg(lua_State *L, FinRegGen **genp)
{
  CTState *cts;
  FinRegGen *gen;
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "local ffi = require('ffi'); "
    "return ffi.gc(ffi.new('int[1]'), function() end)") == LUA_OK);
  assert(tviscdata(L->top - 1));
  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  gen = fin_gen_head_acq(cts);
  assert(gen != NULL && fin_gen_tab_acq(gen) != NULL);
  if (genp)
    *genp = gen;
  return fin_gen_tab_acq(gen);
}

static void assert_no_membership_effect(global_State *g, TGState *tg,
                                       GCtab *fin, GCtab *other)
{
  GCRef *next = lj_tg_ssb_next_acq(tg);
  uint64_t grey = gc2_grey_pushed_acq(g);
  uint64_t marks = gc2_marks_this_round_acq(g);
  uint64_t remote = lj_arena_remote_active_acq(lj_arena_of(fin));
  uint32_t readers = gc2_smr_readers_acq(g);
  uint32_t i;
  assert(lj_gc2_ismarked(g, obj2gco(fin)) == 0);
  for (i = 0; i < 32u; i++) {
    assert(lj_ctype_fin_istab(g, fin) == LJ_CTYPE_FIN_FOUND);
    assert(lj_ctype_fin_istab(g, other) == LJ_CTYPE_FIN_MISS);
  }
  assert(lj_gc2_ismarked(g, obj2gco(fin)) == 0);
  assert(lj_tg_ssb_next_acq(tg) == next);
  assert(gc2_grey_pushed_acq(g) == grey);
  assert(gc2_marks_this_round_acq(g) == marks);
  assert(gc2_smr_readers_acq(g) == readers);
  assert(lj_arena_remote_active_acq(lj_arena_of(fin)) == remote);
  assert(gc2_recovery_items_acq(g) == 0);
  assert(gc2_recovery_failed_acq(g) == 0);
}

static void test_finreg_membership_observation(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtab *fin, *other;
  FinRegGen *gen, *scan;
  LJGC2Lease raw = { 0 };
  LJGC2ActivationSnap sweep;
  void *blocked[2];
  uint32_t i, expect;
  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L);
  tg = G2TG(g);
  fin = make_finreg(L, &gen);
  lua_newtable(L);
  other = tabV(L->top - 1);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  lj_gc2_mark_begin(g);
  drain_all(g, tg);
  /* A MISS walks the entire generation spine. Prime every raw generation
  ** under its existing lease contract before asserting that later inspection
  ** does not turn any FINREG table into a semantic root. */
  for (scan = gen; scan != NULL;) {
    assert(lj_gc2_mem_lease_acquire(g, scan, &raw) >= 0);
    scan = fin_gen_next_acq(scan);
    lj_gc2_lease_release(&raw);
  }
  assert_no_membership_effect(g, tg, fin, other);
  sweep = enter_sweep(g);
  assert_no_membership_effect(g, tg, fin, other);

  assert(gc2_smr_readers_acq(g) == 0);
  expect = LJ_GC2_SMR_OPEN;
  assert(gc2_smr_reclaiming_cas(g, &expect, LJ_GC2_SMR_META_EXCLUSIVE));
  assert(lj_ctype_fin_istab(g, fin) == LJ_CTYPE_FIN_RETRY);
  assert(gc2_smr_readers_acq(g) == 0);
  gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);

  blocked[0] = gen;
  blocked[1] = fin;
  for (i = 0; i < 2u; i++) {
    GCArena *a = lj_arena_of(blocked[i]);
    uint32_t cell = lj_arena_cellof(blocked[i]);
    assert(!lj_arena_ishuge(a));
    if (i == 0) {
      uint64_t open = 0;
      /* Plain raw storage uses the arena writer word, not GC cell lifetime. */
      assert(!(lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE));
      assert(la_cas64(&a->hdr.remote_active, &open,
        LJ_ARENA_REMOTE_SEALED|LJ_ARENA_REMOTE_PENDING, LA_ACQ_REL, LA_ACQ));
    } else {
      assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
                                       LJ_ARENA_LIFETIME_MUTATING));
    }
    assert(lj_ctype_fin_istab(g, fin) == LJ_CTYPE_FIN_RETRY);
    assert(gc2_smr_readers_acq(g) == 0);
    assert((lj_arena_remote_active_acq(a) &
            LJ_ARENA_REMOTE_COUNT_MASK) == 0);
    if (i == 0)
      la_store64_rel(&a->hdr.remote_active, 0);
    else
      assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_MUTATING,
                                       LJ_ARENA_LIFETIME_LIVE));
    assert_no_membership_effect(g, tg, fin, other);
  }
  fin_gen_tab_disable_rel(fin);
  assert(lj_ctype_fin_istab(g, fin) == LJ_CTYPE_FIN_MISS);
  fin_gen_tab_enable_rel(fin);
  assert_no_membership_effect(g, tg, fin, other);
  leave_sweep(g, &sweep);
  lua_close(L);
}

static int weak_snapshot_has(global_State *g, GCtab *t)
{
  uint32_t i, n = lj_gc2_test_weak_snapshot_count(g);
  for (i = 0; i < n; i++)
    if (lj_gc2_test_weak_snapshot_tab(g, i) == t)
      return 1;
  return 0;
}

static void test_finreg_membership_publication(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtab *fin;
  GCRef *next;
  LJGC2ActivationSnap sweep;
  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L);
  tg = G2TG(g);
  fin = make_finreg(L, NULL);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  lj_gc2_mark_begin(g);
  (void)lj_gc2_markobj(g, obj2gco(fin));
  drain_all(g, tg);
  assert(lj_gc2_test_table_scan_current(g, fin));
  assert(lj_tab_gc2_rescan_state_acq(fin) == LJ_TAB_RESCAN_NONE);
  sweep = enter_sweep(g);
  next = lj_tg_ssb_next_acq(tg);
  assert(lj_ctype_fin_istab(g, fin) == LJ_CTYPE_FIN_FOUND);
  /* This is the self-republication that prevented a consuming FINREG scan
  ** from reaching fixpoint once public SWEEP slots stopped being dropped. */
  assert(lj_tg_ssb_next_acq(tg) == next);
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_recovery_items_acq(g) == 0);
  assert(gc2_recovery_failed_acq(g) == 0);
  leave_sweep(g, &sweep);
  lua_close(L);
}

static void test_finreg_membership_retry(uint32_t consumer)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtab *weak, *value;
  uint64_t deferred;
  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L);
  tg = G2TG(g);
  (void)make_finreg(L, NULL);
  lua_createtable(L, 1, 0);
  weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  assert(lua_setmetatable(L, -2));
  lua_newtable(L);
  value = tabV(L->top - 1);
  copyTVrel(L, &lj_tab_array_acq(weak)[0], L->top - 1);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  lj_gc2_mark_begin(g);
  drain_all(g, tg);
  lj_gc2_jit_mark_request_exit(g);
  assert(gc2_jit_phase_gate_acq(g) == 0);
  gc2_jit_mark_resume_rel(g, 0);
  assert(!lj_gc2_test_table_scan_current(g, weak));
  assert(!weak_snapshot_has(g, weak));
  assert(gc2_table_rescan_pending_acq(g) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(value)) == 0);
  assert(lj_gc2_test_table_rescan_set(g, weak));
  assert(lj_gc2_test_grey_push(g, obj2gco(weak)));
  retry_target = weak;
  retry_at = consumer;
  membership_calls = membership_retries = 0;
  deferred = gc2_deferred_epoch_acq(g);
  assert(lj_gc2_worker_drain(g, 1) == 1u);
  assert(membership_calls == consumer && membership_retries == 1u);
  assert(gc2_deferred_epoch_acq(g) > deferred);
  assert(!lj_gc2_test_table_scan_current(g, weak));
  assert(!weak_snapshot_has(g, weak));
  assert(lj_tab_gc2_rescan_state_acq(weak) == LJ_TAB_RESCAN_COUNTED);
  assert(gc2_table_rescan_pending_acq(g) == 1u);
  assert(!lj_gc2_test_ssb_empty(g));
  assert(gc2_smr_readers_acq(g) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(value)) == 0);
  retry_target = NULL;
  drain_all(g, tg);
  assert(lj_gc2_test_table_scan_current(g, weak));
  assert(weak_snapshot_has(g, weak));
  assert((lj_obj_gcflags(obj2gco(weak)) & LJ_GC_WEAK) == LJ_GC_WEAKVAL);
  assert(lj_gc2_ismarked(g, obj2gco(value)) == 0);
  assert(lj_tab_gc2_rescan_state_acq(weak) == LJ_TAB_RESCAN_NONE);
  assert(gc2_table_rescan_pending_acq(g) == 0);
  lj_gc2_cycle_to_idle(g);
  lua_close(L);
}
#endif

int main(int argc, char **argv)
{
  int publication, cyclic;
  uint32_t requests;
  int all = argc == 1;
  assert(argc <= 2);
  assert(all || strcmp(argv[1], "public") == 0 ||
         strcmp(argv[1], "observe") == 0 ||
         strcmp(argv[1], "publish") == 0 || strcmp(argv[1], "retry") == 0);
  if (all || strcmp(argv[1], "public") == 0)
    for (publication = PUBLIC_ROOT; publication <= PUBLIC_TV_ROOT; publication++)
      for (requests = 1; requests <= 2; requests++)
        for (cyclic = 0; cyclic <= 1; cyclic++)
          test_public_rescan(publication, requests, cyclic);
#if LJ_HASFFI
  if (all || strcmp(argv[1], "observe") == 0)
    test_finreg_membership_observation();
  if (all || strcmp(argv[1], "publish") == 0)
    test_finreg_membership_publication();
  if (all || strcmp(argv[1], "retry") == 0)
    for (requests = 1; requests <= 3u; requests++)
      test_finreg_membership_retry(requests);
#endif
  puts("t-gc2-sweep-public-table-rescan OK: public SSB requests preserve "
       "new table child graphs; FINREG inspection observes and retries");
  return 0;
}
