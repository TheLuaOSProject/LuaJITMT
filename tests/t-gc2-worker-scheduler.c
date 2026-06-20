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

static int wait_u64_at_least(uint64_t *p, uint64_t target)
{
  int i;
  for (i = 0; i < 1000; i++) {
    if (la_load64_acq(p) >= target)
      return 1;
    sleep_ns(1000000L);
  }
  return 0;
}

static lua_State *finalizer_expected_L;
static int finalizer_count;
static int finalizer_order[3];

static int scheduler_udata_finalizer(lua_State *L)
{
  int *id = (int *)lua_touserdata(L, 1);
  assert(L == finalizer_expected_L);
  assert(id != NULL);
  assert(*id >= 1 && *id <= 3);
  assert(finalizer_count < 3);
  finalizer_order[finalizer_count++] = *id;
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

static int unlink_root_object(global_State *g, GCobj *needle)
{
  GCRef *p = &g->gc.root;
  while (gcref(*p)) {
    GCobj *o = gcref(*p);
    if (o == needle) {
      setgcrefr(*p, *lj_obj_gcwref(o));
      lj_obj_setgcwnull(o);
      return 1;
    }
    p = lj_obj_gcwref(o);
  }
  return 0;
}

static void relink_root_object(global_State *g, GCobj *o)
{
  lj_obj_setgcwr(o, g->gc.root);
  setgcref(g->gc.root, o);
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

static void test_two_worker_contention(global_State *g)
{
  uint64_t busy0, parks0, wakes0;

  assert(la_load32_acq(&g->gc2.n_workers) == 2);
  parks0 = la_load64_acq(&g->gc2.worker_parks);
  if (parks0 < 2u)
    assert(wait_u64_at_least(&g->gc2.worker_parks, 2u));

  busy0 = la_load64_acq(&g->gc2.worker_busy_retries);
  wakes0 = la_load64_acq(&g->gc2.worker_wakes);
  la_store32_rel(&g->gc2.worker_active, 1);
  la_store32_rel(&g->gc2.phase, LJ_GC2_MARK);
  lj_gc2_worker_wake(g);
  assert(la_load64_acq(&g->gc2.worker_wakes) > wakes0);
  assert(wait_u64_at_least(&g->gc2.worker_busy_retries, busy0 + 2u));
  la_store32_rel(&g->gc2.phase, LJ_GC2_IDLE);
  la_store32_rel(&g->gc2.worker_active, 0);
}

static void test_worker_finalizer_mpsc_drain(lua_State *L, global_State *g)
{
  GCobj *a, *b;
  uint64_t drained0, runs0, async0;

  lua_settop(L, 0);
  assert(la_load32_acq(&g->gc2.n_workers) == 2);
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_mpsc) == NULL);
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_tail) == NULL);

  lua_newtable(L);
  a = obj2gco(tabV(L->top - 1));
  lua_newtable(L);
  b = obj2gco(tabV(L->top - 1));
  assert(unlink_root_object(g, a));
  assert(unlink_root_object(g, b));

  drained0 = la_load64_acq(&g->gc2.finalizer_mpsc_drained);
  runs0 = la_load64_acq(&g->gc2.worker_runs);
  async0 = la_load64_acq(&g->gc2.worker_async_progress);

  lj_gc2_finalizer_enqueue(g, a);
  lj_gc2_finalizer_enqueue(g, b);
  assert(wait_u64_at_least(&g->gc2.finalizer_mpsc_drained, drained0 + 2u));
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_mpsc) == NULL);
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_tail) != NULL);
  assert(la_load64_acq(&g->gc2.worker_runs) > runs0);
  assert(wait_u64_at_least(&g->gc2.worker_async_progress, async0 + 2u));
  assert(la_load32_acq(&g->gc2.worker_active) == 0);

  assert(lj_gc2_finalizer_dequeue(g) == a);
  assert(lj_gc2_finalizer_dequeue(g) == b);
  assert(lj_gc2_finalizer_dequeue(g) == NULL);
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_tail) == NULL);

  relink_root_object(g, b);
  relink_root_object(g, a);
  lua_settop(L, 0);
}

static void push_scheduler_udata(lua_State *L, int id)
{
  int *slot = (int *)lua_newuserdata(L, sizeof(int));
  *slot = id;
  lua_newtable(L);
  lua_pushcfunction(L, scheduler_udata_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
}

static void test_worker_real_finalizer_dispatch(lua_State *L, global_State *g)
{
  uint64_t queued0, drained0, dequeued0, async0;
  size_t separated;

  lua_settop(L, 0);
  finalizer_expected_L = L;
  finalizer_count = 0;
  finalizer_order[0] = finalizer_order[1] = finalizer_order[2] = 0;

  assert(la_load32_acq(&g->gc2.n_workers) == 2);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_IDLE);
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_mpsc) == NULL);
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_tail) == NULL);

  push_scheduler_udata(L, 1);
  push_scheduler_udata(L, 2);
  push_scheduler_udata(L, 3);

  queued0 = la_load64_acq(&g->gc2.finalizer_queued);
  drained0 = la_load64_acq(&g->gc2.finalizer_mpsc_drained);
  dequeued0 = la_load64_acq(&g->gc2.finalizer_dequeued);
  async0 = la_load64_acq(&g->gc2.worker_async_progress);

  separated = lj_gc_separateudata(g, 1);
  assert(separated >= 3u);
  assert(la_load64_acq(&g->gc2.finalizer_queued) == queued0 + 3u);
  assert(wait_u64_at_least(&g->gc2.finalizer_mpsc_drained, drained0 + 3u));
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_mpsc) == NULL);
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_tail) != NULL);
  assert(wait_u64_at_least(&g->gc2.worker_async_progress, async0 + 3u));

  lj_gc_finalize_udata(L);
  assert(la_load64_acq(&g->gc2.finalizer_dequeued) == dequeued0 + 3u);
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_tail) == NULL);
  assert(finalizer_count == 3);
  assert(finalizer_order[0] == 3);
  assert(finalizer_order[1] == 2);
  assert(finalizer_order[2] == 1);

  lua_settop(L, 0);
  lj_gc_separateudata(g, 1);
  lj_gc_finalize_udata(L);
  assert(finalizer_count == 3);
  finalizer_expected_L = NULL;
}

static void test_finalizer_owner_leave_rewakes_worker(lua_State *L,
						      global_State *g)
{
  GCobj *a, *b;
  uint64_t drained0, parks0, wakes0;

  assert(lj_gc2_workers_set(g, 1) == 1);
  assert(la_load32_acq(&g->gc2.n_workers) == 1);
  lua_settop(L, 0);
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_mpsc) == NULL);
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_tail) == NULL);

  lua_newtable(L);
  a = obj2gco(tabV(L->top - 1));
  lua_newtable(L);
  b = obj2gco(tabV(L->top - 1));
  assert(unlink_root_object(g, a));
  assert(unlink_root_object(g, b));

  drained0 = la_load64_acq(&g->gc2.finalizer_mpsc_drained);
  parks0 = la_load64_acq(&g->gc2.worker_parks);
  wakes0 = la_load64_acq(&g->gc2.worker_wakes);
  lj_gc2_finalizer_enter(g);
  lj_gc2_finalizer_enqueue(g, a);
  lj_gc2_finalizer_enqueue(g, b);
  assert(wait_u64_at_least(&g->gc2.worker_parks, parks0 + 1u));
  assert(la_load64_acq(&g->gc2.finalizer_mpsc_drained) == drained0);
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_mpsc) != NULL);
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_tail) == NULL);
  lj_gc2_finalizer_leave(g);
  assert(wait_u64_at_least(&g->gc2.worker_wakes, wakes0 + 2u));
  assert(wait_u64_at_least(&g->gc2.finalizer_mpsc_drained, drained0 + 2u));
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_mpsc) == NULL);
  assert(la_loadptr_acq((void *const *)&g->gc2.finalizer_tail) != NULL);

  assert(lj_gc2_finalizer_dequeue(g) == a);
  assert(lj_gc2_finalizer_dequeue(g) == b);
  assert(lj_gc2_finalizer_dequeue(g) == NULL);
  relink_root_object(g, b);
  relink_root_object(g, a);
  lua_settop(L, 0);

  assert(lj_gc2_workers_set(g, 2) == 1);
  assert(la_load32_acq(&g->gc2.n_workers) == 2);
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
  uint64_t async0, arenas0, blocks0, wakes0;
  uint32_t sweep_cycle;
  void *extra_plain, *extra_trav;
  GCArena *extra_plain_a, *extra_trav_a, *swept_a;
  int i;

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

  lj_gc2_finalizer_enter(g);
  assert(lj_gc2_finalizer_pending(g));
  g->gc2.cycle++;
  sweep_cycle = g->gc2.cycle;
  g->gc2.phase = LJ_GC2_SWEEP;
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_PLAIN);
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  lj_arena_alloc_restore_sweep_kind(&extra_tg.alloc, LJ_ARENAK_PLAIN);
  extra_tg.alloc.prepare_epoch = sweep_cycle;
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_PLAIN],
			     extra_plain_a));
  assert(arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     extra_trav_a));
  swept_a = extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE];
  assert(swept_a != NULL);

  async0 = la_load64_acq(&g->gc2.worker_async_progress);
  arenas0 = la_load64_acq(&g->gc2.sweep_owner_arenas);
  blocks0 = la_load64_acq(&g->gc2.finalizer_sweep_blocks);
  wakes0 = la_load64_acq(&g->gc2.worker_wakes);
  for (i = 0; i < 1000 &&
	      la_load64_acq(&g->gc2.finalizer_sweep_blocks) == blocks0; i++) {
    lj_gc2_worker_wake(g);
    sleep_ns(1000000L);
  }
  assert(la_load64_acq(&g->gc2.worker_wakes) > wakes0);
  assert(la_load64_acq(&g->gc2.finalizer_sweep_blocks) > blocks0);
  assert(la_load64_acq(&g->gc2.sweep_owner_arenas) == arenas0);
  assert(la_load64_acq(&g->gc2.worker_async_progress) == async0);
  assert(arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     swept_a));
  lj_gc2_finalizer_leave(g);
  assert(!lj_gc2_finalizer_pending(g));
  wakes0 = la_load64_acq(&g->gc2.worker_wakes);
  for (i = 0; i < 1000 &&
	      la_load64_acq(&g->gc2.sweep_owner_arenas) == arenas0; i++) {
    lj_gc2_worker_wake(g);
    sleep_ns(1000000L);
  }
  assert(la_load64_acq(&g->gc2.worker_wakes) > wakes0);
  assert(la_load64_acq(&g->gc2.sweep_owner_arenas) > arenas0);
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
  assert(g->gc2.worker_thread[0] == NULL);
  assert(g->gc2.worker_thread[1] == NULL);
  assert(la_load32_acq(&g->gc2.n_workers) == 0);
  assert(la_load32_acq(&g->gc2.worker_exited) == 2);

  lj_arena_alloc_restore_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  /* Synthetic boundary: main TG had no prepared work. */
  tg->alloc.prepare_epoch = sweep_cycle;
  tg->alloc.sweep_epoch = sweep_cycle;
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
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);

  assert(lj_gc2_workers_set(g, 2) == 1);
  assert(g->gc2.worker_thread[0] != NULL);
  assert(g->gc2.worker_thread[1] != NULL);
  assert(la_load32_acq(&g->gc2.n_workers) == 2);
  assert(la_load32_acq(&g->gc2.worker_started) == 2);

  test_two_worker_contention(g);
  test_worker_finalizer_mpsc_drain(L, g);
  test_worker_real_finalizer_dispatch(L, g);
  test_finalizer_owner_leave_rewakes_worker(L, g);
  test_async_mark(L, g, tg);
  test_async_weak(L, g, tg);
  test_async_sweep_and_stop(L, g, tg);

  lua_close(L);
  printf("t-gc2-worker-scheduler OK: parked worker wake/drain/finalizer handoff verified\n");
  return 0;
}
