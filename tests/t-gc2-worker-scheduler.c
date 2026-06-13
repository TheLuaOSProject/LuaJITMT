/*
** Focused test for the staged parked GC2 worker scheduler.
*/

#include <assert.h>
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
#include "lj_safepoint.h"
#include "lj_tab.h"
#include "lj_tg.h"

static void sleep_ns(long ns)
{
  struct timespec ts;
  ts.tv_sec = ns / 1000000000L;
  ts.tv_nsec = ns % 1000000000L;
  while (nanosleep(&ts, &ts) != 0) {
  }
}

static int wait_until_marked(global_State *g, GCobj *o)
{
  int i;
  for (i = 0; i < 1000; i++) {
    if (lj_gc2_ismarked(g, o) == 1)
      return 1;
    sleep_ns(1000000L);
  }
  return 0;
}

static int wait_u64_gt(uint64_t *p, uint64_t base)
{
  int i;
  for (i = 0; i < 1000; i++) {
    if (la_load64_acq(p) > base)
      return 1;
    sleep_ns(1000000L);
  }
  return 0;
}

static int wait_ssb_empty(global_State *g)
{
  int i;
  for (i = 0; i < 1000; i++) {
    if (lj_gc2_ssb_empty(g))
      return 1;
    sleep_ns(1000000L);
  }
  return 0;
}

static int arena_list_contains(GCArena *a, GCArena *needle)
{
  while (a) {
    if (a == needle)
      return 1;
    a = a->hdr.next;
  }
  return 0;
}

static void make_weak_table(lua_State *L, GCtab **weak, GCtab **key,
			    GCtab **val)
{
  lua_newtable(L);
  *weak = tabV(L->top - 1);
  lua_newtable(L);
  *key = tabV(L->top - 1);
  lua_newtable(L);
  *val = tabV(L->top - 1);
  lua_pushvalue(L, -2);
  lua_pushvalue(L, -2);
  lua_settable(L, -5);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  lua_setmetatable(L, -4);
}

static int weak_entry_is_nil(lua_State *L, GCtab *weak, GCtab *key)
{
  TValue k;
  settabV(L, &k, key);
  return tvisnil(lj_tab_get(L, weak, &k));
}

static void test_async_mark(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child, *grandchild;
  uint64_t async0, runs0, ssb0, grey0, wakes0;

  lua_settop(L, 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_newtable(L);
  grandchild = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);
  lua_pushvalue(L, -2);
  lua_rawseti(L, -4, 1);

  wakes0 = la_load64_acq(&g->gc2.worker_wakes);
  lj_gc2_legacy_mark_begin(g);
  assert(la_load64_acq(&g->gc2.worker_wakes) > wakes0);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);

  async0 = la_load64_acq(&g->gc2.worker_async_progress);
  runs0 = la_load64_acq(&g->gc2.worker_runs);
  ssb0 = la_load64_acq(&g->gc2.worker_ssb_converted);
  grey0 = la_load64_acq(&g->gc2.worker_grey_drained);

  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(wait_until_marked(g, obj2gco(grandchild)));
  assert(wait_ssb_empty(g));
  assert(la_load64_acq(&g->gc2.worker_async_progress) > async0);
  assert(la_load64_acq(&g->gc2.worker_runs) > runs0);
  assert(la_load64_acq(&g->gc2.worker_ssb_converted) > ssb0);
  assert(la_load64_acq(&g->gc2.worker_grey_drained) >= grey0 + 3u);
  assert(la_load32_acq(&g->gc2.worker_active) == 0);

  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}

static void test_async_weak(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *weak, *key, *val;
  uint64_t async0, worker_weak0, clears0;
  int i;

  lua_settop(L, 0);
  make_weak_table(L, &weak, &key, &val);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  for (i = 0; i < 1000 && lj_gc2_weak_snapshot_count(g) == 0; i++)
    sleep_ns(1000000L);
  assert(lj_gc2_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  async0 = la_load64_acq(&g->gc2.worker_async_progress);
  worker_weak0 = la_load64_acq(&g->gc2.worker_weak_drained);
  clears0 = la_load64_acq(&g->gc2.weak_clear_tables);
  lj_gc2_legacy_weak_begin(g);
  for (i = 0; i < 1000 && !weak_entry_is_nil(L, weak, key); i++)
    sleep_ns(1000000L);
  assert(weak_entry_is_nil(L, weak, key));
  assert(la_load64_acq(&g->gc2.worker_async_progress) > async0);
  assert(la_load64_acq(&g->gc2.worker_weak_drained) > worker_weak0);
  assert(la_load64_acq(&g->gc2.weak_clear_tables) > clears0);
  assert(la_load32_acq(&g->gc2.worker_active) == 0);

  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}

static void test_async_sweep_and_stop(lua_State *L, global_State *g,
				      TGState *tg)
{
  TGState extra_tg;
  uint64_t async0, arenas0, wakes0;
  uint32_t sweep_cycle;
  void *extra_plain, *extra_trav;
  GCArena *extra_plain_a, *extra_trav_a, *swept_a;

  lj_tg_init_thread(g, &extra_tg, NULL, 1);
  extra_tg.tid = tg->tid + 4000u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  extra_tg.cur_L = L;
  lj_native_enter(&extra_tg);
  lj_tg_attach(g, &extra_tg);
  assert(g->gc2.n_threads == 2);

  extra_plain = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64, 0);
  extra_trav = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			      LJ_AF_TRAVERSABLE);
  assert(extra_plain != NULL);
  assert(extra_trav != NULL);
  extra_plain_a = lj_arena_of(extra_plain);
  extra_trav_a = lj_arena_of(extra_trav);
  lj_native_enter(tg);

  g->gc2.cycle++;
  sweep_cycle = g->gc2.cycle;
  g->gc2.phase = LJ_GC2_SWEEP;
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_PLAIN);
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  lj_arena_alloc_restore_sweep_kind(&extra_tg.alloc, LJ_ARENAK_PLAIN);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_PLAIN],
			     extra_plain_a));
  assert(arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     extra_trav_a));
  swept_a = extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE];
  assert(swept_a != NULL);

  async0 = la_load64_acq(&g->gc2.worker_async_progress);
  arenas0 = la_load64_acq(&g->gc2.sweep_owner_arenas);
  wakes0 = la_load64_acq(&g->gc2.worker_wakes);
  lj_gc2_worker_wake(g);
  assert(la_load64_acq(&g->gc2.worker_wakes) > wakes0);
  assert(wait_u64_gt(&g->gc2.sweep_owner_arenas, arenas0));
  assert(la_load64_acq(&g->gc2.worker_async_progress) > async0);
  assert(!arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			      swept_a));
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_TRAVERSABLE],
			     swept_a));
  assert((extra_plain_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert((swept_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert(swept_a->hdr.sweep_epoch == sweep_cycle);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_SWEEP);
  assert(!lj_gc2_sweep_pending(g));

  lj_gc2_worker_stop(g);
  assert(g->gc2.worker_thread == NULL);
  assert(la_load32_acq(&g->gc2.n_workers) == 0);
  assert(la_load32_acq(&g->gc2.worker_exited) == 1);

  lj_arena_alloc_restore_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  tg->alloc.sweep_epoch = sweep_cycle;  /* Main TG had no synthetic work. */
  assert(lj_gc2_sweep_to_idle(g) == 1);
  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, &extra_tg);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;

  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  luaL_openlibs(L);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);

  assert(lj_gc2_worker_start(g) == 1);
  assert(g->gc2.worker_thread != NULL);
  assert(la_load32_acq(&g->gc2.n_workers) == 1);
  assert(la_load32_acq(&g->gc2.worker_started) == 1);

  test_async_mark(L, g, tg);
  test_async_weak(L, g, tg);
  test_async_sweep_and_stop(L, g, tg);

  lua_close(L);
  printf("t-gc2-worker-scheduler OK: parked worker wake/drain verified\n");
  return 0;
}
