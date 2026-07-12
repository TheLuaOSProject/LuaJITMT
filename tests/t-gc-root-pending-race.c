/*
** Deterministic pending-root load/xchg/CAS publication regression.
*/

#ifndef LJ_GC2_TEST_HELPERS
#error "t-gc-root-pending-race requires LJ_GC2_TEST_HELPERS"
#endif

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_arena.h"
#include "lj_func.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_udata.h"

typedef struct PendingRace {
  global_State *g;
  TGState *tg;
  GCobj *oldhead;
  GCobj *published;
  uint32_t path;
  uint32_t stage;
  uint32_t flushed;
} PendingRace;

typedef struct RootStateRace {
  global_State *g;
  GCobj *o;
  GCArena *a;
  uint32_t path;
  uint32_t stage;
  int result;
  int done;
} RootStateRace;

static PendingRace *active_race;
static RootStateRace *active_state_race;

static void spin_until(const uint32_t *p, uint32_t value)
{
  while (la_load32_acq(p) != value)
    la_cpu_pause();
}

static void root_state_hook(global_State *g, GCobj *o, uint32_t path)
{
  RootStateRace *race = active_state_race;
  if (!race || race->path != path || race->o != o)
    return;
  assert(race->g == g);
  lj_gc_test_set_root_state_hook(NULL);
  la_store32_rel(&race->stage, 1);
  spin_until(&race->stage, 2);
}

static void *root_state_race_actor(void *arg)
{
  RootStateRace *race = (RootStateRace *)arg;
  if (race->path == LJ_GC_ROOT_STATE_TEST_LINKING)
    race->result = lj_gc_linkobj(race->g, race->o);
  else if (race->path == LJ_GC_ROOT_STATE_TEST_UNLINKING)
    race->result = lj_gc_unlink_root_obj(race->g, race->o);
  else
    race->result = (int)lj_gc_reclaim_gc2_arena(
	      race->g, race->a, 1u, &race->done);
  return NULL;
}

static pthread_t begin_root_state_race(RootStateRace *race, global_State *g,
				       GCobj *o, uint32_t path)
{
  pthread_t thread;
  race->g = g;
  race->o = o;
  race->a = NULL;
  race->path = path;
  race->stage = 0;
  race->result = -99;
  race->done = 0;
  active_state_race = race;
  lj_gc_test_set_root_state_hook(root_state_hook);
  assert(pthread_create(&thread, NULL, root_state_race_actor, race) == 0);
  spin_until(&race->stage, 1);
  return thread;
}

static pthread_t begin_small_reanchor_race(RootStateRace *race,
					    global_State *g, GCobj *o,
					    GCArena *a)
{
  pthread_t thread;
  race->g = g;
  race->o = o;
  race->a = a;
  race->path = LJ_GC_ROOT_STATE_TEST_SMALL_REANCHOR_LINKED;
  race->stage = 0;
  race->result = -99;
  race->done = 0;
  active_state_race = race;
  lj_gc_test_set_root_state_hook(root_state_hook);
  assert(pthread_create(&thread, NULL, root_state_race_actor, race) == 0);
  spin_until(&race->stage, 1);
  return thread;
}

static void finish_root_state_race(RootStateRace *race, pthread_t thread)
{
  la_store32_rel(&race->stage, 2);
  assert(pthread_join(thread, NULL) == 0);
  active_state_race = NULL;
  lj_gc_test_set_root_state_hook(NULL);
}

static int root_contains(global_State *g, GCobj *needle)
{
  GCobj *o;
  uint32_t n = 0;
  for (o = lj_gc_root_acq(g); o != NULL; o = lj_obj_gcw_acq(o)) {
    if (o == needle)
      return 1;
    assert(++n < LJ_GC2_ROOT_SCAN_LIMIT);
  }
  return 0;
}

static uint32_t root_occurrences(global_State *g, GCobj *needle)
{
  GCobj *o;
  uint32_t n = 0, found = 0;
  for (o = lj_gc_root_acq(g); o != NULL; o = lj_obj_gcw_acq(o)) {
    found += o == needle;
    assert(++n < LJ_GC2_ROOT_SCAN_LIMIT);
  }
  return found;
}

static uint32_t small_root_state(GCobj *o)
{
  GCArena *a = lj_arena_of(o);
  uint32_t cell;
  assert(!lj_arena_ishuge(a));
  cell = lj_arena_cellof(o);
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  return lj_arena_root_state_acq(a, cell);
}

static uint32_t small_lifetime_state(const void *base)
{
  GCArena *a = lj_arena_of(base);
  uint32_t cell;
  assert(!lj_arena_ishuge(a));
  cell = lj_arena_cellof(base);
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  return lj_arena_lifetime_state_acq(a, cell);
}

static int after_main_contains(global_State *g, GCobj *needle)
{
  GCobj *o = obj2gco(mainthread_acq(g));
  uint32_t n = 0;
  for (o = lj_obj_gcw_acq(o); o != NULL; o = lj_obj_gcw_acq(o)) {
    if (o == needle)
      return 1;
    assert(++n < LJ_GC2_ROOT_SCAN_LIMIT);
  }
  return 0;
}

static void pending_load_hook(global_State *g, TGState *tg,
			      GCobj *published, GCobj *observed,
			      uint32_t path)
{
  PendingRace *race = active_race;
  if (!race || path != race->path)
    return;
  assert(g == race->g);
  assert(tg == race->tg && tg == g->main_tg);
  assert(observed == race->oldhead);
  assert(la_load32_acq(&race->stage) == 0);
  race->published = published;

  /* One matching publication is enough. Do not let constructor-internal
  ** publications accidentally enter the same scheduling latch. */
  lj_gc_test_set_root_pending_load_hook(NULL);
  la_store32_rel(&race->stage, 1);
  spin_until(&race->stage, 2);
}

static void *flush_loaded_head(void *arg)
{
  PendingRace *race = (PendingRace *)arg;
  spin_until(&race->stage, 1);
  race->flushed = lj_gc_flush_root_pending(race->g);
  if (race->path == LJ_GC_ROOT_PENDING_TEST_AFTER_MAIN)
    assert(lj_tg_gcroot_pending_after_main_acq(race->tg) == NULL);
  else
    assert(lj_tg_gcroot_pending_acq(race->tg) == NULL);
  la_store32_rel(&race->stage, 2);
  return NULL;
}

static pthread_t begin_race(PendingRace *race, global_State *g, TGState *tg,
			    GCobj *oldhead, uint32_t path)
{
  pthread_t thread;
  race->g = g;
  race->tg = tg;
  race->oldhead = oldhead;
  race->published = NULL;
  race->path = path;
  race->stage = 0;
  race->flushed = 0;
  active_race = race;
  lj_gc_test_set_root_pending_load_hook(pending_load_hook);
  assert(pthread_create(&thread, NULL, flush_loaded_head, race) == 0);
  return thread;
}

static void finish_race(PendingRace *race, pthread_t thread)
{
  assert(pthread_join(thread, NULL) == 0);
  assert(la_load32_acq(&race->stage) == 2);
  assert(race->flushed >= 1u);
  assert(race->published != NULL);
  active_race = NULL;
  lj_gc_test_set_root_pending_load_hook(NULL);
}

static void init_closed_upvalue(global_State *g, GCupval *uv)
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

static GCupval *new_closed_upvalue_constructing(lua_State *L)
{
  global_State *g = G(L);
  GCupval *uv = (GCupval *)
    lj_mem_newgco_unlinked_nothrow(L, sizeof(GCupval));
  assert(uv != NULL);
  init_closed_upvalue(g, uv);
  return uv;
}

static GCupval *new_closed_upvalue_live_unlinked(lua_State *L)
{
  global_State *g = G(L);
  GCupval *uv = (GCupval *)lj_mem_newgco_raw_nothrow(
    L, sizeof(GCupval), LJ_AF_TRAVERSABLE);
  assert(uv != NULL);
  init_closed_upvalue(g, uv);
  lj_gc_publishobj_header(g, obj2gco(uv));
  return uv;
}

static void test_construct_commit_and_abandon(lua_State *L)
{
  global_State *g = G(L);
  GCupval *uv = new_closed_upvalue_constructing(L);
  GCobj *o = obj2gco(uv);
  GCArena *a;
  uint32_t cell;

  assert(small_lifetime_state(o) == LJ_ARENA_LIFETIME_CONSTRUCT);
  assert(small_root_state(o) == LJ_ARENA_ROOT_LINKING);
  assert(lj_gc_linkobj_new(g, o) == LJ_GC_ROOT_LINKED);
  assert(small_lifetime_state(o) == LJ_ARENA_LIFETIME_LIVE);
  assert(small_root_state(o) == LJ_ARENA_ROOT_MEMBER);

  /* Recovery may own MUTATING after READY while the unique constructor still
  ** owns LINKING. Root commit succeeds without stealing that lifetime lane. */
  uv = new_closed_upvalue_constructing(L);
  o = obj2gco(uv);
  a = lj_arena_of(o);
  cell = lj_arena_cellof(o);
  lj_gc_publishobj_header(g, o);
  assert(lj_arena_lifetime_state_cas(a, cell,
				     LJ_ARENA_LIFETIME_CONSTRUCT,
				     LJ_ARENA_LIFETIME_MUTATING));
  assert(lj_gc_linkobj_new(g, o) == LJ_GC_ROOT_LINKED);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_MEMBER);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_MUTATING);
  assert(lj_arena_lifetime_state_cas(a, cell,
				     LJ_ARENA_LIFETIME_MUTATING,
				     LJ_ARENA_LIFETIME_LIVE));

  /* The symmetric cancellation clears LINKING while recovery retains its
  ** mutation claim. Its completion restores LIVE before ordinary free. */
  uv = new_closed_upvalue_constructing(L);
  o = obj2gco(uv);
  a = lj_arena_of(o);
  cell = lj_arena_cellof(o);
  lj_gc_publishobj_header(g, o);
  assert(lj_arena_lifetime_state_cas(a, cell,
				     LJ_ARENA_LIFETIME_CONSTRUCT,
				     LJ_ARENA_LIFETIME_MUTATING));
  assert(lj_mem_abandon_gco_unpublished(g, o) ==
	 LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_MUTATING);
  assert(lj_arena_lifetime_state_cas(a, cell,
				     LJ_ARENA_LIFETIME_MUTATING,
				     LJ_ARENA_LIFETIME_LIVE));
  lj_mem_free(g, o, sizeof(GCupval));
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_FREE);

  uv = new_closed_upvalue_constructing(L);
  o = obj2gco(uv);
  a = lj_arena_of(o);
  cell = lj_arena_cellof(o);
  assert(small_lifetime_state(o) == LJ_ARENA_LIFETIME_CONSTRUCT);
  assert(small_root_state(o) == LJ_ARENA_ROOT_LINKING);
  lj_mem_freegco_unpublished(g, o, sizeof(GCupval));
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_FREE);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
}

static void test_construct_publish_behind_foreign_reclaimer_gate(lua_State *L)
{
  global_State *g = G(L);
  GCupval *uv = new_closed_upvalue_constructing(L);
  GCobj *o = obj2gco(uv);
  uint32_t expect = 0;

  /* This thread did not acquire a reclaimer certificate. A fresh fixed-layout
  ** constructor nevertheless owns exact CONSTRUCT|LINKING identity and must
  ** publish without entering the process-wide registry SMR reader. */
  assert(gc2_smr_readers_acq(g) == 0);
  assert(!lj_gc2_reclaim_context_held(g));
  assert(gc2_smr_reclaiming_cas(g, &expect, 1));
  assert(lj_gc_linkobj_new(g, o) == LJ_GC_ROOT_LINKED);
  assert(small_lifetime_state(o) == LJ_ARENA_LIFETIME_LIVE);
  assert(small_root_state(o) == LJ_ARENA_ROOT_MEMBER);
  gc2_smr_reclaiming_rel(g, 0);

  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(root_contains(g, o));
}

static void test_destructor_terminal_gate(lua_State *L)
{
  global_State *g = G(L);
  GCupval *uv = new_closed_upvalue_live_unlinked(L);
  GCArena *a = lj_arena_of(uv);
  uint32_t cell = lj_arena_cellof(uv);
  LJGCDestructCtx dctx, duplicate;

  assert(lj_gc_destructor_enter(g, uv, sizeof(GCupval), &dctx) ==
	 LJ_GC_DESTRUCT_ACQUIRED);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_FREE);
  assert(lj_gc_destructor_enter(g, uv, sizeof(GCupval), &duplicate) ==
	 LJ_GC_DESTRUCT_OWNED);
  /* The type function may mutate/account only after the unique terminal LP. */
  lj_func_freeuv(g, uv);
  lj_gc_destructor_leave(g, &dctx);
}

static void test_duplicate_link_is_idempotent(lua_State *L)
{
  global_State *g = G(L);
  GCtab *t = lj_tab_new(L, 0, 0);
  GCobj *o = obj2gco(t);
  GCobj *next;

  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(root_occurrences(g, o) == 1u);
  assert(small_root_state(o) == LJ_ARENA_ROOT_MEMBER);
  next = lj_obj_gcw_acq(o);
  assert(lj_gc_linkobj(g, o) == LJ_GC_ROOT_LINK_ALREADY);
  assert(lj_obj_gcw_acq(o) == next);
  assert(root_occurrences(g, o) == 1u);
  assert(small_root_state(o) == LJ_ARENA_ROOT_MEMBER);
}

static void test_explicit_membership_claim_races(lua_State *L)
{
  global_State *g = G(L);
  RootStateRace race;
  pthread_t thread;
  GCupval *uv = new_closed_upvalue_live_unlinked(L);
  GCobj *o = obj2gco(uv);
  GCobj *next;

  assert(small_root_state(o) == LJ_ARENA_ROOT_NONE);
  assert(small_lifetime_state(o) == LJ_ARENA_LIFETIME_LIVE);
  thread = begin_root_state_race(&race, g, o,
				 LJ_GC_ROOT_STATE_TEST_LINKING);
  assert(small_root_state(o) == LJ_ARENA_ROOT_LINKING);
  assert(small_lifetime_state(o) == LJ_ARENA_LIFETIME_MUTATING);
  next = lj_obj_gcw_acq(o);
  assert(lj_gc_linkobj(g, o) == LJ_GC_ROOT_LINK_DEFER);
  assert(lj_obj_gcw_acq(o) == next);
  assert(lj_gc_unlink_root_obj(g, o) == LJ_GC_ROOT_UNLINK_UNPROVEN);
  assert(root_occurrences(g, o) == 0u);
  finish_root_state_race(&race, thread);
  assert(race.result == LJ_GC_ROOT_LINKED);
  assert(small_root_state(o) == LJ_ARENA_ROOT_MEMBER);
  assert(small_lifetime_state(o) == LJ_ARENA_LIFETIME_LIVE);
  assert(root_occurrences(g, o) == 1u);

  thread = begin_root_state_race(&race, g, o,
				 LJ_GC_ROOT_STATE_TEST_UNLINKING);
  assert(small_root_state(o) == LJ_ARENA_ROOT_UNLINKING);
  assert(small_lifetime_state(o) == LJ_ARENA_LIFETIME_LIVE);
  next = lj_obj_gcw_acq(o);
  assert(lj_gc_linkobj(g, o) == LJ_GC_ROOT_LINK_DEFER);
  assert(lj_obj_gcw_acq(o) == next);
  assert(root_occurrences(g, o) == 1u);
  finish_root_state_race(&race, thread);
  assert(race.result == LJ_GC_ROOT_UNLINKED);
  assert(small_root_state(o) == LJ_ARENA_ROOT_NONE);
  assert(small_lifetime_state(o) == LJ_ARENA_LIFETIME_LIVE);
  assert(root_occurrences(g, o) == 0u);

  assert(lj_gc_linkobj(g, o) == LJ_GC_ROOT_LINKED);
  assert(small_root_state(o) == LJ_ARENA_ROOT_MEMBER);
  assert(small_lifetime_state(o) == LJ_ARENA_LIFETIME_LIVE);
  assert(root_occurrences(g, o) == 1u);
}

#if LJ_HASFFI
static void test_variable_cdata_arbitrates_before_validation(lua_State *L)
{
  global_State *g = G(L);
  GCcdata *cd;
  GCobj *o;
  GCArena *a;
  void *base;
  uint32_t cell;
  uint8_t gct;

  assert(luaL_dostring(L,
    "local ffi=require('ffi'); return ffi.new('uint8_t[?]', 257)") ==
    LUA_OK);
  assert(tviscdata(L->top - 1));
  cd = cdataV(L->top - 1);
  assert(cdataisv(cd));
  o = obj2gco(cd);
  base = memcdatav(cd);
  a = lj_arena_of(base);
  assert(!lj_arena_ishuge(a));
  cell = lj_arena_cellof(base);

  (void)lj_gc_flush_root_pending(g);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_MEMBER);
  assert(lj_gc_unlink_root_obj(g, o) == LJ_GC_ROOT_UNLINKED);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_LIVE);

  /* Model another descriptor owner, then make every post-arbitration check
  ** fail. The losing base-hinted requeue must return DEFER without consulting
  ** the interior header or READY; validation-first code returns INVALID. */
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_MUTATING));
  gct = la_load8_acq(&cd->gct);
  la_store8_rel(&cd->gct, 0);
  lj_arena_bm_clear(a->ready, cell);
  assert(lj_gc_linkobj_at(g, o, base) == LJ_GC_ROOT_LINK_DEFER);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_MUTATING);
  la_store8_rel(&cd->gct, gct);
  lj_arena_bm_set(a->ready, cell);
  assert(lj_arena_lifetime_state_cas(a, cell,
				     LJ_ARENA_LIFETIME_MUTATING,
				     LJ_ARENA_LIFETIME_LIVE));

  /* late[] is irrevocable logical-free provenance. It is rechecked while the
  ** linker owns MUTATING and must defeat publication before header validation. */
  (void)la_bit_test_and_set64(&a->late[cell >> 6], cell & 63);
  la_store8_rel(&cd->gct, 0);
  lj_arena_bm_clear(a->ready, cell);
  assert(lj_gc_linkobj_at(g, o, base) == LJ_GC_ROOT_LINK_DEFER);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
  lj_arena_bm_clear(a->late, cell);
  la_store8_rel(&cd->gct, gct);
  lj_arena_bm_set(a->ready, cell);

  /* Ordinary root publication never manufactures RESCUE. A tentative
  ** destructor retains DESTRUCT on a losing root attempt; once its exact
  ** DESTRUCT->FREE LP wins, the same poisoned-header attempt still defers. */
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_DESTRUCT));
  la_store8_rel(&cd->gct, 0);
  lj_arena_bm_clear(a->ready, cell);
  assert(lj_gc_linkobj_at(g, o, base) == LJ_GC_ROOT_LINK_DEFER);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_DESTRUCT);
  assert(lj_arena_lifetime_state_cas(a, cell,
				     LJ_ARENA_LIFETIME_DESTRUCT,
				     LJ_ARENA_LIFETIME_FREE));
  assert(lj_gc_linkobj_at(g, o, base) == LJ_GC_ROOT_LINK_DEFER);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_FREE);
  /* Fixture-only rollback: no destructor touched bytes in this synthetic LP. */
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_FREE,
				     LJ_ARENA_LIFETIME_LIVE));
  la_store8_rel(&cd->gct, gct);
  lj_arena_bm_set(a->ready, cell);

  assert(lj_gc_linkobj_at(g, o, base) == LJ_GC_ROOT_LINKED);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_MEMBER);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  lua_pop(L, 1);
}
#endif

static void enter_synthetic_sweep(global_State *g)
{
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lj_gc2_test_activation_mirror_edge(g, LJ_GC2_IDLE, LJ_GC2_MARK);
  gc2_phase_rel(g, LJ_GC2_MARK);
  gc2_phase_rel(g, LJ_GC2_WEAK);
  lj_gc2_test_activation_mirror_edge(g, LJ_GC2_MARK, LJ_GC2_WEAK);
  lj_gc2_test_activation_mirror_edge(g, LJ_GC2_WEAK, LJ_GC2_SWEEP);
  gc2_phase_rel(g, LJ_GC2_SWEEP);
  assert(!lj_gc2_activation_reclaim_veto(g));
}

static void leave_synthetic_sweep(global_State *g)
{
  lj_gc2_test_activation_mirror_edge(g, LJ_GC2_SWEEP, LJ_GC2_IDLE);
  gc2_phase_rel(g, LJ_GC2_IDLE);
  assert(!lj_gc2_activation_reclaim_veto(g));
}

static void test_small_reanchor_commits_once(lua_State *L)
{
  global_State *g = G(L);
  RootStateRace race;
  pthread_t thread;
  GCupval *uv = new_closed_upvalue_live_unlinked(L);
  GCobj *o = obj2gco(uv);
  GCArena *a = lj_arena_of(o);
  GCobj *next;
  uint32_t cell = lj_arena_cellof(o);
  uint32_t flags0 = lj_arena_flags_acq(a);
  uint32_t cursor0 = a->hdr.reclaim_cell;

  assert(lj_gc_linkobj(g, o) == LJ_GC_ROOT_LINKED);
  assert(lj_gc_unlink_root_obj(g, o) == LJ_GC_ROOT_UNLINKED);
  assert(small_root_state(o) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_WHITE,
					  LJ_ARENA_SWEEP_LIVE));
  la_store32_rel(&a->hdr.flags, flags0 | LJ_AF_QUARANTINE);
  a->hdr.reclaim_cell = cell;
  enter_synthetic_sweep(g);

  thread = begin_small_reanchor_race(&race, g, o, a);
  assert(small_root_state(o) == LJ_ARENA_ROOT_MEMBER);
  assert(lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_LIVE);
  assert(root_occurrences(g, o) == 1u);
  next = lj_obj_gcw_acq(o);
  assert(lj_gc_linkobj(g, o) == LJ_GC_ROOT_LINK_ALREADY);
  assert(lj_obj_gcw_acq(o) == next);
  assert(root_occurrences(g, o) == 1u);

  finish_root_state_race(&race, thread);
  assert(race.result == 1);
  assert(small_root_state(o) == LJ_ARENA_ROOT_MEMBER);
  assert(lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_WHITE);
  assert(root_occurrences(g, o) == 1u);

  leave_synthetic_sweep(g);
  a->hdr.reclaim_cell = cursor0;
  la_store32_rel(&a->hdr.flags, flags0);
}

static void assert_single_thread_fastpath_window(global_State *g,
					  TGState *tg)
{
  assert(tg == g->main_tg);
  assert(mt_active_acq(g) == 0);
  assert(mt_entering_acq(g) == 0);
  assert(gc2_n_workers_acq(g) == 0);
}

static void test_ordinary_retry(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  PendingRace race;
  pthread_t thread;
  GCtab *old, *fresh;

  (void)lj_gc_flush_root_pending(g);
  assert_single_thread_fastpath_window(g, tg);
  old = lj_tab_new(L, 0, 0);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(old));

  thread = begin_race(&race, g, tg, obj2gco(old),
		      LJ_GC_ROOT_PENDING_TEST_ORDINARY);
  fresh = lj_tab_new(L, 0, 0);
  finish_race(&race, thread);

  assert(race.published == obj2gco(fresh));
  assert(root_contains(g, obj2gco(old)));
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(fresh));
  /* The xchg transferred old to a different intrusive list after the
  ** publisher sampled its address. The failed CAS must rewrite this link from
  ** the fresh NULL head; retaining old here is the stale-tail/ABA bug. */
  assert(lj_obj_gcw_acq(obj2gco(fresh)) == NULL);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(root_contains(g, obj2gco(fresh)));
}

#if LJ_TARGET_X64 && LJ_GC64
static void test_vm_tnew_retry(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  PendingRace race;
  pthread_t thread;
  GCtab *old, *fresh;

  /* Load the closure before arming the hook, so the only matching publication
  ** is BC_TNEW's inlined x64 bump path. Its test-build bridge pauses after the
  ** initial pending-head load and before the retrying CMPXCHG. */
  assert(luaL_loadstring(L, "return {}\n") == LUA_OK);
  (void)lj_gc_flush_root_pending(g);
  assert_single_thread_fastpath_window(g, tg);
  old = lj_tab_new(L, 0, 0);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(old));

  thread = begin_race(&race, g, tg, obj2gco(old),
		      LJ_GC_ROOT_PENDING_TEST_VM_TNEW);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  finish_race(&race, thread);

  assert(tvistab(L->top - 1));
  fresh = tabV(L->top - 1);
  assert(race.published == obj2gco(fresh));
  /* The real VM CAS has completed both pending-head hints before exposing the
  ** final MEMBER lane. Flush may transfer the edge, but never owns this state. */
  assert(small_root_state(obj2gco(fresh)) == LJ_ARENA_ROOT_MEMBER);
  assert(root_contains(g, obj2gco(old)));
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(fresh));
  assert(lj_obj_gcw_acq(obj2gco(fresh)) == NULL);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(root_contains(g, obj2gco(fresh)));
  assert(small_root_state(obj2gco(fresh)) == LJ_ARENA_ROOT_MEMBER);
  lua_pop(L, 1);
}
#endif

static void test_chain_tail_retry(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  PendingRace race;
  pthread_t thread;
  GCtab *old;
  GCupval *head, *tail;

  (void)lj_gc_flush_root_pending(g);
  assert_single_thread_fastpath_window(g, tg);
  old = lj_tab_new(L, 0, 0);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(old));
  head = new_closed_upvalue_constructing(L);
  tail = new_closed_upvalue_constructing(L);
  assert(small_lifetime_state(head) == LJ_ARENA_LIFETIME_CONSTRUCT);
  assert(small_lifetime_state(tail) == LJ_ARENA_LIFETIME_CONSTRUCT);
  assert(small_root_state(obj2gco(head)) == LJ_ARENA_ROOT_LINKING);
  assert(small_root_state(obj2gco(tail)) == LJ_ARENA_ROOT_LINKING);
  lj_obj_setgcwrel(obj2gco(head), obj2gco(tail));

  thread = begin_race(&race, g, tg, obj2gco(old),
		      LJ_GC_ROOT_PENDING_TEST_CHAIN);
  lj_gc_linkobj_new_chain(g, obj2gco(head), obj2gco(tail));
  finish_race(&race, thread);

  assert(race.published == obj2gco(tail));
  assert(root_contains(g, obj2gco(old)));
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(head));
  assert(lj_obj_gcw_acq(obj2gco(head)) == obj2gco(tail));
  assert(lj_obj_gcw_acq(obj2gco(tail)) == NULL);
  assert(small_lifetime_state(head) == LJ_ARENA_LIFETIME_LIVE);
  assert(small_lifetime_state(tail) == LJ_ARENA_LIFETIME_LIVE);
  assert(small_root_state(obj2gco(head)) == LJ_ARENA_ROOT_MEMBER);
  assert(small_root_state(obj2gco(tail)) == LJ_ARENA_ROOT_MEMBER);
  assert(lj_gc_flush_root_pending(g) == 2u);
  assert(root_contains(g, obj2gco(head)));
  assert(root_contains(g, obj2gco(tail)));
}

static void test_after_main_retry(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  PendingRace race;
  pthread_t thread;
  lua_State *old, *fresh;

  (void)lj_gc_flush_root_pending(g);
  assert_single_thread_fastpath_window(g, tg);
  old = lua_newthread(L);
  assert(old != NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == obj2gco(old));

  thread = begin_race(&race, g, tg, obj2gco(old),
		      LJ_GC_ROOT_PENDING_TEST_AFTER_MAIN);
  fresh = lua_newthread(L);
  assert(fresh != NULL);
  finish_race(&race, thread);

  assert(race.published == obj2gco(fresh));
  assert(after_main_contains(g, obj2gco(old)));
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == obj2gco(fresh));
  assert(lj_obj_gcw_acq(obj2gco(fresh)) == NULL);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(after_main_contains(g, obj2gco(fresh)));
  lua_pop(L, 2);
}

static void test_permanent_main_needs_no_live_tail_reanchor(lua_State *L)
{
  global_State *g = G(L);
  lua_State *mainL = mainthread_acq(g);
  GCobj *root0, *next0;
  lua_State *fresh;

  (void)lj_gc_flush_root_pending(g);
  root0 = lj_gc_root_acq(g);
  assert(root0 != NULL && root_contains(g, obj2gco(mainL)));
  next0 = lj_obj_gcw_acq(obj2gco(mainL));

  /* Model defensive prefix loss without allowing the flusher to walk the live
  ** after-main chain for a raw tail. The permanent anchor invariant makes this
  ** unreachable in a healthy spine; the safe response is to publish the child
  ** through main's slot and leave the independently side-rooted segment alone. */
  lj_gc_root_rel(g, NULL);
  fresh = lua_newthread(L);
  assert(fresh != NULL);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(lj_gc_root_acq(g) == NULL);
  assert(lj_obj_gcw_acq(obj2gco(mainL)) == obj2gco(fresh));
  assert(lj_obj_gcw_acq(obj2gco(fresh)) == next0);

  lj_gc_root_rel(g, root0);
  lj_gcroot_repair_epoch_add(g);
  assert(root_contains(g, obj2gco(mainL)));
  assert(root_contains(g, obj2gco(fresh)));
  lua_pop(L, 1);
}

static void test_finreg_udata_has_single_reanchor_owner(lua_State *L)
{
  global_State *g = G(L);
  lua_State *mainL = mainthread_acq(g);
  GCudata *ud;
  GCobj *o, *next0;
  GCArena *a;
  GCRef *cursor0;
  uint32_t cell, flags0, done0, phase0, state0;
  int mark0;

  (void)lj_gc_flush_root_pending(g);
  (void)lua_newuserdata(L, 8);
  ud = udataV(L->top - 1);
  o = obj2gco(ud);
  (void)lj_gc_flush_root_pending(g);
  assert(lj_obj_gcw_acq(obj2gco(mainL)) == o);
  lj_obj_addgcflags_atomic(o, LJ_GC_UDATA_FINREG);

  a = lj_arena_of(o);
  assert(!lj_arena_ishuge(a));
  cell = lj_arena_cellof(o);
  flags0 = lj_arena_flags_acq(a);
  state0 = lj_arena_sweep_state_acq(a, cell);
  mark0 = lj_arena_bm_get(a->mark, cell);
  phase0 = gc2_phase_acq(g);
  cursor0 = gc2_sweep_root_cursor_acq(g);
  done0 = gc2_sweep_root_done_acq(g);
  next0 = lj_obj_gcw_acq(o);
  assert(state0 == LJ_ARENA_SWEEP_WHITE);
  assert(phase0 == LJ_GC2_IDLE);

  /* Restrict the synthetic prune batch to this exact after-main object. With
  ** FINREG set it must remain linked: dispatch requeues before clearing FINREG,
  ** so creating a concurrent RETIRED/LIVE ticket would give O two owners. */
  lj_obj_setgcwnullrel(o);
  la_store32_rel(&a->hdr.flags, flags0 | LJ_AF_NEEDSWEEP);
  gc2_sweep_root_cursor_rel(g, lj_obj_gcwref(obj2gco(mainL)));
  gc2_sweep_root_done_rel(g, 0);
  gc2_phase_rel(g, LJ_GC2_SWEEP);
  assert(lj_gc_sweep_gc2_unmarked(g) == 0);
  assert(lj_obj_gcw_acq(obj2gco(mainL)) == o);
  assert(lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_WHITE);

  gc2_phase_rel(g, phase0);
  gc2_sweep_root_cursor_rel(g, cursor0);
  gc2_sweep_root_done_rel(g, done0);
  la_store32_rel(&a->hdr.flags, flags0);
  if (mark0)
    lj_arena_bm_set(a->mark, cell);
  else
    lj_arena_bm_clear(a->mark, cell);
  lj_obj_setgcwrel(o, next0);
  lj_obj_cleargcflags_atomic(o, LJ_GC_UDATA_FINREG);
  lua_pop(L, 1);
}

static void test_root_unlink_rejects_string_successor(lua_State *L)
{
  global_State *g = G(L);
  GCtab *target = lj_tab_new(L, 0, 0);
  GCstr *stale = lj_str_newlit(L, "root-unlink-string-sentinel");
  GCobj *saved;

  (void)lj_gc_flush_root_pending(g);
  assert(root_contains(g, obj2gco(target)));
  assert(lj_gc_unlink_root_obj(g, obj2gco(target)) ==
	 LJ_GC_ROOT_UNLINKED);
  assert(!root_contains(g, obj2gco(target)));
  assert(lj_gc_unlink_root_obj(g, obj2gco(target)) ==
	 LJ_GC_ROOT_UNLINK_ABSENT);

  /* Restore the target, then inject a real interned string as the acquired
  ** root head. Unlink must sever that incoming edge without interpreting the
  ** string hash successor as gcw, and must report that target absence was not
  ** proved. The original valid chain is restored before leaving the fixture. */
  lj_gc_linkobj(g, obj2gco(target));
  saved = lj_gc_root_acq(g);
  lj_gc_root_rel(g, obj2gco(stale));
  lj_gcroot_repair_epoch_add(g);
  assert(lj_gc_unlink_root_obj(g, obj2gco(target)) ==
	 LJ_GC_ROOT_UNLINK_UNPROVEN);
  assert(lj_gc_root_acq(g) == NULL);
  lj_gc_root_rel(g, saved);
  lj_gcroot_repair_epoch_add(g);
  assert(root_contains(g, obj2gco(target)));
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  luaL_openlibs(L);
  test_construct_commit_and_abandon(L);
  test_construct_publish_behind_foreign_reclaimer_gate(L);
  test_destructor_terminal_gate(L);
  test_duplicate_link_is_idempotent(L);
  test_explicit_membership_claim_races(L);
#if LJ_HASFFI
  test_variable_cdata_arbitrates_before_validation(L);
#endif
  test_small_reanchor_commits_once(L);
  test_ordinary_retry(L);
#if LJ_TARGET_X64 && LJ_GC64
  test_vm_tnew_retry(L);
#endif
  test_chain_tail_retry(L);
  test_after_main_retry(L);
  test_permanent_main_needs_no_live_tail_reanchor(L);
  test_finreg_udata_has_single_reanchor_owner(L);
  test_root_unlink_rejects_string_successor(L);
  lua_close(L);
  puts("t-gc-root-pending-race OK: construction/lifetime CAS, retries, stable FINREG ownership, and safe unlink verified");
  return 0;
}
