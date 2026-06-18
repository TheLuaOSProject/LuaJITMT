/*
** Focused test for GC2 allocation accounting bridge.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  void *p;
  uint64_t total;
  uint64_t epoch0;
  uint64_t cycle_requests0, cycle_starts0;
  uint64_t major_starts0, minor_requests0;
  uint64_t minor_deferred0;
  uint64_t remembered_drained0;
  uint64_t remembered_pushed0, remembered_filtered0;
  uint64_t assist_runs0, assist_grey0, assist_ssb0;
  uint64_t assist_weak0;
  uint64_t weak_clear_tables0, weak_clear_cleared0;
  GCtab *parent, *child, *grandchild;
  GCtab *weak, *key, *val;

  assert(L != NULL);
  g = G(L);
  tg = L2TG(L);
  assert(g != NULL);
  assert(tg != NULL);
  assert(la_load32_acq(&g->gc2.gcpause_pct) == 100);
  assert(la_load32_acq(&g->gc2.cycle_leader) == 0);
  assert(la_load64_acq(&g->gc2.cycle_requests) == 0);
  assert(la_load64_acq(&g->gc2.cycle_starts) == 0);
  assert(la_load64_acq(&g->gc2.major_cycle_starts) == 0);
  assert(la_load64_acq(&g->gc2.minor_cycle_requests) == 0);
  assert(la_load32_acq(&g->gc2.cycle_minor_requested) == 0);
  assert(la_load32_acq(&g->gc2.cycle_sweep_minor) == 0);
  assert(la_load32_acq(&g->gc2.minor_sweep_enabled) == 0);
  assert(la_load64_acq(&g->gc2.minor_sweep_deferred) == 0);
  assert(la_load64_acq(&g->gc2.minor_sweep_arenas) == 0);
  assert(la_load32_acq(&g->gc2.force_major) == 0);
  assert(la_load64_acq(&g->gc2.remembered_filtered) == 0);
  assert(la_load64_acq(&g->gc2.remembered_drained) == 0);
  assert(la_load32_acq(&g->gc2.assist_shift) ==
	 lj_gc2_assist_shift_from_stepmul(g->gc.stepmul));
  assert(la_load64_acq(&g->gc2.trigger_bytes) >= LJ_GC2_ACCT_FLUSH);
  assert(la_load64_acq(&g->gc2.hard_bytes) ==
	 2u * la_load64_acq(&g->gc2.trigger_bytes));
  assert(la_load64_acq(&g->gc2.assist_runs) == 0);
  assert(la_load64_acq(&g->gc2.assist_grey_drained) == 0);
  assert(la_load64_acq(&g->gc2.assist_ssb_converted) == 0);
  assert(la_load64_acq(&g->gc2.assist_weak_drained) == 0);
  assert(la_load64_acq(&g->gc2.jit_hard_checks) == 0);
  assert(la_load32_acq(&g->gc2.assist_active) == 0);

  (void)lua_gc(L, LUA_GCSETPAUSE, 150);
  assert(la_load32_acq(&g->gc2.gcpause_pct) == 150);
  assert(la_load64_acq(&g->gc2.trigger_bytes) >= LJ_GC2_ACCT_FLUSH);
  assert(la_load64_acq(&g->gc2.hard_bytes) ==
	 2u * la_load64_acq(&g->gc2.trigger_bytes));
  (void)lua_gc(L, LUA_GCSETSTEPMUL, 400);
  assert(la_load32_acq(&g->gc2.assist_shift) ==
	 lj_gc2_assist_shift_from_stepmul(400));

  (void)lj_gc2_flush_alloc(g, tg);
  (void)la_xchg64_acqrel(&g->gc2.alloc_since_trigger, 0);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 0);

  p = lj_mem_realloc(L, NULL, 0, 128);
  assert(p != NULL);
  assert(la_load64_acq(&tg->local_total) == 128);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 0);
  assert(lj_gc2_flush_alloc(g, tg) == 128);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 128);

  lj_mem_free(g, p, 128);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 128);

  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH - 1u);
  assert(la_load64_acq(&tg->local_total) == LJ_GC2_ACCT_FLUSH - 1u);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 128);
  lj_gc2_account_alloc(g, tg, 1);
  assert(la_load64_acq(&tg->local_total) == 0);
  total = la_load64_acq(&g->gc2.alloc_since_trigger);
  assert(total == 128 + LJ_GC2_ACCT_FLUSH);

  lj_gc2_account_alloc(g, tg, 7);
  assert(la_load64_acq(&tg->local_total) == 7);
  epoch0 = la_load64_acq(&g->gc2.hs_epoch);
  assert(lj_gc2_handshake(g, LJ_GC2_HS_REDISPATCH) == 1);
  assert(la_load64_acq(&g->gc2.hs_epoch) == epoch0 + 1u);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == total + 7);

  lj_gc_threshold_store(g, g->gc.total + 4u * LJ_GC2_ACCT_FLUSH);
  la_store64_rel(&g->gc2.trigger_bytes, 1);
  (void)la_xchg64_acqrel(&g->gc2.alloc_since_trigger, 0);
  cycle_requests0 = la_load64_acq(&g->gc2.cycle_requests);
  cycle_starts0 = la_load64_acq(&g->gc2.cycle_starts);
  major_starts0 = la_load64_acq(&g->gc2.major_cycle_starts);
  minor_requests0 = la_load64_acq(&g->gc2.minor_cycle_requests);
  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(lj_gc_threshold_load(g) == g->gc.total);
  assert(la_load32_acq(&g->gc2.cycle_leader) == tg->tid);
  assert(la_load64_acq(&g->gc2.cycle_requests) == cycle_requests0 + 1u);
  assert(la_load64_acq(&g->gc2.cycle_starts) == cycle_starts0);
  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH);
  assert(la_load64_acq(&g->gc2.cycle_requests) == cycle_requests0 + 1u);
  lj_gc2_legacy_mark_begin(g);
  assert(la_load32_acq(&g->gc2.cycle_leader) == 0);
  assert(la_load64_acq(&g->gc2.cycle_starts) == cycle_starts0 + 1u);
  assert(la_load64_acq(&g->gc2.major_cycle_starts) == major_starts0 + 1u);
  assert(la_load64_acq(&g->gc2.minor_cycle_requests) == minor_requests0);
  assert(la_load32_acq(&g->gc2.cycle_minor_requested) == 0);
  lj_gc2_legacy_cycle_end(g);

  la_store32_rel(&g->gc2.generational, 1);
  major_starts0 = la_load64_acq(&g->gc2.major_cycle_starts);
  minor_requests0 = la_load64_acq(&g->gc2.minor_cycle_requests);
  minor_deferred0 = la_load64_acq(&g->gc2.minor_sweep_deferred);
  lj_gc2_legacy_mark_begin(g);
  assert(la_load64_acq(&g->gc2.major_cycle_starts) == major_starts0 + 1u);
  assert(la_load64_acq(&g->gc2.minor_cycle_requests) ==
	 minor_requests0 + 1u);
  assert(la_load32_acq(&g->gc2.cycle_minor_requested) == 1);
  assert(la_load32_acq(&g->gc2.cycle_sweep_minor) == 0);
  assert(la_load64_acq(&g->gc2.minor_sweep_deferred) ==
	 minor_deferred0 + 1u);
  lj_gc2_legacy_cycle_end(g);

  major_starts0 = la_load64_acq(&g->gc2.major_cycle_starts);
  minor_requests0 = la_load64_acq(&g->gc2.minor_cycle_requests);
  lj_gc2_force_major(g);
  lj_gc2_legacy_mark_begin(g);
  assert(la_load64_acq(&g->gc2.major_cycle_starts) == major_starts0 + 1u);
  assert(la_load64_acq(&g->gc2.minor_cycle_requests) == minor_requests0);
  assert(la_load32_acq(&g->gc2.cycle_minor_requested) == 0);
  assert(la_load32_acq(&g->gc2.force_major) == 0);
  lj_gc2_legacy_cycle_end(g);

  lua_settop(L, 0);
  lj_gc2_set_generational(g, 1);
  assert(la_load32_acq(&g->gc2.generational) == 1);
  assert(tg->mark_active == 1);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  assert(lj_gc2_ssb_push(g, obj2gco(parent)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  remembered_drained0 = la_load64_acq(&g->gc2.remembered_drained);
  lj_gc2_legacy_mark_begin(g);
  assert(la_load32_acq(&g->gc2.cycle_minor_requested) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ssb_empty(g));
  assert(la_load64_acq(&g->gc2.remembered_drained) >=
	 remembered_drained0 + 1u);
  lj_gc2_legacy_cycle_end(g);
  lj_gc2_set_generational(g, 0);
  lua_settop(L, 0);

  lj_gc2_set_generational(g, 1);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 1);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_newtable(L);
  grandchild = tabV(L->top - 1);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(child)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  remembered_pushed0 = la_load64_acq(&g->gc2.remembered_pushed);
  remembered_filtered0 = la_load64_acq(&g->gc2.remembered_filtered);
  lj_gc2_barrier_obj_pair(L, obj2gco(parent), obj2gco(grandchild));
  assert(la_load64_acq(&g->gc2.remembered_pushed) == remembered_pushed0 + 1u);
  lj_gc2_barrier_obj_pair(L, obj2gco(parent), obj2gco(child));
  assert(la_load64_acq(&g->gc2.remembered_filtered) ==
	 remembered_filtered0 + 1u);
  assert(la_load64_acq(&g->gc2.remembered_pushed) ==
	 remembered_pushed0 + 1u);
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  (void)lj_gc2_drain_ssb(g);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 0);
  lj_gc2_set_generational(g, 0);
  lua_settop(L, 0);

  lj_gc_threshold_store(g, LJ_MAX_MEM);
  (void)la_xchg64_acqrel(&g->gc2.alloc_since_trigger, 0);
  cycle_requests0 = la_load64_acq(&g->gc2.cycle_requests);
  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(lj_gc_threshold_load(g) == LJ_MAX_MEM);
  assert(la_load32_acq(&g->gc2.cycle_leader) == 0);
  assert(la_load64_acq(&g->gc2.cycle_requests) == cycle_requests0);
  lj_gc_threshold_store(g, g->gc.total + 4u * LJ_GC2_ACCT_FLUSH);
  lj_gc2_update_pacing(g);

  lj_gc2_account_alloc(g, tg, 99);
  assert(la_load64_acq(&tg->local_total) == 99);
  lj_gc2_legacy_mark_begin(g);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 0);
  lj_gc2_legacy_cycle_end(g);

  lua_settop(L, 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_newtable(L);
  grandchild = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);  /* child[1] = grandchild. */
  lua_pushvalue(L, -2);
  lua_rawseti(L, -4, 1);  /* parent[1] = child. */

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  la_store64_rel(&g->gc2.hard_bytes, 1);
  la_store32_rel(&g->gc2.assist_shift, 0);
  (void)la_xchg64_acqrel(&g->gc2.alloc_since_trigger, 0);
  assist_runs0 = la_load64_acq(&g->gc2.assist_runs);
  assist_grey0 = la_load64_acq(&g->gc2.assist_grey_drained);
  assist_ssb0 = la_load64_acq(&g->gc2.assist_ssb_converted);
  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH);
  assert(tg->gc_assist == 0);
  assert(la_load32_acq(&g->gc2.assist_active) == 0);
  assert(la_load64_acq(&g->gc2.assist_runs) == assist_runs0 + 1u);
  assert(la_load64_acq(&g->gc2.assist_grey_drained) == assist_grey0 + 1u);
  assert(la_load64_acq(&g->gc2.assist_ssb_converted) == assist_ssb0 + 1u);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  assert(!lj_gc2_ssb_empty(g));
  (void)lj_gc2_drain_ssb(g);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 1);
  lj_gc2_legacy_cycle_end(g);

  lua_settop(L, 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_newtable(L);
  grandchild = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);  /* child[1] = grandchild. */
  lua_pushvalue(L, -2);
  lua_rawseti(L, -4, 1);  /* parent[1] = child. */

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(!lj_gc2_ssb_empty(g));
  la_store64_rel(&g->gc2.hard_bytes, 1);
  la_store32_rel(&g->gc2.assist_shift, 0);
  (void)la_xchg64_acqrel(&g->gc2.alloc_since_trigger, 0);
  assist_runs0 = la_load64_acq(&g->gc2.assist_runs);
  assist_grey0 = la_load64_acq(&g->gc2.assist_grey_drained);
  assist_ssb0 = la_load64_acq(&g->gc2.assist_ssb_converted);
  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH);
  assert(tg->gc_assist == 0);
  assert(la_load32_acq(&g->gc2.assist_active) == 0);
  assert(la_load64_acq(&g->gc2.assist_runs) == assist_runs0 + 1u);
  assert(la_load64_acq(&g->gc2.assist_grey_drained) == assist_grey0 + 1u);
  assert(la_load64_acq(&g->gc2.assist_ssb_converted) == assist_ssb0 + 1u);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  assert(!lj_gc2_ssb_empty(g));
  (void)lj_gc2_drain_ssb(g);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 1);
  lj_gc2_legacy_cycle_end(g);

  lua_settop(L, 0);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  key = tabV(L->top - 1);
  lua_newtable(L);
  val = tabV(L->top - 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_settable(L, 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  (void)lj_gc2_drain_ssb(g);
  assert(lj_gc2_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  lj_gc2_legacy_weak_begin(g);
  la_store64_rel(&g->gc2.hard_bytes, 1);
  la_store32_rel(&g->gc2.assist_shift, 0);
  (void)la_xchg64_acqrel(&g->gc2.alloc_since_trigger, 0);
  assist_runs0 = la_load64_acq(&g->gc2.assist_runs);
  assist_weak0 = la_load64_acq(&g->gc2.assist_weak_drained);
  weak_clear_tables0 = la_load64_acq(&g->gc2.weak_clear_tables);
  weak_clear_cleared0 = la_load64_acq(&g->gc2.weak_clear_cleared);
  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH);
  assert(tg->gc_assist == 0);
  assert(la_load32_acq(&g->gc2.assist_active) == 0);
  assert(la_load64_acq(&g->gc2.assist_runs) == assist_runs0 + 1u);
  assert(la_load64_acq(&g->gc2.assist_weak_drained) == assist_weak0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_clear_tables) == weak_clear_tables0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_clear_cleared) ==
	 weak_clear_cleared0 + 1u);
  lua_pushvalue(L, 2);
  lua_gettable(L, 1);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);
  lj_gc2_legacy_cycle_end(g);

  lua_close(L);
  puts("t-gc2-alloc-account OK: allocation accounting flushes by threshold and safepoint");
  return 0;
}
