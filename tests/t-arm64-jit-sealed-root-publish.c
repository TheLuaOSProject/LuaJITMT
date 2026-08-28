/*
** Bounded GC-root publication contract for a sealed ARM64 JIT transaction.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_obj.h"
#include "lj_tg.h"
#include "lj_thr.h"

#if !LJ_TARGET_ARM64 || !LJ_HASJIT
#error "t-arm64-jit-sealed-root-publish requires ARM64 JIT"
#endif

typedef struct FlushAttempt {
  global_State *g;
  uint32_t flushed;
} FlushAttempt;

static void *flush_attempt_main(void *opaque)
{
  FlushAttempt *attempt = (FlushAttempt *)opaque;
  attempt->flushed = lj_gc_flush_root_pending(attempt->g);
  return NULL;
}

static void init_closed_upvalue(global_State *g, GCupval *uv)
{
  uv->gct = (uint8_t)~LJ_TUPVAL;
  uv->closed = 1;
  uv->immutable = 0;
  setnilV(&uv->tv);
  setmref(uv->v, &uv->tv);
  uv->dhash = 0;
  newwhite(g, uv);
  lj_obj_setgcwnullrel(obj2gco(uv));
}

static GCupval *new_constructing_upvalue(lua_State *L)
{
  GCupval *uv = (GCupval *)lj_mem_newgco_unlinked_nothrow(
	L, (GCSize)sizeof(GCupval));
  assert(uv != NULL);
  init_closed_upvalue(G(L), uv);
  return uv;
}

static void assert_constructing(GCupval *uv)
{
  GCArena *a = lj_arena_of(uv);
  uint32_t cell = lj_arena_cellof(uv);
  assert(!lj_arena_ishuge(a));
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_CONSTRUCT);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_LINKING);
}

static void assert_member(GCupval *uv)
{
  GCArena *a = lj_arena_of(uv);
  uint32_t cell = lj_arena_cellof(uv);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_MEMBER);
}

static void test_publish_and_flush_exclusion(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = lj_thr_get_tg();
  LJGCNewRootPublishPlan plan;
  GCupval *seed, *after_seed, *uv;
  GCobj *oldhead, *old_after_head;
  FlushAttempt attempt;
  pthread_t thread;

  assert(tg != NULL && tg->gl == g);
  (void)lj_gc_flush_root_pending(g);
  seed = new_constructing_upvalue(L);
  assert(lj_gc_linkobj_new(g, obj2gco(seed)) == LJ_GC_ROOT_LINKED);
  oldhead = lj_tg_gcroot_pending_acq(tg);
  assert(oldhead == obj2gco(seed));
  after_seed = new_constructing_upvalue(L);
  assert(lj_gc_linkobj_new_after_main(g, obj2gco(after_seed)) ==
	 LJ_GC_ROOT_LINKED);
  old_after_head = lj_tg_gcroot_pending_after_main_acq(tg);
  assert(old_after_head == obj2gco(after_seed));
  assert(lj_gcroot_pending_hint_acq(g) != 0);

  uv = new_constructing_upvalue(L);
  assert_constructing(uv);
  assert(lj_gc_linkobj_new_sealed_prepare(g, obj2gco(uv), &plan));
  assert(plan.g == g && plan.tg == tg && plan.object == obj2gco(uv));
  assert(plan.pending_head == oldhead && plan.arena == lj_arena_of(uv) &&
	 plan.cell == lj_arena_cellof(uv) && plan.armed != 0);
  assert(lj_tg_gcroot_pending_owner_acq(tg) ==
	 (uint32_t)LJ_TG_ROOT_PENDING_SEALED_TRACE);
  assert(lj_arena_ready_get(lj_arena_of(uv), lj_arena_cellof(uv)));
  assert_constructing(uv);

  /* A real peer flush performs one failed owner CAS and skips this TG. It
  ** cannot exchange either pending head under the plan. Clear the earlier
  ** publication hint so the peer must republish it after losing that CAS;
  ** this also proves it did not return at the global fast path or SMR gate. */
  lj_gcroot_pending_hint_rel(g, 0);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  attempt.g = g;
  attempt.flushed = UINT32_MAX;
  assert(pthread_create(&thread, NULL, flush_attempt_main, &attempt) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(attempt.flushed == 0);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_tg_gcroot_pending_acq(tg) == oldhead);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == old_after_head);
  assert(lj_tg_gcroot_pending_owner_acq(tg) ==
	 (uint32_t)LJ_TG_ROOT_PENDING_SEALED_TRACE);

  lj_gc_linkobj_new_sealed_publish(&plan);
  assert(plan.armed == 0);
  assert(lj_tg_gcroot_pending_owner_acq(tg) ==
	 (uint32_t)LJ_TG_ROOT_PENDING_IDLE);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(uv));
  assert(lj_obj_gcw_acq(obj2gco(uv)) == oldhead);
  assert_member(uv);
  assert(lj_gc_flush_root_pending(g) >= 3u);
}

static void test_prepare_contention_and_abort(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = lj_thr_get_tg();
  LJGCNewRootPublishPlan plan;
  GCupval *contended, *huge, *republished, *abandoned;
  GCArena *abandoned_arena;
  uint32_t abandoned_cell;
  GCSize huge_size = (GCSize)LJ_HUGE_THRESHOLD +
	(GCSize)sizeof(GCupval);

  assert(lj_tg_gcroot_pending_owner_try(
	tg, (uint32_t)LJ_TG_ROOT_PENDING_FLUSH));
  contended = new_constructing_upvalue(L);
  assert(!lj_gc_linkobj_new_sealed_prepare(g, obj2gco(contended), &plan));
  assert(plan.armed == 0);
  assert(lj_tg_gcroot_pending_owner_release(
	tg, (uint32_t)LJ_TG_ROOT_PENDING_FLUSH));
  assert(lj_gc_linkobj_new(g, obj2gco(contended)) == LJ_GC_ROOT_LINKED);

  huge = (GCupval *)lj_mem_newgco_unlinked_nothrow(L, huge_size);
  assert(huge != NULL && lj_arena_ishuge(lj_arena_of(huge)));
  init_closed_upvalue(g, huge);
  assert(!lj_gc_linkobj_new_sealed_prepare(g, obj2gco(huge), &plan));
  assert(plan.armed == 0 &&
	 lj_tg_gcroot_pending_owner_acq(tg) ==
	   (uint32_t)LJ_TG_ROOT_PENDING_IDLE);
  lj_mem_freegco_unpublished(g, huge, huge_size);

  republished = new_constructing_upvalue(L);
  assert(lj_gc_linkobj_new_sealed_prepare(
	g, obj2gco(republished), &plan));
  assert_constructing(republished);
  lj_gc_linkobj_new_sealed_abort(&plan);
  assert(plan.armed == 0);
  assert(lj_tg_gcroot_pending_owner_acq(tg) ==
	 (uint32_t)LJ_TG_ROOT_PENDING_IDLE);
  assert_constructing(republished);
  assert(lj_gc_linkobj_new(g, obj2gco(republished)) == LJ_GC_ROOT_LINKED);

  abandoned = new_constructing_upvalue(L);
  abandoned_arena = lj_arena_of(abandoned);
  abandoned_cell = lj_arena_cellof(abandoned);
  assert(lj_gc_linkobj_new_sealed_prepare(
	g, obj2gco(abandoned), &plan));
  assert_constructing(abandoned);
  lj_gc_linkobj_new_sealed_abort(&plan);
  assert(plan.armed == 0);
  assert_constructing(abandoned);
  /* Header READY is harmless after abort; the original constructor still owns
  ** LINKING and can be abandoned/freed by the real unpublished cleanup path. */
  lj_mem_freegco_unpublished(g, abandoned, (GCSize)sizeof(GCupval));
  assert(lj_arena_lifetime_state_acq(abandoned_arena, abandoned_cell) ==
	 LJ_ARENA_LIFETIME_FREE);
  assert(lj_arena_root_state_acq(abandoned_arena, abandoned_cell) ==
	 LJ_ARENA_ROOT_NONE);
  assert(lj_gc_flush_root_pending(g) >= 2u);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  test_publish_and_flush_exclusion(L);
  test_prepare_contention_and_abort(L);
  lua_close(L);
  puts("t-arm64-jit-sealed-root-publish OK");
  return 0;
}
