/*
** Focused test for the GC2 legacy phase scaffold.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tg.h"

static int arena_list_contains(GCArena *a, GCArena *needle)
{
  while (a) {
    if (a == needle)
      return 1;
    a = a->hdr.next;
  }
  return 0;
}

static void assert_idle(global_State *g, TGState *tg)
{
  assert(g->gc2.phase == LJ_GC2_IDLE);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);
}

typedef struct PeerRelease {
  global_State *g;
  long delay_ns;
} PeerRelease;

static void sleep_ns(long ns)
{
  struct timespec ts;
  ts.tv_sec = ns / 1000000000L;
  ts.tv_nsec = ns % 1000000000L;
  while (nanosleep(&ts, &ts) != 0) {
  }
}

static void *release_worker_active(void *arg)
{
  PeerRelease *rel = (PeerRelease *)arg;
  sleep_ns(rel->delay_ns);
  la_store32_rel(&rel->g->gc2.worker_active, 0);
  return NULL;
}

static int finalizer_churn(lua_State *L)
{
  int status = luaL_dostring(L,
    "local hold = {}\n"
    "for i = 1, 96 do\n"
    "  local t = {i, i+1, i+2}\n"
    "  for j = 4, 48 do t[j] = j end\n"
    "  hold[i] = t\n"
    "end\n");
  if (status != LUA_OK)
    lua_pop(L, 1);
  return 0;
}

static void test_phase_transition_guards(global_State *g, TGState *tg)
{
  uint64_t mark_to_weak0, weak_to_sweep0;

  assert_idle(g, tg);
  mark_to_weak0 = la_load64_acq(&g->gc2.mark_to_weak);
  weak_to_sweep0 = la_load64_acq(&g->gc2.weak_to_sweep);
  lj_gc2_mark_to_weak(g);
  assert(g->gc2.phase == LJ_GC2_IDLE);
  assert(la_load64_acq(&g->gc2.mark_to_weak) == mark_to_weak0);
  lj_gc2_weak_to_sweep(g);
  assert(g->gc2.phase == LJ_GC2_IDLE);
  assert(la_load64_acq(&g->gc2.weak_to_sweep) == weak_to_sweep0);

  la_store32_rel(&g->gc2.phase, LJ_GC2_MARK);
  lj_gc2_weak_to_sweep(g);
  assert(g->gc2.phase == LJ_GC2_MARK);
  assert(la_load64_acq(&g->gc2.weak_to_sweep) == weak_to_sweep0);

  la_store32_rel(&g->gc2.phase, LJ_GC2_WEAK);
  lj_gc2_mark_to_weak(g);
  assert(g->gc2.phase == LJ_GC2_WEAK);
  assert(la_load64_acq(&g->gc2.mark_to_weak) == mark_to_weak0);

  la_store32_rel(&g->gc2.phase, LJ_GC2_IDLE);
  assert_idle(g, tg);
}

static void test_mark_complete_waits_for_peer(lua_State *L, global_State *g,
					      TGState *tg)
{
  GCtab *parent, *child;
  PeerRelease rel;
  pthread_t thread;
  uint64_t runs0, hits0, waits0;

  lua_settop(L, 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);
  lua_pop(L, 1);

  lj_gc2_legacy_mark_begin(g);
  setgcrefnull(g->gc.gray);
  setgcrefnull(g->gc.grayagain);
  setgcrefnull(g->gc.weak);
  g->gc.state = GCSpropagate;
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(!lj_gc2_ssb_empty(g));

  la_store32_rel(&g->gc2.worker_active, 1);
  rel.g = g;
  rel.delay_ns = 20000000L;
  runs0 = la_load64_acq(&g->gc2.mark_complete_runs);
  hits0 = la_load64_acq(&g->gc2.mark_complete_hits);
  waits0 = la_load64_acq(&g->gc2.mark_complete_peer_waits);
  assert(pthread_create(&thread, NULL, release_worker_active, &rel) == 0);
  assert(lj_gc2_mark_complete(g, L, 2, ~(uint32_t)0) == 1);
  assert(pthread_join(thread, NULL) == 0);
  assert(la_load32_acq(&g->gc2.worker_active) == 0);
  assert(la_load64_acq(&g->gc2.mark_complete_runs) == runs0 + 1u);
  assert(la_load64_acq(&g->gc2.mark_complete_hits) == hits0 + 1u);
  assert(la_load64_acq(&g->gc2.mark_complete_peer_waits) > waits0);
  assert(lj_gc2_ssb_empty(g));
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);

  g->gc.state = GCSpause;
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);
}

static void test_incremental_worker_step(lua_State *L, global_State *g,
					 TGState *tg)
{
  GCtab *parent, *child, *grandchild;
  uint64_t worker_runs0, worker_grey0, worker_ssb0;
  uint32_t old_stepmul = g->gc.stepmul;

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
  setgcrefnull(g->gc.gray);
  setgcrefnull(g->gc.grayagain);
  setgcrefnull(g->gc.weak);
  g->gc.state = GCSpropagate;
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(!lj_gc2_ssb_empty(g));

  worker_runs0 = la_load64_acq(&g->gc2.worker_runs);
  worker_grey0 = la_load64_acq(&g->gc2.worker_grey_drained);
  worker_ssb0 = la_load64_acq(&g->gc2.worker_ssb_converted);
  g->gc.stepmul = 1;
  g->gc.debt = 0;
  lj_gc_threshold_store(g, g->gc.total);
  assert(lj_gc_step(L) <= 0);
  assert(g->gc.state == GCSpropagate);
  assert(lj_gc2_ssb_empty(g));
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 1);
  assert(la_load64_acq(&g->gc2.worker_runs) == worker_runs0 + 1u);
  assert(la_load64_acq(&g->gc2.worker_grey_drained) == worker_grey0 + 3u);
  assert(la_load64_acq(&g->gc2.worker_ssb_converted) == worker_ssb0 + 1u);

  g->gc.stepmul = old_stepmul;
  g->gc.state = GCSpause;
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}

static void test_incremental_fixpoint_round(lua_State *L, global_State *g)
{
  GCtab *parent, *child, *grandchild;
  uint64_t rounds0, hits0, worker_runs0, worker_grey0, worker_ssb0;
  uint32_t old_stepmul = g->gc.stepmul;

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
  lua_pop(L, 2);  /* Keep only parent as the stack root. */

  lj_gc2_legacy_mark_begin(g);
  setgcrefnull(g->gc.gray);
  setgcrefnull(g->gc.grayagain);
  setgcrefnull(g->gc.weak);
  g->gc.state = GCSpropagate;
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  assert(lj_gc2_ssb_empty(g));

  rounds0 = la_load64_acq(&g->gc2.fixpoint_rounds);
  hits0 = la_load64_acq(&g->gc2.fixpoint_hits);
  worker_runs0 = la_load64_acq(&g->gc2.worker_runs);
  worker_grey0 = la_load64_acq(&g->gc2.worker_grey_drained);
  worker_ssb0 = la_load64_acq(&g->gc2.worker_ssb_converted);
  g->gc.stepmul = 1;
  g->gc.debt = 0;
  lj_gc_threshold_store(g, g->gc.total);
  assert(lj_gc_step(L) <= 0);
  assert(g->gc2.phase == LJ_GC2_MARK);
  assert(g->gc.state == GCSpropagate);
  assert(la_load64_acq(&g->gc2.fixpoint_rounds) == rounds0 + 1u);
  assert(la_load64_acq(&g->gc2.fixpoint_hits) == hits0);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 1);
  assert(la_load64_acq(&g->gc2.worker_runs) > worker_runs0);
  assert(la_load64_acq(&g->gc2.worker_grey_drained) > worker_grey0);
  assert(la_load64_acq(&g->gc2.worker_ssb_converted) > worker_ssb0);

  g->gc.stepmul = old_stepmul;
  g->gc.state = GCSpause;
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);
}

static void test_isolated_weak_skip_case(const char *mode)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  uint64_t skipped0, fallbacks0, weak_clear_tables0, weak_clear_cleared0;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert_idle(g, tg);

  lua_newtable(L);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushstring(L, mode);
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  lua_newtable(L);
  lua_pushvalue(L, -2);
  lua_pushvalue(L, -2);
  lua_settable(L, 1);
  lua_pop(L, 2);  /* Keep only the weak table as a stack root. */

  skipped0 = la_load64_acq(&g->gc2.weak_legacy_skipped);
  fallbacks0 = la_load64_acq(&g->gc2.weak_legacy_fallbacks);
  weak_clear_tables0 = la_load64_acq(&g->gc2.weak_clear_tables);
  weak_clear_cleared0 = la_load64_acq(&g->gc2.weak_clear_cleared);
  lj_gc_fullgc(L);
  assert(la_load64_acq(&g->gc2.weak_clear_tables) > weak_clear_tables0);
  assert(la_load64_acq(&g->gc2.weak_clear_cleared) > weak_clear_cleared0);
  assert(la_load64_acq(&g->gc2.weak_legacy_skipped) == skipped0 + 1u);
  assert(la_load64_acq(&g->gc2.weak_legacy_fallbacks) == fallbacks0);
  assert_idle(g, tg);

  lua_close(L);
}

int main(void)
{
  lua_State *L;
  global_State *g;
  TGState *tg;
  GCtab *phase_tab, *phase_child;
  uint32_t cycle0;
  uint32_t ssb_published0, ssb_drained0;
  uint64_t grey_pushed0, grey_drained0;
  uint64_t fixpoint_rounds0, fixpoint_hits0;
  uint64_t mark_complete_runs0, mark_complete_hits0, mark_to_weak0;
  uint64_t weak_complete_runs0, weak_complete_progress0, weak_to_sweep0;
  uint64_t sweep_to_idle0, preserve_abort_to_idle0;
  uint64_t sweep_live_updates0, live_estimate;
  uint64_t worker_weak0, weak_clear_tables0, weak_clear_cleared0;
  uint64_t weak_legacy_fallbacks0;
  uint64_t finalizer_enters0, finalizer_leaves0, finalizer_blocks0;
  MSize weak_n;
  void *phase_plain, *phase_trav;
  GCArena *phase_plain_a, *phase_trav_a;
  GCRef mmudata0;
  int i, done = 0, saw_mark = 0, saw_sweep = 0;

  test_isolated_weak_skip_case("v");
  test_isolated_weak_skip_case("k");
  test_isolated_weak_skip_case("kv");

  L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert(g->gc2.tg_list == tg);
  assert(g->gc2.n_threads == 1);
  assert_idle(g, tg);

  test_phase_transition_guards(g, tg);
  test_incremental_worker_step(L, g, tg);
  test_incremental_fixpoint_round(L, g);
  test_mark_complete_waits_for_peer(L, g, tg);

  lua_newtable(L);
  phase_tab = tabV(L->top - 1);
  lua_newtable(L);
  phase_child = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "child");
  cycle0 = g->gc2.cycle;
  lj_gc2_legacy_mark_begin(g);
  assert(g->gc2.phase == LJ_GC2_MARK);
  assert(g->gc2.cycle == cycle0 + 1u);
  assert(la_load64_acq(&g->gc2.marks_this_round) == 0);
  assert(tg->mark_active == 1);
  assert(tg->alloc.alloc_black == 1);
  assert(lj_gc2_ismarked(g, obj2gco(phase_tab)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(phase_child)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(phase_tab)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(phase_tab)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  ssb_published0 = la_load32_acq(&g->gc2.ssb_published);
  ssb_drained0 = la_load32_acq(&g->gc2.ssb_drained);
  grey_pushed0 = la_load64_acq(&g->gc2.grey_pushed);
  grey_drained0 = la_load64_acq(&g->gc2.grey_drained);
  phase_plain = lj_arena_alloc(&tg->alloc, &tg->prng, 64, 0);
  phase_trav = lj_arena_alloc(&tg->alloc, &tg->prng, 64,
			      LJ_AF_TRAVERSABLE);
  assert(phase_plain != NULL);
  assert(phase_trav != NULL);
  phase_plain_a = lj_arena_of(phase_plain);
  phase_trav_a = lj_arena_of(phase_trav);
  lj_gc2_legacy_weak_begin(g);
  assert(g->gc2.phase == LJ_GC2_WEAK);
  assert(tg->mark_active == 1);
  assert(tg->alloc.alloc_black == 1);
  lj_gc2_legacy_sweep_begin(g);
  assert(g->gc2.phase == LJ_GC2_SWEEP);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 1);
  assert(la_load32_acq(&g->gc2.ssb_published) == ssb_published0 + 1u);
  assert(tg->ssb_next == tg->ssb_base);
  assert(la_load32_acq(&g->gc2.ssb_drained) == ssb_drained0 + 1u);
  assert(lj_gc2_ssb_empty(g));
  assert(lj_gc2_ismarked(g, obj2gco(phase_tab)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(phase_child)) == 1);
  assert(la_load64_acq(&g->gc2.grey_pushed) == grey_pushed0 + 2u);
  assert(la_load64_acq(&g->gc2.grey_drained) == grey_drained0 + 2u);
  assert(tg->alloc.bump[LJ_ARENAK_PLAIN].a == NULL);
  assert(tg->alloc.bump[LJ_ARENAK_TRAVERSABLE].a == NULL);
  assert(arena_list_contains(tg->alloc.needsweep[LJ_ARENAK_PLAIN],
			     phase_plain_a));
  assert(arena_list_contains(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     phase_trav_a));
  assert((phase_plain_a->hdr.flags & LJ_AF_NEEDSWEEP) != 0);
  assert((phase_trav_a->hdr.flags & LJ_AF_NEEDSWEEP) != 0);
  lj_arena_alloc_restore_sweep_kind(&tg->alloc, LJ_ARENAK_TRAVERSABLE);
  lj_arena_alloc_restore_sweep_kind(&tg->alloc, LJ_ARENAK_PLAIN);
  assert(tg->alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  tg->alloc.sweep_epoch = g->gc2.cycle;  /* Synthetic close boundary. */
  sweep_to_idle0 = la_load64_acq(&g->gc2.sweep_to_idle);
  sweep_live_updates0 = la_load64_acq(&g->gc2.sweep_live_updates);
  finalizer_blocks0 = la_load64_acq(&g->gc2.finalizer_sweep_blocks);
  lj_gc2_finalizer_enter(g);
  assert(lj_gc2_finalizer_pending(g));
  assert(!lj_gc2_finalizer_sweep_pending(g));
  lj_gc2_finalizer_leave(g);
  assert(!lj_gc2_finalizer_pending(g));
  setgcrefr(mmudata0, g->gc.mmudata);
  setgcref(g->gc.mmudata, obj2gco(phase_tab));
  assert(lj_gc2_finalizer_pending(g));
  assert(lj_gc2_finalizer_sweep_pending(g));
  assert(lj_gc2_sweep_to_idle(g) == 0);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_SWEEP);
  assert(la_load64_acq(&g->gc2.sweep_to_idle) == sweep_to_idle0);
  assert(la_load64_acq(&g->gc2.finalizer_sweep_blocks) ==
	 finalizer_blocks0 + 1u);
  setgcrefr(g->gc.mmudata, mmudata0);
  assert(!lj_gc2_finalizer_pending(g));
  assert(!lj_gc2_finalizer_sweep_pending(g));
  assert(lj_gc2_sweep_to_idle(g) == 1);
  assert(la_load64_acq(&g->gc2.sweep_to_idle) == sweep_to_idle0 + 1u);
  assert(la_load64_acq(&g->gc2.sweep_live_updates) ==
	 sweep_live_updates0 + 1u);
  assert_idle(g, tg);
  lua_pop(L, 2);
  lj_arena_free(&tg->alloc, phase_plain, 64);
  lj_arena_free(&tg->alloc, phase_trav, 64);
  lj_gc2_legacy_mark_begin(g);
  assert(g->gc2.phase == LJ_GC2_MARK);
  assert(tg->mark_active == 1);
  assert(tg->alloc.alloc_black == 1);
  lj_gc2_legacy_weak_begin(g);
  assert(g->gc2.phase == LJ_GC2_WEAK);
  assert(tg->mark_active == 1);
  assert(tg->alloc.alloc_black == 1);
  preserve_abort_to_idle0 = la_load64_acq(&g->gc2.preserve_abort_to_idle);
  lj_gc2_legacy_preserve_abort(g);
  assert(la_load64_acq(&g->gc2.preserve_abort_to_idle) ==
	 preserve_abort_to_idle0 + 1u);
  assert_idle(g, tg);

  assert(luaL_dostring(L,
    "hold = {}\n"
    "for i = 1, 2600 do\n"
    "  local t = {}\n"
    "  for j = 1, 18 do t[j] = i + j end\n"
    "  hold[i] = t\n"
    "end\n") == LUA_OK);
  fixpoint_rounds0 = la_load64_acq(&g->gc2.fixpoint_rounds);
  fixpoint_hits0 = la_load64_acq(&g->gc2.fixpoint_hits);
  mark_complete_runs0 = la_load64_acq(&g->gc2.mark_complete_runs);
  mark_complete_hits0 = la_load64_acq(&g->gc2.mark_complete_hits);
  mark_to_weak0 = la_load64_acq(&g->gc2.mark_to_weak);
  weak_complete_runs0 = la_load64_acq(&g->gc2.weak_complete_runs);
  weak_to_sweep0 = la_load64_acq(&g->gc2.weak_to_sweep);
  sweep_live_updates0 = la_load64_acq(&g->gc2.sweep_live_updates);
  lj_gc_fullgc(L);
  assert(la_load64_acq(&g->gc2.fixpoint_rounds) > fixpoint_rounds0);
  assert(la_load64_acq(&g->gc2.fixpoint_hits) > fixpoint_hits0);
  assert(la_load64_acq(&g->gc2.mark_complete_runs) > mark_complete_runs0);
  assert(la_load64_acq(&g->gc2.mark_complete_hits) > mark_complete_hits0);
  assert(la_load64_acq(&g->gc2.mark_to_weak) > mark_to_weak0);
  assert(la_load64_acq(&g->gc2.weak_complete_runs) > weak_complete_runs0);
  assert(la_load64_acq(&g->gc2.weak_to_sweep) > weak_to_sweep0);
  assert(la_load64_acq(&g->gc2.sweep_live_updates) > sweep_live_updates0);
  live_estimate = la_load64_acq(&g->gc2.live_estimate);
  assert(live_estimate > 0);
  assert(la_load64_acq(&g->gc2.trigger_bytes) >= LJ_GC2_ACCT_FLUSH);
  assert(la_load64_acq(&g->gc2.trigger_bytes) >= live_estimate);
  assert_idle(g, tg);

  assert(luaL_dostring(L,
    "weakcase = setmetatable({}, {__mode = 'v'})\n"
    "do\n"
    "  local k = {}\n"
    "  weakcase[k] = {}\n"
    "end\n") == LUA_OK);
  worker_weak0 = la_load64_acq(&g->gc2.worker_weak_drained);
  weak_complete_progress0 = la_load64_acq(&g->gc2.weak_complete_progress);
  weak_clear_tables0 = la_load64_acq(&g->gc2.weak_clear_tables);
  weak_clear_cleared0 = la_load64_acq(&g->gc2.weak_clear_cleared);
  weak_legacy_fallbacks0 = la_load64_acq(&g->gc2.weak_legacy_fallbacks);
  lj_gc_fullgc(L);
  assert(la_load64_acq(&g->gc2.worker_weak_drained) > worker_weak0);
  assert(la_load64_acq(&g->gc2.weak_complete_progress) >
	 weak_complete_progress0);
  assert(la_load64_acq(&g->gc2.weak_clear_tables) > weak_clear_tables0);
  assert(la_load64_acq(&g->gc2.weak_clear_cleared) > weak_clear_cleared0);
  assert(la_load64_acq(&g->gc2.weak_legacy_fallbacks) >=
	 weak_legacy_fallbacks0);
  assert_idle(g, tg);
  lua_pushnil(L);
  lua_setglobal(L, "weakcase");

  weak_n = g->gc2.weak_capacity + 1u;
  lua_pushinteger(L, (lua_Integer)weak_n);
  lua_setglobal(L, "weak_n");
  assert(luaL_dostring(L,
    "weakmany = {}\n"
    "for i = 1, weak_n do\n"
    "  local w = setmetatable({}, {__mode = 'v'})\n"
    "  do\n"
    "    local k = {}\n"
    "    w[k] = {}\n"
    "  end\n"
    "  weakmany[i] = w\n"
    "end\n") == LUA_OK);
  weak_legacy_fallbacks0 = la_load64_acq(&g->gc2.weak_legacy_fallbacks);
  lj_gc_fullgc(L);
  assert(la_load64_acq(&g->gc2.weak_legacy_fallbacks) >
	 weak_legacy_fallbacks0);
  assert_idle(g, tg);
  lua_pushnil(L);
  lua_setglobal(L, "weakmany");
  lua_pushnil(L);
  lua_setglobal(L, "weak_n");

  g->gc.stepmul = 1;
  g->gc.threshold = 0;
  assert(lj_gc_step(L) <= 0);
  assert(g->gc2.phase != LJ_GC2_IDLE);
  lj_gc_fullgc(L);
  assert_idle(g, tg);

  g->gc.stepmul = 1;
  g->gc.threshold = 0;
  for (i = 0; i < 100000; i++) {
    int rc = lj_gc_step(L);
    if (g->gc2.phase == LJ_GC2_MARK)
      saw_mark = 1;
    if (g->gc2.phase == LJ_GC2_SWEEP)
      saw_sweep = 1;
    if (rc > 0) {
      done = 1;
      break;
    }
  }
  assert(done);
  assert(saw_mark);
  assert(saw_sweep);
  assert_idle(g, tg);

  lua_pushnil(L);
  lua_setglobal(L, "hold");
  lj_gc_fullgc(L);
  assert_idle(g, tg);

  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, finalizer_churn);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
  finalizer_enters0 = la_load64_acq(&g->gc2.finalizer_enters);
  finalizer_leaves0 = la_load64_acq(&g->gc2.finalizer_leaves);
  lj_gc_fullgc(L);
  assert(la_load32_acq(&g->gc2.finalizer_active) == 0);
  assert(la_load64_acq(&g->gc2.finalizer_enters) > finalizer_enters0);
  assert(la_load64_acq(&g->gc2.finalizer_leaves) > finalizer_leaves0);
  assert(la_load64_acq(&g->gc2.finalizer_enters) ==
	 la_load64_acq(&g->gc2.finalizer_leaves));
  assert_idle(g, tg);

  lua_close(L);
  printf("t-gc2-phase OK: legacy GC2 phases and mirrors verified\n");
  return 0;
}
