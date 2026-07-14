/*
** Deterministic no-drop recovery tests for GC2 queue saturation.
*/

#ifndef LJ_GC2_TEST_HELPERS
#error "t-gc2-recovery requires LJ_GC2_TEST_HELPERS"
#endif

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

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

typedef struct RecoveryFreeCtx {
  TGAlloc *alloc;
  void *p;
  size_t size;
} RecoveryFreeCtx;

static int recovery_self_cfunc(lua_State *L)
{
  (void)L;
  return 0;
}

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
  assert(gc2_recovery_huge_items_acq(f.g) == 0);
  return f;
}

static void recovery_fixture_close(RecoveryFixture *f)
{
  assert(f != NULL && f->L != NULL);
  assert(gc2_recovery_items_acq(f->g) == 0);
  assert(gc2_recovery_huge_items_acq(f->g) == 0);
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

static void *recovery_free_thread(void *arg)
{
  RecoveryFreeCtx *ctx = (RecoveryFreeCtx *)arg;
  lj_arena_free(ctx->alloc, ctx->p, ctx->size);
  return NULL;
}

static GCtab *recovery_make_unlinked_table(RecoveryFixture *f)
{
  GCtab *t;
  GCArena *a;
  uint32_t cell;
  lua_newtable(f->L);
  t = tabV(f->L->top - 1);
  assert(lj_gc_flush_root_pending(f->g) != 0);
  assert(lj_gc_unlink_root_obj(f->g, obj2gco(t)) == LJ_GC_ROOT_UNLINKED);
  lua_pop(f->L, 1);
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(lj_arena_bm_get(a->block, cell));
  assert(lj_arena_ready_get(a, cell));
  return t;
}

static GCudata *recovery_make_unlinked_huge_udata(RecoveryFixture *f)
{
  GCudata *ud;
  (void)lua_newuserdata(f->L, LJ_HUGE_THRESHOLD + 1024u);
  ud = udataV(f->L->top - 1);
  assert(sizeudata(ud) > LJ_HUGE_THRESHOLD);
  assert(lj_arena_ishuge(lj_arena_of(ud)));
  assert(lj_gc_flush_root_pending(f->g) != 0);
  assert(lj_gc_unlink_root_obj(f->g, obj2gco(ud)) == LJ_GC_ROOT_UNLINKED);
  lua_pop(f->L, 1);
  assert(lj_gc2_test_recovery_state(f->g, obj2gco(ud)) ==
	 LJ_ARENA_RECOVERY_IDLE);
  return ud;
}

static void recovery_init_closed_upvalue(global_State *g, GCupval *uv)
{
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  uv->immutable = 0;
  setnilV(&uv->tv);
  setmref(uv->v, &uv->tv);
  uv->dhash = 0;
  newwhite(g, uv);
  lj_obj_setgcwnullrel(obj2gco(uv));
}

static GCupval *recovery_new_constructing_upvalue(RecoveryFixture *f)
{
  GCupval *uv = (GCupval *)lj_mem_newgco_unlinked_nothrow(
    f->L, sizeof(GCupval));
  assert(uv != NULL);
  recovery_init_closed_upvalue(f->g, uv);
  lj_gc_publishobj_header(f->g, obj2gco(uv));
  assert(lj_arena_lifetime_state_acq(
	   lj_arena_of(uv), lj_arena_cellof(uv)) ==
	 LJ_ARENA_LIFETIME_CONSTRUCT);
  assert(lj_arena_root_state_acq(lj_arena_of(uv), lj_arena_cellof(uv)) ==
	 LJ_ARENA_ROOT_LINKING);
  return uv;
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

static void test_full_ssb_barrier_coalesces_reserved_recovery(void)
{
  RecoveryFixture f = recovery_fixture_open();
  RecoveryPublishCtx publish = {0};
  pthread_t thread;
  GC2SSBNode *active, *held;
  GCRef *base, *end;
  GCtab *parent, *child;
  GCstr *filler;
  GCArena *a;
  uint64_t recovery_published0, recovery_drained0;
  uint64_t ssb_published0, ssb_drained0;
  uint32_t cell, i;

  lua_createtable(f.L, 1, 0);
  parent = tabV(f.L->top - 1);
  lua_newtable(f.L);
  child = tabV(f.L->top - 1);
  a = lj_arena_of(parent);
  cell = lj_arena_cellof(parent);

  lj_gc2_mark_begin(f.g);
  recovery_mark_table(&f, parent);
  assert(lj_gc2_ismarked(f.g, obj2gco(child)) == 0);

  /* A generic non-destructive owner has no exact recovery reservation. It
  ** must remain distinguishable from the counted RECOVERY state, otherwise a
  ** semantic publisher could return success with no durable retry identity. */
  assert(lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_MUTATING));
  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(parent)) == 0);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_MUTATING, LJ_ARENA_LIFETIME_LIVE));

  /* Force the production table barrier through recovery: the active SSB is
  ** full and its only replacement is held outside the owner free list. */
  held = lj_tg_ssb_free_pop(f.tg);
  assert(held != NULL);
  assert(lj_tg_ssb_free_acq(f.tg) == NULL);
  active = lj_tg_ssb_active_acq(f.tg);
  base = lj_tg_ssb_base_acq(f.tg);
  end = lj_tg_ssb_end_acq(f.tg);
  assert(active != NULL && base == active->slot);
  assert(end == base + TG_GC2_SSB_SLOTS);
  lua_pushliteral(f.L, "gc2 reserved recovery filler");
  filler = strV(f.L->top - 1);
  for (i = 0; i < TG_GC2_SSB_SLOTS; i++)
    assert(lj_gc2_test_ssb_push(f.g, obj2gco(filler)) == 1);
  assert(lj_tg_ssb_next_acq(f.tg) == end);

  recovery_published0 = gc2_recovery_published_acq(f.g);
  recovery_drained0 = gc2_recovery_drained_acq(f.g);
  ssb_published0 = gc2_ssb_items_published_acq(f.g);
  ssb_drained0 = gc2_ssb_items_drained_acq(f.g);
  assert(gc2_recovery_failed_acq(f.g) == 0);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_IDLE);

  publish.g = f.g;
  publish.o = obj2gco(parent);
  lj_gc2_test_recovery_pause(LJ_GC2_RECOVERY_TEST_RESERVED);
  assert(pthread_create(&thread, NULL, recovery_publish_thread, &publish) == 0);
  recovery_wait_paused(LJ_GC2_RECOVERY_TEST_RESERVED);

  /* Publisher A has already reserved the global close veto and owns the exact
  ** RECOVERY lifetime, but has not exposed PENDING in the side plane. A
  ** second semantic publication must coalesce with that durable reservation;
  ** it must not classify this bounded gap as an unrecoverable queue drop. */
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_RECOVERY);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_published_acq(f.g) == recovery_published0);

  settabV(f.L, &lj_tab_array_acq(parent)[0], child);
  lj_gc2_barrier_tab_g(f.g, parent);

  assert(lj_tg_ssb_active_acq(f.tg) == active);
  assert(lj_tg_ssb_next_acq(f.tg) == end);
  assert(lj_gc2_ssb_count_acq(active) == 0);
  assert(gc2_ssb_head_acq(f.g) == NULL);
  assert(gc2_ssb_drain_acq(f.g) == NULL);
  assert(gc2_ssb_items_published_acq(f.g) == ssb_published0);
  assert(gc2_recovery_failed_acq(f.g) == 0);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_published_acq(f.g) == recovery_published0);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_RECOVERY);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_IDLE);

  lj_gc2_test_recovery_release();
  assert(pthread_join(thread, NULL) == 0);
  assert(publish.result == 1);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_published_acq(f.g) == recovery_published0 + 1u);

  /* A's exact identity covers the mutation performed in the reservation gap.
  ** Its one traversal must discover the child and discharge the sole count. */
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1);
  assert(lj_gc2_ismarked(f.g, obj2gco(child)) == 1);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_drained_acq(f.g) == recovery_drained0 + 1u);
  assert(gc2_recovery_failed_acq(f.g) == 0);

  lj_tg_ssb_free_push(f.tg, held);
  assert(lj_gc2_flush_ssb(f.g, f.tg) == TG_GC2_SSB_SLOTS);
  recovery_drain_all(f.g, f.tg);
  assert(gc2_ssb_items_published_acq(f.g) ==
	 ssb_published0 + TG_GC2_SSB_SLOTS);
  assert(gc2_ssb_items_drained_acq(f.g) ==
	 ssb_drained0 + TG_GC2_SSB_SLOTS);

  recovery_fixture_close(&f);
}

static void test_stale_idle_sample_rechecks_mutating_recovery(void)
{
  RecoveryFixture f = recovery_fixture_open();
  RecoveryPublishCtx stale = {0};
  pthread_t thread;
  GCtab *t;
  GCArena *a;
  uint32_t cell;
  uint64_t published0, drained0;

  lua_newtable(f.L);
  t = tabV(f.L->top - 1);
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  lj_gc2_mark_begin(f.g);
  recovery_mark_table(&f, t);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_IDLE);
  published0 = gc2_recovery_published_acq(f.g);
  drained0 = gc2_recovery_drained_acq(f.g);

  /* Publisher B samples IDLE first. Disarm only future hook entries while B
  ** remains paused, then let publisher A create the real counted identity. */
  stale.g = f.g;
  stale.o = obj2gco(t);
  lj_gc2_test_recovery_pause(
	LJ_GC2_RECOVERY_TEST_SMALL_IDLE_SAMPLED);
  assert(pthread_create(&thread, NULL, recovery_publish_thread, &stale) == 0);
  recovery_wait_paused(LJ_GC2_RECOVERY_TEST_SMALL_IDLE_SAMPLED);
  lj_gc2_test_recovery_pause_disarm();
  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(t)) == 1);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_published_acq(f.g) == published0 + 1u);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_PENDING);

  /* This is the exact recovery-drain gap: PENDING already owns the count and
  ** the worker has changed lifetime to MUTATING but has not claimed the side
  ** lane yet. B's stale IDLE sample must re-read the side lane and coalesce. */
  assert(lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_MUTATING));
  assert(lj_gc2_test_recovery_mutating_recheck(a, cell) == 1);
  lj_gc2_test_recovery_release();
  assert(pthread_join(thread, NULL) == 0);
  assert(stale.result == 1);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_published_acq(f.g) == published0 + 1u);
  assert(gc2_recovery_failed_acq(f.g) == 0);

  assert(lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_MUTATING, LJ_ARENA_LIFETIME_LIVE));
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_drained_acq(f.g) == drained0 + 1u);

  /* If the drain completed between the two rechecks, its IDLE publication
  ** follows lifetime restoration. The final lifetime load therefore retries;
  ** stable generic MUTATING+IDLE remains the only rejecting snapshot. */
  assert(lj_gc2_test_recovery_mutating_recheck(a, cell) == 1);
  assert(lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_MUTATING));
  assert(lj_gc2_test_recovery_mutating_recheck(a, cell) == 0);
  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(t)) == 0);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_MUTATING, LJ_ARENA_LIFETIME_LIVE));

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
  /* MARK begins with a bounded native scheduling lease. This test launches a
  ** single one-shot drain thread, so close entry explicitly instead of
  ** allowing that thread to consume its quantum by honoring the lease and
  ** return before it reaches the deterministic pause point. */
  lj_gc2_jit_mark_request_exit(f.g);
  assert(gc2_jit_phase_gate_acq(f.g) == 0);
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
  assert(gc2_jit_phase_gate_acq(f.g) != 0);
  /* An ordinary worker quantum may grant the next bounded MARK mutator turn. */
  lj_gc2_jit_mark_request_exit(f.g);
  assert(gc2_jit_phase_gate_acq(f.g) == 0);
  (void)lj_gc2_worker_drain(f.g, 1);
  assert(gc2_jit_phase_gate_acq(f.g) != 0);
  /* The leader-side non-owner round uses the same worker drain machinery, but
  ** its nested drain must retain the closed gate needed by the fixpoint proof. */
  recovery_clean_fixpoint(&f);
  assert(gc2_jit_phase_gate_acq(f.g) == 0);
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

static void test_public_lease_excludes_destructor(void)
{
  RecoveryFixture f = recovery_fixture_open();
  RecoveryFreeCtx fr = {0};
  LJGC2Lease objlease, memlease, badlease, zero;
  pthread_t thread;
  GCtab *t = recovery_make_unlinked_table(&f);
  GCArena *a = lj_arena_of(t);
  uint32_t cell = lj_arena_cellof(t);
  uint32_t gct = 0;
  uint32_t allocf_arena0;
  memset(&objlease, 0xa5, sizeof(objlease));
  memset(&memlease, 0xa5, sizeof(memlease));
  memset(&badlease, 0xa5, sizeof(badlease));
  memset(&zero, 0, sizeof(zero));

  /* Every DEAD path must overwrite caller junk with an idempotent no-op token. */
  assert(lj_gc2_obj_lease_acquire(f.g, obj2gco(t),
	 (uint32_t)~LJ_TFUNC, NULL, &badlease) < 0);
  assert(memcmp(&badlease, &zero, sizeof(badlease)) == 0);
  lj_gc2_lease_release(&badlease);
  lj_gc2_lease_release(&badlease);
  memset(&badlease, 0xa5, sizeof(badlease));
  assert(lj_gc2_obj_lease_acquire(f.g, NULL, 0, NULL, &badlease) < 0);
  assert(memcmp(&badlease, &zero, sizeof(badlease)) == 0);
  lj_gc2_lease_release(&badlease);
  memset(&badlease, 0xa5, sizeof(badlease));
  assert(lj_gc2_mem_lease_acquire(f.g, NULL, &badlease) < 0);
  assert(memcmp(&badlease, &zero, sizeof(badlease)) == 0);
  lj_gc2_lease_release(&badlease);

  /* The documented temporary custom-lua_Alloc boundary retains raw storage
  ** globally, so a public raw lease succeeds with an explicit no-op token. */
  allocf_arena0 = la_load32_acq(&f.g->allocf_arena);
  assert(allocf_arena0 != 0);
  la_store32_rel(&f.g->allocf_arena, 0);
  memset(&badlease, 0xa5, sizeof(badlease));
  assert(lj_gc2_mem_lease_acquire(f.g, t, &badlease) >= 0);
  assert(memcmp(&badlease, &zero, sizeof(badlease)) == 0);
  lj_gc2_lease_release(&badlease);
  lj_gc2_lease_release(&badlease);
  la_store32_rel(&f.g->allocf_arena, allocf_arena0);

  assert(lj_gc2_obj_lease_acquire(f.g, obj2gco(t),
	 (uint32_t)~LJ_TTAB, &gct, &objlease) >= 0);
  assert(gct == (uint32_t)~LJ_TTAB);
  assert(objlease.arena == (void *)a);
  assert(objlease.admission != 0);
  /* The raw API protects the exact same registered allocation independently. */
  assert(lj_gc2_mem_lease_acquire(f.g, t, &memlease) >= 0);
  assert(memlease.arena == (void *)a);
  assert(memlease.admission != 0);
  lj_gc2_lease_release(&memlease);
  assert(memcmp(&memlease, &zero, sizeof(memlease)) == 0);
  lj_gc2_lease_release(&memlease);  /* Idempotent no-op after zeroing. */

  fr.alloc = &f.tg->alloc;
  fr.p = t;
  fr.size = sizeof(GCtab);
  lj_arena_test_lifetime_pause(1);
  assert(pthread_create(&thread, NULL, recovery_free_thread, &fr) == 0);
  while (lj_arena_test_lifetime_paused() == 0)
    la_cpu_pause();
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_DESTRUCT);
  assert(lj_arena_late_get(a, cell));
  /* A reader admitted before DESTRUCT may continue through its final byte read. */
  assert((uint32_t)la_load8_acq(&t->gct) == (uint32_t)~LJ_TTAB);
  assert(lj_arena_bm_get(a->block, cell));
  assert(lj_arena_ready_get(a, cell));

  /* Let the writer perform its reader proof while the object lease is held. */
  lj_arena_test_lifetime_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_late_get(a, cell));
  assert(lj_arena_bm_get(a->block, cell));
  assert(lj_arena_ready_get(a, cell));
  assert((uint32_t)la_load8_acq(&t->gct) == (uint32_t)~LJ_TTAB);

  lj_gc2_lease_release(&objlease);
  assert(memcmp(&objlease, &zero, sizeof(objlease)) == 0);
  lj_gc2_lease_release(&objlease);  /* Idempotent no-op after zeroing. */
  recovery_fixture_close(&f);
}

static void test_small_recovery_vs_free_both_orders(void)
{
  RecoveryFixture f = recovery_fixture_open();
  RecoveryPublishCtx publish = {0};
  RecoveryFreeCtx fr = {0};
  pthread_t thread;
  GCtab *t;
  GCArena *a;
  uint32_t cell;
  uint64_t published0, drained0;

  /* Recovery wins the lifetime lane. The losing free publishes only late[];
  ** recovery consumes its exact count without touching the logically dead
  ** body, leaving late[] authoritative for terminal sweep/free processing. */
  t = recovery_make_unlinked_table(&f);
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  published0 = gc2_recovery_published_acq(f.g);
  drained0 = gc2_recovery_drained_acq(f.g);
  publish.g = f.g;
  publish.o = obj2gco(t);
  lj_gc2_test_recovery_pause(LJ_GC2_RECOVERY_TEST_RESERVED);
  assert(pthread_create(&thread, NULL, recovery_publish_thread, &publish) == 0);
  recovery_wait_paused(LJ_GC2_RECOVERY_TEST_RESERVED);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_RECOVERY);
  assert(lj_arena_recovery_state_acq(a, cell) == LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  lj_arena_free(&f.tg->alloc, t, sizeof(GCtab));
  assert(lj_arena_late_get(a, cell));
  assert(lj_arena_bm_get(a->block, cell));
  assert(lj_arena_ready_get(a, cell));
  lj_gc2_test_recovery_release();
  assert(pthread_join(thread, NULL) == 0);
  assert(publish.result == 1);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_published_acq(f.g) == published0 + 1u);
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1);
  assert(lj_arena_recovery_state_acq(a, cell) == LJ_ARENA_RECOVERY_IDLE);
  assert(lj_arena_late_get(a, cell));
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_drained_acq(f.g) == drained0 + 1u);

  /* Irrevocable external free publishes late before DESTRUCT. Recovery must
  ** not RESCUE or traverse that provenance; the writer commits FREE and clears
  ** late only after old-body discovery is gone. */
  t = recovery_make_unlinked_table(&f);
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  published0 = gc2_recovery_published_acq(f.g);
  drained0 = gc2_recovery_drained_acq(f.g);
  fr.alloc = &f.tg->alloc;
  fr.p = t;
  fr.size = sizeof(GCtab);
  lj_arena_test_lifetime_pause(1);
  assert(pthread_create(&thread, NULL, recovery_free_thread, &fr) == 0);
  while (lj_arena_test_lifetime_paused() == 0)
    la_cpu_pause();
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_DESTRUCT);
  assert(lj_arena_late_get(a, cell));
  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(t)) == 1);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_DESTRUCT);
  assert(lj_arena_recovery_state_acq(a, cell) == LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_published_acq(f.g) == published0);
  lj_arena_test_lifetime_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_FREE);
  assert(!lj_arena_late_get(a, cell));
  assert(!lj_arena_bm_get(a->block, cell));
  assert(gc2_recovery_drained_acq(f.g) == drained0);

  /* Tentative GC destruction has no late provenance. The semantic publisher
  ** wins DESTRUCT->RESCUE in the same lane, publishes exact recovery, restores
  ** LIVE, and makes the writer's terminal DESTRUCT->FREE CAS fail untouched. */
  t = recovery_make_unlinked_table(&f);
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  published0 = gc2_recovery_published_acq(f.g);
  drained0 = gc2_recovery_drained_acq(f.g);
  assert(!lj_arena_late_get(a, cell));
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_DESTRUCT));
  gc2_phase_rel(f.g, LJ_GC2_SWEEP);
  assert(lj_gc2_trace_sweep_root(f.g, obj2gco(t)) == 1);
  gc2_phase_rel(f.g, LJ_GC2_IDLE);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(!lj_arena_lifetime_state_cas(a, cell,
	 LJ_ARENA_LIFETIME_DESTRUCT, LJ_ARENA_LIFETIME_FREE));
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_published_acq(f.g) == published0 + 1u);
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1);
  assert(lj_arena_recovery_state_acq(a, cell) == LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_drained_acq(f.g) == drained0 + 1u);
  assert(lj_gc_linkobj_terminal(f.g, obj2gco(t)) == LJ_GC_ROOT_LINKED);

  recovery_fixture_close(&f);
}

static void test_constructor_recovery_overlap_one(uint32_t stage, int commit)
{
  RecoveryFixture f = recovery_fixture_open();
  RecoveryPublishCtx publish = {0};
  pthread_t thread;
  GCupval *uv = recovery_new_constructing_upvalue(&f);
  GCobj *o = obj2gco(uv);
  GCArena *a = lj_arena_of(o);
  uint32_t cell = lj_arena_cellof(o);
  uint64_t published0 = gc2_recovery_published_acq(f.g);
  uint64_t drained0 = gc2_recovery_drained_acq(f.g);

  publish.g = f.g;
  publish.o = o;
  lj_gc2_test_recovery_pause(stage);
  assert(pthread_create(&thread, NULL, recovery_publish_thread, &publish) == 0);
  recovery_wait_paused(stage);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_RECOVERY);
  if (stage == LJ_GC2_RECOVERY_TEST_RESERVED)
    assert(lj_arena_recovery_state_acq(a, cell) ==
	   LJ_ARENA_RECOVERY_IDLE);
  else
    assert(lj_arena_recovery_state_acq(a, cell) ==
	   LJ_ARENA_RECOVERY_PENDING);

  if (commit) {
    assert(lj_gc_linkobj_new(f.g, o) == LJ_GC_ROOT_LINKED);
    assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_MEMBER);
  } else {
    assert(lj_mem_abandon_gco_unpublished(f.g, o) ==
	   LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE);
    assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
  }
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_RECOVERY);
  lj_gc2_test_recovery_release();
  assert(pthread_join(thread, NULL) == 0);
  assert(publish.result == 1);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_published_acq(f.g) == published0 + 1u);
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1);
  assert(lj_arena_recovery_state_acq(a, cell) == LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_drained_acq(f.g) == drained0 + 1u);
  if (!commit) {
    lj_mem_free(f.g, o, sizeof(GCupval));
    assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_FREE);
  }
  recovery_fixture_close(&f);
}

static void test_constructor_recovery_overlaps(void)
{
  test_constructor_recovery_overlap_one(
    LJ_GC2_RECOVERY_TEST_RESERVED, 1);
  test_constructor_recovery_overlap_one(
    LJ_GC2_RECOVERY_TEST_PRE_LIFETIME_RESTORE, 1);
  test_constructor_recovery_overlap_one(
    LJ_GC2_RECOVERY_TEST_RESERVED, 0);
  test_constructor_recovery_overlap_one(
    LJ_GC2_RECOVERY_TEST_PRE_LIFETIME_RESTORE, 0);
}

static void test_postclaim_late_retained_requeues(void)
{
  RecoveryFixture f = recovery_fixture_open();
  RecoveryDrainCtx drain = {0};
  pthread_t thread;
  GCupval *uv = recovery_new_constructing_upvalue(&f);
  GCobj *o = obj2gco(uv);
  GCArena *a = lj_arena_of(o);
  uint32_t cell = lj_arena_cellof(o);
  uint64_t drained0 = gc2_recovery_drained_acq(f.g);

  assert(lj_gc2_test_recovery_publish(f.g, o) == 1);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_CONSTRUCT);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_LINKING);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(f.g) == 1u);

  lj_gc2_test_recovery_pause(LJ_GC2_RECOVERY_TEST_POST_CLAIM);
  drain.g = f.g;
  drain.limit = 1;
  assert(pthread_create(&thread, NULL, recovery_drain_thread, &drain) == 0);
  recovery_wait_paused(LJ_GC2_RECOVERY_TEST_POST_CLAIM);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_MUTATING);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_CLAIMED);

  /* The external free can publish its irrevocable intent after CLAIMED, but
  ** constructor ownership still names the object. The drain must restore the
  ** constructor lane and replay the same counted recovery identity. */
  lj_arena_free(&f.tg->alloc, uv, sizeof(GCupval));
  assert(lj_arena_late_get(a, cell));
  assert(lj_arena_bm_get(a->block, cell));
  assert(lj_arena_ready_get(a, cell));
  lj_gc2_test_recovery_release();
  assert(pthread_join(thread, NULL) == 0);
  assert(drain.result == 1u);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_CONSTRUCT);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_LINKING);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_drained_acq(f.g) == drained0);

  assert(lj_mem_abandon_gco_unpublished(f.g, o) ==
	 LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1u);
  assert(lj_arena_recovery_state_acq(a, cell) == LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_drained_acq(f.g) == drained0 + 1u);
  assert(lj_arena_late_get(a, cell));
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

static void test_sweep_empty_string_is_immortal(void)
{
  RecoveryFixture f = recovery_fixture_open();
  GCtab *parent;
  GCstr *empty;
  uint64_t published0 = gc2_recovery_published_acq(f.g);
  uint64_t drained0 = gc2_recovery_drained_acq(f.g);

  lua_createtable(f.L, 1, 1);
  parent = tabV(f.L->top - 1);
  lua_pushliteral(f.L, "");
  empty = strV(f.L->top - 1);
  assert(empty == &f.g->strempty);
  lua_rawseti(f.L, -2, 1);
  lua_pushliteral(f.L, "");
  assert(strV(f.L->top - 1) == empty);
  lua_pushliteral(f.L, "");
  assert(strV(f.L->top - 1) == empty);
  lua_rawset(f.L, -3);

  /* Recovery traversal during SWEEP visits both table slots through
  ** gc2_mark_table_child_tv_worker(). The embedded immortal is a valid leaf,
  ** not an arena-classification failure or a recovery identity. */
  gc2_phase_rel(f.g, LJ_GC2_SWEEP);
  assert(lj_gc2_trace_sweep_root(f.g, obj2gco(empty)) == 1u);
  assert(gc2_recovery_failed_acq(f.g) == 0);
  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(parent)) == 1);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1u);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_failed_acq(f.g) == 0);
  assert(gc2_recovery_published_acq(f.g) == published0 + 1u);
  assert(gc2_recovery_drained_acq(f.g) == drained0 + 1u);

  gc2_phase_rel(f.g, LJ_GC2_IDLE);
  lua_pop(f.L, 1);
  recovery_fixture_close(&f);
}

static void test_huge_recovery_exact_lane_accounting(void)
{
  RecoveryFixture f = recovery_fixture_open();
  RecoveryPublishCtx pub[2] = {{0}, {0}};
  RecoveryDrainCtx drain = {0};
  pthread_t publisher[2], drainer;
  GCudata *ud = recovery_make_unlinked_huge_udata(&f);
  uint64_t redirtied0;

  /* Both publishers reserve before either can publish PENDING. One wins the
  ** exact state CAS; the loser must roll both reservations back. */
  pub[0].g = pub[1].g = f.g;
  pub[0].o = pub[1].o = obj2gco(ud);
  lj_gc2_test_recovery_pause(LJ_GC2_RECOVERY_TEST_RESERVED);
  assert(pthread_create(&publisher[0], NULL,
	 recovery_publish_thread, &pub[0]) == 0);
  assert(pthread_create(&publisher[1], NULL,
	 recovery_publish_thread, &pub[1]) == 0);
  while (gc2_recovery_items_acq(f.g) != 2u ||
	 gc2_recovery_huge_items_acq(f.g) != 2u)
    la_cpu_pause();
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(ud)) ==
	 LJ_ARENA_RECOVERY_IDLE);
  lj_gc2_test_recovery_release();
  assert(pthread_join(publisher[0], NULL) == 0);
  assert(pthread_join(publisher[1], NULL) == 0);
  assert(pub[0].result == 1 && pub[1].result == 1);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_huge_items_acq(f.g) == 1u);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(ud)) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1u);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_huge_items_acq(f.g) == 0);

  /* A publication racing CLAIMED changes only the state to REDIRTY. The
  ** completion requeues the same counted identity, and its replay consumes
  ** the huge subcount and aggregate count together. */
  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(ud)) == 1);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_huge_items_acq(f.g) == 1u);
  redirtied0 = gc2_recovery_redirtied_acq(f.g);
  drain.g = f.g;
  drain.limit = 1;
  lj_gc2_test_recovery_pause(LJ_GC2_RECOVERY_TEST_PRE_COMPLETE);
  assert(pthread_create(&drainer, NULL,
	 recovery_drain_thread, &drain) == 0);
  recovery_wait_paused(LJ_GC2_RECOVERY_TEST_PRE_COMPLETE);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(ud)) ==
	 LJ_ARENA_RECOVERY_CLAIMED);
  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(ud)) == 1);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(ud)) ==
	 LJ_ARENA_RECOVERY_REDIRTY);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_huge_items_acq(f.g) == 1u);
  assert(gc2_recovery_redirtied_acq(f.g) == redirtied0 + 1u);
  lj_gc2_test_recovery_release();
  assert(pthread_join(drainer, NULL) == 0);
  assert(drain.result == 1u);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(ud)) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_huge_items_acq(f.g) == 1u);
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1u);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_huge_items_acq(f.g) == 0);

  /* Terminal discard first reconciles both exact counters against a
  ** non-destructive authoritative scan, then consumes the proven locators. */
  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(ud)) == 1);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_huge_items_acq(f.g) == 1u);
  assert(lj_gc2_test_recovery_discard_terminal(f.g) == 1u);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_huge_items_acq(f.g) == 0);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(ud)) ==
	 LJ_ARENA_RECOVERY_IDLE);

  recovery_fixture_close(&f);
}

static void test_empty_huge_lane_is_skipped(void)
{
  RecoveryFixture f = recovery_fixture_open();
  GCtab *small;

  /* Starting on lane 2 used to scan every 65,536-slot HugeTab before finding
  ** aggregate work in main or small. A zero exact huge subcount makes both
  ** drains bypass that directory walk deterministically. */
  assert(lj_gc2_test_recovery_publish(
	 f.g, obj2gco(mainthread_acq(f.g))) == 1);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_huge_items_acq(f.g) == 0);
  gc2_recovery_scan_lane_store_rlx(f.g, 2u);
  lj_gc2_test_recovery_huge_scans_reset();
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1u);
  assert(lj_gc2_test_recovery_huge_scans() == 0);

  small = recovery_make_unlinked_table(&f);
  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(small)) == 1);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_huge_items_acq(f.g) == 0);
  gc2_recovery_scan_lane_store_rlx(f.g, 2u);
  lj_gc2_test_recovery_huge_scans_reset();
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1u);
  assert(lj_gc2_test_recovery_huge_scans() == 0);
  assert(gc2_recovery_items_acq(f.g) == 0);

  recovery_fixture_close(&f);
}

static void test_current_cyclic_table_private_edge_is_consumed(void)
{
  RecoveryFixture f = recovery_fixture_open();
  RecoveryDrainCtx drain = {0};
  pthread_t drainer;
  GC2SSBNode *active, *held;
  GCRef *base, *end;
  GCtab *t;
  GCstr *filler;
  uint64_t redirtied0;
  uint32_t i;

  lua_newtable(f.L);
  t = tabV(f.L->top - 1);
  lua_pushvalue(f.L, -1);
  lua_rawseti(f.L, -2, 1);  /* Exact self-edge which used to replay forever. */
  assert(lj_gc_flush_root_pending(f.g) != 0);
  assert(lj_gc_unlink_root_obj(f.g, obj2gco(t)) == LJ_GC_ROOT_UNLINKED);
  lua_pop(f.L, 1);

  lj_gc2_mark_begin(f.g);
  recovery_mark_table(&f, t);
  assert(lj_gc2_test_table_scan_current(f.g, t));
  assert(!(lj_obj_gcflags(obj2gco(t)) & LJ_GC_NEEDSCAN));
  gc2_phase_rel(f.g, LJ_GC2_SWEEP);

  /* Remove the replacement node and fill the active SSB so any publication can
  ** only use recovery. The admitted outer table still traverses normally, but
  ** its already-current private self-edge must be consumed without changing
  ** the CLAIMED recovery identity to REDIRTY. */
  held = lj_tg_ssb_free_pop(f.tg);
  assert(held != NULL);
  assert(lj_tg_ssb_free_acq(f.tg) == NULL);
  active = lj_tg_ssb_active_acq(f.tg);
  base = lj_tg_ssb_base_acq(f.tg);
  end = lj_tg_ssb_end_acq(f.tg);
  assert(active != NULL && base == active->slot);
  lua_pushliteral(f.L, "gc2 cyclic recovery filler");
  filler = strV(f.L->top - 1);
  for (i = 0; i < TG_GC2_SSB_SLOTS; i++)
    assert(lj_gc2_test_ssb_push(f.g, obj2gco(filler)) == 1);
  assert(lj_tg_ssb_next_acq(f.tg) == end);

  redirtied0 = gc2_recovery_redirtied_acq(f.g);
  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(t)) == 1);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(t)) ==
	 LJ_ARENA_RECOVERY_PENDING);
  gc2_recovery_scan_lane_store_rlx(f.g, 1u);
  lj_gc2_test_worker_table_skips_reset();
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1u);
  assert(lj_gc2_test_worker_table_skips() != 0);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_redirtied_acq(f.g) == redirtied0);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(t)) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(lj_tg_ssb_next_acq(f.tg) == end);

  /* The same current stamp is not a semantic-mutation proof. While a second
  ** recovery pass is CLAIMED and the SSB remains full, the public root/barrier
  ** path must remain unfiltered and change that identity to REDIRTY. */
  redirtied0 = gc2_recovery_redirtied_acq(f.g);
  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(t)) == 1);
  drain.g = f.g;
  drain.limit = 1;
  lj_gc2_test_recovery_pause(LJ_GC2_RECOVERY_TEST_PRE_COMPLETE);
  assert(pthread_create(&drainer, NULL, recovery_drain_thread, &drain) == 0);
  recovery_wait_paused(LJ_GC2_RECOVERY_TEST_PRE_COMPLETE);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(t)) ==
	 LJ_ARENA_RECOVERY_CLAIMED);
  assert(lj_gc2_trace_sweep_root(f.g, obj2gco(t)) == 1u);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(t)) ==
	 LJ_ARENA_RECOVERY_REDIRTY);
  assert(gc2_recovery_redirtied_acq(f.g) == redirtied0 + 1u);
  lj_gc2_test_recovery_release();
  assert(pthread_join(drainer, NULL) == 0);
  assert(drain.result == 1u);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(t)) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1u);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(t)) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 0);

  lj_tg_ssb_free_push(f.tg, held);
  assert(lj_gc2_flush_ssb(f.g, f.tg) == TG_GC2_SSB_SLOTS);
  recovery_drain_all(f.g, f.tg);
  lua_pop(f.L, 1);
  gc2_phase_rel(f.g, LJ_GC2_IDLE);
  recovery_fixture_close(&f);
}

static void test_current_self_metatable_metadata_is_private(void)
{
  RecoveryFixture f = recovery_fixture_open();
  GC2SSBNode *active, *held;
  GCRef *base, *end;
  GCtab *t;
  GCstr *filler;
  uint64_t redirtied0;
  uint32_t i;

  lua_newtable(f.L);
  t = tabV(f.L->top - 1);
  lua_pushvalue(f.L, -1);
  assert(lua_setmetatable(f.L, -2) == 1);
  assert(lj_tab_metatable_acq(t) == t);
  assert(lj_gc_flush_root_pending(f.g) != 0);
  assert(lj_gc_unlink_root_obj(f.g, obj2gco(t)) == LJ_GC_ROOT_UNLINKED);
  lua_pop(f.L, 1);

  lj_gc2_mark_begin(f.g);
  recovery_mark_table(&f, t);
  assert(lj_gc2_test_table_scan_current(f.g, t));
  assert(!(lj_obj_gcflags(obj2gco(t)) & LJ_GC_NEEDSCAN));
  gc2_phase_rel(f.g, LJ_GC2_SWEEP);

  /* Weak-mode classification must retain the metatable body while reading
  ** __mode, but this worker-private metadata read is not a semantic SWEEP root.
  ** With no SSB replacement, the former public admission deterministically
  ** changed this table's CLAIMED recovery state to REDIRTY forever. */
  held = lj_tg_ssb_free_pop(f.tg);
  assert(held != NULL);
  assert(lj_tg_ssb_free_acq(f.tg) == NULL);
  active = lj_tg_ssb_active_acq(f.tg);
  base = lj_tg_ssb_base_acq(f.tg);
  end = lj_tg_ssb_end_acq(f.tg);
  assert(active != NULL && base == active->slot);
  lua_pushliteral(f.L, "gc2 self metatable recovery filler");
  filler = strV(f.L->top - 1);
  for (i = 0; i < TG_GC2_SSB_SLOTS; i++)
    assert(lj_gc2_test_ssb_push(f.g, obj2gco(filler)) == 1);
  assert(lj_tg_ssb_next_acq(f.tg) == end);

  redirtied0 = gc2_recovery_redirtied_acq(f.g);
  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(t)) == 1);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(t)) ==
	 LJ_ARENA_RECOVERY_PENDING);
  gc2_recovery_scan_lane_store_rlx(f.g, 1u);
  assert(lj_gc2_test_recovery_drain(f.g, 1) == 1u);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_redirtied_acq(f.g) == redirtied0);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(t)) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(lj_tg_ssb_next_acq(f.tg) == end);

  lj_tg_ssb_free_push(f.tg, held);
  assert(lj_gc2_flush_ssb(f.g, f.tg) == TG_GC2_SSB_SLOTS);
  recovery_drain_all(f.g, f.tg);
  lua_pop(f.L, 1);
  gc2_phase_rel(f.g, LJ_GC2_IDLE);
  recovery_fixture_close(&f);
}

static void recovery_assert_cyclic_container_bounded(RecoveryFixture *f,
					       GCobj *o,
					       const char *filler_text)
{
  GC2SSBNode *active, *held;
  GCRef *base, *end;
  GCstr *filler;
  uint64_t redirtied0;
  uint32_t i;

  assert(lj_gc2_markobj(f->g, o) == 1);
  recovery_drain_all(f->g, f->tg);
  assert(lj_gc2_ismarked(f->g, o) == 1);
  lj_obj_cleargcflags(o, LJ_GC_NEEDSCAN);
  gc2_phase_rel(f->g, LJ_GC2_SWEEP);

  held = lj_tg_ssb_free_pop(f->tg);
  assert(held != NULL);
  assert(lj_tg_ssb_free_acq(f->tg) == NULL);
  active = lj_tg_ssb_active_acq(f->tg);
  base = lj_tg_ssb_base_acq(f->tg);
  end = lj_tg_ssb_end_acq(f->tg);
  assert(active != NULL && base == active->slot);
  lua_pushstring(f->L, filler_text);
  filler = strV(f->L->top - 1);
  for (i = 0; i < TG_GC2_SSB_SLOTS; i++)
    assert(lj_gc2_test_ssb_push(f->g, obj2gco(filler)) == 1);
  assert(lj_tg_ssb_next_acq(f->tg) == end);

  redirtied0 = gc2_recovery_redirtied_acq(f->g);
  assert(lj_gc2_test_recovery_publish(f->g, o) == 1);
  assert(gc2_recovery_items_acq(f->g) == 1u);
  gc2_recovery_scan_lane_store_rlx(f->g, 1u);
  assert(lj_gc2_test_recovery_drain(f->g, 1) == 1u);
  assert(gc2_recovery_items_acq(f->g) == 1u);
  assert(gc2_recovery_redirtied_acq(f->g) == redirtied0 + 1u);
  assert(lj_obj_gcflags(o) & LJ_GC_NEEDSCAN);
  assert(lj_gc2_test_recovery_state(f->g, o) ==
	 LJ_ARENA_RECOVERY_PENDING);

  /* Replay sees the persistent private traversal token and consumes the exact
  ** recovery identity without walking the cycle again. */
  assert(lj_gc2_test_recovery_drain(f->g, 1) == 1u);
  assert(gc2_recovery_items_acq(f->g) == 0);
  assert(gc2_recovery_redirtied_acq(f->g) == redirtied0 + 1u);
  assert(lj_gc2_test_recovery_state(f->g, o) ==
	 LJ_ARENA_RECOVERY_IDLE);

  lj_tg_ssb_free_push(f->tg, held);
  assert(lj_gc2_flush_ssb(f->g, f->tg) == TG_GC2_SSB_SLOTS);
  recovery_drain_all(f->g, f->tg);
  lua_pop(f->L, 1);
  lj_gc2_test_rescan_pending_clear_cycle(f->g, o);
  gc2_phase_rel(f->g, LJ_GC2_IDLE);
}

static void test_cyclic_non_table_recovery_is_bounded(void)
{
  RecoveryFixture f = recovery_fixture_open();
  GCfunc *fn;

  lua_pushnil(f.L);
  lua_pushcclosure(f.L, recovery_self_cfunc, 1);
  fn = funcV(f.L->top - 1);
  lua_pushvalue(f.L, -1);
  assert(lua_setupvalue(f.L, -2, 1) != NULL);
  assert(lj_gc_flush_root_pending(f.g) != 0);
  assert(lj_gc_unlink_root_obj(f.g, obj2gco(fn)) == LJ_GC_ROOT_UNLINKED);
  lua_pop(f.L, 1);
  lj_gc2_mark_begin(f.g);
  recovery_assert_cyclic_container_bounded(
    &f, obj2gco(fn), "gc2 self closure recovery filler");
  recovery_fixture_close(&f);

  f = recovery_fixture_open();
  luaL_openlibs(f.L);
  assert(luaL_dostring(f.L,
    "local th = require('threading')\n"
    "local ch = th.channel(1)\n"
    "assert(ch:send(ch))\n"
    "return ch\n") == 0);
  assert(tvisudata(f.L->top - 1));
  {
    GCudata *ud = udataV(f.L->top - 1);
    assert(lj_gc_flush_root_pending(f.g) != 0);
    assert(lj_gc_unlink_root_obj(f.g, obj2gco(ud)) == LJ_GC_ROOT_UNLINKED);
    lua_pop(f.L, 1);
    lj_gc2_mark_begin(f.g);
    recovery_assert_cyclic_container_bounded(
      &f, obj2gco(ud), "gc2 self channel recovery filler");
  }
  recovery_fixture_close(&f);
}

static void test_new_mark_clears_unlinked_prior_cycle_token(void)
{
  RecoveryFixture f = recovery_fixture_open();
  GCfunc *fn;

  lua_pushnil(f.L);
  lua_pushcclosure(f.L, recovery_self_cfunc, 1);
  fn = funcV(f.L->top - 1);
  assert(lj_gc_flush_root_pending(f.g) != 0);
  assert(lj_gc_unlink_root_obj(f.g, obj2gco(fn)) == LJ_GC_ROOT_UNLINKED);
  lua_pop(f.L, 1);

  lj_gc2_mark_begin(f.g);
  assert(lj_gc2_markobj(f.g, obj2gco(fn)) == 1);
  recovery_drain_all(f.g, f.tg);
  lj_obj_addgcflags_atomic(obj2gco(fn), LJ_GC_NEEDSCAN);
  assert(lj_obj_gcflags(obj2gco(fn)) & LJ_GC_NEEDSCAN);
  lj_gc2_cycle_to_idle(f.g);
  assert(gc2_phase_acq(f.g) == LJ_GC2_IDLE);

  /* The object is intentionally absent from both bounded root-spine cleanup
  ** walks, so the old token survives until the exact next major NEW winner. */
  assert(lj_obj_gcflags(obj2gco(fn)) & LJ_GC_NEEDSCAN);
  lj_gc2_force_major(f.g);
  lj_gc2_mark_begin(f.g);
  assert(lj_obj_gcflags(obj2gco(fn)) & LJ_GC_NEEDSCAN);
  assert(lj_gc2_markobj(f.g, obj2gco(fn)) == 1);
  assert(!(lj_obj_gcflags(obj2gco(fn)) & LJ_GC_NEEDSCAN));
  recovery_drain_all(f.g, f.tg);
  lj_gc2_cycle_to_idle(f.g);
  recovery_fixture_close(&f);
}

static void test_lost_huge_completion_count_fail_stops(void)
{
  uint32_t huge_count;
  for (huge_count = 0; huge_count <= 1u; huge_count++) {
    pid_t child = fork();
    int status = 0;
    assert(child >= 0);
    if (child == 0) {
      struct rlimit core_limit = {0, 0};
      RecoveryFixture f = recovery_fixture_open();
      assert(setrlimit(RLIMIT_CORE, &core_limit) == 0);
      /* Model both impossible post-state-clear conditions: missing huge lane
      ** identity, then a present huge identity with no aggregate identity.
      ** Neither may return to caller UNMAP/SWEEP effects. */
      gc2_recovery_huge_items_store_rlx(f.g, huge_count);
      assert(gc2_recovery_items_acq(f.g) == 0);
      assert(gc2_recovery_huge_items_acq(f.g) == huge_count);
      lj_gc2_test_recovery_huge_count_complete(f.g);
      _exit(127);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGABRT);
  }
}

static void test_lost_rollback_counts_fail_stop(void)
{
  uint32_t huge;
  for (huge = 0; huge <= 1u; huge++) {
    pid_t child = fork();
    int status = 0;
    assert(child >= 0);
    if (child == 0) {
      struct rlimit core_limit = {0, 0};
      RecoveryFixture f = recovery_fixture_open();
      assert(setrlimit(RLIMIT_CORE, &core_limit) == 0);
      /* Model a CAS loser whose reservation was already lost/corrupted. Both
      ** aggregate and huge-lane rollback must refuse zero without wrapping. */
      if (huge)
	lj_gc2_test_recovery_huge_count_rollback(f.g);
      else
	lj_gc2_test_recovery_count_rollback(f.g);
      _exit(127);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGABRT);
  }
}

static void test_terminal_preflight_preserves_mismatch_locator(void)
{
  RecoveryFixture f = recovery_fixture_open();
  GCudata *ud = recovery_make_unlinked_huge_udata(&f);

  assert(lj_gc2_test_recovery_publish(f.g, obj2gco(ud)) == 1);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_huge_items_acq(f.g) == 1u);
  gc2_recovery_huge_items_store_rlx(f.g, 0);
  assert(lj_gc2_test_recovery_terminal_preflight(f.g) == 0);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(ud)) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(f.g) == 1u);
  assert(gc2_recovery_huge_items_acq(f.g) == 0);
  assert(gc2_recovery_failed_acq(f.g) == 1u);

  gc2_recovery_huge_items_store_rlx(f.g, 1);
  assert(lj_gc2_test_recovery_discard_terminal(f.g) == 1u);
  assert(lj_gc2_test_recovery_state(f.g, obj2gco(ud)) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_huge_items_acq(f.g) == 0);
  recovery_fixture_close(&f);
}

static void test_sticky_failure_without_items_is_bounded(void)
{
  RecoveryFixture f = recovery_fixture_open();
  const uint32_t lane0 = 2u;
  const uint32_t small_slot0 = 1234u;
  const uint32_t small_cell0 = LJ_AFIRST_CELL + 17u;
  const uint32_t huge_slot0 = 40503u;
  uint32_t i;

  assert(gc2_phase_acq(f.g) == LJ_GC2_IDLE);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_failed_acq(f.g) == 0);
  gc2_recovery_scan_lane_store_rlx(f.g, lane0);
  gc2_recovery_small_slot_store_rlx(f.g, small_slot0);
  gc2_recovery_small_cell_store_rlx(f.g, small_cell0);
  gc2_recovery_huge_slot_store_rlx(f.g, huge_slot0);

  lj_gc2_test_recovery_fail_closed(f.g);
  assert(gc2_recovery_failed_acq(f.g) == 1u);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(lj_gc2_activation_reclaim_veto(f.g));

  /* A sticky failure has no locator to scan. Repeating a maximal drain must
  ** remain constant-time: drain_one never rotates or advances any lane cursor,
  ** and the sticky failure is neither cleared nor converted into fake work. */
  for (i = 0; i < 4096u; i++)
    assert(lj_gc2_test_recovery_drain(f.g, ~(uint32_t)0) == 0);
  assert(gc2_recovery_scan_lane_rlx(f.g) == lane0);
  assert(gc2_recovery_small_slot_rlx(f.g) == small_slot0);
  assert(gc2_recovery_small_cell_rlx(f.g) == small_cell0);
  assert(gc2_recovery_huge_slot_rlx(f.g) == huge_slot0);
  assert(gc2_recovery_failed_acq(f.g) == 1u);

  /* Full, bounded-step, worker, forced-close, and preserve-abort drivers all
  ** return/defer without spinning or weakening the absorbing reclamation veto. */
  gc2_phase_rel(f.g, LJ_GC2_SWEEP);
  assert(lj_gc2_collect_active(f.L) == 0);
  assert(lj_gc2_step_explicit(f.L, 1u << 20) == 0);
  assert(lj_gc2_worker_drain(f.g, LJ_GC2_SWEEP_BATCH) == 0);
  assert(!lj_gc2_sweep_bridge_can_progress(f.g));
  assert(lj_gc2_sweep_to_idle(f.g) == 0);
  lj_gc2_cycle_to_idle(f.g);
  lj_gc2_preserve_abort_to_idle(f.g);
  assert(gc2_phase_acq(f.g) == LJ_GC2_SWEEP);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_failed_acq(f.g) == 1u);
  assert(lj_gc2_activation_reclaim_veto(f.g));
  assert(gc2_recovery_scan_lane_rlx(f.g) == lane0);

  recovery_fixture_close(&f);
}

int main(void)
{
  test_full_active_ssb_fallback();
  test_full_ssb_barrier_coalesces_reserved_recovery();
  test_stale_idle_sample_rechecks_mutating_recovery();
  test_grey_growth_transaction();
  test_reservation_gap_blocks_mark_close();
  test_claimed_redirty_replay();
  test_public_lease_excludes_destructor();
  test_small_recovery_vs_free_both_orders();
  test_constructor_recovery_overlaps();
  test_postclaim_late_retained_requeues();
  test_weak_clear_recovery_gate();
  test_sweep_empty_string_is_immortal();
  test_huge_recovery_exact_lane_accounting();
  test_empty_huge_lane_is_skipped();
  test_current_cyclic_table_private_edge_is_consumed();
  test_current_self_metatable_metadata_is_private();
  test_cyclic_non_table_recovery_is_bounded();
  test_new_mark_clears_unlinked_prior_cycle_token();
  test_lost_huge_completion_count_fail_stops();
  test_lost_rollback_counts_fail_stop();
  test_terminal_preflight_preserves_mismatch_locator();
  test_sticky_failure_without_items_is_bounded();
  printf("t-gc2-recovery OK: no-drop SSB/recovery, exact huge-lane accounting/skip, bounded cyclic private/metadata edges, free/lifetime races, constructor overlap, closure vetoes, empty-string SWEEP, sticky failure, and REDIRTY replay verified\n");
  return 0;
}
