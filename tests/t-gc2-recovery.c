/*
** Deterministic no-drop recovery tests for GC2 queue saturation.
*/

#ifndef LJ_GC2_TEST_HELPERS
#error "t-gc2-recovery requires LJ_GC2_TEST_HELPERS"
#endif

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tg.h"

typedef struct RecoveryFixture {
  lua_State *L;
  global_State *g;
  TGState *tg;
} RecoveryFixture;

typedef struct RecoveryPublishCtx {
  global_State *g;
  GCobj *o;
  int result;
} RecoveryPublishCtx;

typedef struct RecoveryDrainCtx {
  global_State *g;
  uint32_t limit;
  uint32_t result;
} RecoveryDrainCtx;

static RecoveryFixture recovery_fixture_open(void)
{
  RecoveryFixture f;
  f.L = luaL_newstate();
  assert(f.L != NULL);
  lua_gc(f.L, LUA_GCSTOP, 0);
  f.g = G(f.L);
  f.tg = G2TG(f.g);
  assert(f.tg != NULL);
  assert(gc2_recovery_items_acq(f.g) == 0);
  return f;
}

static void recovery_fixture_close(RecoveryFixture *f)
{
  assert(f != NULL && f->L != NULL);
  assert(gc2_recovery_items_acq(f->g) == 0);
  if (gc2_phase_acq(f->g) != LJ_GC2_IDLE)
    lj_gc2_cycle_to_idle(f->g);
  lua_close(f->L);
  f->L = NULL;
}

static void recovery_drain_all(global_State *g, TGState *tg)
{
  uint32_t i;
  (void)lj_gc2_flush_ssb(g, tg);
  for (i = 0; i < 64u && !lj_gc2_test_ssb_empty(g); i++)
    (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
}

static void recovery_wait_paused(uint32_t stage)
{
  while (lj_gc2_test_recovery_paused() != stage)
    la_cpu_pause();
}

static void *recovery_publish_thread(void *arg)
{
  RecoveryPublishCtx *ctx = (RecoveryPublishCtx *)arg;
  ctx->result = lj_gc2_test_recovery_publish(ctx->g, ctx->o);
  return NULL;
}

static void *recovery_drain_thread(void *arg)
{
  RecoveryDrainCtx *ctx = (RecoveryDrainCtx *)arg;
  ctx->result = lj_gc2_test_recovery_drain(ctx->g, ctx->limit);
  return NULL;
}

static void *gc_worker_drain_thread(void *arg)
{
  RecoveryDrainCtx *ctx = (RecoveryDrainCtx *)arg;
  ctx->result = lj_gc2_worker_drain(ctx->g, ctx->limit);
  return NULL;
}

static void recovery_mark_table(RecoveryFixture *f, GCtab *t)
{
  assert(lj_gc2_markobj(f->g, obj2gco(t)) == 1);
  recovery_drain_all(f->g, f->tg);
  assert(lj_gc2_ismarked(f->g, obj2gco(t)) == 1);
}

static void recovery_clean_fixpoint(RecoveryFixture *f)
{
  uint32_t i;
  for (i = 0; i < 64u; i++) {
    if (lj_gc2_fixpoint_round(f->g, f->L, ~(uint32_t)0))
      return;
  }
  assert(0 && "GC2 failed to reach a clean MARK fixpoint");
}

static void recovery_make_weak_table(lua_State *L, GCtab **weak,
				     GCtab **key, GCtab **value)
{
  lua_newtable(L);
  *weak = tabV(L->top - 1);
  lua_newtable(L);
  *key = tabV(L->top - 1);
  lua_newtable(L);
  *value = tabV(L->top - 1);
  lua_pushvalue(L, -2);
  lua_pushvalue(L, -2);
  lua_settable(L, -5);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  lua_setmetatable(L, -4);
}

static int recovery_weak_entry_is_nil(lua_State *L, GCtab *weak,
				       GCtab *key)
{
  TValue k;
  settabV(L, &k, key);
  return tvisnil(lj_tab_get(L, weak, &k));
}

static void test_full_active_ssb_fallback(void)
{
  RecoveryFixture f = recovery_fixture_open();
  GC2SSBNode *active, *held;
  GCRef *base, *end;
  GCtab *parent, *child;
  GCstr *filler;
  uint64_t recovery_published0, recovery_drained0;
  uint64_t ssb_published0, ssb_drained0;
  uint32_t i;

  lua_createtable(f.L, 1, 0);
  parent = tabV(f.L->top - 1);
  lua_newtable(f.L);
  child = tabV(f.L->top - 1);

  lj_gc2_mark_begin(f.g);
  recovery_mark_table(&f, parent);
  assert(lj_gc2_ismarked(f.g, obj2gco(child)) == 0);

  held = lj_tg_ssb_free_pop(f.tg);
  assert(held != NULL);
  assert(lj_tg_ssb_free_acq(f.tg) == NULL);
  active = lj_tg_ssb_active_acq(f.tg);
  base = lj_tg_ssb_base_acq(f.tg);
  end = lj_tg_ssb_end_acq(f.tg);
  assert(active != NULL && base == active->slot);
  assert(end == base + TG_GC2_SSB_SLOTS);

  lua_pushliteral(f.L, "gc2 full SSB filler");
  filler = strV(f.L->top - 1);
  for (i = 0; i < TG_GC2_SSB_SLOTS; i++)
    assert(lj_gc2_test_ssb_push(f.g, obj2gco(filler)) == 1);
  assert(lj_tg_ssb_next_acq(f.tg) == end);
  assert(gcref_acq(active->slot[0]) == obj2gco(filler));
  assert(gcref_acq(active->slot[TG_GC2_SSB_SLOTS - 1u]) ==
	 obj2gco(filler));

  recovery_published0 = gc2_recovery_published_acq(f.g);
  recovery_drained0 = gc2_recovery_drained_acq(f.g);
  ssb_published0 = gc2_ssb_items_published_acq(f.g);
  ssb_drained0 = gc2_ssb_items_drained_acq(f.g);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_IDLE);

  settabV(f.L, &lj_tab_array_acq(parent)[0], child);
  lj_gc2_barrier_tab_g(f.g, parent);

  /* With no replacement node and no published node to recycle, the full
  ** active SSB remains the source of truth and the new identity falls back to
  ** the allocation-free recovery plane. */
  assert(lj_tg_ssb_active_acq(f.tg) == active);
  assert(lj_tg_ssb_next_acq(f.tg) == end);
  assert(lj_gc2_ssb_count_acq(active) == 0);
  assert(gc2_ssb_head_acq(f.g) == NULL);
  assert(gc2_ssb_drain_acq(f.g) == NULL);
  assert(gcref_acq(active->slot[0]) == obj2gco(filler));
  assert(gcref_acq(active->slot[TG_GC2_SSB_SLOTS - 1u]) ==
	 obj2gco(filler));
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_published_acq(f.g) == recovery_published0 + 1u);
  assert(gc2_ssb_items_published_acq(f.g) == ssb_published0);
  assert(!lj_gc2_test_ssb_empty(f.g));

  lj_tg_ssb_free_push(f.tg, held);
  assert(lj_gc2_flush_ssb(f.g, f.tg) == TG_GC2_SSB_SLOTS);
  recovery_drain_all(f.g, f.tg);
  assert(lj_gc2_ismarked(f.g, obj2gco(child)) == 1);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_drained_acq(f.g) == recovery_drained0 + 1u);
  assert(gc2_ssb_items_published_acq(f.g) ==
	 ssb_published0 + TG_GC2_SSB_SLOTS);
  assert(gc2_ssb_items_drained_acq(f.g) ==
	 ssb_drained0 + TG_GC2_SSB_SLOTS);

  recovery_fixture_close(&f);
}

static void test_grey_growth_transaction(void)
{
  RecoveryFixture f = recovery_fixture_open();
  RecoveryDrainCtx drain = {0};
  pthread_t thread;
  GC2SSBNode *published;
  GCtab *parent, *child;
  GCstr *filler;
  uint64_t recovery_published0, recovery_drained0, ssb_drained0;

  lua_createtable(f.L, 1, 0);
  parent = tabV(f.L->top - 1);
  lua_newtable(f.L);
  child = tabV(f.L->top - 1);
  settabV(f.L, &lj_tab_array_acq(parent)[0], child);
  lua_pushliteral(f.L, "gc2 tiny grey filler");
  filler = strV(f.L->top - 1);

  lj_gc2_mark_begin(f.g);
  assert(gc2_grey_stack_acq(f.g) == f.g->gc2.grey_embedded);
  assert(gc2_grey_top_acq(f.g) == gc2_grey_bottom_acq(f.g));
  gc2_grey_top_store_rlx(f.g, 0);
  gc2_grey_bottom_store_rlx(f.g, 0);
  gc2_grey_capacity_rel(f.g, 1);
  setgcref(f.g->gc2.grey_embedded[0], obj2gco(filler));
  la_fence_rel();
  la_store64_rel(&f.g->gc2.grey_bottom, 1);

  assert(lj_gc2_test_ssb_push(f.g, obj2gco(parent)) == 1);
  assert(lj_gc2_flush_ssb(f.g, f.tg) == 1);
  published = gc2_ssb_head_acq(f.g);
  assert(published != NULL);
  assert(lj_gc2_ssb_count_acq(published) == 1);
  assert(gcref_acq(published->slot[0]) == obj2gco(parent));

  recovery_published0 = gc2_recovery_published_acq(f.g);
  recovery_drained0 = gc2_recovery_drained_acq(f.g);
  ssb_drained0 = gc2_ssb_items_drained_acq(f.g);
  lj_gc2_test_recovery_fail_grey_grow(1);
  lj_gc2_test_recovery_pause(LJ_GC2_RECOVERY_TEST_SSB_COMMITTED);
  drain.g = f.g;
  drain.limit = 1;
  assert(pthread_create(&thread, NULL, gc_worker_drain_thread, &drain) == 0);
  recovery_wait_paused(LJ_GC2_RECOVERY_TEST_SSB_COMMITTED);

  /* The failed grow has already committed the exact destination identity,
  ** while the published SSB slot and count are deliberately still intact. */
  assert(gc2_ssb_consumer_active_acq(f.g) == 1);
  assert(lj_gc2_ssb_count_acq(published) == 1);
  assert(gcref_acq(published->slot[0]) == obj2gco(parent));
  assert(gc2_grey_top_acq(f.g) == 0);
  assert(gc2_grey_bottom_acq(f.g) == 1);
  assert(gcref_acq(f.g->gc2.grey_embedded[0]) == obj2gco(filler));
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_published_acq(f.g) == recovery_published0 + 1u);
  assert(gc2_ssb_items_drained_acq(f.g) == ssb_drained0);
  assert(!lj_gc2_test_ssb_empty(f.g));

  lj_gc2_test_recovery_release();
  assert(pthread_join(thread, NULL) == 0);
  assert(drain.result == 1);
  assert(lj_gc2_ssb_count_acq(published) == 0);
  assert(gcref_acq(published->slot[0]) == NULL);
  assert(gc2_ssb_items_drained_acq(f.g) == ssb_drained0 + 1u);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_PENDING);

  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_drained_acq(f.g) == recovery_drained0 + 1u);
  assert(lj_gc2_ismarked(f.g, obj2gco(child)) == 1);
  recovery_drain_all(f.g, f.tg);

  assert(gc2_grey_stack_acq(f.g) == f.g->gc2.grey_embedded);
  gc2_grey_top_store_rlx(f.g, 0);
  gc2_grey_bottom_store_rlx(f.g, 0);
  gc2_grey_capacity_rel(f.g, LJ_GC2_GREY_EMBEDDED);
  recovery_fixture_close(&f);
}

static void test_reservation_gap_blocks_mark_close(void)
{
  RecoveryFixture f = recovery_fixture_open();
  RecoveryPublishCtx publish = {0};
  pthread_t thread;
  GCtab *parent;
  uint64_t published0, drained0, mark_to_weak0;
  uint32_t i;

  lua_newtable(f.L);
  parent = tabV(f.L->top - 1);
  lj_gc2_mark_begin(f.g);
  recovery_clean_fixpoint(&f);
  assert(lj_gc2_test_ssb_empty(f.g));
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_IDLE);

  published0 = gc2_recovery_published_acq(f.g);
  drained0 = gc2_recovery_drained_acq(f.g);
  mark_to_weak0 = gc2_mark_to_weak_acq(f.g);
  publish.g = f.g;
  publish.o = obj2gco(parent);
  lj_gc2_test_recovery_pause(LJ_GC2_RECOVERY_TEST_RESERVED);
  assert(pthread_create(&thread, NULL, recovery_publish_thread, &publish) == 0);
  recovery_wait_paused(LJ_GC2_RECOVERY_TEST_RESERVED);

  /* The count reservation is the close veto for the interval before the exact
  ** arena state becomes visible. */
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_published_acq(f.g) == published0);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_ssb_head_acq(f.g) == NULL);
  assert(gc2_ssb_drain_acq(f.g) == NULL);
  assert(!lj_gc2_test_ssb_empty(f.g));
  assert(lj_gc2_mark_complete(f.g, f.L, 1, 1) == 0);
  assert(gc2_phase_acq(f.g) == LJ_GC2_MARK);
  assert(gc2_mark_to_weak_acq(f.g) == mark_to_weak0);

  lj_gc2_test_recovery_release();
  assert(pthread_join(thread, NULL) == 0);
  assert(publish.result == 1);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_published_acq(f.g) == published0 + 1u);

  for (i = 0; i < 64u && gc2_phase_acq(f.g) == LJ_GC2_MARK; i++)
    (void)lj_gc2_mark_complete(f.g, f.L, 4, ~(uint32_t)0);
  assert(gc2_phase_acq(f.g) == LJ_GC2_WEAK);
  assert(gc2_mark_to_weak_acq(f.g) == mark_to_weak0 + 1u);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_drained_acq(f.g) == drained0 + 1u);

  recovery_fixture_close(&f);
}

static void test_claimed_redirty_replay(void)
{
  RecoveryFixture f = recovery_fixture_open();
  RecoveryDrainCtx drain = {0};
  pthread_t thread;
  GCtab *parent, *child;
  uint64_t published0, redirtied0, drained0;

  lua_createtable(f.L, 1, 0);
  parent = tabV(f.L->top - 1);
  lua_newtable(f.L);
  child = tabV(f.L->top - 1);
  lj_gc2_mark_begin(f.g);
  recovery_mark_table(&f, parent);
  assert(lj_gc2_ismarked(f.g, obj2gco(child)) == 0);

  published0 = gc2_recovery_published_acq(f.g);
  redirtied0 = gc2_recovery_redirtied_acq(f.g);
  drained0 = gc2_recovery_drained_acq(f.g);
  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(parent)) == 1);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(f.g) == 1u);

  lj_gc2_test_recovery_pause(LJ_GC2_RECOVERY_TEST_PRE_COMPLETE);
  drain.g = f.g;
  drain.limit = 1;
  assert(pthread_create(&thread, NULL, recovery_drain_thread, &drain) == 0);
  recovery_wait_paused(LJ_GC2_RECOVERY_TEST_PRE_COMPLETE);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_CLAIMED);
  assert(gc2_recovery_items_acq(f.g) == 1u);

  /* Mutate only after the first traversal has finished. The racing
  ** publication must retain the same identity for an additional bounded pass. */
  settabV(f.L, &lj_tab_array_acq(parent)[0], child);
  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(parent)) == 1);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_REDIRTY);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_published_acq(f.g) == published0 + 1u);
  assert(gc2_recovery_redirtied_acq(f.g) == redirtied0 + 1u);
  assert(gc2_recovery_drained_acq(f.g) == drained0);
  assert(lj_gc2_ismarked(f.g, obj2gco(child)) == 0);

  lj_gc2_test_recovery_release();
  assert(pthread_join(thread, NULL) == 0);
  assert(drain.result == 1);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_drained_acq(f.g) == drained0);
  assert(lj_gc2_ismarked(f.g, obj2gco(child)) == 0);

  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(parent)) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_drained_acq(f.g) == drained0 + 1u);
  assert(lj_gc2_ismarked(f.g, obj2gco(child)) == 1);
  recovery_drain_all(f.g, f.tg);

  recovery_fixture_close(&f);
}

static void test_weak_clear_recovery_gate(void)
{
  RecoveryFixture f = recovery_fixture_open();
  RecoveryPublishCtx publish = {0};
  pthread_t thread;
  GCtab *weak, *key, *value, *blocker;
  uint64_t cursor0;

  recovery_make_weak_table(f.L, &weak, &key, &value);
  lua_newtable(f.L);
  blocker = tabV(f.L->top - 1);

  lj_gc2_mark_begin(f.g);
  assert(lj_gc2_markobj(f.g, obj2gco(weak)) == 1);
  recovery_drain_all(f.g, f.tg);
  assert(lj_gc2_test_weak_snapshot_count(f.g) == 1u);
  assert(lj_gc2_ismarked(f.g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(f.g, obj2gco(value)) == 0);
  assert(!recovery_weak_entry_is_nil(f.L, weak, key));

  lj_gc2_mark_to_weak(f.g);
  assert(gc2_phase_acq(f.g) == LJ_GC2_WEAK);
  gc2_weak_root_scanned_rel(f.g, 1);
  gc2_weak_mark_closed_rel(f.g, 1);
  cursor0 = gc2_weak_clear_cursor_acq(f.g);
  assert(cursor0 == 0);

  publish.g = f.g;
  publish.o = obj2gco(blocker);
  lj_gc2_test_recovery_pause(LJ_GC2_RECOVERY_TEST_RESERVED);
  assert(pthread_create(&thread, NULL, recovery_publish_thread, &publish) == 0);
  recovery_wait_paused(LJ_GC2_RECOVERY_TEST_RESERVED);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(blocker)) ==
	 LJ_ARENA_RECOVERY_IDLE);

  /* Weak clearing is irreversible. The reserve-before-state interval must
  ** leave both its cursor and the clearable table slot untouched. */
  assert(lj_gc2_test_weak_drain(f.g, 1) == 0);
  assert(gc2_weak_clear_cursor_acq(f.g) == cursor0);
  assert(!recovery_weak_entry_is_nil(f.L, weak, key));

  lj_gc2_test_recovery_release();
  assert(pthread_join(thread, NULL) == 0);
  assert(publish.result == 1);
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(blocker)) ==
	 LJ_ARENA_RECOVERY_IDLE);

  /* A sticky classification failure has no drainable side identity, but is
  ** still an exact fail-closed veto for the same weak-clear boundary. */
  gc2_recovery_failed_rel(f.g, 1);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(lj_gc2_test_weak_drain(f.g, 1) == 0);
  assert(gc2_weak_clear_cursor_acq(f.g) == cursor0);
  assert(!recovery_weak_entry_is_nil(f.L, weak, key));
  gc2_recovery_failed_rel(f.g, 0);

  assert(lj_gc2_test_weak_drain(f.g, 1) == 1);
  assert(gc2_weak_clear_cursor_acq(f.g) == cursor0 + 1u);
  assert(recovery_weak_entry_is_nil(f.L, weak, key));
  recovery_fixture_close(&f);
}

int main(void)
{
  test_full_active_ssb_fallback();
  test_grey_growth_transaction();
  test_reservation_gap_blocks_mark_close();
  test_claimed_redirty_replay();
  test_weak_clear_recovery_gate();
  printf("t-gc2-recovery OK: full-SSB fallback, failed grey growth, transactional SSB consumption, reservation/weak-clear vetoes, and REDIRTY replay verified\n");
  return 0;
}
