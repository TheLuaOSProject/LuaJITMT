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

static PendingRace *active_race;

static void spin_until(const uint32_t *p, uint32_t value)
{
  while (la_load32_acq(p) != value)
    la_cpu_pause();
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

static GCupval *new_closed_upvalue_unlinked(lua_State *L)
{
  global_State *g = G(L);
  GCupval *uv = (GCupval *)
    lj_mem_newgco_unlinked_nothrow(L, sizeof(GCupval));
  assert(uv != NULL);
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  uv->immutable = 0;
  setnilV(&uv->tv);
  setmref(uv->v, &uv->tv);
  uv->dhash = 0;
  newwhite(g, uv);
  lj_obj_setgcwnullrel(obj2gco(uv));
  return uv;
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
  assert(root_contains(g, obj2gco(old)));
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(fresh));
  assert(lj_obj_gcw_acq(obj2gco(fresh)) == NULL);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(root_contains(g, obj2gco(fresh)));
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
  head = new_closed_upvalue_unlinked(L);
  tail = new_closed_upvalue_unlinked(L);
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
  puts("t-gc-root-pending-race OK: CAS retries, stable main/FINREG ownership, and string-safe unlink verified");
  return 0;
}
