/*
** Focused test for GC2 SSB-to-grey traversal.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_arena.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_udata.h"
#include "lj_state.h"
#include "lj_safepoint.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_lib.h"
#include "lj_meta.h"
#if LJ_HASFFI
#include "lj_cdata.h"
#endif
#if LJ_HASJIT
#include "lj_dispatch.h"
#include "lj_jit.h"
#include "lj_trace.h"
#endif

#include "lib/thread_fixture_helpers.h"

static void flush_and_drain(global_State *g, TGState *tg)
{
  (void)lj_gc2_flush_ssb(g, tg);
  (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_thread_scan_needscan_pending_acq(g) == 0);
}

static void worker_drain_all(global_State *g)
{
  uint32_t i;
  for (i = 0; i < 1024; i++) {
    if (lj_gc2_test_ssb_empty(g))
      return;
    assert(lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH) != 0);
  }
  assert(lj_gc2_test_ssb_empty(g));
}

static void settle_automatic_cycle(global_State *g)
{
  uint32_t attempts;
  for (attempts = 0;
       gc2_phase_acq(g) != LJ_GC2_IDLE && attempts < 4096u;
       attempts++) {
    (void)lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH);
    lj_gc2_cycle_to_idle(g);
  }
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
}

static void pin_mark_closed_for_worker_fixture(global_State *g)
{
  /* Tight synthetic drain loops test GC ownership/progress, not the bounded
  ** native scheduling turn granted by the production MARK driver. */
  lj_gc2_jit_mark_request_exit(g);
  assert(gc2_jit_phase_gate_acq(g) == 0);
  gc2_jit_mark_resume_rel(g, 0);
}

static int weak_snapshot_has(global_State *g, GCtab *t);

#if defined(LJ_GC2_TEST_HELPERS)
static int table_retry_locator_visible(global_State *g, GCobj *o)
{
  uint32_t recovery = lj_gc2_test_recovery_state(g, o);
  return !lj_gc2_test_ssb_empty(g) ||
    gc2_grey_top_acq(g) != gc2_grey_bottom_acq(g) ||
    recovery == LJ_ARENA_RECOVERY_PENDING ||
    recovery == LJ_ARENA_RECOVERY_CLAIMED ||
    recovery == LJ_ARENA_RECOVERY_REDIRTY;
}

#endif
#if LJ_HASFFI
static int gc2_cdata_counting_finalizer(lua_State *L);
#endif

static void test_strong_table(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child;
  uint64_t grey_pushed0, grey_drained0, grey_pushed1, grey_drained1;

  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "child");

  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  grey_pushed0 = gc2_grey_pushed_acq(g);
  grey_drained0 = gc2_grey_drained_acq(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(parent->asize > 0);
  assert(parent->hmask > 0);
  assert(lj_gc2_ismarkedmem(g, lj_tab_array_hdrw(lj_tab_array_acq(parent))) == 1);
  assert(lj_gc2_ismarkedmem(g, lj_tab_node_hdrw(lj_tab_node_acq(parent))) == 1);
  grey_pushed1 = gc2_grey_pushed_acq(g);
  grey_drained1 = gc2_grey_drained_acq(g);
  /* The explicit parent/child graph requires two traversals. The live-store
  ** guard may also retain one exact parent rescan from the pre-MARK writes. */
  assert(grey_pushed1 >= grey_pushed0 + 2u);
  assert(grey_drained1 >= grey_drained0 + 2u);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
}

static void test_retired_table_owner_nonsemantic(lua_State *L,
						 global_State *g, TGState *tg)
{
  GCtab *parent, *child;
  TValue *array;
  MSize asize, hmask;

  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "retired-child");
  (void)lj_tab_node_snapshot_acq(parent, &hmask);
  asize = lj_tab_array_snapshot_acq(parent, &array);
  assert(hmask > 0);

  /* Isolate retire-record semantics from the legitimate rescan retained by
  ** the setup store itself. Both table roots remain on the Lua stack. */
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  /* Retire metadata preserves only its record/vector. ret->tab is an identity
  ** used under a cold counted lease, never a semantic root for the dead owner. */
  lj_tab_resize(L, parent, (uint32_t)asize,
		(hmask > 0 ? lj_fls((uint32_t)hmask) + 2u : 2u));
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  /* Nor may raw owner marking poison the semantic plane: the first real edge
  ** must still report NEW and enqueue the child graph. */
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
}

static void test_retired_table_root_cycle_retries(lua_State *L,
						   global_State *g)
{
  TabNodeRetire *ret, *savednext, *oldhead;
  GCtab *t;
  MSize hmask;
  uint64_t marks0;

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top - 1);
  lua_pushinteger(L, 1);
  lua_setfield(L, -2, "retired-root-cycle");
  oldhead = lj_tab_node_retired_head_acq(g);
  (void)lj_tab_node_snapshot_acq(t, &hmask);
  assert(hmask > 0 && lj_fls((uint32_t)hmask) + 2u <= LJ_MAX_HBITS);
  lj_tab_resize(L, t, lj_tab_asize_acq(t),
		lj_fls((uint32_t)hmask) + 2u);
  ret = lj_tab_node_retired_head_acq(g);
  assert(ret != NULL && ret != oldhead);
  savednext = lj_tab_node_retired_next_acq(ret);

  lj_gc2_mark_begin(g);
  gc2_mark_root_scanned_rel(g, 1);
  marks0 = gc2_marks_this_round_acq(g);
  /* A corrupt intrusive cycle must reject the exact root certificate promptly;
  ** the former count cap either hung below the cap or silently certified above
  ** it. Repair the synthetic edge before ordinary reclamation resumes. */
  lj_tab_node_retired_next_rel(ret, ret);
  lj_gc2_test_scan_roots(g, L);
  assert(gc2_mark_root_scanned_acq(g) == 0);
  assert(gc2_marks_this_round_acq(g) > marks0);
  lj_tab_node_retired_next_rel(ret, savednext);

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);
}

static void test_false_candidate_mark_admission(lua_State *L,
					 global_State *g, TGState *tg)
{
  GCtab *parent, *child;

  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "admission-child");

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj_expected_status(g, obj2gco(parent),
					 (uint32_t)~LJ_TFUNC, NULL) < 0);
  assert(lj_gc2_ismarkedmem(g, parent) == 0);
  /* An aligned interior word in a non-cdata graph allocation must be rejected
  ** before the containing table receives a semantic mark. */
  assert(lj_gc2_markobj_expected_status(
	 g, (GCobj *)(void *)((char *)parent + 16),
	 (uint32_t)~LJ_TTAB, NULL) < 0);
  assert(lj_gc2_ismarkedmem(g, parent) == 0);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
}

static void test_embedded_empty_string_mark(global_State *g)
{
  GCobj *empty = obj2gco(&g->strempty);
  uint64_t marks0, pushed0;
  uint32_t gct = 0;

  settle_automatic_cycle(g);
  lj_gc2_force_major(g);
  lj_gc2_mark_begin(g);
  marks0 = gc2_marks_this_round_acq(g);
  pushed0 = gc2_grey_pushed_acq(g);

  /* strempty is a permanent global_State leaf, not an arena candidate. A
  ** generic root marker must report live without making a thread scan retry;
  ** an expected-type mismatch must still be rejected. */
  assert(lj_gc2_markobj_status(g, empty, &gct) == 0);
  assert(gct == (uint32_t)~LJ_TSTR);
  assert(lj_gc2_markobj_expected_status(
	 g, empty, (uint32_t)~LJ_TSTR, NULL) == 0);
  assert(lj_gc2_markobj_expected_status(
	 g, empty, (uint32_t)~LJ_TFUNC, NULL) < 0);
  assert(gc2_marks_this_round_acq(g) == marks0);
  assert(gc2_grey_pushed_acq(g) == pushed0);

  lj_gc2_cycle_to_idle(g);
}

#if LJ_HASFFI
static void test_cdata_exact_coverage_admission(lua_State *L,
						global_State *g)
{
  GCcdata *cd = lj_cdata_new_(L, CTID_INT8, 1);
  CTypeID saved = cd->ctypeid;
  setcdataV(L, L->top, cd);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;

  lj_gc2_mark_begin(g);
  /* INT8 and INT64 bodies occupy the same two 16-byte cells here. Coverage
  ** alone would overaccept the forged larger CType; byte-tail metadata must
  ** reject it without setting the allocation mark. */
  cd->ctypeid = CTID_INT64;
  assert(lj_gc2_markobj_status(g, obj2gco(cd), NULL) < 0);
  assert(lj_gc2_ismarkedmem(g, cd) == 0);
  cd->ctypeid = saved;
  assert(lj_gc2_markobj(g, obj2gco(cd)) == 1);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);
}
#endif

static void test_huge_false_type_discharge(lua_State *L, global_State *g,
					    TGState *tg)
{
  GCudata *ud;
  GCtab *child;
  ud = lj_udata_new(L, (MSize)LJ_HUGE_THRESHOLD + 1024u, NULL);
  setudataV(L, L->top, ud);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  assert(lua_setmetatable(L, -3) == 1);
  assert(lj_arena_ishuge(lj_arena_of(ud)));

  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(ud)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  /* Huge MARK is the mapping lease and necessarily precedes header type
  ** rejection. The false expected tag must therefore queue the authoritative
  ** userdata graph before returning DEAD. */
  assert(lj_gc2_markobj_expected_status(g, obj2gco(ud),
					 (uint32_t)~LJ_TFUNC, NULL) < 0);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(ud)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
}

static void test_grey_deque_growth(lua_State *L, global_State *g, TGState *tg)
{
  enum { GC2_DEQUE_GROW_N = 300 };
  GCtab *parent, *child[GC2_DEQUE_GROW_N];
  uint64_t grey_pushed0, grey_drained0, grey_pushed1, grey_drained1;
  int i;

  lua_createtable(L, GC2_DEQUE_GROW_N, 0);
  parent = tabV(L->top - 1);
  for (i = 0; i < GC2_DEQUE_GROW_N; i++) {
    lua_newtable(L);
    child[i] = tabV(L->top - 1);
    lua_rawseti(L, -2, i + 1);
  }

  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child[0])) == 0);
  grey_pushed0 = gc2_grey_pushed_acq(g);
  grey_drained0 = gc2_grey_drained_acq(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  for (i = 0; i < GC2_DEQUE_GROW_N; i++)
    assert(lj_gc2_ismarked(g, obj2gco(child[i])) == 1);
  grey_pushed1 = gc2_grey_pushed_acq(g);
  grey_drained1 = gc2_grey_drained_acq(g);
  assert(grey_pushed1 >= grey_pushed0 + GC2_DEQUE_GROW_N + 1u);
  assert(grey_drained1 >= grey_drained0 + GC2_DEQUE_GROW_N + 1u);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);
}

#if defined(LJ_GC2_TEST_HELPERS)
typedef struct GreyRaceCtx {
  global_State *g;
  ljt_barrier_t barrier;
  GCobj *stolen;
} GreyRaceCtx;
#endif

typedef struct WorkerDrainCtx {
  global_State *g;
  uint32_t limit;
  uint32_t drained;
} WorkerDrainCtx;

typedef struct WorkerDrainRaceCtx {
  global_State *g;
  ljt_barrier_t barrier;
  uint32_t limit;
  uint32_t progress[2];
} WorkerDrainRaceCtx;

typedef struct WorkerDrainRaceThread {
  WorkerDrainRaceCtx *ctx;
  uint32_t idx;
} WorkerDrainRaceThread;

typedef struct WeakPeerWriteCtx {
  lua_State *L;
  ljt_barrier_t barrier;
  int status;
} WeakPeerWriteCtx;

#if LJ_HASFFI && (defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA)
typedef struct FinclaimPublishCtx {
  lua_State *L;
  global_State *g;
  GCobj *o;
  TValue fin;
  int ok;
} FinclaimPublishCtx;
#endif

static void grey_wait(ljt_barrier_t *barrier)
{
  int rc = ljt_barrier_wait(barrier);
  assert(rc == 0 || rc == LJT_BARRIER_SERIAL_THREAD);
}

#if defined(LJ_GC2_TEST_HELPERS)
static void *grey_owner_thread(void *arg)
{
  GreyRaceCtx *ctx = (GreyRaceCtx *)arg;
  grey_wait(&ctx->barrier);
  (void)lj_gc2_test_ssb_drain(ctx->g);
  return NULL;
}

static void *grey_thief_thread(void *arg)
{
  GreyRaceCtx *ctx = (GreyRaceCtx *)arg;
  grey_wait(&ctx->barrier);
  ctx->stolen = lj_gc2_test_grey_steal(ctx->g);
  return NULL;
}
#endif

static void *grey_worker_drain_thread(void *arg)
{
  WorkerDrainCtx *ctx = (WorkerDrainCtx *)arg;
  ctx->drained = lj_gc2_worker_drain(ctx->g, ctx->limit);
  return NULL;
}

#if LJ_HASFFI && (defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA)
static void *finclaim_publish_thread(void *arg)
{
  FinclaimPublishCtx *ctx = (FinclaimPublishCtx *)arg;
  ctx->ok = lj_gc2_test_finreg_cdata_preclaim(ctx->L, ctx->g, ctx->o,
					 &ctx->fin);
  return NULL;
}
#endif

static void *grey_worker_drain_race_thread(void *arg)
{
  WorkerDrainRaceThread *argt = (WorkerDrainRaceThread *)arg;
  WorkerDrainRaceCtx *ctx = argt->ctx;
  grey_wait(&ctx->barrier);
  ctx->progress[argt->idx] =
    lj_gc2_worker_drain(ctx->g, ctx->limit);
  return NULL;
}

static void *weak_peer_write_thread(void *arg)
{
  WeakPeerWriteCtx *ctx = (WeakPeerWriteCtx *)arg;
  lua_State *L = ctx->L;
  grey_wait(&ctx->barrier);
  if (!lj_threading_attach(L)) {
    ctx->status = 1;
    return NULL;
  }
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 4);
  if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
    lua_pop(L, 1);
    lj_threading_detach(L, 1);
    ctx->status = 2;
    return NULL;
  }
  lj_threading_detach(L, 1);
  ctx->status = 0;
  return NULL;
}

#if defined(LJ_GC2_TEST_HELPERS)
static void grey_publish_test_item(global_State *g, GCobj *o)
{
  uint64_t top = la_load64_acq(&g->gc2.grey_top);
  uint64_t bottom = la_load64_acq(&g->gc2.grey_bottom);
  MSize cap = g->gc2.grey_capacity;
  assert(top == bottom);
  assert(cap > 0);
  setgcref(g->gc2.grey_stack[(MSize)(bottom % cap)], o);
  la_fence_rel();  /* 05 section 5.6.3: publish slot before bottom. */
  la_store64_rel(&g->gc2.grey_bottom, bottom + 1);
}

static void test_grey_deque_steal_race(lua_State *L, global_State *g,
				       TGState *tg)
{
  enum { GC2_STEAL_RACE_N = 256 };
  int i;
  UNUSED(tg);

  lj_gc2_mark_begin(g);
  assert(g->gc2.grey_stack != NULL);
  assert(g->gc2.grey_capacity > 0);

  lua_pushliteral(L, "gc2 direct steal");
  grey_publish_test_item(g, obj2gco(strV(L->top - 1)));
  assert(lj_gc2_test_grey_steal(g) == obj2gco(strV(L->top - 1)));
  assert(lj_gc2_test_grey_steal(g) == NULL);
  assert(lj_gc2_test_ssb_empty(g));
  lua_pop(L, 1);

  lua_pushliteral(L, "gc2 owner pop");
  grey_publish_test_item(g, obj2gco(strV(L->top - 1)));
  assert(lj_gc2_test_ssb_drain(g) == 0);
  assert(lj_gc2_test_grey_steal(g) == NULL);
  assert(lj_gc2_test_ssb_empty(g));
  lua_pop(L, 1);

  for (i = 0; i < GC2_STEAL_RACE_N; i++) {
    GreyRaceCtx ctx;
    pthread_t owner, thief;
    uint64_t drained0, drained1;
    GCobj *o;
    int owner_won, thief_won;

    lua_pushfstring(L, "gc2 steal race %d", i);
    o = obj2gco(strV(L->top - 1));
    grey_publish_test_item(g, o);
    drained0 = gc2_grey_drained_acq(g);
    ctx.g = g;
    ctx.stolen = NULL;
    assert(ljt_barrier_init(&ctx.barrier, 2) == 0);
    assert(pthread_create(&owner, NULL, grey_owner_thread, &ctx) == 0);
    assert(pthread_create(&thief, NULL, grey_thief_thread, &ctx) == 0);
    assert(pthread_join(owner, NULL) == 0);
    assert(pthread_join(thief, NULL) == 0);
    assert(ljt_barrier_destroy(&ctx.barrier) == 0);

    drained1 = gc2_grey_drained_acq(g);
    owner_won = drained1 == drained0 + 1u;
    thief_won = ctx.stolen == o;
    assert(owner_won || thief_won);
    assert(!(owner_won && thief_won));
    assert(lj_gc2_test_ssb_empty(g));
    lua_pop(L, 1);
  }

  lj_gc2_cycle_to_idle(g);
}
#endif

static void test_worker_drain(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child, *grandchild;
  WorkerDrainCtx ctx;
  pthread_t worker;
  uint64_t grey_drained0, worker_runs0, worker_grey0, worker_ssb0;

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

  lj_gc2_mark_begin(g);
  /* Cooperative MARK begins with a bounded native lease. This fixture tests
  ** SSB/grey drain ownership, so close admission explicitly instead of racing
  ** the unrelated 50us scheduling deadline. */
  lj_gc2_jit_mark_request_exit(g);
  assert(gc2_jit_phase_gate_acq(g) == 0);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(!lj_gc2_test_ssb_empty(g));

  grey_drained0 = gc2_grey_drained_acq(g);
  worker_runs0 = gc2_worker_runs_acq(g);
  worker_grey0 = gc2_worker_grey_drained_acq(g);
  worker_ssb0 = gc2_worker_ssb_converted_acq(g);

  ctx.g = g;
  ctx.limit = 8;
  ctx.drained = 0;
  assert(pthread_create(&worker, NULL, grey_worker_drain_thread, &ctx) == 0);
  assert(pthread_join(worker, NULL) == 0);

  /* One SSB conversion plus the three-object graph is mandatory. Guarded
  ** setup stores may contribute additional exact rescans. */
  assert(ctx.drained >= 4);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 1);
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_grey_drained_acq(g) >= grey_drained0 + 3u);
  assert(gc2_worker_runs_acq(g) == worker_runs0 + 1u);
  assert(gc2_worker_grey_drained_acq(g) >= worker_grey0 + 3u);
  assert(gc2_worker_ssb_converted_acq(g) >= worker_ssb0 + 1u);
  assert((uint64_t)ctx.drained ==
	 gc2_worker_grey_drained_acq(g) - worker_grey0 +
	 gc2_worker_ssb_converted_acq(g) - worker_ssb0);

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 3);
}

static void test_worker_drain_race(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child, *grandchild;
  WorkerDrainRaceCtx ctx;
  WorkerDrainRaceThread arg0, arg1;
  pthread_t worker0, worker1;
  uint32_t total;
  uint64_t worker_runs0, worker_grey0, worker_ssb0, idle0, busy0;

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

  lj_gc2_mark_begin(g);
  /* As above, this race covers the exclusive drain token, not native-lease
  ** scheduling. Close the fresh cooperative turn before launching contenders. */
  lj_gc2_jit_mark_request_exit(g);
  assert(gc2_jit_phase_gate_acq(g) == 0);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(!lj_gc2_test_ssb_empty(g));

  worker_runs0 = gc2_worker_runs_acq(g);
  worker_grey0 = gc2_worker_grey_drained_acq(g);
  worker_ssb0 = gc2_worker_ssb_converted_acq(g);
  idle0 = gc2_worker_idle_declares_acq(g);
  busy0 = gc2_worker_busy_retries_acq(g);

  ctx.g = g;
  ctx.limit = 8;
  ctx.progress[0] = ctx.progress[1] = 0;
  arg0.ctx = &ctx; arg0.idx = 0;
  arg1.ctx = &ctx; arg1.idx = 1;
  assert(ljt_barrier_init(&ctx.barrier, 2) == 0);
  assert(pthread_create(&worker0, NULL, grey_worker_drain_race_thread,
			&arg0) == 0);
  assert(pthread_create(&worker1, NULL, grey_worker_drain_race_thread,
			&arg1) == 0);
  assert(pthread_join(worker0, NULL) == 0);
  assert(pthread_join(worker1, NULL) == 0);
  assert(ljt_barrier_destroy(&ctx.barrier) == 0);

  total = ctx.progress[0] + ctx.progress[1];
  assert(total >= 4u);
  assert(gc2_worker_active_acq(g) == 0);
  assert(gc2_worker_runs_acq(g) == worker_runs0 + 1u);
  assert(gc2_worker_grey_drained_acq(g) >= worker_grey0 + 3u);
  assert(gc2_worker_ssb_converted_acq(g) >= worker_ssb0 + 1u);
  assert((uint64_t)total ==
	 gc2_worker_grey_drained_acq(g) - worker_grey0 +
	 gc2_worker_ssb_converted_acq(g) - worker_ssb0);
  /* The loser either observes the drain token/empty queue, or arrives after
  ** the winner republishes the next bounded native lease and defers there. */
  assert(gc2_worker_idle_declares_acq(g) > idle0 ||
	 gc2_worker_busy_retries_acq(g) > busy0 ||
	 gc2_jit_phase_gate_acq(g) != 0);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 1);
  assert(lj_gc2_test_ssb_empty(g));

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 3);
}

static void test_worker_leaf_ssb(lua_State *L, global_State *g, TGState *tg)
{
  GCstr *s, *s2;
  uint64_t worker_runs0, worker_ssb0, worker_grey0;

  lua_pushliteral(L, "gc2 worker leaf ssb");
  s = strV(L->top - 1);
  lua_pushliteral(L, "gc2 worker leaf progress");
  s2 = strV(L->top - 1);

  lj_gc2_mark_begin(g);
  lj_gc2_jit_mark_request_exit(g);
  assert(gc2_jit_phase_gate_acq(g) == 0);
  assert(lj_gc2_test_ssb_push(g, obj2gco(s)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(!lj_gc2_test_ssb_empty(g));

  worker_runs0 = gc2_worker_runs_acq(g);
  worker_ssb0 = gc2_worker_ssb_converted_acq(g);
  worker_grey0 = gc2_worker_grey_drained_acq(g);

  assert(lj_gc2_worker_drain(g, 1) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(s)) == 1);
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_worker_runs_acq(g) == worker_runs0 + 1u);
  assert(gc2_worker_ssb_converted_acq(g) == worker_ssb0 + 1u);
  assert(gc2_worker_grey_drained_acq(g) == worker_grey0);

  assert(lj_gc2_test_ssb_push(g, obj2gco(s2)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  worker_runs0 = gc2_worker_runs_acq(g);
  worker_ssb0 = gc2_worker_ssb_converted_acq(g);
  worker_grey0 = gc2_worker_grey_drained_acq(g);
  lj_gc2_jit_mark_request_exit(g);
  assert(gc2_jit_phase_gate_acq(g) == 0);
  assert(lj_gc2_worker_drain(g, 1) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(s2)) == 1);
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_worker_runs_acq(g) == worker_runs0 + 1u);
  assert(gc2_worker_ssb_converted_acq(g) == worker_ssb0 + 1u);
  assert(gc2_worker_grey_drained_acq(g) == worker_grey0);

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
}

static void test_fixpoint_round(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child, *grandchild;
  uint64_t rounds0, hits0, worker_runs0;
  UNUSED(tg);

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
  lua_pop(L, 2);  /* Keep only parent as a stack root. */

  lj_gc2_mark_begin(g);
  /* This is a direct fixpoint primitive fixture, not the production
  ** mark-complete driver. Pin native resumption closed for the cycle so its
  ** worker drains can reach the root certificate without yielding between
  ** owner-local SSB rotations. */
  pin_mark_closed_for_worker_fixture(g);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);

  rounds0 = gc2_fixpoint_rounds_acq(g);
  hits0 = gc2_fixpoint_hits_acq(g);
  worker_runs0 = gc2_worker_runs_acq(g);

  assert(lj_gc2_fixpoint_round(g, L, 1) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  /*
  ** A one-unit round is bounded by worker progress, not object-graph depth. The
  ** owner acknowledgement marks the stack root, then its single post-root item
  ** may only detach/convert one retained publication. The live-store guard is
  ** allowed to expose the child through its exact parent rescan, but the
  ** grandchild frontier remains open and the round must not report a fixpoint.
  */
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  assert(!lj_gc2_test_ssb_empty(g));
  assert(la_load64_acq(&g->gc2.marks_this_round) > 0);
  assert(gc2_fixpoint_rounds_acq(g) == rounds0 + 1u);
  assert(gc2_fixpoint_hits_acq(g) == hits0);
  assert(gc2_worker_runs_acq(g) > worker_runs0);

  assert(lj_gc2_fixpoint_round(g, L, ~(uint32_t)0) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 1);
  assert(lj_gc2_test_ssb_empty(g));
  assert(la_load64_acq(&g->gc2.marks_this_round) > 0);
  assert(gc2_fixpoint_rounds_acq(g) == rounds0 + 2u);
  assert(gc2_fixpoint_hits_acq(g) == hits0);

  assert(lj_gc2_fixpoint_round(g, L, ~(uint32_t)0) == 1);
  assert(lj_gc2_test_ssb_empty(g));
  assert(la_load64_acq(&g->gc2.marks_this_round) == 0);
  assert(gc2_fixpoint_rounds_acq(g) == rounds0 + 3u);
  assert(gc2_fixpoint_hits_acq(g) == hits0 + 1u);

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);
}

static void test_c_value_barrier(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child;

  lua_createtable(L, 1, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
}

static void test_c_table_rescan_barrier(lua_State *L, global_State *g,
					TGState *tg)
{
  GCtab *parent, *child;

  lua_createtable(L, 1, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  assert(parent->asize > 0);
  settabV(L, &lj_tab_array_acq(parent)[0], child);
  lj_gc_anybarriert(L, parent);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
}

static void test_table_rescan_idle_clear(lua_State *L, global_State *g,
					 TGState *tg, int preserve_abort)
{
  GCtab *parent, *child;

  lua_createtable(L, 1, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  assert(parent->asize > 0);
  settabV(L, &lj_tab_array_acq(parent)[0], child);
  lj_gc_anybarriert(L, parent);
  assert(lj_obj_gcflags(obj2gco(parent)) & LJ_GC_NEEDSCAN);
  assert(!lj_gc2_test_ssb_empty(g));

  if (preserve_abort)
    lj_gc2_preserve_abort_to_idle(g);
  else
    lj_gc2_cycle_to_idle(g);

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  if (preserve_abort) {
    /* Preserve abort carries queue membership into the next cycle instead of
    ** racing a worker-owned detached suffix at IDLE. */
    assert(lj_obj_gcflags(obj2gco(parent)) & LJ_GC_NEEDSCAN);
    assert(!lj_gc2_test_ssb_empty(g));
    lj_gc2_mark_begin(g);
    flush_and_drain(g, tg);
    lj_gc2_cycle_to_idle(g);
  }
  assert((lj_obj_gcflags(obj2gco(parent)) & LJ_GC_NEEDSCAN) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  lua_pop(L, 2);
}

static void test_upval_needscan_preserve_abort(lua_State *L, global_State *g,
					       TGState *tg, int preserve_abort)
{
  int top = lua_gettop(L);
  GCfunc *outer, *target;
  GCupval *uv;
  TValue snap;

  assert(luaL_dostring(L,
    "local function target() return 42 end\n"
    "return function() return target end\n") == LUA_OK);
  outer = funcV(L->top - 1);
  assert(isluafunc(outer) && lj_funcL_nupvalues(&outer->l) == 1);
  uv = gco2uv(gcref(outer->l.uvptr[0]));
  assert(uv->closed && uvval(uv) == &uv->tv);
  lj_tv_load_acq(&snap, uvval(uv));
  assert(tvisfunc(&snap));
  target = funcV(&snap);
  assert(target != outer && isluafunc(target));

  /* Chunk compilation may cross the allocation trigger and leave an automatic
  ** cycle in SWEEP before this focused cycle starts. Finish it first so the
  ** force-major hook below actually resets marks for the asserted NEW result. */
  settle_automatic_cycle(g);
  lj_gc2_force_major(g);
  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(outer)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(uv)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(target)) == 1);
  assert(lj_obj_gcflags(obj2gco(uv)) & LJ_GC_NEEDSCAN);
  assert(lj_gc2_test_ssb_empty(g));

  if (preserve_abort)
    lj_gc2_preserve_abort_to_idle(g);
  else
    lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(lj_obj_gcflags(obj2gco(uv)) & LJ_GC_NEEDSCAN);
  assert(lj_gc2_test_ssb_empty(g));

  lj_gc2_force_major(g);
  lj_gc2_mark_begin(g);
  assert((lj_obj_gcflags(obj2gco(uv)) & LJ_GC_NEEDSCAN) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(target)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(outer)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(uv)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(target)) == 1);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, top);
}

static void test_vm_upvalue_barrier(lua_State *L, global_State *g, TGState *tg)
{
  GCfunc *fn;
  GCupval *uv;
  GCtab *old, *child;

  assert(luaL_dostring(L,
    "local x = {}\n"
    "return function(v) x = v end, x\n") == LUA_OK);
  fn = funcV(L->top - 2);
  old = tabV(L->top - 1);
  assert(isluafunc(fn));
  assert(lj_funcL_nupvalues(&fn->l) == 1);
  uv = gco2uv(gcref(fn->l.uvptr[0]));
  assert(uv->closed);
  assert(uvval(uv) == &uv->tv);
  assert(tabV(uvval(uv)) == old);

  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(uv)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(uv)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(old)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  lua_pushvalue(L, -3);
  lua_pushvalue(L, -2);
  lua_call(L, 1, 0);
  assert(tabV(uvval(uv)) == child);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 3);
}

static void test_vm_table_barrier(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child;

  assert(luaL_dostring(L,
    "return function(t, v) t[1] = v end\n") == LUA_OK);
  lua_createtable(L, 1, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  lua_pushvalue(L, -3);
  lua_pushvalue(L, -3);
  lua_pushvalue(L, -3);
  lua_call(L, 2, 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 3);
}

static void test_vm_meta_tset_barrier(lua_State *L, global_State *g,
				      TGState *tg)
{
  GCtab *parent, *key, *child;

  assert(luaL_dostring(L,
    "return function(t, k, v) t[k] = v end\n") == LUA_OK);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  key = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  lua_pushvalue(L, -4);
  lua_pushvalue(L, -4);
  lua_pushvalue(L, -4);
  lua_pushvalue(L, -4);
  lua_call(L, 3, 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 4);
}

static void test_capi_newindex_target_parent_barrier(lua_State *L,
						     global_State *g,
						     TGState *tg)
{
  GCtab *target, *proxy, *key, *child_settable, *child_setfield;

  lua_settop(L, 0);
  lua_newtable(L);
  target = tabV(L->top - 1);
  lua_newtable(L);
  proxy = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__newindex");
  lua_pushvalue(L, 1);
  lua_settable(L, -3);
  lua_setmetatable(L, 2);
  lua_newtable(L);
  key = tabV(L->top - 1);
  lua_newtable(L);
  child_settable = tabV(L->top - 1);
  lua_newtable(L);
  child_setfield = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(target)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(proxy)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child_settable)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child_setfield)) == 0);

  lua_pushvalue(L, 3);
  lua_pushvalue(L, 4);
  lua_settable(L, 2);
  assert(lj_gc2_ismarked(g, obj2gco(child_settable)) == 1);

  lua_pushvalue(L, 5);
  lua_setfield(L, 2, "field");
  assert(lj_gc2_ismarked(g, obj2gco(child_setfield)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);

  lua_pushvalue(L, 3);
  lua_gettable(L, 1);
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == child_settable);
  lua_pop(L, 1);
  lua_getfield(L, 1, "field");
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == child_setfield);
  lua_pop(L, 1);

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 5);
}

static void test_userdata_constructor_publish_barrier(lua_State *L,
						      global_State *g,
						      TGState *tg)
{
  GCtab *env, *mt;
  GCudata *ud;

  lua_settop(L, 0);
  lua_newtable(L);
  env = tabV(L->top - 1);
  lua_newtable(L);
  mt = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(la_load8_acq(&tg->alloc.alloc_black) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(env)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(mt)) == 0);

  ud = lj_udata_new(L, 1, env);
  assert(lj_gc2_ismarked(g, obj2gco(ud)) == 1);
  assert(tabref_acq(ud->env) == env);
  assert(lj_gc2_ismarked(g, obj2gco(env)) == 1);
  setgcrefrel(ud->metatable, obj2gco(mt));
  lj_gc_pubobjobj(L, ud, mt);
  assert(tabref_acq(ud->metatable) == mt);
  assert(lj_gc2_ismarked(g, obj2gco(mt)) == 1);
  setudataV(L, L->top++, ud);
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 3);
}

static void test_thread_constructor_env_barrier(lua_State *L, global_State *g,
						TGState *tg)
{
  GCtab *env;
  lua_State *L1;
  uint32_t anchoridx;

  lua_settop(L, 0);
  /* The main thread environment is itself a global thread root and is now
  ** correctly marked by cycle startup, so it cannot isolate the constructor
  ** edge. Use a fresh stack table whose root scan has not run yet. */
  lua_newtable(L);
  env = tabV(L->top - 1);
  lj_gc2_mark_begin(g);
  assert(la_load8_acq(&tg->alloc.alloc_black) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(env)) == 0);

  L1 = lj_state_new_withenv(L, env, &anchoridx);
  assert(lj_gc2_ismarked(g, obj2gco(L1)) == 1);
  assert(tabref_acq(L1->env) == env);
  assert(lj_gc2_ismarked(g, obj2gco(env)) == 1);
  setthreadV(L, L->top++, L1);
  lj_state_stack_pubtv(L, L, L->top - 1);
  lj_tg_root_anchor_pop(L2TG(L), anchoridx);
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
}

static void test_thread_spawn_constructor_child_barrier(lua_State *L,
						       global_State *g,
						       TGState *tg)
{
  GCudata *ud;
  LJThread *th;
  lua_State *child;
  uint32_t round;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "local threading = require('threading')\n"
    "return function() return threading.spawn(function() return true end) end\n") ==
    LUA_OK);

  lj_gc2_mark_begin(g);
  assert(la_load8_acq(&tg->alloc.alloc_black) == 1);

  lua_pushvalue(L, -1);
  lua_call(L, 0, 1);
  ud = udataV(L->top - 1);
  assert(lj_udata_udtype_acq(ud) == UDTYPE_THREAD);
  th = (LJThread *)uddata(ud);
  child = lj_thread_state_load_acq(th);
  assert(child != NULL);
  /* Spawn publishes a late native root and conservatively aborts this snapshot.
  ** The child can publish its result-root handoff after the first replacement
  ** starts, legitimately aborting that cycle too. Keep requesting a live MARK
  ** cycle while yielding peer progress, then drive its root fixpoint. A bare
  ** SSB drain is deliberately not a scan of the native live-thread list. The
  ** root snapshot queues the traversable userdata, whose payload marks child. */
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  for (round = 0; round < 256; round++) {
    if (gc2_phase_acq(g) == LJ_GC2_IDLE)
      lj_gc2_mark_begin(g);
    if (gc2_phase_acq(g) == LJ_GC2_MARK) {
      (void)lj_gc2_fixpoint_round(g, L, ~(uint32_t)0);
      if (gc2_phase_acq(g) == LJ_GC2_MARK &&
	  lj_gc2_ismarked(g, obj2gco(child)) == 1)
	break;
    }
    (void)lj_thr_retry_yield(NULL);
  }
  assert(round < 256);

  lua_getfield(L, -1, "join");
  lua_pushvalue(L, -2);
  lua_call(L, 1, 1);
  assert(tvistruecond(L->top - 1));
  lua_pop(L, 1);

  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
}

static int gc2_cclosure_return_upvalue(lua_State *L)
{
  lua_pushvalue(L, lua_upvalueindex(1));
  return 1;
}

static void test_cclosure_constructor_publish_barrier(lua_State *L,
						      global_State *g,
						      TGState *tg)
{
  GCfunc *fn;
  GCtab *env, *up;
  TValue uv;

  lua_settop(L, 0);
  env = tabref_acq(L->env);
  assert(env != NULL);
  lua_newtable(L);
  up = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(la_load8_acq(&tg->alloc.alloc_black) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(env)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(up)) == 0);

  lua_pushvalue(L, -1);
  lua_pushcclosure(L, gc2_cclosure_return_upvalue, 1);
  fn = funcV(L->top - 1);
  assert(lj_gc2_ismarked(g, obj2gco(fn)) == 1);
  assert(tabref_acq(fn->c.env) == env);
  assert(lj_gc2_ismarked(g, obj2gco(env)) == 1);
  lj_tv_load_acq(&uv, &fn->c.upvalue[0]);
  assert(tvistab(&uv));
  assert(tabV(&uv) == up);
  assert(lj_gc2_ismarked(g, obj2gco(up)) == 1);

  lua_pushvalue(L, -1);
  lua_call(L, 0, 1);
  assert(tabV(L->top - 1) == up);
  lua_pop(L, 1);

  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
}

static void test_lua_closure_constructor_publish_barrier(lua_State *L,
							global_State *g,
							TGState *tg)
{
  GCfunc *fn;
  GCtab *payload;
  GCupval *uv;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "local payload = { tag = 'lua-closure' }\n"
    "return function() return function() return payload end end, payload\n") ==
    LUA_OK);
  payload = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  /* The explicit start request makes the current allocation threshold due so
  ** ordinary pacing can advance it. This fixture needs FNEW itself to execute
  ** under active-black allocation; retain the surrounding LUA_GCSTOP state. */
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  assert(la_load8_acq(&tg->alloc.alloc_black) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(payload)) == 0);

  lua_pushvalue(L, -2);
  lua_call(L, 0, 1);
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  assert(lj_gc2_ismarked(g, obj2gco(fn)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(funcproto(fn))) == 1);
  assert(lj_funcL_nupvalues(&fn->l) == 1);
  uv = &gcref(fn->l.uvptr[0])->uv;
  assert(lj_gc2_ismarked(g, obj2gco(uv)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(payload)) == 1);

  lua_pushvalue(L, -1);
  lua_call(L, 0, 1);
  assert(tabV(L->top - 1) == payload);
  lua_pop(L, 1);

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 3);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
}

static void test_proto_chunkname_publish_barrier(lua_State *L, global_State *g,
						 TGState *tg)
{
  GCstr *chunkname;
  GCfunc *fn;
  GCproto *pt;
  const char src[] = "return function() return 42 end";

  lua_settop(L, 0);
  lua_pushliteral(L, "@gc2_proto_chunk");
  chunkname = strV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(la_load8_acq(&tg->alloc.alloc_black) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(chunkname)) == 0);

  assert(luaL_loadbuffer(L, src, sizeof(src) - 1, strdata(chunkname)) == LUA_OK);
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  assert(proto_chunkname_acq(pt) == chunkname);
  /* Parser allocation checks may drive this deliberately-started cycle all
  ** the way back to IDLE. A mark bit has no liveness meaning after that point;
  ** while the cycle remains active, the proto publication barrier must mark
  ** the chunk name immediately. */
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE ||
	 lj_gc2_ismarked(g, obj2gco(chunkname)) == 1);

  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
}

#if LJ_HASBUFFER
static void test_buffer_decode_metatable_barrier(lua_State *L, global_State *g,
						TGState *tg)
{
  GCtab *decoded, *mt;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "local buffer = require('string.buffer')\n"
    "local mt = { tag = 'mt' }\n"
    "local b = buffer.new({ metatable = { mt } })\n"
    "local encoded = b:reset():encode(setmetatable({ value = 42 }, mt)):get()\n"
    "return function() return b:set(encoded):decode() end, mt\n") == LUA_OK);
  mt = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  /* Keep buffer allocation/decoding inside this explicitly started MARK. */
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  assert(la_load8_acq(&tg->alloc.alloc_black) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(mt)) == 0);

  lua_pushvalue(L, -2);
  lua_call(L, 0, 1);
  decoded = tabV(L->top - 1);
  assert(lj_gc2_ismarked(g, obj2gco(decoded)) == 1);
  assert(tabref_acq(decoded->metatable) == mt);
  assert(lj_gc2_ismarked(g, obj2gco(mt)) == 1);
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 3);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
}

static void test_buffer_constructor_dict_barrier(lua_State *L, global_State *g,
						 TGState *tg)
{
  GCtab *dict_str, *dict_mt;
  GCudata *ud;
  SBufExt *sbx;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "local buffer = require('string.buffer')\n"
    "local dict_str = { 'alpha' }\n"
    "local dict_mt = { { tag = 'mt' } }\n"
    "return function() return buffer.new({ dict = dict_str, metatable = dict_mt }) end,\n"
    "       dict_str, dict_mt\n") == LUA_OK);
  dict_str = tabV(L->top - 2);
  dict_mt = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  /* Keep buffer userdata construction inside this explicitly started MARK. */
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  assert(la_load8_acq(&tg->alloc.alloc_black) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(dict_str)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(dict_mt)) == 0);

  lua_pushvalue(L, -3);
  lua_call(L, 0, 1);
  ud = udataV(L->top - 1);
  sbx = (SBufExt *)uddata(ud);
  assert(lj_bufx_dict_str_acq(sbx) == dict_str);
  assert(lj_bufx_dict_mt_acq(sbx) == dict_mt);
  assert(lj_gc2_ismarked(g, obj2gco(dict_str)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(dict_mt)) == 1);
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 4);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
}
#endif

#if LJ_HASJIT
static GCtrace *find_trace(global_State *g)
{
  jit_State *J = G2J(g);
  MSize i;
  for (i = 1; i < J->sizetrace; i++) {
    GCtrace *T = traceref(J, i);
    if (T != NULL && trace_traceno_acq(T) > 0)
      return T;
  }
  return NULL;
}

static void test_jit_hotcall_root(lua_State *L, global_State *g)
{
  int i;
  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "return function(x) return x + 1 end\n") == LUA_OK);
  /* Drive only the function-entry counter: the caller is this C fixture. */
  for (i = 1; i <= 20; i++) {
    lua_pushvalue(L, -1);
    lua_pushinteger(L, i);
    lua_call(L, 1, 1);
    assert(lua_tointeger(L, -1) == i + 1);
    lua_pop(L, 1);
  }
  assert(find_trace(g) != NULL);
  lua_pop(L, 1);
}

static void test_jit_table_store_helper_barrier(lua_State *L, global_State *g,
						TGState *tg)
{
  GCtab *parent, *child;
  cTValue *stored;
  TValue key;

  assert(luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "return function(t, v, n)\n"
    "  for i = 1, n do\n"
    "    if i > 1 then t[1] = v end\n"
    "  end\n"
    "end\n") == LUA_OK);
  lua_createtable(L, 1, 0);
  lua_newtable(L);
  lua_pushvalue(L, -3);
  lua_pushvalue(L, -3);
  lua_pushvalue(L, -3);
  lua_pushinteger(L, 100);
  lua_call(L, 3, 0);
  /* M6: existing non-weak table stores trace through the helper bridge. */
  assert(find_trace(g) != NULL);
  lua_pop(L, 2);
  settle_automatic_cycle(g);

  lua_createtable(L, 1, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  /* Give the active-MT existing-store helper the semantic shape recorded by
  ** the warm trace. The authentic trace above proves the generated lowering;
  ** call the same helper directly below so an entry checkpoint cannot close
  ** the deliberately synthetic MARK cycle before the barrier under test. */
  lua_pushinteger(L, 0);
  lua_rawseti(L, -3, 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  setintV(&key, 1);
  assert(lj_tab_storetv_existing_forjit(L, parent, &key, L->top - 1) == 1);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  stored = lj_tab_getint(parent, 1);
  assert(tvistab(stored) && tabV(stored) == child);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 3);
}

static void test_jit_weak_table_store_helper_barrier(lua_State *L,
						     global_State *g,
						     TGState *tg)
{
  GCtab *weak, *key, *val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "return function(t, k, v, n)\n"
    "  for i = 1, n do\n"
    "    if i > 1 then t[k] = v end\n"
    "  end\n"
    "end\n") == LUA_OK);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "k");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  key = tabV(L->top - 1);
  lua_newtable(L);
  /* Warm the trace with an existing non-nil slot, then overwrite it in P_WEAK. */
  lua_newtable(L);
  val = tabV(L->top - 1);

  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 4);
  lua_pushinteger(L, 100);
  lua_call(L, 4, 0);
  /* M8: existing weak table stores trace through a P_WEAK-aware helper. */
  assert(find_trace(g) != NULL);

  /* Trace compilation/warmup may cross the automatic trigger. The focused
  ** assertions below require a fresh mark generation, not a still-open sweep
  ** whose survivor mark correctly reports ALREADY. */
  settle_automatic_cycle(g);
  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  weak_keys0 = gc2_weak_keys_marked_acq(g);
  weak_vals0 = gc2_weak_values_marked_acq(g);

  lj_gc2_mark_to_weak(g);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 5);
  lua_pushinteger(L, 20);
  lua_call(L, 4, 0);
  assert(find_trace(g) != NULL);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(gc2_weak_keys_marked_acq(g) >= weak_keys0);
  assert(gc2_weak_values_marked_acq(g) >= weak_vals0);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 5);
}

static void test_jit_weak_array_store_helper_barrier(lua_State *L,
						     global_State *g,
						     TGState *tg)
{
  GCtab *weak, *oldval, *val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "return function(t, v, n)\n"
    "  for i = 1, n do\n"
    "    if i > 1 then t[1] = v end\n"
    "  end\n"
    "end\n") == LUA_OK);
  lua_createtable(L, 1, 0);
  weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  oldval = tabV(L->top - 1);
  lua_pushvalue(L, 3);
  lua_rawseti(L, 2, 1);
  lua_newtable(L);
  val = tabV(L->top - 1);

  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushinteger(L, 100);
  lua_call(L, 3, 0);
  /* M8: existing weak-value array stores trace through the array helper. */
  assert(find_trace(g) != NULL);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(oldval)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  weak_keys0 = gc2_weak_keys_marked_acq(g);
  weak_vals0 = gc2_weak_values_marked_acq(g);

  lj_gc2_mark_to_weak(g);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 4);
  lua_pushinteger(L, 20);
  lua_call(L, 3, 0);
  assert(find_trace(g) != NULL);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(gc2_weak_keys_marked_acq(g) == weak_keys0);
  assert(gc2_weak_values_marked_acq(g) >= weak_vals0);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 4);
}

static void test_jit_upvalue_barrier(lua_State *L, global_State *g,
				     TGState *tg)
{
  GCfunc *fn;
  GCupval *uv;
  GCtab *old, *child;

  assert(luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local x = {}\n"
    "local function setuv(v, n)\n"
    "  for i = 1, n do\n"
    "    if i > 1 then x = v end\n"
    "  end\n"
    "end\n"
    "return setuv, x\n") == LUA_OK);
  fn = funcV(L->top - 2);
  old = tabV(L->top - 1);
  assert(isluafunc(fn));
  assert(lj_funcL_nupvalues(&fn->l) == 1);
  uv = gco2uv(gcref(fn->l.uvptr[0]));
  assert(uv->closed);
  assert(tabV(uvval(uv)) == old);

  lua_newtable(L);
  lua_pushvalue(L, -3);
  lua_pushvalue(L, -2);
  lua_pushinteger(L, 100);
  lua_call(L, 2, 0);
  assert(find_trace(g) != NULL);
  old = tabV(uvval(uv));
  lua_pop(L, 1);

  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(uv)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(uv)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(old)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  lua_pushvalue(L, -3);
  lua_pushvalue(L, -2);
  lua_pushinteger(L, 20);
  lua_call(L, 2, 0);
  assert(tabV(uvval(uv)) == child);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 3);
}

static void test_jit_current_trace_root(lua_State *L, global_State *g,
					TGState *tg)
{
  jit_State *J = G2J(g);
  GCobj *bad = (GCobj *)(uintptr_t)U64x(00004000,00000000);
  GCtrace saved;
  TraceState savedstate;
  GCfunc *fn;
  GCproto *pt;
  GCtab *nonproto;
  UNUSED(tg);

  /* This fixture keeps a prototype only in a raw C local while probing exact
  ** PC ownership. Start from IDLE and stop automatic pacing so setup
  ** allocations cannot begin a black-allocation cycle behind that assertion. */
  settle_automatic_cycle(g);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  assert(luaL_dostring(L, "return function() return 42 end\n") == LUA_OK);
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  assert(lj_gc2_test_trace_pc_proto_candidate(g, obj2gco(pt),
					      proto_bc(pt)) == 1);
  assert(lj_gc2_test_trace_pc_proto_candidate(g, obj2gco(pt),
					      proto_bc(pt) + pt->sizebc) == 0);
  assert(lj_gc2_test_trace_pc_proto_candidate(g, bad, proto_bc(pt)) == 0);
  lua_pop(L, 1);

  lua_newtable(L);
  nonproto = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(pt)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(nonproto)) == 0);
  lj_gc2_smr_read_enter(g);
  assert(lj_gc2_mark_proto_for_pc(g, proto_bc(pt)) == 1);
  assert(lj_gc2_mark_proto_for_pc(g, proto_bc(pt) + pt->sizebc - 1u) == 1);
  assert(lj_gc2_mark_proto_for_pc(g, proto_bc(pt) + pt->sizebc) == 0);
  lj_gc2_smr_read_leave(g);
  assert(lj_gc2_ismarked(g, obj2gco(pt)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(nonproto)) == 0);
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(pt)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(nonproto)) == 0);
  assert(lj_gc2_test_trace_pc_proto_candidate(g, obj2gco(nonproto),
					      proto_bc(pt)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(nonproto)) == 0);
  saved = J->cur;
  savedstate = lj_trace_state_load(J);
  memset(&J->cur, 0, sizeof(J->cur));
  J->cur.traceno = 1;
  J->cur.nk = REF_BASE;
  J->cur.nins = REF_BASE;
  J->cur.ir = J->irbuf;
  setgcref(J->cur.startpt, obj2gco(pt));
  /* Active J->cur construction is deliberately NOBARRIER. It can never back a
  ** persistent root certificate, even when the scanner is the recorder owner;
  ** closure aborts it and retries after the state machine reaches IDLE. */
  lj_trace_state_store(J, LJ_TRACE_START);
  lj_gc2_test_scan_roots(g, L);
  assert(lj_trace_state_aborted(lj_trace_state_load(J)));
  assert(gc2_mark_root_scanned_acq(g) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(pt)) == 0);
  lj_trace_state_store(J, savedstate);
  J->cur = saved;
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);
  assert(lua_gc(L, LUA_GCRESTART, 0) == 0);
}

static void test_jit_tg_executing_trace_root(lua_State *L, global_State *g,
					     TGState *tg)
{
  GCtrace *T;
  int i;
  uint32_t round;
  uint32_t old_vmstate;
  uint64_t trace_roots0;

  /* Earlier barrier fixtures intentionally let their private traces die.
  ** Record and retain this fixture's own trace so the TG vmstate edge is not
  ** coupled to incidental trace lifetime or a preceding jit.flush(). */
  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "return function(x) return x + 1 end\n") == LUA_OK);
  for (i = 1; i <= 20; i++) {
    lua_pushvalue(L, -1);
    lua_pushinteger(L, i);
    lua_call(L, 1, 1);
    assert(lua_tointeger(L, -1) == i + 1);
    lua_pop(L, 1);
  }
  T = find_trace(g);
  assert(T != NULL);
  assert(trace_traceno_acq(T) > 0);

  old_vmstate = (uint32_t)lj_tg_vmstate_load_acq(tg);
  trace_roots0 = la_load64_acq(&g->gc2.tg_trace_roots);
  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(T)) == 0);
  lj_tg_vmstate_store_rel(tg, (int32_t)trace_traceno_acq(T));
  lj_gc2_scan_cycle_owner_tg_roots(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(T)) == 1);
  assert(la_load64_acq(&g->gc2.tg_trace_roots) == trace_roots0 + 1u);
  lj_tg_vmstate_store_rel(tg, (int32_t)old_vmstate);
  /* The owner scan queues the complete TG-private semantic graph. Close its
  ** table-rescan work before forcing this synthetic cycle idle. As in the
  ** direct fixpoint fixture above, suppress the production native-yield lease
  ** while exercising the primitive in a tight test loop. */
  pin_mark_closed_for_worker_fixture(g);
  for (round = 0; round < 64; round++)
    if (lj_gc2_fixpoint_round(g, L, ~(uint32_t)0))
      break;
  assert(round < 64);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);
}
#endif

static void enter_weak_clear_fixture(global_State *g, TGState *tg);

#if LJ_HASPROFILE
static int gc2_profile_callback(lua_State *L)
{
  UNUSED(L);
  return 0;
}

static void test_jit_profile_registry_weak_barrier(void)
{
  TGState *oldtg = lj_thr_get_tg();
  lua_State *L2 = luaL_newstate();
  global_State *g2;
  TGState *tg2;
  GCtab *reg;
  GCfunc *cb;
  uint64_t weak_vals0;

  assert(L2 != NULL);
  lua_gc(L2, LUA_GCSTOP, 0);
  lua_pushcfunction(L2, luaopen_base);
  lua_call(L2, 0, 1);
  lua_pop(L2, 1);
  lua_pushcfunction(L2, luaopen_package);
  lua_call(L2, 0, 1);
  lua_pop(L2, 1);
  lua_pushcfunction(L2, luaopen_jit);
  lua_pushliteral(L2, LUA_JITLIBNAME);
  lua_call(L2, 1, 1);
  lua_pop(L2, 1);
  g2 = G(L2);
  tg2 = G2TG(g2);
  assert(tg2 != NULL);
  lj_thr_set_tg(tg2);

  lua_pushcfunction(L2, gc2_profile_callback);
  cb = funcV(L2->top - 1);
  lua_pushvalue(L2, LUA_REGISTRYINDEX);
  reg = tabV(L2->top - 1);
  lua_newtable(L2);
  lua_pushliteral(L2, "__mode");
  lua_pushliteral(L2, "v");
  lua_settable(L2, -3);
  lua_setmetatable(L2, -2);

  lj_gc2_mark_begin(g2);
  assert(lj_gc2_markobj(g2, obj2gco(reg)) == 1);
  flush_and_drain(g2, tg2);
  assert(weak_snapshot_has(g2, reg));
  assert(lj_gc2_ismarked(g2, obj2gco(cb)) == 0);
  lua_pop(L2, 1);

  weak_vals0 = gc2_weak_values_marked_acq(g2);
  enter_weak_clear_fixture(g2, tg2);
  lua_getglobal(L2, "require");
  lua_pushliteral(L2, LUA_JITLIBNAME ".profile");
  lua_call(L2, 1, 1);
  lua_getfield(L2, -1, "start");
  lua_pushliteral(L2, "");
  lua_pushvalue(L2, 1);
  lua_call(L2, 2, 0);
  assert(lj_gc2_ismarked(g2, obj2gco(cb)) == 1);
  while (lj_gc2_test_weak_drain(g2, 1) != 0)
    ;
  assert(gc2_weak_values_marked_acq(g2) >= weak_vals0);
  luaJIT_profile_stop(L2);
  lj_gc2_cycle_to_idle(g2);
  lua_close(L2);
  lj_thr_set_tg(oldtg);
}
#endif

static void make_weak_table(lua_State *L, const char *mode,
			    GCtab **weak, GCtab **key, GCtab **val)
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
  lua_pushstring(L, mode);
  lua_settable(L, -3);
  lua_setmetatable(L, -4);
}

static int weak_snapshot_has(global_State *g, GCtab *t)
{
  uint32_t i, n = lj_gc2_test_weak_snapshot_count(g);
  for (i = 0; i < n; i++)
    if (lj_gc2_test_weak_snapshot_tab(g, i) == t)
      return 1;
  return 0;
}

static void enter_weak_clear_fixture(global_State *g, TGState *tg)
{
  lj_gc2_mark_to_weak(g);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  assert(gc2_thread_scan_needscan_pending_acq(g) == 0);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_ssb_empty(g));
  /*
  ** These fixtures build a first-mark weak snapshot by hand and keep the
  ** candidate weak keys/values on the C test stack. Production
  ** lj_gc2_weak_complete() closes the weak mark frontier by rescanning roots
  ** before clearing; using it here would make the test about those C-stack
  ** roots instead of the direct clear cursor, worker drain, or post-clear
  ** write barrier being exercised.
  */
  gc2_weak_mark_closed_rel(g, 1);
}

static void assert_weak_mode_marked(global_State *g, GCtab *t)
{
  GCtab *mt = tabref_acq(t->metatable);
  cTValue *mode;
  assert(mt != NULL);
  mode = lj_tab_getstr(mt, mmname_str(g, MM_mode));
  assert(mode != NULL && tvisstr(mode));
  assert(lj_gc2_ismarked(g, gcV(mode)) == 1);
}

static void clear_weak_mode_raw(lua_State *L, global_State *g, GCtab *t)
{
  GCtab *mt = tabref_acq(t->metatable);
  TValue *slot;
  assert(mt != NULL);
  /* Avoid public stack publication while testing first-mark weak state. */
  slot = lj_tab_setstr(L, mt, mmname_str(g, MM_mode));
  assert(tvisstr(slot));
  lj_tab_storenilraw(slot);
}

static int weak_entry_is_nil(lua_State *L, GCtab *weak, GCtab *key)
{
  TValue k;
  settabV(L, &k, key);
  return tvisnil(lj_tab_get(L, weak, &k));
}

static void weak_bridge_link(GCRef *head, GCtab *t, int weak)
{
  lj_obj_masksetgcflags(obj2gco(t), LJ_GC_WEAK, weak);
  setgcrefr(t->gclist, *head);
  setgcref(*head, obj2gco(t));
}

static Node *install_hmask0_node(lua_State *L, GCtab *t, GCtab *key, GCtab *val)
{
  GCSize size = lj_tab_node_bytes(0);
  TabNodeHdr *hdr = (TabNodeHdr *)lj_mem_new(L, size);
  Node *node = (Node *)(void *)((char *)(void *)hdr + sizeof(TabNodeHdr));
  hdr->hmask = 0;
  hdr->flags = 0;
  setmref(hdr->next_gen, NULL);
  lj_tab_nextnode_set(node, NULL);
  settabV(L, &node->key, key);
  settabV(L, &node->val, val);
  lj_tab_node_rel(t, node);
  lj_tab_hmask_rel(t, 0);
  setfreetop(t, node, node);
  return node;
}

static void restore_hmask0_node(global_State *g, GCtab *t, Node *node)
{
  setnilV(&node->key);
  setnilV(&node->val);
  lj_tab_node_rel(t, &g->nilnode);
  lj_tab_hmask_rel(t, 0);
  setfreetop(t, &g->nilnode, &g->nilnode);
  lj_mem_free(g, lj_tab_node_hdrw(node), lj_tab_node_bytes(0));
}

static void make_weak_table_batch(lua_State *L, MSize n)
{
  MSize i;
  GCtab *weak, *key, *val;
  assert(lua_checkstack(L, (int)(n * 3u + 8u)));
  for (i = 0; i < n; i++)
    make_weak_table(L, "v", &weak, &key, &val);
}

static void mark_weak_table_batch(lua_State *L, global_State *g, MSize n)
{
  MSize i;
  assert((MSize)(L->top - L->base) >= n * 3u);
  for (i = 0; i < n; i++) {
    TValue *tv = L->base + i * 3u;
    assert(tvistab(tv));
    assert(lj_gc2_markobj(g, obj2gco(tabV(tv))) == 1);
  }
}

static void test_weak_snapshot_growth(lua_State *L, global_State *g, TGState *tg)
{
  MSize cap0, cap1, n;
  uint64_t overflow0, overflow1;

  lua_settop(L, 0);
  if (g->gc2.weak_capacity == 0) {
    lj_gc2_mark_begin(g);
    lj_gc2_cycle_to_idle(g);
  }
  cap0 = g->gc2.weak_capacity;
  assert(cap0 > 0);
  n = cap0 + 1u;
  assert(n > cap0);

  make_weak_table_batch(L, n);
  overflow0 = gc2_weak_tables_overflow_acq(g);
  lj_gc2_mark_begin(g);
  mark_weak_table_batch(L, g, n);
  flush_and_drain(g, tg);
  assert(g->gc2.weak_capacity == cap0);
  assert(la_load64_acq(&g->gc2.weak_count) == n);
  assert(lj_gc2_test_weak_snapshot_count(g) == cap0);
  assert(gc2_weak_tables_overflow_acq(g) == overflow0 + 1u);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);

  make_weak_table_batch(L, n);
  overflow1 = gc2_weak_tables_overflow_acq(g);
  lj_gc2_mark_begin(g);
  cap1 = g->gc2.weak_capacity;
  assert(cap1 > cap0);
  assert(cap1 >= n);
  mark_weak_table_batch(L, g, n);
  flush_and_drain(g, tg);
  assert(la_load64_acq(&g->gc2.weak_count) == n);
  assert(lj_gc2_test_weak_snapshot_count(g) == n);
  assert(gc2_weak_tables_overflow_acq(g) == overflow1);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

static void test_weak_snapshot_ready_publication(lua_State *L, global_State *g)
{
  GCtab *t;
  uint64_t idx;

  lua_settop(L, 0);
  lua_newtable(L);
  t = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(g->gc2.weak_stack != NULL);
  assert(g->gc2.weak_ready != NULL);
  assert(lj_gc2_test_weak_snapshot_count(g) == 0);
  idx = la_add64_rlx(&g->gc2.weak_count, 1);
  assert(idx == 0);
  assert(lj_gc2_test_weak_snapshot_count(g) == 0);
  assert(lj_gc2_test_weak_snapshot_clear(g, 1) == 0);
  assert(la_load64_acq(&g->gc2.weak_clear_cursor) == 0);
  setgcref(g->gc2.weak_stack[0], obj2gco(t));
  la_store8_rel(&g->gc2.weak_ready[0], 1);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_test_weak_snapshot_clear(g, 1) == 1u);
  assert(la_load64_acq(&g->gc2.weak_clear_cursor) == 1u);
  assert(lj_gc2_test_weak_snapshot_tab(g, 0) == t);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);
}

static void test_weak_snapshot_rejects_nonobject(lua_State *L, global_State *g)
{
  GCobj *bad = (GCobj *)(uintptr_t)U64x(00004000,00000000);
  uint64_t idx;

  lua_settop(L, 0);
  lj_gc2_mark_begin(g);
  assert(g->gc2.weak_stack != NULL);
  assert(g->gc2.weak_ready != NULL);
  assert(lj_gc2_test_weak_snapshot_count(g) == 0);

  idx = la_add64_rlx(&g->gc2.weak_count, 1);
  assert(idx == 0);
  setgcref(g->gc2.weak_stack[0], bad);
  la_store8_rel(&g->gc2.weak_ready[0], 1);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_test_weak_snapshot_tab(g, 0) == NULL);
  assert(lj_gc2_test_weak_snapshot_scan(g, 1) == 0);
  assert(gc2_weak_scan_cursor_acq(g) == 1u);
  assert(lj_gc2_test_weak_snapshot_clear(g, 1) == 0);
  assert(gc2_weak_clear_cursor_acq(g) == 1u);
  lj_gc2_cycle_to_idle(g);
}

static void test_weak_snapshot_transient_replays_same_index(lua_State *L,
						     global_State *g)
{
  GCArena *a;
  GCtab *t;
  uint32_t cell;
  uint64_t idx;

  lua_settop(L, 0);
  lua_newtable(L);
  t = tabV(L->top - 1);
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);

  lj_gc2_mark_begin(g);
  assert(g->gc2.weak_stack != NULL);
  assert(g->gc2.weak_ready != NULL);
  idx = la_add64_rlx(&g->gc2.weak_count, 1);
  assert(idx == 0);
  setgcref(g->gc2.weak_stack[0], obj2gco(t));
  la_store8_rel(&g->gc2.weak_ready[0], 1);

  /* Model the interval after a destructive owner has claimed the lifetime LP
  ** but before it either commits FREE or restores LIVE. No table body may be
  ** inspected and neither cursor may consume this identity. */
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
					     LJ_ARENA_LIFETIME_DESTRUCT));
  assert(lj_gc2_test_weak_snapshot_scan(g, 1) == 0);
  assert(gc2_weak_scan_cursor_acq(g) == 0);
  assert(lj_gc2_test_weak_snapshot_clear(g, 1) == 0);
  assert(gc2_weak_clear_cursor_acq(g) == 0);

  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_DESTRUCT,
					     LJ_ARENA_LIFETIME_LIVE));
  assert(lj_gc2_test_weak_snapshot_scan(g, 1) == 1u);
  assert(gc2_weak_scan_cursor_acq(g) == 1u);
  assert(lj_gc2_test_weak_snapshot_clear(g, 1) == 1u);
  assert(gc2_weak_clear_cursor_acq(g) == 1u);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);
}

static void test_weak_snapshot_keylock_replays_same_index(lua_State *L,
						   global_State *g,
						   TGState *tg)
{
  GCtab *weak, *key, *val;
  GCtab *mt;
  GCstr *modekey;
  Node *node, *slot = NULL, *mnode, *modeslot = NULL;
  TValue savedkey, savedmodekey, keylock, modeout;
  MSize i, hmask, mhmask;

  lua_settop(L, 0);
  make_weak_table(L, "v", &weak, &key, &val);
  UNUSED(key); UNUSED(val);
  mt = tabref_acq(weak->metatable);
  modekey = mmname_str(g, MM_mode);
  assert(mt != NULL && modekey != NULL);
  mnode = lj_tab_node_snapshot_acq(mt, &mhmask);
  for (i = 0; i <= mhmask; i++) {
    TValue k;
    lj_tv_load_acq(&k, &mnode[i].key);
    if (tvisstr(&k) && strV(&k) == modekey) {
      modeslot = &mnode[i];
      break;
    }
  }
  assert(modeslot != NULL);
  lj_tv_load_acq(&savedmodekey, &modeslot->key);
  setkeylockV(&keylock);
  tv_rawstore_rel(&modeslot->key, tv_rawload(&keylock));
  assert(lj_gc2_smr_read_try(g));
  assert(lj_tab_getstr_held_try(g, mt, modekey, &modeout) ==
	 LJ_TAB_GC_LOOKUP_RETRY);
  lj_gc2_smr_read_leave(g);
  tv_rawstore_rel(&modeslot->key, tv_rawload(&savedmodekey));
  assert(lj_gc2_smr_read_try(g));
  assert(lj_tab_getstr_held_try(g, mt, modekey, &modeout) ==
	 LJ_TAB_GC_LOOKUP_FOUND);
  lj_gc2_smr_read_leave(g);
  assert(tvisstr(&modeout));

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  node = lj_tab_node_snapshot_acq(weak, &hmask);
  assert(node != NULL && node != &g->nilnode);
  for (i = 0; i <= hmask; i++) {
    TValue v;
    lj_tv_load_acq(&v, &node[i].val);
    if (!tvisnil(&v)) {
      slot = &node[i];
      break;
    }
  }
  assert(slot != NULL);
  lj_tv_load_acq(&savedkey, &slot->key);
  assert(!tviskeylock(&savedkey));

  /* A descheduled structural writer may leave KEYLOCK published indefinitely.
  ** Both weak passes must return without consuming their cursor, then replay
  ** the same table after the writer publishes its key. */
  tv_rawstore_rel(&slot->key, tv_rawload(&keylock));
  assert(lj_gc2_test_weak_snapshot_scan(g, 1) == 0);
  assert(gc2_weak_scan_cursor_acq(g) == 0);
  tv_rawstore_rel(&slot->key, tv_rawload(&savedkey));
  assert(lj_gc2_test_weak_snapshot_scan(g, 1) == 1u);
  assert(gc2_weak_scan_cursor_acq(g) == 1u);

  tv_rawstore_rel(&slot->key, tv_rawload(&keylock));
  assert(lj_gc2_test_weak_snapshot_clear(g, 1) == 0);
  assert(gc2_weak_clear_cursor_acq(g) == 0);
  tv_rawstore_rel(&slot->key, tv_rawload(&savedkey));
  assert(lj_gc2_test_weak_snapshot_clear(g, 1) == 1u);
  assert(gc2_weak_clear_cursor_acq(g) == 1u);

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 3);
}

static void test_weak_self_metatable_publish_barrier(lua_State *L,
						     global_State *g,
						     TGState *tg)
{
  GCtab *t;
  TValue *mode_slot;

  lua_settop(L, 0);
  lj_gc2_mark_begin(g);
  assert(la_load8_acq(&tg->alloc.alloc_black) == 1);
  assert(lj_gc2_test_weak_snapshot_count(g) == 0);

  t = lj_tab_new(L, 0, 1);
  settabV(L, L->top++, t);
  assert(lj_gc2_ismarked(g, obj2gco(t)) == 1);
  setgcrefrel(t->metatable, obj2gco(t));
  mode_slot = lj_tab_setstr(L, t, lj_str_newlit(L, "__mode"));
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 0);

  lj_tab_storestr(L, mode_slot, lj_str_newlit(L, "kv"));
  lj_tab_nomm_rel(t, (uint8_t)(~(1u<<MM_mode)));
  lj_gc_pubtab(L, t);

  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert(weak_snapshot_has(g, t));
  assert((lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAK) == LJ_GC_WEAK);
  assert_weak_mode_marked(g, t);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);
}

static void test_weak_snapshot_bridge_coverage(lua_State *L, global_State *g,
					       TGState *tg)
{
  GCtab *weak, *key, *val;
  GCtab *absent, *akey, *aval;
  GCRef weak_head;
  uint64_t idx, count;

  setgcrefnull(weak_head);
  lua_settop(L, 0);
  make_weak_table(L, "v", &weak, &key, &val);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  make_weak_table(L, "v", &absent, &akey, &aval);
  /* The coverage oracle needs `absent` genuinely absent from this cycle's
  ** snapshot, rather than retained by its own construction-store rescan. */
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  weak_bridge_link(&weak_head, weak, LJ_GC_WEAKVAL);
  assert(!lj_gc2_test_weak_snapshot_covers_bridge(g, gcref(weak_head)));

  enter_weak_clear_fixture(g, tg);
  assert(!lj_gc2_test_weak_snapshot_covers_bridge(g, gcref(weak_head)));
  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  gc2_weak_drain_active_rel(g, 1);
  assert(!lj_gc2_test_weak_snapshot_covers_bridge(g, gcref(weak_head)));
  gc2_weak_drain_active_rel(g, 0);
  assert(lj_gc2_test_weak_snapshot_covers_bridge(g, gcref(weak_head)));

  lj_obj_cleargcflags(obj2gco(weak), LJ_GC_WEAK);
  assert(!lj_gc2_test_weak_snapshot_covers_bridge(g, gcref(weak_head)));
  lj_obj_masksetgcflags(obj2gco(weak), LJ_GC_WEAK, LJ_GC_WEAKVAL);
  assert(lj_gc2_test_weak_snapshot_covers_bridge(g, gcref(weak_head)));
  lj_obj_masksetgcflags(obj2gco(weak), LJ_GC_WEAK, LJ_GC_WEAKKEY);
  assert(lj_gc2_test_weak_snapshot_covers_bridge(g, gcref(weak_head)));
  lj_obj_masksetgcflags(obj2gco(weak), LJ_GC_WEAK, LJ_GC_WEAK);
  assert(lj_gc2_test_weak_snapshot_covers_bridge(g, gcref(weak_head)));
  lj_obj_masksetgcflags(obj2gco(weak), LJ_GC_WEAK, LJ_GC_WEAKVAL);

  count = la_load64_acq(&g->gc2.weak_count);
  idx = la_add64_rlx(&g->gc2.weak_count, 1);
  assert(idx == count);
  assert(idx < (uint64_t)g->gc2.weak_capacity);
  assert(!lj_gc2_test_weak_snapshot_covers_bridge(g, gcref(weak_head)));
  la_store64_rlx(&g->gc2.weak_count, count);
  assert(lj_gc2_test_weak_snapshot_covers_bridge(g, gcref(weak_head)));

  la_store64_rlx(&g->gc2.weak_count, (uint64_t)g->gc2.weak_capacity + 1u);
  assert(!lj_gc2_test_weak_snapshot_covers_bridge(g, gcref(weak_head)));
  la_store64_rlx(&g->gc2.weak_count, count);

  setgcrefnull(weak_head);
  weak_bridge_link(&weak_head, absent, LJ_GC_WEAKVAL);
  assert(!lj_gc2_test_weak_snapshot_covers_bridge(g, gcref(weak_head)));

  setgcrefnull(weak_head);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 6);
}

static void test_weak_complete_bridge(lua_State *L, global_State *g,
				      TGState *tg)
{
  GCtab *weak, *key, *val;
  GCtab *missing, *mkey, *mval;
  GCRef weak_head;
  uint32_t attempts;
  uint64_t runs0, progress0, skipped0, fallbacks0, backfills0;
  uint64_t backfill_tables0, backfill_cleared0;
  uint64_t clear_tables0, clear_cleared0;

  setgcrefnull(weak_head);
  lua_settop(L, 0);
  make_weak_table(L, "v", &weak, &key, &val);
  /*
  ** This white-box bridge fixture manually marks the weak table. Clear the C
  ** test stack so lj_gc2_weak_complete() observes the intended graph: weak
  ** table and strong key live, weak value unreachable.
  */
  lua_settop(L, 0);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  weak_bridge_link(&weak_head, weak, LJ_GC_WEAKVAL);
  lj_gc2_mark_to_weak(g);
  runs0 = gc2_weak_complete_runs_acq(g);
  progress0 = gc2_weak_complete_progress_acq(g);
  skipped0 = gc2_weak_bridge_skipped_acq(g);
  fallbacks0 = gc2_weak_bridge_fallbacks_acq(g);
  clear_tables0 = gc2_weak_clear_tables_acq(g);
  clear_cleared0 = gc2_weak_clear_cleared_acq(g);
  for (attempts = 0; attempts < 128; attempts++)
    if (lj_gc2_weak_complete(g, L, gcref(weak_head),
			     LJ_GC2_WEAK_DRAIN_BATCH))
      break;
  assert(attempts < 128);
  attempts++;
  assert(weak_entry_is_nil(L, weak, key));
  assert(gc2_weak_complete_runs_acq(g) == runs0 + attempts);
  /*
  ** Weak completion may touch the bridge table through both the snapshot clear
  ** cursor and bridge coverage checks. Count exact cleared slots, but require
  ** only monotonic table/progress accounting.
  */
  assert(gc2_weak_complete_progress_acq(g) > progress0);
  assert(gc2_weak_bridge_skipped_acq(g) == skipped0 + 1u);
  assert(gc2_weak_bridge_fallbacks_acq(g) == fallbacks0);
  assert(gc2_weak_clear_tables_acq(g) > clear_tables0);
  assert(gc2_weak_clear_cleared_acq(g) == clear_cleared0 + 1u);
  setgcrefnull(weak_head);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);

  make_weak_table(L, "v", &weak, &key, &val);
  make_weak_table(L, "v", &missing, &mkey, &mval);
  /* Backfill coverage requires `missing` to have no construction-rescan
  ** snapshot in the cycle under test. */
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  /*
  ** As above, keep the bridge tables alive through explicit GC2/root links
  ** rather than through the fixture's Lua stack.
  */
  lua_settop(L, 0);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  weak_bridge_link(&weak_head, missing, LJ_GC_WEAKVAL);
  weak_bridge_link(&weak_head, weak, LJ_GC_WEAKVAL);
  lj_gc2_mark_to_weak(g);
  runs0 = gc2_weak_complete_runs_acq(g);
  skipped0 = gc2_weak_bridge_skipped_acq(g);
  fallbacks0 = gc2_weak_bridge_fallbacks_acq(g);
  backfills0 = gc2_weak_bridge_backfills_acq(g);
  backfill_tables0 = gc2_weak_bridge_backfill_tables_acq(g);
  backfill_cleared0 = gc2_weak_bridge_backfill_cleared_acq(g);
  for (attempts = 0; attempts < 128; attempts++)
    if (lj_gc2_weak_complete(g, L, gcref(weak_head),
			     LJ_GC2_WEAK_DRAIN_BATCH))
      break;
  assert(attempts < 128);
  attempts++;
  assert(weak_entry_is_nil(L, weak, key));
  assert(weak_entry_is_nil(L, missing, mkey));
  assert(gc2_weak_complete_runs_acq(g) == runs0 + attempts);
  assert(gc2_weak_bridge_skipped_acq(g) == skipped0 + 1u);
  assert(gc2_weak_bridge_fallbacks_acq(g) == fallbacks0);
  assert(gc2_weak_bridge_backfills_acq(g) == backfills0 + 1u);
  assert(gc2_weak_bridge_backfill_tables_acq(g) ==
	 backfill_tables0 + 1u);
  assert(gc2_weak_bridge_backfill_cleared_acq(g) ==
	 backfill_cleared0 + 1u);
  setgcrefnull(weak_head);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);

  UNUSED(val);
  UNUSED(mval);
}

static void test_weak_bridge_fallback_hmask0(lua_State *L, global_State *g,
					     TGState *tg)
{
  GCtab *weak, *key, *val;
  GCRef weak_head;
  Node *node;
  uint32_t attempts;
  uint64_t fallbacks0;

  setgcrefnull(weak_head);
  lua_settop(L, 0);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_newtable(L);
  key = tabV(L->top - 1);
  lua_newtable(L);
  val = tabV(L->top - 1);
  /* Give the synthetic hmask==0 body a real weak-value mode. Frontier closure
  ** recomputes current mode from the metatable; gcflags alone are only the
  ** captured bridge-list classification. */
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  assert(lua_setmetatable(L, -4) == 1);
  node = install_hmask0_node(L, weak, key, val);
  assert(!tvisnil(&node->val));
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  /* The GC2 weak bridge decides liveness from GC2 marks, not retired color
  ** bits. Drop the synthetic key/value stack roots before beginning the cycle
  ** so this fixture models genuinely clearable weak edges. */
  lua_settop(L, 0);

  lj_gc2_mark_begin(g);
  assert(g->gc2.weak_stack != NULL);
  assert(g->gc2.weak_ready != NULL);
  assert(g->gc2.weak_capacity > 0);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  weak_bridge_link(&weak_head, weak, LJ_GC_WEAKVAL);
  lj_gc2_mark_to_weak(g);
  la_store64_rlx(&g->gc2.weak_count, 1);
  la_store8_rlx(&g->gc2.weak_ready[0], 0);

  fallbacks0 = gc2_weak_bridge_fallbacks_acq(g);
  for (attempts = 0; attempts < 128 &&
	 gc2_weak_bridge_fallbacks_acq(g) == fallbacks0; attempts++)
    assert(lj_gc2_weak_complete(g, L, gcref(weak_head),
				LJ_GC2_WEAK_DRAIN_BATCH) == 0);
  assert(attempts <= 128);
  assert(gc2_weak_bridge_fallbacks_acq(g) == fallbacks0 + 1u);
  assert(!tvisnil(&node->val));
  lj_gc_clearweak_bridge(g, gcref(weak_head));
  assert(tvisnil(&node->val));

  setgcrefnull(weak_head);
  restore_hmask0_node(g, weak, node);
  lj_gc2_cycle_to_idle(g);
  assert(g->gc2.phase == LJ_GC2_IDLE);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);
  lua_settop(L, 0);
}

static void test_weak_tables(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *weakv, *keyv, *valv;
  GCtab *weakk, *keyk, *valk;
  GCtab *weakkv, *keykv, *valkv;
  uint64_t seen0, weakkey0, weakval0, allweak0, queued0, overflow0;
  uint64_t scan_runs0, scan_tables0, scan_slots0, scan_clearable0;
  uint64_t clear_runs0, clear_tables0, clear_slots0, clear_cleared0;

  make_weak_table(L, "v", &weakv, &keyv, &valv);
  make_weak_table(L, "k", &weakk, &keyk, &valk);
  make_weak_table(L, "kv", &weakkv, &keykv, &valkv);
  seen0 = gc2_weak_tables_seen_acq(g);
  weakkey0 = gc2_weak_tables_weakkey_acq(g);
  weakval0 = gc2_weak_tables_weakval_acq(g);
  allweak0 = gc2_weak_tables_allweak_acq(g);
  queued0 = gc2_weak_tables_queued_acq(g);
  overflow0 = gc2_weak_tables_overflow_acq(g);
  scan_runs0 = gc2_weak_scan_runs_acq(g);
  scan_tables0 = gc2_weak_scan_tables_acq(g);
  scan_slots0 = gc2_weak_scan_slots_acq(g);
  scan_clearable0 = gc2_weak_scan_clearable_acq(g);
  clear_runs0 = gc2_weak_clear_runs_acq(g);
  clear_tables0 = gc2_weak_clear_tables_acq(g);
  clear_slots0 = gc2_weak_clear_slots_acq(g);
  clear_cleared0 = gc2_weak_clear_cleared_acq(g);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_test_weak_snapshot_count(g) == 0);
  assert(lj_gc2_markobj(g, obj2gco(weakv)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(weakk)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(weakkv)) == 1);
  flush_and_drain(g, tg);

  assert(lj_gc2_ismarked(g, obj2gco(keyv)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(valv)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(keyk)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(valk)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(keykv)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(valkv)) == 0);
  assert_weak_mode_marked(g, weakv);
  assert_weak_mode_marked(g, weakk);
  assert_weak_mode_marked(g, weakkv);
  /* Traversal-attempt telemetry may include guarded setup rescans. Snapshot
  ** publication below remains exactly once per table and cycle. */
  assert(gc2_weak_tables_seen_acq(g) >= seen0 + 3u);
  assert(gc2_weak_tables_weakkey_acq(g) >= weakkey0 + 2u);
  assert(gc2_weak_tables_weakval_acq(g) >= weakval0 + 2u);
  assert(gc2_weak_tables_allweak_acq(g) >= allweak0 + 1u);
  assert(gc2_weak_tables_queued_acq(g) == queued0 + 3u);
  assert(gc2_weak_tables_overflow_acq(g) == overflow0);
  assert(lj_gc2_test_weak_snapshot_count(g) == 3u);
  assert(weak_snapshot_has(g, weakv));
  assert(weak_snapshot_has(g, weakk));
  assert(weak_snapshot_has(g, weakkv));
  assert(lj_gc2_test_weak_snapshot_scan(g, 1) == 1u);
  assert(lj_gc2_test_weak_snapshot_scan(g, 1) == 1u);
  assert(lj_gc2_test_weak_snapshot_scan(g, 1) == 1u);
  assert(lj_gc2_test_weak_snapshot_scan(g, 1) == 0);
  assert(gc2_weak_scan_runs_acq(g) == scan_runs0 + 3u);
  assert(gc2_weak_scan_tables_acq(g) == scan_tables0 + 3u);
  assert(gc2_weak_scan_slots_acq(g) == scan_slots0 + 3u);
  assert(gc2_weak_scan_clearable_acq(g) ==
	 scan_clearable0 + 3u);
  assert(lj_gc2_test_weak_drain(g, 1) == 0);
  assert(la_load64_acq(&g->gc2.weak_clear_cursor) == 0);
  enter_weak_clear_fixture(g, tg);
  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  assert(gc2_weak_clear_runs_acq(g) == clear_runs0 + 3u);
  assert(gc2_weak_clear_tables_acq(g) == clear_tables0 + 3u);
  assert(gc2_weak_clear_slots_acq(g) == clear_slots0 + 3u);
  assert(gc2_weak_clear_cleared_acq(g) == clear_cleared0 + 3u);
  assert(weak_entry_is_nil(L, weakv, keyv));
  assert(weak_entry_is_nil(L, weakk, keyk));
  assert(weak_entry_is_nil(L, weakkv, keykv));
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 9);
}

static void test_worker_weak_drain(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *weak, *key, *val;
  uint64_t worker_runs0, worker_weak0, clear_tables0, clear_cleared0, idle0;

  make_weak_table(L, "v", &weak, &key, &val);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  enter_weak_clear_fixture(g, tg);
  worker_runs0 = gc2_worker_runs_acq(g);
  worker_weak0 = gc2_worker_weak_drained_acq(g);
  clear_tables0 = gc2_weak_clear_tables_acq(g);
  clear_cleared0 = gc2_weak_clear_cleared_acq(g);
  assert(lj_gc2_worker_drain(g, 1) == 1u);
  assert(gc2_worker_runs_acq(g) == worker_runs0 + 1u);
  assert(gc2_worker_weak_drained_acq(g) == worker_weak0 + 1u);
  assert(gc2_weak_clear_tables_acq(g) == clear_tables0 + 1u);
  assert(gc2_weak_clear_cleared_acq(g) == clear_cleared0 + 1u);
  assert(weak_entry_is_nil(L, weak, key));
  idle0 = gc2_worker_idle_declares_acq(g);
  assert(lj_gc2_worker_drain(g, 1) == 0);
  assert(gc2_worker_idle_declares_acq(g) == idle0 + 1u);
  assert(gc2_worker_active_acq(g) == 0);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 3);
}

static void test_weak_clear_marks_string_slots(lua_State *L, global_State *g,
					       TGState *tg)
{
  GCtab *weak, *val;
  GCstr *keystr, *modestr;
  uint8_t keystr_white0, modestr_white0;
  uint64_t clear_cleared0;

  lua_settop(L, 0);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_setmetatable(L, -2);
  lua_pushliteral(L, "__mode");
  lua_pushfstring(L, "kv gc2 weak mode %p", (void *)weak);
  modestr = strV(L->top - 1);
  lua_settable(L, -3);
  lua_newtable(L);
  val = tabV(L->top - 1);
  lua_pushfstring(L, "gc2 weak string key %p", (void *)weak);
  keystr = strV(L->top - 1);
  lua_pushvalue(L, 2);
  lua_settable(L, 1);
  keystr_white0 = (uint8_t)(lj_obj_gcflags(obj2gco(keystr)) & LJ_GC_WHITES);
  modestr_white0 = (uint8_t)(lj_obj_gcflags(obj2gco(modestr)) & LJ_GC_WHITES);
  clear_cleared0 = gc2_weak_clear_cleared_acq(g);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(keystr)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(modestr)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  assert(lj_gc2_test_weak_drain(g, 1) == 0);
  enter_weak_clear_fixture(g, tg);
  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(keystr)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(modestr)) == 1);
  assert((lj_obj_gcflags(obj2gco(keystr)) & LJ_GC_WHITES) == keystr_white0);
  assert((lj_obj_gcflags(obj2gco(modestr)) & LJ_GC_WHITES) == modestr_white0);
  assert(gc2_weak_clear_cleared_acq(g) == clear_cleared0 + 1u);
  setstrV(L, L->top, keystr);
  L->top++;
  lua_gettable(L, 1);
  assert(tvisnil(L->top - 1));
  lua_pop(L, 1);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
}

#if defined(LJ_GC2_TEST_HELPERS)
static void make_weak_string_value_table(lua_State *L, const char *mode,
					 GCtab **weak, GCtab **key,
					 GCstr **val)
{
  lua_newtable(L);
  *weak = tabV(L->top - 1);
  lua_newtable(L);
  *key = tabV(L->top - 1);
  lua_pushfstring(L, "gc2 weak retry string value %p", (void *)*weak);
  *val = strV(L->top - 1);
  lua_pushvalue(L, -2);
  lua_pushvalue(L, -2);
  lua_settable(L, -5);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushstring(L, mode);
  lua_settable(L, -3);
  lua_setmetatable(L, -4);
}

static int weak_entry_is_string(lua_State *L, GCtab *weak, GCtab *key,
				 GCstr *val)
{
  TValue k;
  cTValue *slot;
  settabV(L, &k, key);
  slot = lj_tab_get(L, weak, &k);
  return tvisstr(slot) && strV(slot) == val;
}

static void test_weak_table_value_admission_retry(lua_State *L,
					  global_State *g, TGState *tg,
					  int marked)
{
  GCtab *weak, *key, *val;

  lua_settop(L, 0);
  make_weak_table(L, "v", &weak, &key, &val);
  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  if (marked)
    assert(lj_gc2_markobj(g, obj2gco(val)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == marked);
  enter_weak_clear_fixture(g, tg);
  assert(gc2_weak_clear_cursor_acq(g) == 0);

  lj_gc2_test_stack_admission_retry_once(obj2gco(val));
  assert(lj_gc2_test_weak_drain(g, 1) == 0);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert(gc2_weak_clear_cursor_acq(g) == 0);
  assert(!weak_entry_is_nil(L, weak, key));

  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  assert(gc2_weak_clear_cursor_acq(g) == 1u);
  assert(weak_entry_is_nil(L, weak, key) == !marked);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == marked);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

static void test_weak_string_value_admission_retry(lua_State *L,
					   global_State *g, TGState *tg)
{
  GCtab *weak, *key;
  GCstr *val;

  lua_settop(L, 0);
  make_weak_string_value_table(L, "v", &weak, &key, &val);
  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  enter_weak_clear_fixture(g, tg);

  lj_gc2_test_stack_admission_retry_once(obj2gco(val));
  assert(lj_gc2_test_weak_drain(g, 1) == 0);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert(gc2_weak_clear_cursor_acq(g) == 0);
  assert(weak_entry_is_string(L, weak, key, val));
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  assert(gc2_weak_clear_cursor_acq(g) == 1u);
  assert(weak_entry_is_string(L, weak, key, val));
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

static void test_weak_dead_key_does_not_mark_string_value(lua_State *L,
						   global_State *g,
						   TGState *tg)
{
  GCtab *weak, *key;
  GCstr *val;

  lua_settop(L, 0);
  make_weak_string_value_table(L, "kv", &weak, &key, &val);
  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  enter_weak_clear_fixture(g, tg);

  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  assert(gc2_weak_clear_cursor_acq(g) == 1u);
  assert(weak_entry_is_nil(L, weak, key));
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

static void test_weak_hash_retry_beats_clearable_key(lua_State *L,
					      global_State *g, TGState *tg)
{
  GCtab *weak, *key, *val;

  lua_settop(L, 0);
  make_weak_table(L, "kv", &weak, &key, &val);
  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(val)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  enter_weak_clear_fixture(g, tg);

  lj_gc2_test_stack_admission_retry_once(obj2gco(val));
  assert(lj_gc2_test_weak_drain(g, 1) == 0);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert(gc2_weak_clear_cursor_acq(g) == 0);
  assert(!weak_entry_is_nil(L, weak, key));

  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  assert(gc2_weak_clear_cursor_acq(g) == 1u);
  assert(weak_entry_is_nil(L, weak, key));
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

static void test_weak_overflow_retry_stops_bridge(lua_State *L,
					  global_State *g, TGState *tg)
{
  GCtab *overflow = NULL, *overflow_key = NULL, *overflow_val = NULL;
  GCtab *bridge = NULL, *bridge_key = NULL, *bridge_val = NULL;
  GCRef weak_head;
  MSize cap, i, n;

  lua_settop(L, 0);
  cap = gc2_weak_capacity_acq(g);
  assert(cap > 0);
  n = cap + 1u;
  make_weak_table_batch(L, n);
  lj_gc2_mark_begin(g);
  assert(gc2_weak_capacity_acq(g) == cap);
  mark_weak_table_batch(L, g, n);
  flush_and_drain(g, tg);
  assert(gc2_weak_count_acq(g) == (uint64_t)n);
  assert(lj_gc2_test_weak_snapshot_count(g) == cap);

  for (i = 0; i < n; i++) {
    TValue *tv = L->base + i * 3u;
    GCtab *weak = tabV(tv);
    if (weak_snapshot_has(g, weak)) {
      if (!bridge) {
	bridge = weak;
	bridge_key = tabV(tv + 1);
	bridge_val = tabV(tv + 2);
      }
    } else {
      assert(overflow == NULL);
      overflow = weak;
      overflow_key = tabV(tv + 1);
      overflow_val = tabV(tv + 2);
    }
  }
  assert(overflow != NULL && bridge != NULL && overflow != bridge);
  assert(lj_gc2_ismarked(g, obj2gco(overflow_val)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(bridge_val)) == 0);
  setgcrefnull(weak_head);
  weak_bridge_link(&weak_head, bridge, LJ_GC_WEAKVAL);
  enter_weak_clear_fixture(g, tg);

  lj_gc2_test_stack_admission_retry_once(obj2gco(overflow_val));
  assert(!lj_gc2_test_weak_overflow_clear_bridge(g, gcref(weak_head)));
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert(gc2_weak_clear_cursor_acq(g) == 0);
  assert(!weak_entry_is_nil(L, overflow, overflow_key));
  assert(!weak_entry_is_nil(L, bridge, bridge_key));

  assert(lj_gc2_test_weak_overflow_clear_bridge(g, gcref(weak_head)));
  assert(weak_entry_is_nil(L, overflow, overflow_key));
  assert(weak_entry_is_nil(L, bridge, bridge_key));
  setgcrefnull(weak_head);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

static void test_weak_value_admission_tristate(lua_State *L,
					global_State *g, TGState *tg)
{
  test_weak_table_value_admission_retry(L, g, tg, 1);
  test_weak_string_value_admission_retry(L, g, tg);
  test_weak_dead_key_does_not_mark_string_value(L, g, tg);
  test_weak_table_value_admission_retry(L, g, tg, 0);
  test_weak_hash_retry_beats_clearable_key(L, g, tg);
  test_weak_overflow_retry_stops_bridge(L, g, tg);
}

static GCtab *make_weak_mode_metatable(lua_State *L)
{
  GCtab *mt;
  lua_newtable(L);
  mt = tabV(L->top - 1);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  return mt;
}

static void install_weak_metatable_raw(GCtab *weak, GCtab *mt)
{
  assert(weak != NULL && mt != NULL);
  assert((lj_obj_gcflags(obj2gco(weak)) & LJ_GC_WEAKVAL) != 0);
  /* Deliberately bypass the mutator barrier: frontier closure must recover
  ** this synthetic late strong header edge itself. */
  setgcrefrel(weak->metatable, obj2gco(mt));
}

static void prepare_weak_frontier_close(global_State *g, TGState *tg)
{
  lj_gc2_mark_to_weak(g);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  flush_and_drain(g, tg);
  assert(gc2_thread_scan_needscan_pending_acq(g) == 0);
  assert(gc2_table_rescan_pending_acq(g) == 0);
  gc2_weak_root_scanned_rel(g, 1);
  gc2_weak_mark_closed_rel(g, 0);
  (void)gc2_marks_this_round_xchg_acqrel(g, 0);
  assert(gc2_weak_clear_cursor_acq(g) == 0);
}

static void test_weak_frontier_vector_retry(lua_State *L, global_State *g,
					     TGState *tg)
{
  GCtab *weak, *key, *value, *late;

  lua_settop(L, 0);
  make_weak_table(L, "v", &weak, &key, &value);
  late = make_weak_mode_metatable(L);
  /* Clear setup-store rescans while every synthetic object is still rooted;
  ** the fixture then controls the only late edge with a deliberate raw store. */
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  lua_settop(L, 0);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(late)) == 0);
  prepare_weak_frontier_close(g, tg);
  install_weak_metatable_raw(weak, late);

  lj_gc2_test_weak_frontier_fault_once(
    LJ_GC2_WEAK_FRONTIER_FAULT_VECTOR_TAB, 0);
  assert(!lj_gc2_weak_complete(g, L, NULL, LJ_GC2_WEAK_DRAIN_BATCH));
  assert(lj_gc2_test_weak_frontier_fault_hits() == 1u);
  assert(gc2_weak_mark_closed_acq(g) == 0);
  assert(gc2_weak_clear_cursor_acq(g) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(late)) == 0);

  assert(lj_gc2_test_weak_trace_close_frontier(g, NULL));
  assert(lj_gc2_ismarked(g, obj2gco(late)) == 1);
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
  UNUSED(key); UNUSED(value);
}

static uint32_t weak_frontier_pair_marked(global_State *g, GCtab *a,
					   GCtab *b)
{
  return (uint32_t)(lj_gc2_ismarked(g, obj2gco(a)) == 1) +
    (uint32_t)(lj_gc2_ismarked(g, obj2gco(b)) == 1);
}

static void weak_frontier_overflow_fault_attempt(lua_State *L,
						  global_State *g,
						  TGState *tg,
						  GCtab *weak0,
						  GCtab *weak1,
						  GCtab *late0,
						  GCtab *late1,
						  uint32_t kind,
						  uint32_t skip,
						  uint32_t marked)
{
  install_weak_metatable_raw(weak0, late0);
  install_weak_metatable_raw(weak1, late1);
  assert(weak_frontier_pair_marked(g, late0, late1) == 0);
  (void)gc2_marks_this_round_xchg_acqrel(g, 0);

  lj_gc2_test_weak_frontier_fault_once(kind, skip);
  assert(!lj_gc2_weak_complete(g, L, NULL, LJ_GC2_WEAK_DRAIN_BATCH));
  assert(lj_gc2_test_weak_frontier_fault_hits() == 1u);
  assert(gc2_weak_mark_closed_acq(g) == 0);
  assert(gc2_weak_clear_cursor_acq(g) == 0);
  assert(weak_frontier_pair_marked(g, late0, late1) == marked);

  assert(lj_gc2_test_weak_trace_close_frontier(g, NULL));
  assert(weak_frontier_pair_marked(g, late0, late1) == 2u);
  flush_and_drain(g, tg);
  (void)gc2_marks_this_round_xchg_acqrel(g, 0);
}

static void test_weak_frontier_overflow_retries(lua_State *L,
						 global_State *g,
						 TGState *tg)
{
  GCtab *weak0, *key0, *value0, *weak1, *key1, *value1;
  GCtab *late[6];
  MSize cap;
  uint64_t overflow0;
  uint32_t i;

  lua_settop(L, 0);
  make_weak_table(L, "v", &weak0, &key0, &value0);
  make_weak_table(L, "v", &weak1, &key1, &value1);
  for (i = 0; i < 6u; i++)
    late[i] = make_weak_mode_metatable(L);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  lua_settop(L, 0);

  lj_gc2_mark_begin(g);
  cap = gc2_weak_capacity_acq(g);
  assert(cap > 0);
  overflow0 = gc2_weak_tables_overflow_acq(g);
  /* Keep the allocated vector intact while forcing these two discoveries down
  ** the raw overflow list; restore capacity before any frontier snapshot. */
  gc2_weak_capacity_rel(g, 0);
  assert(lj_gc2_markobj(g, obj2gco(weak0)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(weak1)) == 1);
  flush_and_drain(g, tg);
  gc2_weak_capacity_rel(g, cap);
  assert(gc2_weak_tables_overflow_acq(g) == overflow0 + 2u);
  assert(lj_gc2_test_weak_snapshot_count(g) == 0);
  assert(gc2_weak_overflow_acq(g) != NULL);
  for (i = 0; i < 6u; i++)
    assert(lj_gc2_ismarked(g, obj2gco(late[i])) == 0);
  prepare_weak_frontier_close(g, tg);

  /* Unknown head identity must fail closed before either table is traced. */
  weak_frontier_overflow_fault_attempt(
    L, g, tg, weak0, weak1, late[0], late[1],
    LJ_GC2_WEAK_FRONTIER_FAULT_OVERFLOW_NODE, 0, 0);
  /* A transient exact TAB admission must not disappear as a stale entry. */
  weak_frontier_overflow_fault_attempt(
    L, g, tg, weak0, weak1, late[2], late[3],
    LJ_GC2_WEAK_FRONTIER_FAULT_OVERFLOW_TAB, 0, 0);
  /* The second node is admitted while the first node scope is still live.
  ** Failing that handoff traces exactly the head and replays the successor. */
  weak_frontier_overflow_fault_attempt(
    L, g, tg, weak0, weak1, late[4], late[5],
    LJ_GC2_WEAK_FRONTIER_FAULT_OVERFLOW_NODE, 1, 1);

  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
  UNUSED(key0); UNUSED(value0); UNUSED(key1); UNUSED(value1);
}

static void test_weak_frontier_admission_retries(lua_State *L,
						  global_State *g,
						  TGState *tg)
{
  test_weak_frontier_vector_retry(L, g, tg);
  test_weak_frontier_overflow_retries(L, g, tg);
}
#endif

static void test_weak_drain_uses_captured_mode(lua_State *L, global_State *g,
					       TGState *tg)
{
  GCtab *weak, *key, *val;

  lua_settop(L, 0);
  make_weak_table(L, "v", &weak, &key, &val);
  lua_pop(L, 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert((lj_obj_gcflags(obj2gco(weak)) & LJ_GC_WEAK) == LJ_GC_WEAKVAL);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  clear_weak_mode_raw(L, g, weak);
  assert((lj_obj_gcflags(obj2gco(weak)) & LJ_GC_WEAK) == LJ_GC_WEAKVAL);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  enter_weak_clear_fixture(g, tg);
  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  assert(weak_entry_is_nil(L, weak, key));
  while (lj_gc2_test_weak_drain(g, 1) != 0)
    ;
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
}

static void test_weak_pre_clear_late_write_survives_drain(lua_State *L,
							  global_State *g,
							  TGState *tg)
{
  GCtab *weak, *key, *oldval, *late_val;
  uint8_t oldstate;

  lua_settop(L, 0);
  make_weak_table(L, "v", &weak, &key, &oldval);
  lua_newtable(L);
  late_val = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(oldval)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 0);

  enter_weak_clear_fixture(g, tg);
  gc2_weak_write_active_add(g, 1);
  assert(lj_gc2_test_weak_drain(g, 1) == 0);
  assert(gc2_weak_clear_cursor_acq(g) == 0);
  assert(!weak_entry_is_nil(L, weak, key));
  oldstate = g->gc.state;
  g->gc.state = GCSatomic;
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 4);
  lua_settable(L, 1);  /* P_WEAK late write before weak drain. */
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 1);
  {
    uint32_t old = gc2_weak_write_active_sub(g, 1);
    assert(old != 0);
  }
  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  g->gc.state = oldstate;

  lua_pushvalue(L, 2);
  lua_gettable(L, 1);
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == late_val);
  lua_pop(L, 1);

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 4);
}

static void test_weak_post_clear_resurrection_write(lua_State *L,
						    global_State *g,
						    TGState *tg)
{
  GCtab *weak, *key, *val, *late_key, *late_val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  make_weak_table(L, "kv", &weak, &key, &val);
  lua_newtable(L);
  late_key = tabV(L->top - 1);
  lua_newtable(L);
  late_val = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(late_key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 0);

  clear_weak_mode_raw(L, g, weak);

  enter_weak_clear_fixture(g, tg);
  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  assert(weak_entry_is_nil(L, weak, key));
  while (lj_gc2_test_weak_drain(g, 1) != 0)
    ;
  weak_keys0 = gc2_weak_keys_marked_acq(g);
  weak_vals0 = gc2_weak_values_marked_acq(g);

  lua_pushvalue(L, 4);
  lua_pushvalue(L, 5);
  lua_settable(L, 1);
  assert(lj_gc2_ismarked(g, obj2gco(late_key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 1);
  assert(gc2_weak_keys_marked_acq(g) >= weak_keys0);
  assert(gc2_weak_values_marked_acq(g) >= weak_vals0);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lua_pushvalue(L, 4);
  lua_gettable(L, 1);
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == late_val);
  lua_pop(L, 1);

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 5);
}

static void test_vm_weak_post_clear_existing_key_write(lua_State *L,
						       global_State *g,
						       TGState *tg)
{
  GCtab *weak, *key, *oldval, *late_val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "return function(t, k, v) t[k] = v end\n") == LUA_OK);
  make_weak_table(L, "k", &weak, &key, &oldval);
  lua_newtable(L);
  late_val = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(oldval)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 0);

  enter_weak_clear_fixture(g, tg);
  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  assert(weak_entry_is_nil(L, weak, key));
  while (lj_gc2_test_weak_drain(g, 1) != 0)
    ;
  weak_keys0 = gc2_weak_keys_marked_acq(g);
  weak_vals0 = gc2_weak_values_marked_acq(g);

  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 5);
  lua_call(L, 3, 0);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 1);
  assert(gc2_weak_keys_marked_acq(g) >= weak_keys0);
  assert(gc2_weak_values_marked_acq(g) == weak_vals0);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lua_pushvalue(L, 3);
  lua_gettable(L, 2);
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == late_val);
  lua_pop(L, 1);

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 5);
}

static void test_capi_rawset_weak_write_barrier(lua_State *L, global_State *g,
						TGState *tg)
{
  GCtab *hash_weak, *hash_key, *hash_val, *array_weak, *array_val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  lua_newtable(L);
  hash_weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "kv");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  hash_key = tabV(L->top - 1);
  lua_newtable(L);
  hash_val = tabV(L->top - 1);

  lua_newtable(L);
  array_weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  array_val = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(hash_weak)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(array_weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(hash_weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(array_weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(hash_key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(hash_val)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(array_val)) == 0);

  lj_gc2_mark_to_weak(g);
  weak_keys0 = gc2_weak_keys_marked_acq(g);
  weak_vals0 = gc2_weak_values_marked_acq(g);

  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_rawset(L, 1);  /* C API raw hash weak write. */
  lua_pushvalue(L, 5);
  lua_rawseti(L, 4, 1);  /* C API raw array weak write. */

  assert(lj_gc2_ismarked(g, obj2gco(hash_key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(hash_val)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(array_val)) == 1);
  assert(gc2_weak_keys_marked_acq(g) >= weak_keys0);
  assert(gc2_weak_values_marked_acq(g) >= weak_vals0);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 5);
}

static void test_weak_key_write_barrier(lua_State *L, global_State *g,
					TGState *tg)
{
  GCtab *weak, *key, *val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "k");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  key = tabV(L->top - 1);
  lua_newtable(L);
  val = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  weak_keys0 = gc2_weak_keys_marked_acq(g);
  weak_vals0 = gc2_weak_values_marked_acq(g);

  lj_gc2_mark_to_weak(g);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_settable(L, 1);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(gc2_weak_keys_marked_acq(g) >= weak_keys0);
  assert(gc2_weak_values_marked_acq(g) >= weak_vals0);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 3);
}

static void test_vm_weak_key_write_barrier(lua_State *L, global_State *g,
					   TGState *tg)
{
  GCtab *weak, *key, *val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "return function(t, k, v) t[k] = v end\n") == LUA_OK);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "k");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  key = tabV(L->top - 1);
  lua_newtable(L);
  val = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  weak_keys0 = gc2_weak_keys_marked_acq(g);
  weak_vals0 = gc2_weak_values_marked_acq(g);

  lj_gc2_mark_to_weak(g);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 4);
  lua_call(L, 3, 0);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(gc2_weak_keys_marked_acq(g) >= weak_keys0);
  assert(gc2_weak_values_marked_acq(g) == weak_vals0);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 4);
}

static void test_peer_weak_key_write_barrier(lua_State *L, global_State *g,
					     TGState *tg)
{
  lua_State *peer;
  GCtab *weak, *key, *val;
  WeakPeerWriteCtx ctx;
  pthread_t thread;
  uint64_t weak_keys0, weak_vals0;
  int i;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "return function(t, k, v) t[k] = v end\n") == LUA_OK);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "k");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  key = tabV(L->top - 1);
  lua_newtable(L);
  val = tabV(L->top - 1);
  peer = lua_newthread(L);
  assert(peer != NULL);
  assert(lua_checkstack(peer, 4));
  for (i = 1; i <= 4; i++) {
    lua_pushvalue(L, i);
    lua_xmove(L, peer, 1);
  }

  ctx.L = peer;
  ctx.status = -1;
  assert(ljt_barrier_init(&ctx.barrier, 2) == 0);
  assert(pthread_create(&thread, NULL, weak_peer_write_thread, &ctx) == 0);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  weak_keys0 = gc2_weak_keys_marked_acq(g);
  weak_vals0 = gc2_weak_values_marked_acq(g);

  lj_gc2_mark_to_weak(g);
  grey_wait(&ctx.barrier);  /* Peer TG performs the P_WEAK table write. */
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.status == 0);
  assert(ljt_barrier_destroy(&ctx.barrier) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(gc2_weak_keys_marked_acq(g) >= weak_keys0);
  assert(gc2_weak_values_marked_acq(g) == weak_vals0);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_settop(peer, 0);
  lua_pop(L, 5);
}

static void test_vm_weak_value_hash_key_barrier(lua_State *L, global_State *g,
						TGState *tg)
{
  GCtab *weak, *key, *val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "return function(t, k, v) t[k] = v end\n") == LUA_OK);
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

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  weak_keys0 = gc2_weak_keys_marked_acq(g);
  weak_vals0 = gc2_weak_values_marked_acq(g);

  lj_gc2_mark_to_weak(g);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 4);
  lua_call(L, 3, 0);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(gc2_weak_keys_marked_acq(g) >= weak_keys0);
  assert(gc2_weak_values_marked_acq(g) >= weak_vals0);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 4);
}

static void test_vm_weak_value_array_barrier(lua_State *L, global_State *g,
					     TGState *tg)
{
  GCtab *weak, *val;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "return function(t, v) t[1] = v end\n") == LUA_OK);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_pushboolean(L, 0);
  lua_rawseti(L, -2, 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  val = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  lj_gc2_mark_to_weak(g);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_call(L, 2, 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 3);
}

static void test_table_insert_weak_value_array_barrier(lua_State *L,
						       global_State *g,
						       TGState *tg)
{
  GCtab *weak, *val;
  uint64_t weak_vals0;

  lua_settop(L, 0);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_pushboolean(L, 0);
  lua_rawseti(L, -2, 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  val = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  lj_gc2_mark_to_weak(g);
  weak_vals0 = gc2_weak_values_marked_acq(g);
  lua_getglobal(L, "table");
  lua_getfield(L, -1, "insert");
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_call(L, 2, 0);  /* table.insert weak-value array write. */
  lua_pop(L, 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(gc2_weak_values_marked_acq(g) >= weak_vals0);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
}

static void test_capi_weak_newindex_target_write_barrier(lua_State *L,
							 global_State *g,
							 TGState *tg)
{
  GCtab *weak, *key, *oldval, *proxy, *late_val, *field_val;
  uint64_t weak_keys0, weak_vals0;

  lua_settop(L, 0);
  make_weak_table(L, "v", &weak, &key, &oldval);
  lua_newtable(L);
  proxy = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__newindex");
  lua_pushvalue(L, 1);
  lua_settable(L, -3);
  lua_setmetatable(L, 4);
  lua_newtable(L);
  late_val = tabV(L->top - 1);
  lua_newtable(L);
  field_val = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(proxy)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(oldval)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(field_val)) == 0);

  enter_weak_clear_fixture(g, tg);
  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  assert(weak_entry_is_nil(L, weak, key));
  while (lj_gc2_test_weak_drain(g, 1) != 0)
    ;
  weak_keys0 = gc2_weak_keys_marked_acq(g);
  weak_vals0 = gc2_weak_values_marked_acq(g);

  lua_pushvalue(L, 2);
  lua_pushvalue(L, 5);
  lua_settable(L, 4);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 1);
  assert(gc2_weak_keys_marked_acq(g) == weak_keys0);
  assert(gc2_weak_values_marked_acq(g) >= weak_vals0);

  lua_pushvalue(L, 6);
  lua_setfield(L, 4, "field");
  assert(lj_gc2_ismarked(g, obj2gco(field_val)) == 1);
  assert(gc2_weak_values_marked_acq(g) >= weak_vals0);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);

  lua_pushvalue(L, 2);
  lua_gettable(L, 1);
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == late_val);
  lua_pop(L, 1);
  lua_getfield(L, 1, "field");
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == field_val);
  lua_pop(L, 1);

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 6);
}

static void test_vm_weak_newindex_target_write_barrier(lua_State *L,
						       global_State *g,
						       TGState *tg)
{
  GCtab *weak, *key, *oldval, *proxy, *late_val, *field_val;
  uint64_t weak_vals0;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "return function(t, k, v, f)\n"
    "  t[k] = v\n"
    "  t.field = f\n"
    "end\n") == LUA_OK);
  make_weak_table(L, "v", &weak, &key, &oldval);
  lua_newtable(L);
  proxy = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__newindex");
  lua_pushvalue(L, 2);
  lua_settable(L, -3);
  lua_setmetatable(L, 5);
  lua_newtable(L);
  late_val = tabV(L->top - 1);
  lua_newtable(L);
  field_val = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(proxy)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(oldval)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(field_val)) == 0);

  enter_weak_clear_fixture(g, tg);
  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  assert(weak_entry_is_nil(L, weak, key));
  while (lj_gc2_test_weak_drain(g, 1) != 0)
    ;
  weak_vals0 = gc2_weak_values_marked_acq(g);

  lua_pushvalue(L, 1);
  lua_pushvalue(L, 5);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 6);
  lua_pushvalue(L, 7);
  lua_call(L, 4, 0);
  assert(lj_gc2_ismarked(g, obj2gco(late_val)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(field_val)) == 1);
  assert(gc2_weak_values_marked_acq(g) >= weak_vals0);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);

  lua_pushvalue(L, 3);
  lua_gettable(L, 2);
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == late_val);
  lua_pop(L, 1);
  lua_getfield(L, 2, "field");
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == field_val);
  lua_pop(L, 1);

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 7);
}

static void test_tvalue_range_barrier(lua_State *L, global_State *g,
				      TGState *tg, GCtab *child1,
				      GCtab *child2)
{
  TValue vals[2];

  settabV(L, &vals[0], child1);
  settabV(L, &vals[1], child2);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(child1)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child2)) == 0);
  lj_gc2_barrier_tvn_pair_g(g, NULL, vals, 2);
  assert(lj_gc2_ismarked(g, obj2gco(child1)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child2)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
}

static void test_vm_tsetm_range_barrier(lua_State *L, global_State *g,
					TGState *tg)
{
  enum { TSETM_BIG_N = 96 };
  GCtab *child1, *child2, *parent, *src, *last;
  int i;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "return function(a, b)\n"
    "  local function many() return a, b end\n"
    "  return { many() }\n"
    "end,\n"
    "function(src, n)\n"
    "  local function many() return unpack(src, 1, n) end\n"
    "  return { many() }\n"
    "end\n") == LUA_OK);
  lua_newtable(L);
  child1 = tabV(L->top - 1);
  lua_newtable(L);
  child2 = tabV(L->top - 1);

  test_tvalue_range_barrier(L, g, tg, child1, child2);

  lj_gc2_mark_begin(g);
  /* Keep the VM TSETM constructor call inside the synthetic MARK cycle. */
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  assert(lj_gc2_ismarked(g, obj2gco(child1)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child2)) == 0);

  lua_pushvalue(L, 1);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 4);
  lua_call(L, 2, 1);
  parent = tabV(L->top - 1);
  assert(lj_tab_asize_acq(parent) >= 2);
  assert(tabV(lj_tab_getint(parent, 1)) == child1);
  assert(tabV(lj_tab_getint(parent, 2)) == child2);
  assert(lj_gc2_ismarked(g, obj2gco(child1)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child2)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);

  lua_createtable(L, TSETM_BIG_N, 0);
  src = tabV(L->top - 1);
  last = NULL;
  for (i = 1; i <= TSETM_BIG_N; i++) {
    lua_newtable(L);
    if (i == TSETM_BIG_N)
      last = tabV(L->top - 1);
    lua_rawseti(L, -2, i);
  }

  lj_gc2_mark_begin(g);
  /* Keep the large TSETM/resize path inside the synthetic MARK cycle too. */
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  assert(lj_gc2_ismarked(g, obj2gco(src)) == 0);
  assert(last != NULL);
  assert(lj_gc2_ismarked(g, obj2gco(last)) == 0);

  lua_pushvalue(L, 2);
  lua_pushvalue(L, 5);
  lua_pushinteger(L, TSETM_BIG_N);
  lua_call(L, 2, 1);
  parent = tabV(L->top - 1);
  assert(lj_tab_asize_acq(parent) >= TSETM_BIG_N);
  assert(tabV(lj_tab_getint(parent, TSETM_BIG_N)) == last);
  assert(lj_gc2_ismarked(g, obj2gco(last)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 6);
}

static void test_closure(lua_State *L, global_State *g, TGState *tg)
{
  GCfunc *fn;
  GCtab *up;

  assert(luaL_dostring(L,
    "local x = {}\n"
    "return function() return x end, x\n") == LUA_OK);
  fn = funcV(L->top - 2);
  up = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(up)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(fn)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(funcproto(fn))) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(up)) == 1);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
}

static void test_tg_thread_roots(lua_State *L, global_State *g, TGState *tg)
{
  TGState extra_tg;
  lua_State *thread_L, *cur_L, *registry_L, *ownerless_L, *stale_L, *fail_L;
  GCtab *thread_tab, *cur_tab, *registry_tab, *ownerless_tab;
  GCtab *anchor_tab, *stale_tab, *fail_tab;
  uint64_t thread_roots0, cur_roots0, major_roots0;
  uint64_t registry_epoch0, ownerless_epoch0, stale_epoch0, fail_epoch0;
  uint64_t busy0, claims0;
  uint32_t n_threads0, anchor_idx, registry_stacksize, fail_stacksize;
  uint32_t round;
  TGState *old_tg = lj_thr_get_tg();
  int base = lua_gettop(L);

  thread_L = lua_newthread(L);
  assert(thread_L != NULL);
  lua_newtable(thread_L);
  thread_tab = tabV(thread_L->top - 1);
  cur_L = lua_newthread(L);
  assert(cur_L != NULL);
  lua_newtable(cur_L);
  cur_tab = tabV(cur_L->top - 1);

  lj_tg_init_thread(g, &extra_tg, thread_L, 1);
  extra_tg.tid = tg->tid + 6000u;
  if (extra_tg.tid == 0 || extra_tg.tid == LJ_THREAD_GCSCAN)
    extra_tg.tid = 6000u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  extra_tg.cur_L = cur_L;
  cur_L->tg_hint = &extra_tg;
  lj_state_owner_rel(thread_L, extra_tg.tid);
  lj_state_owner_rel(cur_L, extra_tg.tid);

  /* A registry-only foreign state is an identity root in the global pass. Its
  ** stack belongs to extra_tg and is reached only through NEEDSCAN. */
  registry_L = lua_newthread(L);
  assert(registry_L != NULL);
  lua_newtable(registry_L);
  registry_tab = tabV(registry_L->top - 1);
  registry_L->tg_hint = &extra_tg;
  lj_state_owner_rel(registry_L, extra_tg.tid);
  lj_state_thread_registry_publish(g, registry_L);

  /* Ownerless registry membership still grants identity, not a direct global
  ** stack scan. Ordinary queued traversal may GCSCAN-claim it later. */
  ownerless_L = lua_newthread(L);
  assert(ownerless_L != NULL);
  lua_newtable(ownerless_L);
  ownerless_tab = tabV(ownerless_L->top - 1);
  lj_state_owner_rel(ownerless_L, 0);
  lj_state_thread_registry_publish(g, ownerless_L);

  stale_L = lua_newthread(L);
  assert(stale_L != NULL);
  lua_newtable(stale_L);
  stale_tab = tabV(stale_L->top - 1);
  stale_L->tg_hint = tg;
  lj_state_owner_rel(stale_L, lj_tg_tid_acq(tg));

  /* Keep a validation-failure state off the main stack and outside the global
  ** registry. It becomes a TG publication only for the failure/rescan check. */
  fail_L = lua_newthread(L);
  assert(fail_L != NULL);
  lua_newtable(fail_L);
  fail_tab = tabV(fail_L->top - 1);
  fail_L->tg_hint = &extra_tg;
  lj_state_owner_rel(fail_L, extra_tg.tid);

  /* Remove all fixture states from the main stack. Their only semantic roots
  ** are now the explicit TG/registry publications under test. */
  lua_settop(L, base);

  lua_newtable(L);
  anchor_tab = tabV(L->top - 1);
  assert(lj_tg_root_anchor_push(L, &extra_tg, L->top - 1,
				&anchor_idx) != NULL);
  lua_pop(L, 1);

  lj_gc2_mark_begin(g);
  /* The synthetic foreign-owner loops below exercise NEEDSCAN ownership, not
  ** cooperative native scheduling. Keep MARK admission closed so their tight
  ** bounded drain retries cannot all land inside one native lease. */
  pin_mark_closed_for_worker_fixture(g);
  thread_roots0 = la_load64_acq(&g->gc2.tg_thread_roots);
  cur_roots0 = la_load64_acq(&g->gc2.tg_cur_roots);
  major_roots0 = gc2_major_root_scans_acq(g);
  /*
  ** mark_begin() is allowed to catch up the main TG registration after library
  ** bootstrap. The auxiliary TG assertion measures only the attach/detach delta
  ** for this fixture-owned state.
  */
  n_threads0 = g->gc2.n_threads;
  assert(lj_gc2_ismarked(g, obj2gco(thread_L)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(cur_L)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(registry_tab)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(ownerless_tab)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(anchor_tab)) == 0);
  lj_tg_attach(g, &extra_tg);
  assert(lj_tg_find_owner(g, extra_tg.tid) == &extra_tg);
  assert(g->gc2.n_threads >= n_threads0);
  /* The once-per-handshake global pass must not inspect auxiliary TG
  ** publications. Only the TG-owner entry may mark/stamp these states. */
  registry_epoch0 = lj_state_scan_epoch_acq(registry_L);
  ownerless_epoch0 = lj_state_scan_epoch_acq(ownerless_L);
  registry_stacksize = registry_L->stacksize;
  registry_L->stacksize = 0;  /* Remote identity traversal must not validate it. */
  lj_gc2_scan_cycle_global_roots(g);
  assert(lj_gc2_ismarked(g, obj2gco(thread_L)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(cur_L)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(thread_tab)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(cur_tab)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(anchor_tab)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(registry_L)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(ownerless_L)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(registry_tab)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(ownerless_tab)) == 0);
  assert(lj_state_scan_epoch_acq(registry_L) == registry_epoch0);
  assert(lj_state_scan_epoch_acq(ownerless_L) == ownerless_epoch0);
  assert(gc2_major_root_scans_acq(g) == major_roots0 + 1u);
  assert(la_load64_acq(&g->gc2.tg_thread_roots) == thread_roots0);
  assert(la_load64_acq(&g->gc2.tg_cur_roots) == cur_roots0);

  /* Drain until the foreign registry identity becomes a tid-addressed handoff.
  ** stacksize remains poisoned: reaching this bit proves traversal checked
  ** ownership before any mutable stack geometry. */
  for (round = 0; round < 64 &&
       !(lj_obj_gcflags(obj2gco(registry_L)) & LJ_GC_NEEDSCAN); round++) {
    (void)lj_gc2_flush_ssb(g, tg);
    (void)lj_gc2_worker_drain(g, 64);
  }
  assert(round < 64);
  assert(lj_obj_gcflags(obj2gco(registry_L)) & LJ_GC_NEEDSCAN);
  assert(lj_gc2_ismarked(g, obj2gco(registry_tab)) == 0);
  assert(lj_state_scan_epoch_acq(registry_L) == registry_epoch0);
  registry_L->stacksize = registry_stacksize;

  lj_thr_set_tg(&extra_tg);  /* Model the real owner-side safepoint ACK. */
  lj_gc2_scan_cycle_owner_tg_roots(g, &extra_tg);
  lj_thr_set_tg(old_tg);
  assert(lj_gc2_ismarked(g, obj2gco(thread_L)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(cur_L)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(thread_tab)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(cur_tab)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(anchor_tab)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(registry_tab)) == 1);
  assert(lj_state_scan_epoch_acq(thread_L) == gc2_thread_scan_cycle_acq(g));
  assert(lj_state_scan_epoch_acq(cur_L) == gc2_thread_scan_cycle_acq(g));
  assert(lj_state_scan_epoch_acq(registry_L) == gc2_thread_scan_cycle_acq(g));
  assert(la_load64_acq(&g->gc2.tg_thread_roots) == thread_roots0 + 1u);
  assert(la_load64_acq(&g->gc2.tg_cur_roots) == cur_roots0 + 1u);

  /* A stale auxiliary cur_L alias owned by the main TG is an identity root,
  ** but extra_tg must neither read nor stamp its foreign stack. */
  extra_tg.cur_L = stale_L;
  stale_epoch0 = lj_state_scan_epoch_acq(stale_L);
  lj_thr_set_tg(&extra_tg);
  lj_gc2_scan_cycle_owner_tg_roots(g, &extra_tg);
  lj_thr_set_tg(old_tg);
  assert(lj_gc2_ismarked(g, obj2gco(stale_L)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(stale_tab)) == 0);
  assert(lj_state_scan_epoch_acq(stale_L) == stale_epoch0);
  extra_tg.cur_L = cur_L;

  /* A same-owner publication whose stack validation transiently fails remains
  ** an identity root and queues a rescan; it must not silently stamp freshness. */
  extra_tg.cur_L = fail_L;
  fail_epoch0 = lj_state_scan_epoch_acq(fail_L);
  fail_stacksize = fail_L->stacksize;
  fail_L->stacksize = 0;
  lj_thr_set_tg(&extra_tg);
  lj_gc2_scan_cycle_owner_tg_roots(g, &extra_tg);
  lj_thr_set_tg(old_tg);
  assert(lj_gc2_ismarked(g, obj2gco(fail_L)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(fail_tab)) == 0);
  assert(lj_state_scan_epoch_acq(fail_L) == fail_epoch0);
  assert(!lj_gc2_test_ssb_empty(g));
  fail_L->stacksize = fail_stacksize;
  lj_thr_set_tg(&extra_tg);
  lj_gc2_scan_cycle_owner_tg_roots(g, &extra_tg);
  lj_thr_set_tg(old_tg);
  assert(lj_gc2_ismarked(g, obj2gco(fail_tab)) == 1);
  assert(lj_state_scan_epoch_acq(fail_L) == gc2_thread_scan_cycle_acq(g));
  extra_tg.cur_L = cur_L;

  (void)lj_gc2_flush_ssb(g, &extra_tg);
  flush_and_drain(g, tg);
  lj_tg_root_anchor_pop(&extra_tg, anchor_idx);
  lj_state_owner_rel(thread_L, 0);
  lj_state_owner_rel(cur_L, 0);
  lj_state_owner_rel(registry_L, 0);
  lj_state_owner_rel(stale_L, 0);
  thread_L->tg_hint = tg;
  cur_L->tg_hint = tg;
  registry_L->tg_hint = tg;

  /* Detach publishes DEAD before some lifecycle wrappers release their state
  ** owner. A DEAD-but-registered tid is not stale: the collector must requeue
  ** without validating or CAS-taking the state until release completes. */
  lj_gc2_smr_read_enter(g);  /* Retain the retired TG through late release. */
  lj_tg_detach(g, &extra_tg);
  assert(lj_tg_flags_test_acq(&extra_tg, TGF_DEAD));
  lj_state_scan_epoch_rel(fail_L, 0);
  lj_state_scan_handoff_epoch_rel(fail_L, 0);
  fail_stacksize = fail_L->stacksize;
  fail_L->stacksize = 0;
  busy0 = la_load64_acq(&g->gc2.thread_scan_busy);
  assert(lj_gc2_test_ssb_push(g, obj2gco(fail_L)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  for (round = 0; round < 8 &&
       la_load64_acq(&g->gc2.thread_scan_busy) == busy0; round++)
    (void)lj_gc2_worker_drain(g, 1);
  assert(round < 8);
  assert(lj_state_owner_acq(fail_L) == extra_tg.tid);
  assert(lj_state_scan_epoch_acq(fail_L) == 0);
  fail_L->stacksize = fail_stacksize;
  lj_state_release(fail_L, extra_tg.tid);
  lj_gc2_smr_read_leave(g);
  claims0 = la_load64_acq(&g->gc2.thread_scan_claims);
  for (round = 0; round < 64 &&
       lj_state_scan_epoch_acq(fail_L) != gc2_thread_scan_cycle_acq(g); round++)
    (void)lj_gc2_worker_drain(g, 64);
  assert(round < 64);
  assert(la_load64_acq(&g->gc2.thread_scan_claims) > claims0);
  assert(lj_state_owner_acq(fail_L) == 0);
  fail_L->tg_hint = tg;
  assert(g->gc2.n_threads <= n_threads0 + 1u);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(lj_tg_find_owner(g, extra_tg.tid) != &extra_tg);
  lj_tg_fini_thread(g, &extra_tg);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, base);
}

static int gc2_capi_collect_live_values(lua_State *L)
{
  global_State *g = G(L);
  GCtab *parent, *child;

  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);
  lua_pop(L, 1);

  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(lj_gc2_mem_registered_known(g, parent));
  assert(lj_gc2_mem_registered_known(g, child));
  assert(tabV(lj_tab_getint(parent, 1)) == child);
  return 1;
}

static void test_capi_collect_live_values(lua_State *L)
{
  lua_pushcfunction(L, gc2_capi_collect_live_values);
  lua_setglobal(L, "gc2_capi_collect_live_values");
  assert(luaL_dostring(L,
    "local t = gc2_capi_collect_live_values()\n"
    "assert(type(t) == 'table' and type(t[1]) == 'table')\n") == LUA_OK);
  lua_pushnil(L);
  lua_setglobal(L, "gc2_capi_collect_live_values");
}

#if LJ_HASFFI
static void push_raw_cdata(lua_State *L, GCcdata *cd)
{
  setcdataV(L, L->top++, cd);
}

static void test_pre_ctstate_cdata_edges(lua_State *L, global_State *g,
					 TGState *tg)
{
  GCtab *parent, *weak;
  GCcdata *array_cd, *hash_cd, *mark_cd, *sweep_cd;
  GCcdata *weak_key_cd, *weak_val_cd;
  int base = lua_gettop(L);
  int complete = 0, i;

  assert(ctype_ctsG(g) == NULL);

  /* Strong array and hash edges must work before lazy FFI initialization. */
  lua_newtable(L);
  parent = tabV(L->top - 1);
  array_cd = lj_cdata_new_(L, CTID_INT32, 4);
  push_raw_cdata(L, array_cd);
  lua_rawseti(L, -2, 1);
  hash_cd = lj_cdata_new_(L, CTID_INT32, 4);
  push_raw_cdata(L, hash_cd);
  lua_setfield(L, -2, "hash");
  lua_pushvalue(L, -1);
  lua_setfield(L, LUA_REGISTRYINDEX, "gc2_pre_ctstate_edges");

  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(ctype_ctsG(g) == NULL);
  assert(lj_gc2_mem_registered_known(g, array_cd));
  assert(lj_gc2_mem_registered_known(g, hash_cd));
  assert(cdataV(lj_tab_getint(parent, 1)) == array_cd);
  {
    GCstr *key = lj_str_newlit(L, "hash");
    assert(cdataV(lj_tab_getstr(parent, key)) == hash_cd);
  }

  /* Publish an old, unmarked child after the parent/root snapshot in MARK. */
  mark_cd = lj_cdata_new_(L, CTID_INT32, 4);
  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(mark_cd)) == 0);
  lj_gc2_test_scan_roots(g, L);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(mark_cd)) == 0);
  push_raw_cdata(L, mark_cd);
  lua_rawseti(L, -2, 2);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(mark_cd)) == 1);
  lj_gc2_cycle_to_idle(g);

  /* A SWEEP-time table publication must synchronously preserve the child. */
  sweep_cd = lj_cdata_new_(L, CTID_INT32, 4);
  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(sweep_cd)) == 0);
  lj_gc2_test_scan_roots(g, L);
  flush_and_drain(g, tg);
  lj_gc2_mark_to_weak(g);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  for (i = 0; i < 128 && !complete; i++)
    complete = lj_gc2_weak_complete(g, L, NULL,
				    LJ_GC2_WEAK_DRAIN_BATCH);
  assert(complete);
  lj_gc2_weak_to_sweep(g, L);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  push_raw_cdata(L, sweep_cd);
  lua_setfield(L, -2, "sweep");
  assert(lj_gc2_ismarked(g, obj2gco(sweep_cd)) == 1);
  lj_gc2_cycle_to_idle(g);

  /* Concurrent writes into an all-weak table survive the active weak pass. */
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "kv");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_pushvalue(L, -1);
  lua_setfield(L, LUA_REGISTRYINDEX, "gc2_pre_ctstate_weak");
  weak_key_cd = lj_cdata_new_(L, CTID_INT32, 4);
  weak_val_cd = lj_cdata_new_(L, CTID_INT32, 4);
  lj_gc2_mark_begin(g);
  lj_gc2_test_scan_roots(g, L);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(weak_key_cd)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(weak_val_cd)) == 0);
  lj_gc2_mark_to_weak(g);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  push_raw_cdata(L, weak_key_cd);
  push_raw_cdata(L, weak_val_cd);
  lua_settable(L, -3);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(weak_key_cd)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(weak_val_cd)) == 1);
  lj_gc2_cycle_to_idle(g);

  lua_pushnil(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "gc2_pre_ctstate_weak");
  lua_pushnil(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "gc2_pre_ctstate_edges");
  lua_settop(L, base);
}

static void test_post_ctstate_invalid_cdata_edge(lua_State *L,
						 global_State *g)
{
  GCcdata *cd;
  CTypeID oldid;
  TValue tv;

  assert(ctype_ctsG(g) != NULL);
  lua_getfield(L, LUA_REGISTRYINDEX, "gc2_minor_preclaim_cdata");
  assert(tviscdata(L->top - 1));
  cd = cdataV(L->top - 1);
  oldid = cd->ctypeid;
  setcdataV(L, &tv, cd);
  assert(lj_gc2_tv_gcref_valid_edge(g, &tv));
  cd->ctypeid = 0;  /* Invalid after CTState publication, even for a live cell. */
  assert(!lj_gc2_tv_gcref_valid_edge(g, &tv));
  assert(!lj_gc_tv_gcref_valid(g, &tv));
  cd->ctypeid = oldid;
  assert(lj_gc2_tv_gcref_valid_edge(g, &tv));
  lua_pop(L, 1);
}
#endif

static void test_minor_root_scan(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *registry_tab, *stack_tab;
#if LJ_HASFFI
  GCcdata *preclaim_cd;
#endif
  uint32_t generational0 = la_load32_acq(&g->gc2.generational);
  uint32_t sweep_gate0 = la_load32_acq(&g->gc2.minor_sweep_enabled);
  uint32_t roots_gate0 = la_load32_acq(&g->gc2.minor_roots_enabled);
  uint64_t major_roots0, minor_roots0;
  UNUSED(tg);

  lua_newtable(L);
  registry_tab = tabV(L->top - 1);
  {
    GCtab *reg = tabV(registry(L));
    TValue *slot = lj_tab_setstr(
      L, reg, lj_str_newlit(L, "gc2_minor_root_scan"));
    /* This fixture measures root-class policy, not the public table-store
    ** barrier. Install its registry-only edge raw while the value is rooted. */
    copyTVrel(L, slot, L->top - 1);
    lua_pop(L, 1);
  }
  lua_newtable(L);
  stack_tab = tabV(L->top - 1);
#if LJ_HASFFI
  preclaim_cd = lj_cdata_new_(L, CTID_INT32, 4);
  setcdataV(L, L->top++, preclaim_cd);
  /* Keep the raw cdata anchored until later FFI tests initialize CTState. */
  {
    GCtab *reg = tabV(registry(L));
    TValue *slot = lj_tab_setstr(
      L, reg, lj_str_newlit(L, "gc2_minor_preclaim_cdata"));
    copyTVrel(L, slot, L->top - 1);
  }
  lua_pushcfunction(L, gc2_cdata_counting_finalizer);
  assert(lj_gc2_test_finreg_cdata_preclaim(L, g, obj2gco(preclaim_cd),
				      L->top - 1));
  lua_pop(L, 1);
  lua_pop(L, 1);
#endif

  la_store32_rel(&g->gc2.generational, 0);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 1);
  la_store32_rel(&g->gc2.minor_roots_enabled, 1);
  lj_gc2_mark_begin(g);
  assert(la_load32_acq(&g->gc2.cycle_roots_minor) == 0);
  major_roots0 = gc2_major_root_scans_acq(g);
  minor_roots0 = gc2_minor_root_scans_acq(g);
  lj_gc2_test_scan_minor_roots(g, L);
  assert(gc2_major_root_scans_acq(g) == major_roots0);
  assert(gc2_minor_root_scans_acq(g) == minor_roots0);
  assert(lj_gc2_ismarked(g, obj2gco(stack_tab)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(registry_tab)) == 0);
#if LJ_HASFFI
  assert(lj_gc2_ismarked(g, obj2gco(preclaim_cd)) == 0);
#endif
  lj_gc2_cycle_to_idle(g);

  la_store32_rel(&g->gc2.generational, 1);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 1);
  la_store32_rel(&g->gc2.minor_roots_enabled, 1);
  lj_gc2_mark_begin(g);
  assert(la_load32_acq(&g->gc2.cycle_roots_minor) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(stack_tab)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(registry_tab)) == 0);
#if LJ_HASFFI
  assert(lj_gc2_ismarked(g, obj2gco(preclaim_cd)) == 0);
#endif
  major_roots0 = gc2_major_root_scans_acq(g);
  minor_roots0 = gc2_minor_root_scans_acq(g);
  assert(lj_gc2_handshake(g, LJ_GC2_HS_SCAN_ROOTS) == 1);
  assert(gc2_major_root_scans_acq(g) == major_roots0);
  assert(gc2_minor_root_scans_acq(g) > minor_roots0);
  assert(lj_gc2_ismarked(g, obj2gco(stack_tab)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(registry_tab)) == 0);
#if LJ_HASFFI
  assert(lj_gc2_ismarked(g, obj2gco(preclaim_cd)) == 1);
#endif
  la_store32_rel(&g->gc2.generational, generational0);
  la_store32_rel(&g->gc2.minor_sweep_enabled, sweep_gate0);
  la_store32_rel(&g->gc2.minor_roots_enabled, roots_gate0);
  lj_gc2_cycle_to_idle(g);
#if LJ_HASFFI
  {
    TValue fin;
    assert(lj_gc2_test_finreg_cdata_preclaim_take(L, g, obj2gco(preclaim_cd),
					     &fin));
    assert(tvisfunc(&fin));

    /*
    ** Exercise the first real GC2 sweep while CTState is still absent. The
    ** registry edge is authoritative even though full CType validation cannot
    ** yet resolve this fixed-size cdata. A dropped edge used to free the arena
    ** cell and leave the registry pointing at memory later reused by FINREG.
    */
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCSTOP, 0);
    assert(lj_gc2_mem_registered_known(g, preclaim_cd));
    lua_getfield(L, LUA_REGISTRYINDEX, "gc2_minor_preclaim_cdata");
    assert(tviscdata(L->top - 1));
    assert(cdataV(L->top - 1) == preclaim_cd);
    assert(cdataV(L->top - 1)->ctypeid == CTID_INT32);
    lua_pop(L, 1);
  }
#endif

  {
    GCtab *reg = tabV(registry(L));
    TValue *slot = lj_tab_setstr(
      L, reg, lj_str_newlit(L, "gc2_minor_root_scan"));
    lj_tab_storenilraw(slot);
  }
  lua_pop(L, 1);
}

static void test_thread(lua_State *L, global_State *g, TGState *tg)
{
  lua_State *th, *busy, *owner_L, *oldcur;
  TGState extra_tg;
  GCtab *stack_tab, *busy_tab, *late_tab;
  uint64_t claims0, busy0, requeues0, owner_scans0;
  uint64_t needscan0, owner_needscans0;
  uint32_t needscan_pending0;
  uint32_t n_threads0;
  uint32_t busy_owner = tg->tid + 5000u;
  if (busy_owner == 0 || busy_owner == LJ_THREAD_GCSCAN)
    busy_owner = 123u;

  th = lua_newthread(L);
  assert(th != NULL);
  lua_newtable(th);
  stack_tab = tabV(th->top - 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(stack_tab)) == 0);
  assert(lj_state_claim(th, LJ_THREAD_GCSCAN) == 0);
  assert(lj_state_owner_acq(th) == 0);
  claims0 = la_load64_acq(&g->gc2.thread_scan_claims);
  assert(lj_gc2_markobj(g, obj2gco(th)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarkedmem(g, tvref(th->stack)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(stack_tab)) == 1);
  assert(la_load64_acq(&g->gc2.thread_scan_claims) == claims0 + 1u);
  assert(lj_state_owner_acq(th) == 0);
  lj_gc2_cycle_to_idle(g);

  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  busy_tab = tabV(busy->top - 1);
  lj_state_owner_rel(busy, busy_owner);
  busy0 = la_load64_acq(&g->gc2.thread_scan_busy);
  requeues0 = la_load64_acq(&g->gc2.thread_scan_requeues);
  claims0 = la_load64_acq(&g->gc2.thread_scan_claims);
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(busy)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(lj_gc2_worker_drain(g, 2) == 2u);
  assert(la_load64_acq(&g->gc2.thread_scan_busy) == busy0 + 1u);
  /* A synthetic owner id has no live TG to acknowledge NEEDSCAN. Under the TG
  ** registry lease GC2 CAS-transfers that exact stale id to GCSCAN, scans the
  ** quiescent state, then releases it to the ordinary ownerless state.
  */
  assert(la_load64_acq(&g->gc2.thread_scan_requeues) == requeues0);
  assert(lj_state_owner_acq(busy) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 1);
  assert(la_load64_acq(&g->gc2.thread_scan_claims) == claims0 + 1u);
  lj_state_owner_rel(busy, 0);
  worker_drain_all(g);
  assert(lj_gc2_test_ssb_empty(g));
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);

  owner_L = lua_newthread(L);
  assert(owner_L != NULL);
  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  busy_tab = tabV(busy->top - 1);
#if defined(LJ_GC2_TEST_HELPERS)
  lua_newtable(busy);
  late_tab = tabV(busy->top - 1);
  busy->top--;
#endif
  lj_tg_init_thread(g, &extra_tg, owner_L, 1);
  extra_tg.tid = tg->tid + 7000u;
  if (extra_tg.tid == 0 || extra_tg.tid == LJ_THREAD_GCSCAN)
    extra_tg.tid = 7000u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  owner_L->tg_hint = &extra_tg;
  busy->tg_hint = &extra_tg;
  lj_state_owner_rel(owner_L, extra_tg.tid);
  lj_state_owner_rel(busy, extra_tg.tid);
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  /*
  ** The auxiliary TG is a lookup fixture for the owner handoff path, not a real
  ** runnable thread. Attach it after mark-begin's global barrier handshake so it
  ** can satisfy owner lookup without owning an unacknowledgeable safepoint slot.
  */
  n_threads0 = g->gc2.n_threads;
  lj_tg_attach(g, &extra_tg);
  assert(lj_tg_find_owner(g, extra_tg.tid) == &extra_tg);
  assert(g->gc2.n_threads >= n_threads0);
  busy0 = la_load64_acq(&g->gc2.thread_scan_busy);
  requeues0 = la_load64_acq(&g->gc2.thread_scan_requeues);
  owner_scans0 = la_load64_acq(&g->gc2.thread_scan_owner_scans);
  needscan0 = la_load64_acq(&g->gc2.thread_scan_needscan);
  owner_needscans0 = la_load64_acq(&g->gc2.thread_scan_owner_needscans);
  needscan_pending0 = la_load32_acq(&g->gc2.thread_scan_needscan_pending);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(busy)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(lj_gc2_worker_drain(g, 2) != 0);
  assert(la_load64_acq(&g->gc2.thread_scan_busy) == busy0 + 1u);
  assert(la_load64_acq(&g->gc2.thread_scan_requeues) == requeues0 + 1u);
  assert(la_load64_acq(&g->gc2.thread_scan_needscan) == needscan0 + 1u);
  assert(la_load32_acq(&g->gc2.thread_scan_needscan_pending) ==
			 needscan_pending0 + 1u);
  assert(lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);
  lj_gc2_test_scan_owned_needscan(g, owner_L);
  assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  assert(lj_state_scan_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 1);
  assert(la_load64_acq(&g->gc2.thread_scan_owner_needscans) ==
	 owner_needscans0 + 1u);
  assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  assert(la_load32_acq(&g->gc2.thread_scan_needscan_pending) ==
	 needscan_pending0);
#if defined(LJ_GC2_TEST_HELPERS)
  assert(lj_gc2_ismarked(g, obj2gco(late_tab)) == 0);
  /* Expose one fresh stack edge after the owner scan. This white-box fixture
  ** deliberately leaves it for the queued GC traversal below to discover. */
  settabV(busy, busy->top++, late_tab);

  /* Preserve the claimed state across its owner's registry retirement. A
  ** DEAD-but-still-registered TG remains authoritative, so the first replay
  ** retains the exact owner id. Once the registry body is physically absent,
  ** the already-queued state is provably stale and may transfer to GCSCAN. */
  lj_state_owner_rel(owner_L, 0);
  owner_L->tg_hint = tg;
  lj_tg_detach(g, &extra_tg);
  assert(lj_tg_flags_test_acq(&extra_tg, TGF_DEAD));
  assert(g->gc2.n_threads <= n_threads0 + 1u);
  assert(lj_tg_find_owner(g, extra_tg.tid) == &extra_tg);

  busy0 = la_load64_acq(&g->gc2.thread_scan_busy);
  requeues0 = la_load64_acq(&g->gc2.thread_scan_requeues);
  claims0 = la_load64_acq(&g->gc2.thread_scan_claims);
  lj_state_scan_epoch_rel(busy, 0);
  lj_gc2_test_scan_tg_thread_root(g, tg, busy);
  {
    uint32_t i;
    for (i = 0; i < 64u &&
	 la_load64_acq(&g->gc2.thread_scan_busy) == busy0; i++)
      (void)lj_gc2_worker_drain(g, 1);
  }
  assert(la_load64_acq(&g->gc2.thread_scan_busy) == busy0 + 1u);
  assert(la_load64_acq(&g->gc2.thread_scan_requeues) == requeues0);
  assert(la_load64_acq(&g->gc2.thread_scan_claims) == claims0);
  assert(lj_state_owner_acq(busy) == extra_tg.tid);
  assert(lj_gc2_ismarked(g, obj2gco(late_tab)) == 0);
  assert(!lj_gc2_test_ssb_empty(g));

  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(lj_tg_find_owner(g, extra_tg.tid) != &extra_tg);
  busy->tg_hint = tg;
  lj_tg_fini_thread(g, &extra_tg);
  lj_gc2_cycle_to_idle(g);
  assert(la_load64_acq(&g->gc2.thread_scan_busy) >= busy0 + 2u);
  assert(la_load64_acq(&g->gc2.thread_scan_requeues) == requeues0);
  assert(la_load64_acq(&g->gc2.thread_scan_claims) >= claims0 + 1u);
  assert(lj_state_owner_acq(busy) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(late_tab)) == 1);
#else
  lj_state_owner_rel(owner_L, 0);
  lj_state_owner_rel(busy, 0);
  owner_L->tg_hint = tg;
  busy->tg_hint = tg;
  lj_tg_detach(g, &extra_tg);
  assert(lj_tg_flags_test_acq(&extra_tg, TGF_DEAD));
  assert(g->gc2.n_threads <= n_threads0 + 1u);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(lj_tg_find_owner(g, extra_tg.tid) != &extra_tg);
  lj_tg_fini_thread(g, &extra_tg);
  (void)lj_gc2_flush_ssb(g, tg);
#endif
  worker_drain_all(g);
  assert(la_load64_acq(&g->gc2.thread_scan_owner_scans) == owner_scans0);
  assert(lj_gc2_test_ssb_empty(g));
#if !defined(LJ_GC2_TEST_HELPERS)
  lj_gc2_cycle_to_idle(g);
#endif
  lua_pop(L, 2);

  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  busy_tab = tabV(busy->top - 1);
  lua_newtable(busy);
  late_tab = tabV(busy->top - 1);
  busy->top--;
  oldcur = tg->cur_L;
  lj_state_owner_rel(busy, tg->tid);
  lj_tg_setcur_L(g, busy);
  busy0 = la_load64_acq(&g->gc2.thread_scan_busy);
  requeues0 = la_load64_acq(&g->gc2.thread_scan_requeues);
  owner_scans0 = la_load64_acq(&g->gc2.thread_scan_owner_scans);
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  assert(lj_state_scan_epoch_acq(busy) != gc2_thread_scan_cycle_acq(g));
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(busy)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(lj_gc2_worker_drain(g, 2) != 0);
  /*
  ** This fixture is running on the owning OS thread. Same-owner traversal scans
  ** directly; owner handoff is only needed when a different live TG owns the
  ** state.
  */
  assert(la_load64_acq(&g->gc2.thread_scan_busy) == busy0);
  assert(la_load64_acq(&g->gc2.thread_scan_requeues) == requeues0);
  assert(la_load64_acq(&g->gc2.thread_scan_owner_scans) == owner_scans0);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(late_tab)) == 0);
  lj_state_owner_rel(busy, 0);
  if (oldcur)
    lj_tg_setcur_L(g, oldcur);
  else
    lj_tg_clearcur_L(g);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);

  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  busy_tab = tabV(busy->top - 1);
  lua_newtable(busy);
  late_tab = tabV(busy->top - 1);
  busy->top--;
  oldcur = tg->cur_L;
  lj_state_owner_rel(busy, tg->tid);
  lj_tg_setcur_L(g, busy);
  busy0 = la_load64_acq(&g->gc2.thread_scan_busy);
  requeues0 = la_load64_acq(&g->gc2.thread_scan_requeues);
  owner_scans0 = la_load64_acq(&g->gc2.thread_scan_owner_scans);
  needscan0 = la_load64_acq(&g->gc2.thread_scan_needscan);
  needscan_pending0 = la_load32_acq(&g->gc2.thread_scan_needscan_pending);
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  lj_gc2_test_scan_roots(g, busy);
  assert(lj_state_scan_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert(lj_state_scan_dirty_epoch_acq(busy) ==
	 lj_tg_stack_dirty_epoch_acq(tg));
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(late_tab)) == 0);
  settabV(busy, busy->top++, late_tab);
  assert(lj_gc2_flush_ssb(g, tg) != 0);
  {
    int i;
    for (i = 0; i < 64 && !lj_gc2_ismarked(g, obj2gco(late_tab)); i++)
      (void)lj_gc2_worker_drain(g, 64);
  }
  assert(la_load64_acq(&g->gc2.thread_scan_busy) == busy0);
  assert(la_load64_acq(&g->gc2.thread_scan_requeues) == requeues0);
  assert(la_load64_acq(&g->gc2.thread_scan_owner_scans) == owner_scans0);
  assert(la_load64_acq(&g->gc2.thread_scan_needscan) == needscan0);
  assert(la_load32_acq(&g->gc2.thread_scan_needscan_pending) ==
	 needscan_pending0);
  assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(late_tab)) == 1);
  (void)lj_gc2_flush_ssb(g, tg);
  worker_drain_all(g);
  assert(la_load64_acq(&g->gc2.thread_scan_owner_scans) == owner_scans0);
  assert(lj_gc2_test_ssb_empty(g));
  lj_state_owner_rel(busy, 0);
  if (oldcur)
    lj_tg_setcur_L(g, oldcur);
  else
    lj_tg_clearcur_L(g);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);

  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  busy_tab = tabV(busy->top - 1);
  oldcur = tg->cur_L;
  lj_state_owner_rel(busy, tg->tid);
  lj_tg_setcur_L(g, busy);
  busy0 = la_load64_acq(&g->gc2.thread_scan_busy);
  requeues0 = la_load64_acq(&g->gc2.thread_scan_requeues);
  owner_scans0 = la_load64_acq(&g->gc2.thread_scan_owner_scans);
  needscan0 = la_load64_acq(&g->gc2.thread_scan_needscan);
  needscan_pending0 = la_load32_acq(&g->gc2.thread_scan_needscan_pending);
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  assert(lj_gc2_markobj(g, obj2gco(busy)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) != 0);
  assert(lj_gc2_worker_drain(g, 2) != 0);
  assert(la_load64_acq(&g->gc2.thread_scan_busy) == busy0);
  assert(la_load64_acq(&g->gc2.thread_scan_requeues) == requeues0);
  assert(la_load64_acq(&g->gc2.thread_scan_owner_scans) == owner_scans0);
  assert(la_load64_acq(&g->gc2.thread_scan_needscan) == needscan0);
  assert(la_load32_acq(&g->gc2.thread_scan_needscan_pending) ==
	 needscan_pending0);
  lj_gc2_test_scan_roots(g, busy);
  assert(lj_state_scan_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 1);
  assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  (void)lj_gc2_flush_ssb(g, tg);
  worker_drain_all(g);
  assert(la_load64_acq(&g->gc2.thread_scan_owner_scans) == owner_scans0);
  assert(lj_gc2_test_ssb_empty(g));
  lj_state_owner_rel(busy, 0);
  if (oldcur)
    lj_tg_setcur_L(g, oldcur);
  else
    lj_tg_clearcur_L(g);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);
  lua_pop(L, 1);
}

#if defined(LJ_GC2_TEST_HELPERS)
typedef struct TableRescanSetCtx {
  global_State *g;
  GCtab *t;
  int installed;
} TableRescanSetCtx;

typedef struct ExpectedMarkTransitionCtx {
  GCArena *arena;
  uint32_t cell;
} ExpectedMarkTransitionCtx;

typedef struct TableTokenScanCtx {
  global_State *g;
  GCtab *t;
  int completed;
} TableTokenScanCtx;

typedef struct TableTopologyChangeCtx {
  LJGC2TableTopology *topology;
  uint32_t changes;
} TableTopologyChangeCtx;

typedef struct TableTokenPassCtx {
  global_State *g;
  uint32_t budget;
  uint32_t turns;
  uint32_t consumed;
  int result;
} TableTokenPassCtx;

static void *table_rescan_set_thread(void *arg)
{
  TableRescanSetCtx *ctx = (TableRescanSetCtx *)arg;
  ctx->installed = lj_gc2_test_table_rescan_set(ctx->g, ctx->t);
  return NULL;
}

static void *table_token_scan_thread(void *arg)
{
  TableTokenScanCtx *ctx = (TableTokenScanCtx *)arg;
  ctx->completed = lj_gc2_test_table_token_scan_one(ctx->g, ctx->t);
  return NULL;
}

static void *table_topology_change_thread(void *arg)
{
  TableTopologyChangeCtx *ctx = (TableTopologyChangeCtx *)arg;
  uint32_t i;
  for (i = 0; i < ctx->changes; i++)
    assert(lj_gc2_table_topology_changed(ctx->topology) ==
	   LJ_GC2_TABLE_TOPOLOGY_OK);
  return NULL;
}

static void *table_token_pass_thread(void *arg)
{
  TableTokenPassCtx *ctx = (TableTokenPassCtx *)arg;
  ctx->consumed = 0;
  ctx->result = LJ_GC2_TABLE_TOKEN_PASS_PROGRESS;
  for (ctx->turns = 0;
       ctx->turns < 4u &&
	 ctx->result == LJ_GC2_TABLE_TOKEN_PASS_PROGRESS;
       ctx->turns++) {
    uint32_t consumed = 0;
    ctx->result = lj_gc2_test_table_token_pass_step(
      ctx->g, ctx->budget, &consumed);
    assert(consumed <= ctx->budget);
    ctx->consumed += consumed;
  }
  return NULL;
}

static void test_table_topology_primitive(void)
{
  enum { CHANGE_THREADS = 2, CHANGES_PER_THREAD = 4096 };
  LJGC2TableTopology topology;
  LJGC2TableTopologySnap before, after;
  TableTopologyChangeCtx ctx[CHANGE_THREADS];
  pthread_t threads[CHANGE_THREADS];
  uint32_t i;

  assert(lj_gc2_table_topology_init_unpublished(&topology, 1));
  before = lj_gc2_table_topology_snapshot(&topology);
  assert(lj_gc2_table_topology_open(&before) && before.epoch == 1u);
  assert(lj_gc2_table_topology_changed(&topology) ==
	 LJ_GC2_TABLE_TOPOLOGY_OK);
  after = lj_gc2_table_topology_snapshot(&topology);
  assert(lj_gc2_table_topology_open(&after) && after.epoch == 2u);

  for (i = 0; i < CHANGE_THREADS; i++) {
    ctx[i].topology = &topology;
    ctx[i].changes = CHANGES_PER_THREAD;
    assert(pthread_create(&threads[i], NULL, table_topology_change_thread,
			  &ctx[i]) == 0);
  }
  for (i = 0; i < CHANGE_THREADS; i++)
    assert(pthread_join(threads[i], NULL) == 0);
  after = lj_gc2_table_topology_snapshot(&topology);
  assert(lj_gc2_table_topology_open(&after));
  assert(after.epoch == 2u + CHANGE_THREADS * CHANGES_PER_THREAD);

  assert(lj_gc2_table_topology_init_unpublished(&topology, UINT64_MAX));
  assert(lj_gc2_table_topology_changed(&topology) ==
	 LJ_GC2_TABLE_TOPOLOGY_PINNED_RESULT);
  after = lj_gc2_table_topology_snapshot(&topology);
  assert(after.valid && after.pinned && after.epoch == UINT64_MAX);
  assert(lj_gc2_table_topology_changed(&topology) ==
	 LJ_GC2_TABLE_TOPOLOGY_PINNED_RESULT);
  before = lj_gc2_table_topology_snapshot(&topology);
  assert(lj_gc2_table_topology_equal(&after, &before));
}

static LJGC2TabStamp *table_token_test_stamp(GCtab *t)
{
  LJGC2TabStamp *stamp = lj_arena_gc2_stamp_acq(t);
  assert(stamp != NULL);
  return stamp;
}

static void test_table_authority_saturation(void)
{
  lua_State *L;
  global_State *g;
  TGState *tg;
  GCtab *t;
  LJGC2TabStamp *stamp;
  LJGC2ActivationSnap activation;
  uint64_t state;

  /* Dirty saturation must invalidate the covered scan cycle without wrapping
  ** the persistent per-cell identity back to one. Use a private universe
  ** because NO_RECLAIM is intentionally absorbing. */
  L = luaL_newstate();
  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L);
  lua_newtable(L);
  t = tabV(L->top - 1);
  stamp = table_token_test_stamp(t);
  la_store64_rel(&stamp->state,
		 ((uint64_t)UINT32_C(17) << 32) | (UINT32_MAX - 1u));
  lj_gc2_test_table_dirty_bump(g, t);
  state = la_load64_acq(&stamp->state);
  assert((uint32_t)state == UINT32_MAX);
  assert((uint32_t)(state >> 32) == 0u);
  activation = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(activation.state == LJ_GC2_ACT_IDLE);
  lj_gc2_test_table_dirty_bump(g, t);
  state = la_load64_acq(&stamp->state);
  assert((uint32_t)state == UINT32_MAX);
  assert((uint32_t)(state >> 32) == 0u);
  activation = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(activation.state == LJ_GC2_ACT_NO_RECLAIM);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lua_close(L);

  /* Cycle saturation is rejected before either typed or legacy MARK becomes
  ** visible. The exact request is consumed, and later requests remain bounded
  ** no-ops under the absorbing reclaim veto. */
  L = luaL_newstate();
  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  gc2_cycle_store_rlx(g, UINT32_MAX);
  assert(lj_gc2_request_cycle_explicit(g, tg));
  lj_gc2_mark_begin(g);
  activation = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(activation.state == LJ_GC2_ACT_NO_RECLAIM);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_cycle_acq(g) == UINT32_MAX);
  assert(gc2_cycle_leader_acq(g) == 0u);
  assert(!lj_gc2_request_cycle_explicit(g, tg));
  assert(gc2_cycle_leader_acq(g) == 0u);
  lua_close(L);
}

static uint64_t table_token_test_request_next(global_State *g, GCtab *t)
{
  LJGC2TabStamp *stamp = table_token_test_stamp(t);
  uint64_t generation = lj_gc2_test_table_token_request(g, t);
  assert(generation > 0 && generation <= LJ_GC2_TABLE_TOKEN_MAX_GENERATION);
  assert(lj_gc2_table_token_generation(
	 la_load64_acq(&stamp->token.control)) == generation);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) ==
	 LJ_GC2_TABLE_TOKEN_PENDING);
  return generation;
}

static void table_token_test_wait_paused(uint32_t count)
{
  uint32_t spin;
  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_gc2_test_table_token_paused() == count)
      return;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"table token scanner hook did not pause");
}

static void *expected_mark_transition_thread(void *arg)
{
  ExpectedMarkTransitionCtx *ctx = (ExpectedMarkTransitionCtx *)arg;
  uint32_t spin;
  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_gc2_test_queue_post_admit_paused())
      break;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(spin < 1000000u);
  assert(lj_arena_lifetime_state_cas(
    ctx->arena, ctx->cell, LJ_ARENA_LIFETIME_LIVE,
    LJ_ARENA_LIFETIME_DESTRUCT));
  lj_gc2_test_queue_post_admit_release();
  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_gc2_test_queue_retry_witness_paused())
      break;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(spin < 1000000u);
  assert(lj_arena_lifetime_state_cas(
    ctx->arena, ctx->cell, LJ_ARENA_LIFETIME_DESTRUCT,
    LJ_ARENA_LIFETIME_LIVE));
  lj_gc2_test_queue_retry_witness_release();
  return NULL;
}

static void *table_rescan_stale_hint_clear_thread(void *arg)
{
  TableRescanSetCtx *ctx = (TableRescanSetCtx *)arg;
  lj_gc2_test_table_rescan_stale_hint_clear(ctx->g, ctx->t);
  return NULL;
}

static void *queue_post_admit_ssb_drain_thread(void *arg)
{
  WorkerDrainCtx *ctx = (WorkerDrainCtx *)arg;
  ctx->drained = lj_gc2_test_ssb_drain(ctx->g);
  return NULL;
}

static void test_queue_post_admit_wait_paused(void)
{
  uint32_t spin;
  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_gc2_test_queue_post_admit_paused())
      return;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"queue post-admission hook did not pause");
}

static void test_queue_retry_witness_wait_paused(void)
{
  uint32_t spin;
  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_gc2_test_queue_retry_witness_paused())
      return;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"queue retry-witness hook did not pause");
}

static void test_table_rescan_wait_paused(uint32_t stage)
{
  uint32_t spin;
  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_gc2_test_table_rescan_paused() == stage)
      return;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"table rescan hook did not pause");
}

static void test_table_rescan_exact_membership(lua_State *L, global_State *g)
{
  TableRescanSetCtx ctx;
  GCtab *counted, *stale_hint;
  pthread_t publisher;
  uint32_t pending0 = gc2_table_rescan_pending_acq(g);
  int base = lua_gettop(L);

  assert(pending0 == 0);
  lua_newtable(L);
  counted = tabV(L->top - 1);
  lua_newtable(L);
  stale_hint = tabV(L->top - 1);
  assert(lj_tab_gc2_rescan_state_acq(counted) == LJ_TAB_RESCAN_NONE);
  assert(lj_tab_gc2_rescan_state_acq(stale_hint) == LJ_TAB_RESCAN_NONE);

  /* A header-only stale hint must never steal another table's exact credit. */
  assert(lj_gc2_test_table_rescan_set(g, counted));
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  lj_obj_addgcflags_atomic(obj2gco(stale_hint), LJ_GC_NEEDSCAN);
  assert(lj_gc2_test_table_rescan_clear(g, stale_hint) == 1);
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  assert(lj_tab_gc2_rescan_state_acq(counted) == LJ_TAB_RESCAN_COUNTED);
  assert(lj_gc2_test_table_rescan_clear(g, counted) == 1);
  assert(gc2_table_rescan_pending_acq(g) == pending0);

  /* A consumer meets the provisional installer after exact table identity and
  ** its advisory hint are visible but before settlement. CANCELLED leaves the
  ** one aggregate credit with the publisher, which settles it exactly. The
  ** earlier count-before-identity progress gap is documented separately and
  ** requires the helpable descriptor/token replacement. */
  lj_gc2_test_table_rescan_pause(LJ_GC2_TABLE_RESCAN_TEST_INSTALLING);
  ctx.g = g;
  ctx.t = counted;
  ctx.installed = 0;
  assert(pthread_create(&publisher, NULL, table_rescan_set_thread, &ctx) == 0);
  test_table_rescan_wait_paused(LJ_GC2_TABLE_RESCAN_TEST_INSTALLING);
  assert(lj_tab_gc2_rescan_state_acq(counted) ==
	 LJ_TAB_RESCAN_INSTALLING);
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  assert(lj_obj_gcflags(obj2gco(counted)) & LJ_GC_NEEDSCAN);
  assert(lj_gc2_test_table_rescan_clear(g, counted) == 0);
  assert(lj_tab_gc2_rescan_state_acq(counted) ==
	 LJ_TAB_RESCAN_CANCELLED);
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  lj_gc2_test_table_rescan_release();
  assert(pthread_join(publisher, NULL) == 0);
  assert(ctx.installed == 1);
  assert(lj_tab_gc2_rescan_state_acq(counted) == LJ_TAB_RESCAN_NONE);
  assert(gc2_table_rescan_pending_acq(g) == pending0);
  assert((lj_obj_gcflags(obj2gco(counted)) & LJ_GC_NEEDSCAN) == 0);

  assert(lj_gc2_test_table_rescan_set(g, counted));
  assert(lj_tab_gc2_rescan_state_acq(counted) == LJ_TAB_RESCAN_COUNTED);
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  assert(lj_gc2_test_table_rescan_clear(g, counted) == 1);
  assert(gc2_table_rescan_pending_acq(g) == pending0);
  lua_settop(L, base);
}

static void test_table_rescan_ssb_exact_gate(lua_State *L, global_State *g,
					      TGState *tg)
{
  TableRescanSetCtx ctx;
  GCtab *t;
  GCobj *o;
  pthread_t clearer;
  uint32_t pending0;
  int base = lua_gettop(L);

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  flush_and_drain(g, tg);
  pending0 = gc2_table_rescan_pending_acq(g);
  assert(pending0 == 0);

  lua_newtable(L);
  t = tabV(L->top - 1);
  o = obj2gco(t);
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  assert(lj_gc2_markobj(g, o));
  (void)lj_gc2_flush_ssb(g, tg);
  worker_drain_all(g);
  assert(lj_gc2_test_table_scan_current(g, t));

  /* Establish and retire an earlier membership generation, then reinstall an
  ** exact token and its SSB locator. A delayed old hint-clear may erase the
  ** header bit during the new generation's COUNTED lifetime, but it must not
  ** make the SSB locator look stale while the exact token remains installed. */
  assert(lj_gc2_test_table_rescan_set(g, t));
  assert(lj_gc2_test_table_rescan_clear(g, t) == 1);
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_NONE);
  assert(lj_gc2_test_table_rescan_set(g, t));
  assert(lj_gc2_test_ssb_push(g, o));
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_COUNTED);
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);

  lj_gc2_test_table_rescan_pause(LJ_GC2_TABLE_RESCAN_TEST_HINT_CLEARED);
  ctx.g = g;
  ctx.t = t;
  ctx.installed = 0;
  assert(pthread_create(&clearer, NULL, table_rescan_stale_hint_clear_thread,
			&ctx) == 0);
  test_table_rescan_wait_paused(LJ_GC2_TABLE_RESCAN_TEST_HINT_CLEARED);
  assert((lj_obj_gcflags(o) & LJ_GC_NEEDSCAN) == 0);
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_COUNTED);
  assert(lj_gc2_test_table_scan_current(g, t));

  /* The old header+stamp-only shortcut consumed this sole locator and left
  ** COUNTED orphaned. Exact-state classification sends it through grey, whose
  ** admitted traversal discharges both the token and the aggregate credit. */
  assert(lj_gc2_test_ssb_drain(g) != 0);
  assert(lj_gc2_test_ssb_empty(g));
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_NONE);
  assert(gc2_table_rescan_pending_acq(g) == pending0);

  lj_gc2_test_table_rescan_release();
  assert(pthread_join(clearer, NULL) == 0);
  assert((lj_obj_gcflags(o) & LJ_GC_NEEDSCAN) == 0);
  settle_automatic_cycle(g);
  lua_settop(L, base);
}

static void test_table_rescan_queue_admission_retry(lua_State *L,
						     global_State *g,
						     TGState *tg)
{
  ExpectedMarkTransitionCtx expected_ctx;
  WorkerDrainCtx ctx;
  GCtab *t, *child;
  GCArena *a;
  GCobj *o;
  pthread_t drainer;
  uint32_t cell, pending0, i;
  int base = lua_gettop(L);

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  flush_and_drain(g, tg);
  pending0 = gc2_table_rescan_pending_acq(g);
  assert(pending0 == 0);

  /* Expected-type admission uses the same mark transition, but returns a
  ** semantic tri-state rather than owning the original SSB/grey locator. The
  ** parent mark wins before DESTRUCT, then final validation reports RETRY and
  ** the owner restores LIVE before classification. Exact retry discharge must
  ** still schedule the already-marked parent's child graph. */
  lua_newtable(L);
  t = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_pushliteral(L, "expected retry child");
  lua_pushvalue(L, -2);
  lua_rawset(L, base + 1);
  lua_pop(L, 1);  /* Only the parent remains a Lua stack root. */
  o = obj2gco(t);
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  expected_ctx.arena = a;
  expected_ctx.cell = cell;
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  assert(lj_gc2_ismarked(g, o) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  lj_gc2_test_queue_post_admit_pause(o);
  lj_gc2_test_queue_retry_witness_pause(o);
  assert(pthread_create(&drainer, NULL, expected_mark_transition_thread,
			&expected_ctx) == 0);
  assert(lj_gc2_test_table_expected_status(g, t) ==
	 LJ_GC2_TV_EDGE_RETRY);
  assert(pthread_join(drainer, NULL) == 0);
  assert(lj_gc2_flush_ssb(g, tg) != 0);
  worker_drain_all(g);
  assert(lj_gc2_ismarked(g, obj2gco(child)) > 0);
  settle_automatic_cycle(g);
  lua_pop(L, 1);

  /* SSB conversion must retain its owned slot when exact table admission is
  ** transient. Consuming it would orphan the COUNTED close veto. */
  lua_newtable(L);
  t = tabV(L->top - 1);
  o = obj2gco(t);
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  assert(lj_gc2_test_table_rescan_set(g, t));
  assert(lj_gc2_test_ssb_push(g, o));
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_MUTATING));
  assert(lj_gc2_test_ssb_drain(g) == 0);
  assert(!lj_gc2_test_ssb_empty(g));
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_COUNTED);
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_MUTATING,
				     LJ_ARENA_LIFETIME_LIVE));
  assert(lj_gc2_test_ssb_drain(g) != 0);
  assert(lj_gc2_test_ssb_empty(g));
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_NONE);
  assert(gc2_table_rescan_pending_acq(g) == pending0);
  settle_automatic_cycle(g);
  lua_pop(L, 1);

  /* A grey owner-pop has already removed its locator. RETRY must republish it
  ** before yielding, and the bounded worker turn must not spin on that item. */
  lua_newtable(L);
  t = tabV(L->top - 1);
  o = obj2gco(t);
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  assert(lj_gc2_test_table_rescan_set(g, t));
  assert(lj_gc2_test_grey_push(g, o));
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_RECOVERY));
  assert(lj_gc2_worker_drain(g, 1) == 0);
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_COUNTED);
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  assert(lj_gc2_test_grey_steal(g) == o);
  assert(lj_gc2_test_grey_steal(g) == NULL);
  assert(lj_gc2_test_grey_push(g, o));
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_RECOVERY,
				     LJ_ARENA_LIFETIME_LIVE));
  for (i = 0; i < 32u &&
	 lj_tab_gc2_rescan_state_acq(t) != LJ_TAB_RESCAN_NONE; i++)
    (void)lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH);
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_NONE);
  assert(gc2_table_rescan_pending_acq(g) == pending0);
  settle_automatic_cycle(g);
  lua_pop(L, 1);

  /* Validation can succeed while the counted admission is held and still
  ** lose the semantic lifetime immediately before the mark transition. The
  ** SSB consumer must classify that late DEAD as RETRY and leave its exact
  ** published slot (and COUNTED table token) owned. */
  lua_newtable(L);
  t = tabV(L->top - 1);
  o = obj2gco(t);
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  assert(lj_gc2_test_table_rescan_set(g, t));
  assert(lj_gc2_test_ssb_push(g, o));
  assert(lj_gc2_flush_ssb(g, tg) != 0);
  ctx.g = g;
  ctx.limit = 0;
  ctx.drained = ~(uint32_t)0;
  lj_gc2_test_queue_post_admit_pause(o);
  lj_gc2_test_queue_retry_witness_pause(o);
  assert(pthread_create(&drainer, NULL, queue_post_admit_ssb_drain_thread,
			&ctx) == 0);
  test_queue_post_admit_wait_paused();
  assert(lj_arena_remote_active_acq(a) != 0);
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_DESTRUCT));
  lj_gc2_test_queue_post_admit_release();
  test_queue_retry_witness_wait_paused();
  /* The rejecting acquire, rather than a later reload, is the retry witness.
  ** Restore the cancellable owner before the queue caller classifies DEAD. */
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_DESTRUCT,
				     LJ_ARENA_LIFETIME_LIVE));
  lj_gc2_test_queue_retry_witness_release();
  assert(pthread_join(drainer, NULL) == 0);
  assert(ctx.drained == 0);
  assert(!lj_gc2_test_ssb_empty(g));
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_COUNTED);
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  assert(lj_gc2_test_ssb_drain(g) != 0);
  assert(lj_gc2_test_ssb_empty(g));
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_NONE);
  assert(gc2_table_rescan_pending_acq(g) == pending0);
  settle_automatic_cycle(g);
  lua_pop(L, 1);

  /* A grey owner has already popped its sole locator at the same pause. Its
  ** RETRY path must republish that exact pointer before yielding the quantum;
  ** after LIVE is restored, a later drain consumes the token normally. */
  lua_newtable(L);
  t = tabV(L->top - 1);
  o = obj2gco(t);
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  assert(lj_gc2_test_table_rescan_set(g, t));
  assert(lj_gc2_test_grey_push(g, o));
  ctx.g = g;
  ctx.limit = 1;
  ctx.drained = ~(uint32_t)0;
  lj_gc2_test_queue_post_admit_pause(o);
  lj_gc2_test_queue_retry_witness_pause(o);
  assert(pthread_create(&drainer, NULL, grey_worker_drain_thread, &ctx) == 0);
  test_queue_post_admit_wait_paused();
  assert(lj_arena_remote_active_acq(a) != 0);
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_DESTRUCT));
  lj_gc2_test_queue_post_admit_release();
  test_queue_retry_witness_wait_paused();
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_DESTRUCT,
				     LJ_ARENA_LIFETIME_LIVE));
  lj_gc2_test_queue_retry_witness_release();
  assert(pthread_join(drainer, NULL) == 0);
  assert(ctx.drained == 0);
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_COUNTED);
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  assert(lj_gc2_test_grey_steal(g) == o);
  assert(lj_gc2_test_grey_steal(g) == NULL);
  assert(lj_gc2_test_grey_push(g, o));
  for (i = 0; i < 32u &&
	 lj_tab_gc2_rescan_state_acq(t) != LJ_TAB_RESCAN_NONE; i++)
    (void)lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH);
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_NONE);
  assert(gc2_table_rescan_pending_acq(g) == pending0);
  settle_automatic_cycle(g);
  lua_settop(L, base);
}

static void test_table_token_small_live_exact(lua_State *L, global_State *g,
					       TGState *tg)
{
  GCtab *parent, *child;
  LJGC2TabStamp *stamp;
  uint64_t grey0, pending0;
  int base = lua_gettop(L);

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, base + 1, "token-child");
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  lj_gc2_mark_begin(g);
  pending0 = gc2_table_rescan_pending_acq(g);
  grey0 = gc2_grey_pushed_acq(g);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  (void)table_token_test_request_next(g, parent);
  stamp = table_token_test_stamp(parent);
  /* The dormant request helper publishes no SSB/grey/legacy membership. */
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_grey_pushed_acq(g) == grey0);
  assert(gc2_table_rescan_pending_acq(g) == pending0);
  assert(lj_gc2_test_table_token_scan_one(g, parent) == 1);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_NONE);
  assert(lj_gc2_test_table_scan_current(g, parent));
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(gc2_table_rescan_pending_acq(g) == pending0);
  settle_automatic_cycle(g);
  lua_settop(L, base);
  UNUSED(tg);
}

static void test_table_token_small_cursor_budget(lua_State *L,
						 global_State *g)
{
  HugeTab *registry;
  GCArena *want;
  GCtab *t = NULL, *child;
  LJGC2TabStamp *stamp;
  uint64_t maxsteps = 0, steps;
  uint32_t cap, slot, target_cell, done = 0;
  int base = lua_gettop(L), parent_index = 0;
  int attempt;

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  /* Avoid the degenerate cursor origin: a same-kind second allocation is past
  ** the first arena cell unless the preceding attempt crossed an arena end. */
  for (attempt = 0; attempt < 8; attempt++) {
    lua_newtable(L);
    t = tabV(L->top - 1);
    if (lj_arena_cellof(t) > LJ_AFIRST_CELL) {
      parent_index = lua_gettop(L);
      break;
    }
  }
  assert(t != NULL && parent_index != 0);
  target_cell = lj_arena_cellof(t);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, parent_index, "cursor-child");

  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  (void)table_token_test_request_next(g, t);
  stamp = table_token_test_stamp(t);
  registry = (HugeTab *)gc2_small_arena_tab_acq(g);
  want = lj_arena_of(t);
  assert(registry != NULL);
  cap = lj_arena_hugetab_slot_count(registry);
  assert(cap != 0);

  /* Derive a finite no-churn bound from the public physical snapshots. Empty
  ** slots cost one identity; every present mapping costs one turn per cell. */
  for (slot = 0; slot < cap; slot++) {
    void *p = NULL;
    LJHugeInfo hi;
    int snap = lj_arena_hugetab_slot_snapshot_bounded(
      registry, slot, &p, &hi);
    if (snap == LJ_ARENA_HUGETAB_SLOT_PRESENT) {
      assert(hi.size == LJ_ARENA_SIZE && p != NULL);
      if (p == (void *)want) {
        maxsteps += (uint64_t)(target_cell - LJ_AFIRST_CELL) + 1u;
        break;
      }
      maxsteps += (uint64_t)(LJ_ARENA_CELLS - LJ_AFIRST_CELL);
    } else {
      maxsteps++;
    }
  }
  assert(slot < cap && maxsteps > 1u);
  lj_gc2_test_table_token_cursor_reset(g);
  for (steps = 0; steps < maxsteps; steps++) {
    done = lj_gc2_test_table_token_scan_small(g, 1);
    if (done != 0)
      break;
    assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_PENDING);
  }
  assert(done == 1u && steps > 0 && steps < maxsteps);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_NONE);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  settle_automatic_cycle(g);
  lua_settop(L, base);
}

static void test_table_token_request_observational_stale(lua_State *L,
						  global_State *g)
{
  GCtab *parent, *child;
  LJGC2TabStamp *stamp;
  uint64_t control0, generation;
  int base = lua_gettop(L);

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, base + 1, "stale-child");
  stamp = table_token_test_stamp(parent);
  control0 = la_load64_acq(&stamp->token.control);

  /* IDLE cannot manufacture work which this tranche's scanner refuses. */
  assert(!lj_gc2_test_table_token_request(g, parent));
  assert(la_load64_acq(&stamp->token.control) == control0);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  generation = lj_gc2_test_table_token_request(g, parent);
  assert(generation != 0);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_test_table_token_scan_one(g, parent) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  settle_automatic_cycle(g);

  /* A fresh request in a later cycle comes from the one global descriptor
  ** namespace and therefore advances beyond the completed generation. */
  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  control0 = la_load64_acq(&stamp->token.control);
  assert(lj_gc2_table_token_generation(control0) == generation);
  assert(lj_gc2_table_token_state(control0) == LJ_GC2_TABLE_TOKEN_NONE);
  generation = lj_gc2_test_table_token_request(g, parent);
  assert(generation > lj_gc2_table_token_generation(control0));
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_PENDING);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_test_table_token_scan_one(g, parent) == 1);
  settle_automatic_cycle(g);
  lua_settop(L, base);
}

static GCtab *table_token_huge_new(global_State *g, TGState *tg,
				    size_t size, GCtab *mt)
{
  GCArena *a;
  GCtab *t;
  void *p;

  assert(g != NULL && tg != NULL && size > LJ_HUGE_THRESHOLD);
  assert(lj_tg_flags_all_acq(tg, TGF_ARENA_INTERNAL|TGF_HUGETAB));
  p = lj_arena_huge_map(&tg->prng, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  t = (GCtab *)p;
  memset(t, 0, sizeof(*t));
  la_store8_rel(&t->gct, (uint8_t)~LJ_TTAB);
  newwhite(g, t);
  lj_tab_nomm_rel(t, (uint8_t)~0u);
  lj_tab_colo_rel(t, 0);
  lj_tab_gc2_rescan_state_store_rlx(t, LJ_TAB_RESCAN_NONE);
  lj_tab_array_set(t, NULL);
  if (mt)
    lj_tab_metatable_rel(t, mt);
  lj_tab_asize_rel(t, 0);
  lj_tab_acap_rel(t, 0);
  lj_tab_hmask_rel(t, 0);
  lj_tab_node_rel(t, &g->nilnode);
  lj_tab_freetop_rel(t, &g->nilnode);
  lj_tab_struct_owner_store_rlx(t, 0);
  lj_tab_weak_cycle_store_rlx(t, 0);

  a = lj_arena_of(t);
  lj_arena_owner_rel(a, lj_tg_tid_acq(tg));
  lj_arena_gc2_tabledesc_rel(a, &g->gc2.table_rescan_desc);
  lj_arena_progress_g_rel(a, g);
  assert(lj_arena_hugetab_insert(
    &tg->huge, t, size, LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY) == 1);
  return t;
}

static uint32_t table_token_huge_physical_slot(HugeTab *ht,
						const void *target)
{
  uint32_t cap = lj_arena_hugetab_slot_count(ht), slot;
  for (slot = 0; slot < cap; slot++) {
    void *p = NULL;
    LJHugeInfo hi;
    int snap = lj_arena_hugetab_slot_snapshot_bounded(ht, slot, &p, &hi);
    assert(snap != LJ_ARENA_HUGETAB_SLOT_BUSY);
    if (snap == LJ_ARENA_HUGETAB_SLOT_PRESENT && p == target)
      return slot;
  }
  assert(!"huge table fixture lacks a physical slot");
  return 0;
}

static void table_token_huge_delete(TGState *tg, GCtab *t, size_t size)
{
  LJHugeInfo hi;
  assert(lj_arena_hugetab_delete(&tg->huge, t, &hi) == 1);
  assert(hi.size == size && hi.readers == 0u);
  assert(lj_arena_hugetab_lookup(&tg->huge, t, NULL) == 0);
  lj_arena_huge_unmap(t, hi.size);
}

static void table_token_current_cycle_to_weak(lua_State *L,
					       global_State *g, TGState *tg)
{
  lj_gc2_test_scan_roots(g, L);
  flush_and_drain(g, tg);
  lj_gc2_mark_to_weak(g);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
}

static uint64_t table_token_pass_budget_one_limit(global_State *g)
{
  HugeTab *small = (HugeTab *)gc2_small_arena_tab_acq(g);
  LJTGRegistrySlot *node;
  uint64_t units = 0;
  uint32_t cap, slot;

  assert(small != NULL);
  cap = lj_arena_hugetab_slot_count(small);
  assert(cap != 0);
  for (slot = 0; slot < cap; slot++) {
    LJHugeInfo hi;
    void *p = NULL;
    int snap = lj_arena_hugetab_slot_snapshot_bounded(
      small, slot, &p, &hi);
    if (snap == LJ_ARENA_HUGETAB_SLOT_PRESENT)
      units += LJ_ARENA_CELLS - LJ_AFIRST_CELL;
    else
      units++;
  }
  for (node = gc2_tg_registry_head_acq(g); node != NULL;
       node = lj_tgregistry_slot_next_all(node)) {
    LJTGRegistryBodySnap body = lj_tgregistry_slot_body_snapshot(node);
    TGState *tg = (TGState *)body.body;
    uint32_t huge_cap = 0;
    if (tg && lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      huge_cap = lj_arena_hugetab_slot_count(&tg->huge);
    units += huge_cap != 0 ? huge_cap : 1u;
  }
  return units + 16u;  /* Lane handoffs plus a narrow diagnostic margin. */
}

static void test_table_token_pass_certificate(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtab *t;
  LJGC2TabStamp *stamp;
  LJGC2TableDescTicket pre_ticket, post_ticket;
  LJGC2TableDescSnap observed;
  TableTokenPassCtx pass;
  pthread_t scanner;
  uint64_t completed0, hint0, limit, turn;
  uint32_t consumed, incomplete;
  int result = LJ_GC2_TABLE_TOKEN_PASS_PROGRESS;

  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL && gc2_phase_acq(g) == LJ_GC2_IDLE);
  lua_newtable(L);
  t = tabV(L->top - 1);
  stamp = table_token_test_stamp(t);
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);

  /* An incomplete stable TG spine is an unconditional start/ack veto. */
  incomplete = gc2_tg_registry_incomplete_acq(g);
  assert(incomplete == 0u);
  gc2_tg_registry_incomplete_store_rlx(g, 1);
  consumed = UINT32_MAX;
  assert(lj_gc2_test_table_token_pass_step(g, 1, &consumed) ==
	 LJ_GC2_TABLE_TOKEN_PASS_RETRY);
  assert(consumed == 0u);
  assert(!lj_gc2_test_table_token_pass_ack_current(g));
  gc2_tg_registry_incomplete_store_rlx(g, incomplete);

  /* Freeze a clean pass immediately before its final double validation. A
  ** descriptor publisher stopped before token transfer must veto the ack. */
  memset(&pass, 0, sizeof(pass));
  pass.g = g;
  pass.budget = UINT32_MAX;
  lj_gc2_test_table_token_pause(LJ_GC2_TABLE_TOKEN_TEST_PRE_ACK);
  assert(pthread_create(&scanner, NULL, table_token_pass_thread, &pass) == 0);
  table_token_test_wait_paused(1);
  assert(lj_gc2_tabledesc_try_publish(
	 &g->gc2.table_rescan_desc, t, &pre_ticket, &observed) ==
	 LJ_GC2_TABLEDESC_RESULT_OK);
  assert(lj_gc2_tabledesc_snapshot(
	 &g->gc2.table_rescan_desc).state == LJ_GC2_TABLEDESC_ACTIVE);
  lj_gc2_test_table_token_release();
  assert(pthread_join(scanner, NULL) == 0);
  assert(pass.result == LJ_GC2_TABLE_TOKEN_PASS_RETRY);
  assert(!lj_gc2_test_table_token_pass_ack_current(g));
  assert(lj_gc2_tabledesc_finish_help(
	 &g->gc2.table_rescan_desc, &pre_ticket, &observed) ==
	 LJ_GC2_TABLEDESC_RESULT_OK);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_NONE);

  /* Repeat at the publisher's post-transfer pause: PENDING is durable while
  ** ACTIVE(D) still vetoes the old pass. The next clean pass must discover
  ** that token without consulting the sticky requested-generation hint. */
  memset(&pass, 0, sizeof(pass));
  pass.g = g;
  pass.budget = UINT32_MAX;
  lj_gc2_test_table_token_pause(LJ_GC2_TABLE_TOKEN_TEST_PRE_ACK);
  assert(pthread_create(&scanner, NULL, table_token_pass_thread, &pass) == 0);
  table_token_test_wait_paused(1);
  assert(lj_gc2_tabledesc_try_publish(
	 &g->gc2.table_rescan_desc, t, &post_ticket, &observed) ==
	 LJ_GC2_TABLEDESC_RESULT_OK);
  assert(post_ticket.generation > pre_ticket.generation);
  assert(lj_gc2_table_token_transfer_exact(
	 &stamp->token, post_ticket.generation) ==
	 LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_PENDING);
  lj_gc2_test_table_token_release();
  assert(pthread_join(scanner, NULL) == 0);
  assert(pass.result == LJ_GC2_TABLE_TOKEN_PASS_RETRY);
  assert(!lj_gc2_test_table_token_pass_ack_current(g));
  assert(lj_gc2_tabledesc_finish_help(
	 &g->gc2.table_rescan_desc, &post_ticket, &observed) ==
	 LJ_GC2_TABLEDESC_RESULT_OK);

  hint0 = gc2_table_token_scan_requested_acq(g);
  gc2_table_token_scan_requested_store_rlx(g, UINT64_MAX);
  completed0 = gc2_table_token_scan_completed_acq(g);
  limit = table_token_pass_budget_one_limit(g);
  for (turn = 0; turn < limit; turn++) {
    consumed = UINT32_MAX;
    result = lj_gc2_test_table_token_pass_step(g, 1, &consumed);
    assert(consumed <= 1u);
    assert(result == LJ_GC2_TABLE_TOKEN_PASS_PROGRESS ||
	   result == LJ_GC2_TABLE_TOKEN_PASS_ACKED);
    if (result == LJ_GC2_TABLE_TOKEN_PASS_ACKED)
      break;
  }
  assert(result == LJ_GC2_TABLE_TOKEN_PASS_ACKED && turn < limit);
  assert(gc2_table_token_scan_completed_acq(g) == completed0 + 1u);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_NONE);
  assert(lj_gc2_test_table_token_pass_ack_current(g));
  assert(gc2_table_token_scan_requested_acq(g) == UINT64_MAX);
  gc2_table_token_scan_requested_store_rlx(g, hint0);

  /* The certificate is paired with one exact phase/activation authority. */
  table_token_current_cycle_to_weak(L, g, tg);
  assert(!lj_gc2_test_table_token_pass_ack_current(g));
  settle_automatic_cycle(g);
  lua_close(L);
}

static void table_token_current_cycle_to_sweep(lua_State *L,
						global_State *g, TGState *tg)
{
  uint32_t i;
  int complete = 0;
  table_token_current_cycle_to_weak(L, g, tg);
  for (i = 0; i < 4096u && !complete; i++)
    complete = lj_gc2_weak_complete(g, L, NULL,
				    LJ_GC2_WEAK_DRAIN_BATCH);
  assert(complete);
  lj_gc2_weak_to_sweep(g, L);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
}

static void test_table_token_huge_live_exact(lua_State *L, global_State *g,
					      TGState *tg)
{
  const size_t size = LJ_HUGE_THRESHOLD + 4096u;
  LJGC2TabStamp *stamp;
  GCtab *t, *child;
  uint64_t control, generation, hint, payload0, visited0;
  uint32_t incomplete, slot;
  int base = lua_gettop(L);

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lua_newtable(L);
  child = tabV(L->top - 1);
  t = table_token_huge_new(g, tg, size, child);
  stamp = table_token_test_stamp(t);
  control = la_load64_acq(&stamp->token.control);
  generation = 0;

  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  incomplete = gc2_tg_registry_incomplete_acq(g);
  assert(incomplete == 0u);
  gc2_tg_registry_incomplete_store_rlx(g, 1);
  assert(!lj_gc2_test_table_token_request(g, t));
  assert(la_load64_acq(&stamp->token.control) == control);
  gc2_tg_registry_incomplete_store_rlx(g, incomplete);

  generation = lj_gc2_test_table_token_request(g, t);
  assert(generation != 0);
  payload0 = gc2_table_token_scan_payloads_acq(g);
  assert(lj_gc2_test_table_token_scan_one(g, t) == 1);
  assert(gc2_table_token_scan_payloads_acq(g) == payload0 + 1u);
  assert(lj_gc2_test_table_scan_current(g, t));
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_NONE);

  /* A durable token is independently enumerable if its publisher stops before
  ** raising the sticky wake hint. Place the cursor on the exact stable TG and
  ** physical slot so one budget unit is both necessary and sufficient. */
  hint = gc2_table_token_scan_requested_acq(g);
  assert(hint != 0u && generation < LJ_GC2_TABLE_TOKEN_MAX_GENERATION);
  gc2_table_token_scan_requested_store_rlx(g, 0);
  generation++;
  assert(lj_gc2_table_token_transfer_exact(&stamp->token, generation) ==
	 LJ_GC2_TABLE_TOKEN_RESULT_OK);
  slot = table_token_huge_physical_slot(&tg->huge, t);
  gc2_table_token_huge_incarnation_store_rlx(
    g, tg->registry_key.incarnation);
  gc2_table_token_huge_slot_store_rlx(g, slot);
  gc2_table_token_huge_node_store_rlx(g, tg->registry_key.slot);
  visited0 = gc2_table_token_scan_visited_acq(g);
  payload0 = gc2_table_token_scan_payloads_acq(g);
  assert(lj_gc2_test_table_token_scan_huge(g, 1) == 1u);
  assert(gc2_table_token_scan_visited_acq(g) == visited0 + 1u);
  assert(gc2_table_token_scan_payloads_acq(g) == payload0 + 1u);
  assert(gc2_table_token_scan_requested_acq(g) == 0u);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_NONE);
  gc2_table_token_scan_requested_store_rlx(g, hint);

  table_token_huge_delete(tg, t, size);
  settle_automatic_cycle(g);
  lua_settop(L, base);
}

static void test_table_token_huge_phase_behavior(lua_State *L,
						  global_State *g,
						  TGState *tg)
{
  const uint32_t phases[] = { LJ_GC2_MARK, LJ_GC2_WEAK, LJ_GC2_SWEEP };
  const size_t size = LJ_HUGE_THRESHOLD + 8192u;
  uint32_t n;
  int base = lua_gettop(L);

  for (n = 0; n < sizeof(phases)/sizeof(phases[0]); n++) {
    LJGC2TabStamp *stamp;
    LJHugeInfo hi;
    GCtab *t = table_token_huge_new(g, tg, size + n, NULL);
    uint64_t payload0, terminal0;
    uint8_t gct;

    assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
    stamp = table_token_test_stamp(t);
    lj_gc2_mark_begin(g);
    (void)table_token_test_request_next(g, t);
    if (phases[n] == LJ_GC2_WEAK)
      table_token_current_cycle_to_weak(L, g, tg);
    else if (phases[n] == LJ_GC2_SWEEP)
      table_token_current_cycle_to_sweep(L, g, tg);
    assert(gc2_phase_acq(g) == phases[n]);

    /* MARK was covered above. WEAK retains full body authority; SWEEP must
    ** leave a LIVE graph pending without sampling any payload byte. */
    if (phases[n] == LJ_GC2_WEAK) {
      payload0 = gc2_table_token_scan_payloads_acq(g);
      assert(lj_gc2_test_table_token_scan_one(g, t) == 1);
      assert(gc2_table_token_scan_payloads_acq(g) == payload0 + 1u);
      (void)table_token_test_request_next(g, t);
    } else if (phases[n] == LJ_GC2_SWEEP) {
      gct = la_load8_acq(&t->gct);
      la_store8_rel(&t->gct, 0);
      payload0 = gc2_table_token_scan_payloads_acq(g);
      assert(lj_gc2_test_table_token_scan_one(g, t) == 0);
      assert(gc2_table_token_scan_payloads_acq(g) == payload0);
      assert(lj_gc2_table_token_state(
	la_load64_acq(&stamp->token.control)) ==
	LJ_GC2_TABLE_TOKEN_PENDING);
      la_store8_rel(&t->gct, gct);
    }

    /* DEFER_FREE is irreversible logical death in every admitted phase. Its
    ** exact terminal completion is header-only, including with a poisoned GC
    ** type byte, and the last token lease performs the FREEING handoff. */
    gct = la_load8_acq(&t->gct);
    la_store8_rel(&t->gct, 0);
    assert(!lj_arena_hugetab_claim_external_free(&tg->huge, t, &hi));
    assert((hi.flags & LJ_HUGEF_DEFER_FREE) != 0 && hi.readers == 0u);
    payload0 = gc2_table_token_scan_payloads_acq(g);
    terminal0 = gc2_table_token_scan_terminal_acq(g);
    assert(lj_gc2_test_table_token_scan_one(g, t) == 1);
    assert(gc2_table_token_scan_payloads_acq(g) == payload0);
    assert(gc2_table_token_scan_terminal_acq(g) == terminal0 + 1u);
    assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_NONE);
    assert(lj_arena_hugetab_lookup(&tg->huge, t, &hi) == 1);
    assert((hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD)) ==
	   (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD));
    assert((hi.flags & LJ_HUGEF_DEFER_FREE) == 0 && hi.readers == 0u);
    table_token_huge_delete(tg, t, size + n);
    settle_automatic_cycle(g);
  }
  lua_settop(L, base);
}

static void test_table_token_huge_reclaiming_owner(global_State *g,
						     TGState *main_tg)
{
  const size_t size = LJ_HUGE_THRESHOLD + 12289u;
  LJTGRegistryBodySnap body;
  LJTGRegistryKey key;
  LJTGSlotSnap snap;
  LJGC2TabStamp *stamp;
  TGState dead;
  GCtab *t;
  uint64_t payload0;
  uint32_t physical_slot;

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(main_tg == g->main_tg);
  lj_tg_init_thread(g, &dead, NULL, 1);
  lj_tg_tid_rel(&dead, lj_thr_newid());
  t = table_token_huge_new(g, &dead, size, NULL);
  stamp = table_token_test_stamp(t);
  physical_slot = table_token_huge_physical_slot(&dead.huge, t);

  /* Begin the cycle before publishing the synthetic TG so attach catch-up can
  ** acknowledge the active phase like a real late mutator. */
  lj_gc2_mark_begin(g);
  lj_tg_attach(g, &dead);
  key = dead.registry_key;
  assert(lj_tgregistry_key_valid(&key));
  (void)table_token_test_request_next(g, t);
  lj_tg_detach(g, &dead);
  assert(lj_tg_flags_test_acq(&dead, TGF_DEAD));

  /* Stable admission closes first. Transfer cannot erase the only HugeTab
  ** locator while its embedded token is pending, so legacy raw-list ownership
  ** and the exact RECLAIMING body remain available to the scanner. */
  assert(lj_tg_reclaim_dead(g) == 0u);
  assert(lj_tgregistry_key_snapshot(&key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_RECLAIMING && snap.lease_count == 0u);
  body = lj_tgregistry_slot_body_snapshot(key.slot);
  assert(body.body == &dead && body.incarnation == key.incarnation);
  assert(lj_arena_hugetab_lookup(&dead.huge, t, NULL) == 1);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_PENDING);

  gc2_table_token_huge_incarnation_store_rlx(g, key.incarnation);
  gc2_table_token_huge_slot_store_rlx(g, physical_slot);
  gc2_table_token_huge_node_store_rlx(g, key.slot);
  payload0 = gc2_table_token_scan_payloads_acq(g);
  assert(lj_gc2_test_table_token_scan_huge(g, 1) == 1u);
  assert(gc2_table_token_scan_payloads_acq(g) == payload0 + 1u);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_NONE);
  assert(lj_tgregistry_key_snapshot(&key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_RECLAIMING && snap.lease_count == 0u);
  body = lj_tgregistry_slot_body_snapshot(key.slot);
  assert(body.body == &dead && body.incarnation == key.incarnation);

  /* With exact token ownership discharged, the ordinary reclaimer transfers
  ** the mapping to main, unlinks the dead raw TG and clears its tagged slot. */
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(lj_tgregistry_key_snapshot(&key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_EMPTY && snap.lease_count == 0u);
  body = lj_tgregistry_slot_body_snapshot(key.slot);
  assert(body.body == NULL && body.incarnation == key.incarnation);
  assert(lj_arena_hugetab_lookup(&dead.huge, t, NULL) == 0);
  assert(lj_arena_hugetab_lookup(&main_tg->huge, t, NULL) == 1);
  table_token_huge_delete(main_tg, t, size);
  assert(lj_tg_fini_thread(g, &dead));
  lj_gc2_test_table_token_cursor_reset(g);
  settle_automatic_cycle(g);
}

static void test_table_token_small_free_no_body(lua_State *L,
						 global_State *g)
{
  GCtab *t;
  GCobj *o;
  GCArena *a;
  LJGC2TabStamp *stamp;
  uint64_t payload0, terminal0;
  uint32_t cell;
  uint8_t gct;
  int base = lua_gettop(L);

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lua_newtable(L);
  t = tabV(L->top - 1);
  o = obj2gco(t);
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  lj_gc2_mark_begin(g);
  (void)table_token_test_request_next(g, t);
  stamp = table_token_test_stamp(t);
  payload0 = gc2_table_token_scan_payloads_acq(g);
  terminal0 = gc2_table_token_scan_terminal_acq(g);
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_DESTRUCT));
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_DESTRUCT,
				     LJ_ARENA_LIFETIME_FREE));
  gct = la_load8_acq(&o->gch.gct);
  la_store8_rel(&o->gch.gct, 0);  /* Any body/header read must reject. */
  assert(lj_gc2_test_table_token_scan_one(g, t) == 1);
  assert(gc2_table_token_scan_payloads_acq(g) == payload0);
  assert(gc2_table_token_scan_terminal_acq(g) == terminal0 + 1u);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_NONE);
  la_store8_rel(&o->gch.gct, gct);
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_FREE,
				     LJ_ARENA_LIFETIME_LIVE));
  settle_automatic_cycle(g);
  lua_settop(L, base);
}

static void test_table_token_small_proof_races(lua_State *L,
						global_State *g)
{
  TableTokenScanCtx ctx;
  pthread_t scanner;
  GCtab *t;
  LJGC2TabStamp *stamp;
  uint64_t generation, completed0;
  int base = lua_gettop(L);

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lua_newtable(L);
  t = tabV(L->top - 1);
  lj_gc2_mark_begin(g);
  generation = table_token_test_request_next(g, t);
  stamp = table_token_test_stamp(t);
  ctx.g = g;
  ctx.t = t;
  ctx.completed = -1;

  /* Dirty publication before the proof CAS makes exact traversal RETRY and
  ** leaves the captured generation PENDING. */
  lj_gc2_test_table_token_pause(LJ_GC2_TABLE_TOKEN_TEST_PRE_PROOF);
  assert(pthread_create(&scanner, NULL, table_token_scan_thread, &ctx) == 0);
  table_token_test_wait_paused(1);
  /* worker_active is the scanner-owner LP: a peer scanner cannot enter the
  ** same proof or complete the captured generation concurrently. */
  assert(lj_gc2_test_table_token_scan_one(g, t) == 0);
  lj_gc2_test_table_dirty_bump(g, t);
  lj_gc2_test_table_token_release();
  assert(pthread_join(scanner, NULL) == 0);
  assert(ctx.completed == 0);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_PENDING);
  assert(lj_gc2_test_table_token_scan_one(g, t) == 1);

  /* Refresh after a stable proof invalidates only the old completion ticket.
  ** The newer exact generation remains discoverable for the next scanner. */
  generation = lj_gc2_test_table_token_request(g, t);
  assert(generation != 0);
  completed0 = gc2_table_token_scan_completed_acq(g);
  ctx.completed = -1;
  lj_gc2_test_table_token_pause(LJ_GC2_TABLE_TOKEN_TEST_POST_PROOF);
  assert(pthread_create(&scanner, NULL, table_token_scan_thread, &ctx) == 0);
  table_token_test_wait_paused(1);
  {
    uint64_t newer = lj_gc2_test_table_token_request(g, t);
    assert(newer > generation);
    generation = newer;
  }
  lj_gc2_test_table_token_release();
  assert(pthread_join(scanner, NULL) == 0);
  assert(ctx.completed == 0);
  assert(gc2_table_token_scan_completed_acq(g) == completed0);
  assert(lj_gc2_table_token_generation(
	 la_load64_acq(&stamp->token.control)) == generation);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&stamp->token.control)) == LJ_GC2_TABLE_TOKEN_PENDING);
  assert(lj_gc2_test_table_token_scan_one(g, t) == 1);
  settle_automatic_cycle(g);
  lua_settop(L, base);
}

static void test_table_token_small_weak_oom_progress(lua_State *L,
						      global_State *g)
{
  GCtab *first, *first_key, *first_val;
  GCtab *later, *later_key, *later_val;
  LJGC2TabStamp *first_stamp, *later_stamp;
  MSize cap;
  int base = lua_gettop(L);

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  make_weak_table(L, "v", &first, &first_key, &first_val);
  make_weak_table(L, "v", &later, &later_key, &later_val);
  lj_gc2_mark_begin(g);
  (void)table_token_test_request_next(g, first);
  (void)table_token_test_request_next(g, later);
  first_stamp = table_token_test_stamp(first);
  later_stamp = table_token_test_stamp(later);
  cap = gc2_weak_capacity_acq(g);
  assert(cap > 0);

  /* Remove vector capacity for one exact attempt and fail its raw overflow
  ** node. Telemetry is not identity: first remains PENDING. Restoring capacity
  ** lets a later token complete before the transient first token. The
  ** head-nonnull B-overflow close matrix remains future live-cutover work. */
  gc2_weak_capacity_store_rlx(g, 0);
  lj_gc2_test_weak_overflow_fail_alloc(1);
  assert(lj_gc2_test_table_token_scan_one(g, first) == 0);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&first_stamp->token.control)) ==
	 LJ_GC2_TABLE_TOKEN_PENDING);
  assert(gc2_weak_overflow_acq(g) == NULL);
  gc2_weak_capacity_store_rlx(g, cap);
  assert(lj_gc2_test_table_token_scan_one(g, later) == 1);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&later_stamp->token.control)) ==
	 LJ_GC2_TABLE_TOKEN_NONE);
  assert(lj_gc2_table_token_state(
	 la_load64_acq(&first_stamp->token.control)) ==
	 LJ_GC2_TABLE_TOKEN_PENDING);
  assert(lj_gc2_test_table_token_scan_one(g, first) == 1);
  lj_gc2_test_weak_overflow_fail_alloc(0);
  settle_automatic_cycle(g);
  lua_settop(L, base);
  UNUSED(first_key); UNUSED(first_val);
  UNUSED(later_key); UNUSED(later_val);
}

static void test_legacy_weak_oom_requeues(lua_State *L, global_State *g,
					  TGState *tg)
{
  GCtab *weak, *key, *val;
  GCobj *o;
  MSize cap;
  uint64_t defer0, defer1;
  uint32_t pending0, i;
  int base = lua_gettop(L);

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  make_weak_table(L, "v", &weak, &key, &val);
  o = obj2gco(weak);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  pending0 = gc2_table_rescan_pending_acq(g);
  assert(pending0 == 0);
  cap = gc2_weak_capacity_acq(g);
  assert(cap > 0);
  assert(lj_gc2_test_table_rescan_set(g, weak));
  assert(lj_gc2_test_grey_push(g, o));

  /* Exercise the production legacy traversal, not the exact-token wrapper.
  ** Failed overflow allocation must retain exact COUNTED membership and
  ** republish a concrete traversal identity before this bounded worker turn
  ** returns. */
  gc2_weak_capacity_store_rlx(g, 0);
  lj_gc2_test_weak_overflow_fail_alloc(4096u);
  assert(lj_gc2_worker_drain(g, 1) == 1u);
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  assert(lj_tab_gc2_rescan_state_acq(weak) == LJ_TAB_RESCAN_COUNTED);
  assert(gc2_weak_overflow_acq(g) == NULL);
  assert(!weak_snapshot_has(g, weak));
  assert(table_retry_locator_visible(g, o));

  lj_gc2_mark_to_weak(g);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  assert(!lj_gc2_weak_complete(g, L, NULL, LJ_GC2_WEAK_DRAIN_BATCH));
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  assert(gc2_weak_mark_closed_acq(g) == 0);

  /* Persistent failure is a scheduling outcome, not unbounded work. Both
  ** public drivers yield after one durable retry quantum even with a very
  ** large explicit budget, preserving membership and a concrete locator. */
  defer0 = gc2_deferred_epoch_acq(g);
  assert(lj_gc2_collect_active(L) == 0);
  defer1 = gc2_deferred_epoch_acq(g);
  assert(defer1 > defer0 && defer1 - defer0 <= 32u);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  assert(lj_tab_gc2_rescan_state_acq(weak) == LJ_TAB_RESCAN_COUNTED);
  assert(table_retry_locator_visible(g, o));

  defer0 = gc2_deferred_epoch_acq(g);
  assert(lj_gc2_step_explicit(L, 1u << 20) == 0);
  defer1 = gc2_deferred_epoch_acq(g);
  assert(defer1 > defer0 && defer1 - defer0 <= 32u);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  assert(lj_tab_gc2_rescan_state_acq(weak) == LJ_TAB_RESCAN_COUNTED);
  assert(table_retry_locator_visible(g, o));

  /* Keep the fault armed across the forced all-work drain. The pre-fix path
  ** consumed its own SSB retry thousands of times in this one call. A prompt
  ** abort-to-IDLE now stops after the already-requeued outcome and preserves
  ** both exact membership and a concrete locator for the next MARK. */
  lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  assert(lj_tab_gc2_rescan_state_acq(weak) == LJ_TAB_RESCAN_COUNTED);
  assert(table_retry_locator_visible(g, o));

  /* Once weak identity storage is available, the republished legacy item
  ** survives into a new cycle, completes normally, and releases the exact
  ** aggregate close veto. */
  lj_gc2_test_weak_overflow_fail_alloc(0);
  gc2_weak_capacity_store_rlx(g, cap);
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  for (i = 0; i < 64u &&
	 gc2_table_rescan_pending_acq(g) != pending0; i++)
    (void)lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH);
  assert(gc2_table_rescan_pending_acq(g) == pending0);
  assert(lj_tab_gc2_rescan_state_acq(weak) == LJ_TAB_RESCAN_NONE);
  assert(weak_snapshot_has(g, weak));
  settle_automatic_cycle(g);
  lua_settop(L, base);
  UNUSED(tg); UNUSED(key); UNUSED(val);
}

static void test_legacy_weak_oom_recovery_quantum(lua_State *L,
						   global_State *g,
						   TGState *tg)
{
  GCtab *weak, *key, *val;
  GCobj *o;
  GCRef *next0, *end0;
  MSize cap;
  uint64_t items0, defer0;
  uint32_t pending0, i;
  int base = lua_gettop(L);

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  make_weak_table(L, "v", &weak, &key, &val);
  o = obj2gco(weak);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  cap = gc2_weak_capacity_acq(g);
  assert(cap > 0);
  items0 = gc2_recovery_items_acq(g);
  pending0 = gc2_table_rescan_pending_acq(g);
  assert(lj_gc2_test_recovery_publish(g, o));
  assert(lj_gc2_test_recovery_state(g, o) == LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(g) == items0 + 1u);

  /* Remove only this logical TG's SSB publication lane. The failed weak-node
  ** retry must REDIRTY its already-CLAIMED recovery identity, so an UINT_MAX
  ** recovery drain performs one scheduling attempt and promptly yields with
  ** the exact count/state still PENDING. */
  next0 = lj_tg_ssb_next_acq(tg);
  end0 = lj_tg_ssb_end_acq(tg);
  assert(next0 != NULL && end0 != NULL && next0 <= end0);
  lj_tg_ssb_next_rel(tg, NULL);
  lj_tg_ssb_end_rel(tg, NULL);
  gc2_weak_capacity_store_rlx(g, 0);
  lj_gc2_test_weak_overflow_fail_alloc(4096u);
  defer0 = gc2_deferred_epoch_acq(g);
  assert(lj_gc2_test_recovery_drain(g, ~(uint32_t)0) == 1u);
  assert(gc2_deferred_epoch_acq(g) > defer0);
  assert(lj_gc2_test_recovery_state(g, o) == LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(g) == items0 + 1u);
  assert(gc2_table_rescan_pending_acq(g) == pending0 + 1u);
  assert(lj_tab_gc2_rescan_state_acq(weak) == LJ_TAB_RESCAN_COUNTED);

  lj_tg_ssb_end_rel(tg, end0);
  lj_tg_ssb_next_rel(tg, next0);
  lj_gc2_test_weak_overflow_fail_alloc(0);
  gc2_weak_capacity_store_rlx(g, cap);
  assert(lj_gc2_test_recovery_drain(g, ~(uint32_t)0) == 1u);
  assert(lj_gc2_test_recovery_state(g, o) == LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(g) == items0);
  /* The recovered weak table is complete. Its newly marked metatable may own
  ** an independent table token/grey item, so test per-object completion first
  ** and then drain that ordinary child frontier to the aggregate baseline. */
  assert(lj_tab_gc2_rescan_state_acq(weak) == LJ_TAB_RESCAN_NONE);
  assert(weak_snapshot_has(g, weak));
  for (i = 0; i < 64u &&
	 gc2_table_rescan_pending_acq(g) != pending0; i++)
    (void)lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH);
  assert(gc2_table_rescan_pending_acq(g) == pending0);
  settle_automatic_cycle(g);
  lua_settop(L, base);
  UNUSED(key); UNUSED(val);
}

static void test_weak_overflow_headless_reservation_guard(lua_State *L,
						   global_State *g)
{
  GCtab *weak, *key, *val;
  MSize cap, i;
  int base = lua_gettop(L);

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  make_weak_table(L, "v", &weak, &key, &val);
  lj_gc2_mark_begin(g);
  cap = gc2_weak_capacity_acq(g);
  assert(cap > 0 && gc2_weak_stack_acq(g) != NULL &&
	 gc2_weak_ready_acq(g) != NULL && gc2_weak_overflow_acq(g) == NULL);
  /* Model a completely published vector followed by one reserved overflow
  ** whose raw node allocation failed. This directly exercises reserved>cap
  ** with no head, independent of the cap==0 traversal fallback. */
  for (i = 0; i < cap; i++) {
    setgcrefrel(gc2_weak_stack_acq(g)[i], obj2gco(weak));
    la_store8_rel(&gc2_weak_ready_acq(g)[i], 1);
  }
  gc2_weak_count_store_rlx(g, (uint64_t)cap + 1u);
  lj_gc2_mark_to_weak(g);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  assert(!lj_gc2_test_weak_overflow_clear_bridge(g, NULL));
  lj_gc2_cycle_to_idle(g);  /* Next MARK reset discards this synthetic gap. */
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lua_settop(L, base);
  UNUSED(key); UNUSED(val);
}

static void test_weak_overflow_full_oom_count_bounded(lua_State *L,
						       global_State *g)
{
  GCtab *weak, *key, *val;
  GC2WeakOverflow *node;
  MSize cap, i;
  uint32_t attempt;
  int base = lua_gettop(L);

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  make_weak_table(L, "v", &weak, &key, &val);
  lj_gc2_mark_begin(g);
  cap = gc2_weak_capacity_acq(g);
  assert(cap > 0 && gc2_weak_stack_acq(g) != NULL &&
	 gc2_weak_ready_acq(g) != NULL && gc2_weak_overflow_acq(g) == NULL);
  for (i = 0; i < cap; i++) {
    setgcrefrel(gc2_weak_stack_acq(g)[i], obj2gco(weak));
    la_store8_rel(&gc2_weak_ready_acq(g)[i], 1);
  }
  gc2_weak_count_store_rlx(g, (uint64_t)cap);

  /* Real cap-full record attempts reserve beyond the contiguous vector, then
  ** roll that aggregate reservation back if the raw overflow node cannot be
  ** published. Persistent OOM therefore cannot amplify next-cycle sizing. */
  lj_gc2_test_weak_overflow_fail_alloc(64u);
  for (attempt = 0; attempt < 64u; attempt++) {
    assert(!lj_gc2_test_weak_record(g, weak));
    assert(gc2_weak_count_acq(g) == (uint64_t)cap);
    assert(gc2_weak_overflow_acq(g) == NULL);
  }

  lj_gc2_test_weak_overflow_fail_alloc(0);
  assert(lj_gc2_test_weak_record(g, weak));
  assert(gc2_weak_count_acq(g) == (uint64_t)cap + 1u);
  node = gc2_weak_overflow_acq(g);
  assert(node != NULL && lj_gc2_test_weak_overflow_singleton(g, weak));

  lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  /* The next MARK reset consumes the successful raw overflow identity. */
  lj_gc2_mark_begin(g);
  assert(gc2_weak_overflow_acq(g) == NULL);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, base);
  UNUSED(key); UNUSED(val);
}

static void test_weak_record_publication_protocol(lua_State *L,
						  global_State *g)
{
  GCtab *weak, *key, *val;
  uint64_t prior, expect, installing, published, count0;
  MSize cap;
  uint32_t cycle, next_cycle;
  int base = lua_gettop(L);

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  make_weak_table(L, "v", &weak, &key, &val);
  lj_gc2_mark_begin(g);
  cycle = gc2_cycle_acq(g);
  assert(cycle != 0);
  prior = lj_tab_weak_record_acq(weak);
  installing = lj_tab_weak_record_pack(
    cycle, LJ_TAB_WEAK_RECORD_INSTALLING);
  count0 = gc2_weak_count_acq(g);

  /* A duplicate may not mistake the claim CAS for durable publication. */
  expect = prior;
  assert(lj_tab_weak_record_cas(weak, &expect, installing));
  assert(!lj_gc2_test_weak_record(g, weak));
  assert(lj_tab_weak_record_acq(weak) == installing);
  assert(gc2_weak_count_acq(g) == count0);
  expect = installing;
  assert(lj_tab_weak_record_cas(weak, &expect, prior));
  installing = lj_tab_weak_record_pack(
    cycle ^ UINT32_C(0x80000000), LJ_TAB_WEAK_RECORD_INSTALLING);
  expect = prior;
  assert(lj_tab_weak_record_cas(weak, &expect, installing));
  assert(!lj_gc2_test_weak_record(g, weak));
  assert(lj_tab_weak_record_acq(weak) == installing);
  expect = installing;
  assert(lj_tab_weak_record_cas(weak, &expect, prior));
  installing = lj_tab_weak_record_pack(
    cycle, LJ_TAB_WEAK_RECORD_INSTALLING);

  /* Failed publication restores the exact prior cycle/state only after its
  ** aggregate reservation has been repaired. A retry then publishes once. */
  cap = gc2_weak_capacity_acq(g);
  assert(cap > 0);
  gc2_weak_capacity_store_rlx(g, 0);
  lj_gc2_test_weak_overflow_fail_alloc(1);
  assert(!lj_gc2_test_weak_record(g, weak));
  assert(lj_tab_weak_record_acq(weak) == prior);
  assert(gc2_weak_count_acq(g) == count0);
  lj_gc2_test_weak_overflow_fail_alloc(0);
  gc2_weak_capacity_store_rlx(g, cap);

  assert(lj_gc2_test_weak_record(g, weak));
  published = lj_tab_weak_record_pack(
    cycle, LJ_TAB_WEAK_RECORD_PUBLISHED);
  assert(lj_tab_weak_record_acq(weak) == published);
  assert(gc2_weak_count_acq(g) == count0 + 1u);
  assert(lj_gc2_test_weak_record(g, weak));
  assert(gc2_weak_count_acq(g) == count0 + 1u);

  lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lj_gc2_mark_begin(g);
  next_cycle = gc2_cycle_acq(g);
  assert(next_cycle != 0 && next_cycle != cycle);
  assert(gc2_weak_count_acq(g) == 0);
  assert(lj_gc2_test_weak_record(g, weak));
  assert(lj_tab_weak_record_acq(weak) == lj_tab_weak_record_pack(
    next_cycle, LJ_TAB_WEAK_RECORD_PUBLISHED));
  assert(gc2_weak_count_acq(g) == 1u);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, base);
  UNUSED(key); UNUSED(val);
}

static void test_tvalue_edge_status(lua_State *L, global_State *g)
{
  TValue tv;
  GCtab *t;
  GCArena *a;
  uint32_t cell;
  int base = lua_gettop(L);

  lua_newtable(L);
  t = tabV(L->top - 1);
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  settabV(L, &tv, t);
  assert(lj_gc2_tv_gcref_status_edge(g, &tv) == LJ_GC2_TV_EDGE_VALID);

  lj_gc2_test_stack_admission_retry_once(obj2gco(t));
  assert(lj_gc2_tv_gcref_status_edge(g, &tv) == LJ_GC2_TV_EDGE_RETRY);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert(lj_gc2_tv_gcref_status_edge(g, &tv) == LJ_GC2_TV_EDGE_VALID);

  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_MUTATING));
  assert(lj_gc2_tv_gcref_status_edge(g, &tv) == LJ_GC2_TV_EDGE_RETRY);
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_MUTATING,
				     LJ_ARENA_LIFETIME_LIVE));

  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_RECOVERY));
  assert(lj_gc2_tv_gcref_status_edge(g, &tv) == LJ_GC2_TV_EDGE_RETRY);
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_RECOVERY,
				     LJ_ARENA_LIFETIME_LIVE));

  assert(!lj_arena_late_get(a, cell));
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_DESTRUCT));
  assert(lj_gc2_tv_gcref_status_edge(g, &tv) == LJ_GC2_TV_EDGE_RETRY);
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_DESTRUCT,
				     LJ_ARENA_LIFETIME_LIVE));

  /* A stable body with the wrong TValue tag is terminal stale, not RETRY. */
  setgcVraw(&tv, obj2gco(t), LJ_TFUNC);
  assert(lj_gc2_tv_gcref_status_edge(g, &tv) == LJ_GC2_TV_EDGE_STALE);
  lua_settop(L, base);
}

static void enter_sweep_tvalue_fixture(lua_State *L, global_State *g,
					TGState *tg)
{
  uint32_t i;
  int complete = 0;
  lj_gc2_mark_begin(g);
  lj_gc2_test_scan_roots(g, L);
  flush_and_drain(g, tg);
  lj_gc2_mark_to_weak(g);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  for (i = 0; i < 4096u && !complete; i++)
    complete = lj_gc2_weak_complete(g, L, NULL,
				    LJ_GC2_WEAK_DRAIN_BATCH);
  assert(complete);
  lj_gc2_weak_to_sweep(g, L);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
}

static void test_sweep_tvalue_edge_tristate(lua_State *L, global_State *g,
					     TGState *tg)
{
  TValue good, wrong, unmapped;
  GCtab *t;
  GCArena *a;
  uint32_t cell;
  uint64_t items0, published0;
  int veto0;
  int base = lua_gettop(L);

  lua_newtable(L);
  t = tabV(L->top - 1);
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  settabV(L, &good, t);
  setgcVraw(&wrong, obj2gco(t), LJ_TFUNC);
  setgcVraw(&unmapped, (GCobj *)(uintptr_t)0x10000u, LJ_TTAB);
  enter_sweep_tvalue_fixture(L, g, tg);
  assert(gc2_recovery_failed_acq(g) == 0);
  items0 = gc2_recovery_items_acq(g);
  published0 = gc2_recovery_published_acq(g);
  veto0 = lj_gc2_activation_reclaim_veto(g);

  /* A live body paired with the wrong TValue tag and an unregistered pointer
  ** are stable stale snapshots. Neither is semantic recovery authority. */
  lj_gc2_barrier_tv_g(g, &wrong);
  lj_gc2_barrier_tv_g(g, &unmapped);
  assert(gc2_recovery_items_acq(g) == items0);
  assert(gc2_recovery_published_acq(g) == published0);
  assert(gc2_recovery_failed_acq(g) == 0);
  assert(lj_gc2_activation_reclaim_veto(g) == veto0);

  /* Cancellable DESTRUCT is transient, not stale. The barrier must cancel it
  ** into exact recovery work, restore LIVE, and let the worker consume it. */
  assert(!lj_arena_late_get(a, cell));
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_DESTRUCT));
  lj_gc2_barrier_tv_g(g, &good);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(g) == items0 + 1u);
  assert(gc2_recovery_published_acq(g) == published0 + 1u);
  assert(gc2_recovery_failed_acq(g) == 0);
  assert(lj_gc2_test_recovery_drain(g, 1) == 1u);
  assert(lj_arena_recovery_state_acq(a, cell) == LJ_ARENA_RECOVERY_IDLE);
  assert(gc2_recovery_items_acq(g) == items0);
  flush_and_drain(g, tg);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, base);
}

typedef struct ForjitLeaseTransitionCtx {
  global_State *g;
  GCstr *key;
  GCArena *arena;
  uint32_t cell;
} ForjitLeaseTransitionCtx;

typedef struct ForjitResizeTransitionCtx {
  lua_State *L;
  GCtab *tab;
  uint32_t asize;
  uint32_t hbits;
  uint32_t ready;
  int status;
} ForjitResizeTransitionCtx;

typedef struct ForjitResultLeaseCtx {
  global_State *g;
  TGState *tg;
  GCudata *value;
  uint32_t smr_readers0;
  uint32_t huge_readers;
} ForjitResultLeaseCtx;

typedef struct MetaMtCaptureCtx {
  global_State *g;
  GCtab *source;
  GCtab *replacement;
  GCArena *source_arena;
  GCArena *target_arena;
  uint64_t source_remote0;
  uint64_t target_remote0;
  uint32_t smr_readers0;
  uint32_t observed;
  uint32_t target_observed;
} MetaMtCaptureCtx;

static void *forjit_lease_transition_thread(void *arg)
{
  ForjitLeaseTransitionCtx *ctx = (ForjitLeaseTransitionCtx *)arg;
  uint32_t spin;
  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_tab_test_forjit_lease_paused())
      break;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(spin < 1000000u);
  /* Both the source table and key have exact body leases at this hook. */
  assert(lj_arena_remote_active_acq(ctx->arena) != 0);
  assert(lj_arena_lifetime_state_cas(ctx->arena, ctx->cell,
				     LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_MUTATING));
  /* Arm the old nested-admission rejection hook. The rooted reader must not
  ** consume it: one exact key lease now covers the paired current-generation
  ** lookup from hashing through result publication. */
  lj_gc2_test_stack_admission_retry_once(obj2gco(ctx->key));
  lj_tab_test_forjit_lease_release();
  return NULL;
}

static void *forjit_resize_transition_thread(void *arg)
{
  ForjitResizeTransitionCtx *ctx = (ForjitResizeTransitionCtx *)arg;
  uint32_t spin;
  if (!lj_threading_attach(ctx->L)) {
    ctx->status = 1;
    la_store32_rel(&ctx->ready, 1);
    lj_tab_test_forjit_snapshot_release();
    return NULL;
  }
  ctx->status = 0;
  la_store32_rel(&ctx->ready, 1);
  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_tab_test_forjit_snapshot_paused())
      break;
    (void)lj_thr_retry_yield(NULL);
  }
  if (spin == 1000000u) {
    ctx->status = 2;
  } else {
    lj_tab_resize(ctx->L, ctx->tab, ctx->asize, ctx->hbits);
    ctx->status = 3;
  }
  lj_tab_test_forjit_snapshot_release();
  lj_threading_detach(ctx->L, 1);
  return NULL;
}

static void *forjit_result_lease_thread(void *arg)
{
  ForjitResultLeaseCtx *ctx = (ForjitResultLeaseCtx *)arg;
  LJHugeInfo hi;
  uint32_t spin;
  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_tab_test_forjit_result_paused())
      break;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(spin < 1000000u);
  /* A huge weak value has no arena admission in common with its small table
  ** and scalar key. Its HugeTab reader therefore proves the copied result has
  ** its own exact lease, while the SMR count proves that lease was acquired
  ** before the source vector's read interval closed. */
  assert(gc2_smr_readers_acq(ctx->g) > ctx->smr_readers0);
  assert(lj_arena_hugetab_lookup(&ctx->tg->huge, ctx->value, &hi));
  assert(hi.readers != 0);
  ctx->huge_readers = hi.readers;
  lj_tab_test_forjit_result_release();
  return NULL;
}

static void *meta_mt_capture_transition_thread(void *arg)
{
  MetaMtCaptureCtx *ctx = (MetaMtCaptureCtx *)arg;
  uint32_t spin;
  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_meta_test_mt_capture_paused())
      break;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(spin < 1000000u);
  /* The receiver lease precedes its metatable field load, and the SMR scope
  ** remains open until the captured target has its own exact lease. */
  assert(gc2_smr_readers_acq(ctx->g) > ctx->smr_readers0);
  assert((lj_arena_remote_active_acq(ctx->source_arena) &
	  LJ_ARENA_REMOTE_COUNT_MASK) >
	 (ctx->source_remote0 & LJ_ARENA_REMOTE_COUNT_MASK));
  lj_tab_metatable_rel(ctx->source, ctx->replacement);
  lj_tab_nomm_rel(ctx->replacement, 0);
  lj_gc2_barrier_obj_pair_g(ctx->g, obj2gco(ctx->source),
			     obj2gco(ctx->replacement));
  la_store32_rel(&ctx->observed, 1);
  lj_meta_test_mt_capture_release();

  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_meta_test_mt_lease_paused())
      break;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(spin < 1000000u);
  /* source and old target deliberately occupy different arenas. Once the
  ** replacement store above removes the old target's sole semantic edge, this
  ** count delta can only be supplied by the exact target admission. */
  assert(gc2_smr_readers_acq(ctx->g) > ctx->smr_readers0);
  assert((lj_arena_remote_active_acq(ctx->target_arena) &
	  LJ_ARENA_REMOTE_COUNT_MASK) >
	 (ctx->target_remote0 & LJ_ARENA_REMOTE_COUNT_MASK));
  la_store32_rel(&ctx->target_observed, 1);
  lj_meta_test_mt_lease_release();
  return NULL;
}

static void test_forjit_current_hash_key_lease(lua_State *L, global_State *g)
{
  ForjitLeaseTransitionCtx ctx;
  TValue key, out;
  GCtab *t;
  GCstr *s;
  pthread_t transition;
  int base = lua_gettop(L);

  lua_newtable(L);
  t = tabV(L->top - 1);
  lua_pushliteral(L, "forjit leased key");
  s = strV(L->top - 1);
  lua_pushboolean(L, 1);
  lua_rawset(L, -3);
  setstrV(L, &key, s);
  ctx.g = g;
  ctx.key = s;
  ctx.arena = lj_arena_of(s);
  ctx.cell = lj_arena_cellof(s);

  lj_tab_test_forjit_lease_pause();
  assert(pthread_create(&transition, NULL, forjit_lease_transition_thread,
			&ctx) == 0);
  assert(lj_tab_gettv_forjit(L, t, &key, &out) == &out);
  assert(pthread_join(transition, NULL) == 0);
  assert(lj_gc2_test_stack_admission_retry_hits() == 0u);
  /* Do not leave the deliberately unconsumed process-global hook armed for a
  ** later fixture that happens to admit the same string. */
  lj_gc2_test_stack_admission_retry_once(NULL);
  assert(tvistrue(&out));
  assert(lj_arena_lifetime_state_cas(ctx.arena, ctx.cell,
				     LJ_ARENA_LIFETIME_MUTATING,
				     LJ_ARENA_LIFETIME_LIVE));
  lua_settop(L, base);
}

static void test_forjit_hash_to_array_retry(lua_State *L)
{
  uint32_t kind;
  int base = lua_gettop(L);
  for (kind = 0; kind < 2u; kind++) {
    ForjitResizeTransitionCtx ctx;
    TValue key, val, out;
    TValue *array;
    TValue *slot;
    GCtab *t;
    lua_State *peer;
    MSize asize, hmask;
    pthread_t transition;
    uint32_t spin;
    const int32_t ik = 7;

    lua_createtable(L, 0, 4);
    t = tabV(L->top - 1);
    setboolV(&val, 1);
    slot = lj_tab_setinth(L, t, ik);
    assert(slot != NULL);
    (void)lj_tab_storetv(L, slot, &val);
    asize = lj_tab_array_snapshot_acq(t, &array);
    assert((MSize)ik >= asize);  /* The fixture starts in the hash side. */
    (void)lj_tab_node_snapshot_acq(t, &hmask);
    assert(hmask > 0);
    if (kind == 0)
      setintV(&key, ik);
    else
      setnumV(&key, (lua_Number)ik);

    peer = lua_newthread(L);
    assert(peer != NULL);
    memset(&ctx, 0, sizeof(ctx));
    ctx.L = peer;
    ctx.tab = t;
    ctx.asize = (uint32_t)ik + 1u;
    ctx.hbits = lj_fls((uint32_t)hmask) + 1u;
    ctx.status = -1;
    assert(pthread_create(&transition, NULL, forjit_resize_transition_thread,
			  &ctx) == 0);
    for (spin = 0; spin < 1000000u && !la_load32_acq(&ctx.ready); spin++)
      (void)lj_thr_retry_yield(L);
    assert(spin < 1000000u && ctx.status == 0);

    /* Simulate the already-observed first-generation miss, then resize exactly
    ** between the fallback's integral-array and hash observations. The paired
    ** snapshot must reject that mixed generation and retry into the new array. */
    lj_tab_test_forjit_initial_miss_once();
    lj_tab_test_forjit_snapshot_pause();
    assert(lj_tab_gettv_forjit(L, t, &key, &out) == &out);
    assert(pthread_join(transition, NULL) == 0);
    assert(ctx.status == 3);
    assert(tvistrue(&out));
    asize = lj_tab_array_snapshot_acq(t, &array);
    assert((MSize)ik < asize);
    lj_tv_load_acq(&out, &array[ik]);
    assert(tvistrue(&out));
    lua_settop(L, base);
  }
}

static void test_forjit_weak_result_lease(lua_State *L, global_State *g,
					   TGState *tg)
{
  ForjitResultLeaseCtx ctx;
  LJHugeInfo hi;
  TValue key, out;
  TValue *slot;
  GCtab *weak;
  GCudata *value;
  pthread_t transition;
  int base = lua_gettop(L);

  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_rawset(L, -3);
  assert(lua_setmetatable(L, -2) == 1);

  value = lj_udata_new(L, (MSize)LJ_HUGE_THRESHOLD + 1024u, NULL);
  setudataV(L, L->top, value);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  assert(lj_arena_ishuge(lj_arena_of(value)));
  lua_pushinteger(L, 1);
  lua_pushvalue(L, -2);
  lua_rawset(L, base + 1);
  setintV(&key, 1);

  /* A transient result admission retries the whole semantic read. It must not
  ** turn the weak value into a false miss. */
  lj_gc2_test_stack_admission_retry_once(obj2gco(value));
  assert(lj_tab_gettv_forjit(L, weak, &key, &out) == &out);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert(tvisudata(&out) && udataV(&out) == value);

  memset(&ctx, 0, sizeof(ctx));
  ctx.g = g;
  ctx.tg = tg;
  ctx.value = value;
  ctx.smr_readers0 = gc2_smr_readers_acq(g);
  assert(lj_arena_hugetab_lookup(&tg->huge, value, &hi));
  assert(hi.readers == 0);
  lj_tab_test_forjit_result_pause();
  assert(pthread_create(&transition, NULL, forjit_result_lease_thread,
			&ctx) == 0);
  assert(lj_tab_gettv_forjit(L, weak, &key, &out) == &out);
  assert(pthread_join(transition, NULL) == 0);
  assert(ctx.huge_readers != 0);
  assert(tvisudata(&out) && udataV(&out) == value);
  assert(gc2_smr_readers_acq(g) == ctx.smr_readers0);
  assert(lj_arena_hugetab_lookup(&tg->huge, value, &hi));
  assert(hi.readers == 0);

  /* A stable tag/header mismatch is terminal stale. Returning nil is required;
  ** treating it like RETRY would spin forever on the unchanged weak slot. */
  slot = lj_tab_setinth(L, weak, 1);
  assert(slot != NULL);
  setgcVraw(slot, obj2gco(value), LJ_TFUNC);
  assert(lj_tab_gettv_forjit(L, weak, &key, &out) == &out);
  assert(tvisnil(&out));
  setudataV(L, slot, value);

  /* Exercise the same terminal classification through the hash-vector path. */
  setintV(&key, -17);
  slot = lj_tab_setinth(L, weak, -17);
  assert(slot != NULL);
  setgcVraw(slot, obj2gco(value), LJ_TFUNC);
  assert(lj_tab_gettv_forjit(L, weak, &key, &out) == &out);
  assert(tvisnil(&out));
  setudataV(L, slot, value);
  lua_settop(L, base);
}

static void test_forjit_nil_key(lua_State *L)
{
  TValue key, out;
  GCtab *t;
  int base = lua_gettop(L);
  lua_newtable(L);
  t = tabV(L->top - 1);
  setnilV(&key);
  assert(lj_tab_gettv_forjit(L, t, &key, &out) == &out);
  assert(tvisnil(&out));
  lua_settop(L, base);
#if LJ_HASJIT
  assert(luaL_dostring(L,
    "local jit, util = require('jit'), require('jit.util')\n"
    "jit.flush()\n"
    "local t = {}\n"
    "jit.off()\n"
    "assert(t[nil] == nil)\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function readnil(tab, key, n)\n"
    "  local seen = 0\n"
    "  for i = 1, n do\n"
    "    if tab[key] == nil then seen = seen + 1 end\n"
    "  end\n"
    "  return seen\n"
    "end\n"
    "assert(readnil(t, nil, 100) == 100)\n"
    /* The fixture activated MT earlier. A nil result is metamethod-sensitive,
    ** so the recorder must fail closed until receiver-to-metatable capture is
    ** represented by a rooted generated helper. */
    "assert(not util.traceinfo(1), 'nil-key read crossed MT meta fence')\n"
    "jit.flush()\n") == LUA_OK);
  lua_settop(L, base);
#endif
}

static void test_meta_metatable_capture_lease(lua_State *L, global_State *g)
{
  MetaMtCaptureCtx ctx;
  TValue obj, out;
  GCtab *source, *oldmt, *newmt;
  pthread_t transition;
  uint32_t allocs;
  int base = lua_gettop(L);

  lua_newtable(L);
  source = tabV(L->top - 1);
  oldmt = NULL;
  /* Isolate the receiver and captured target admissions. Popped filler tables
  ** consume the current traversable arena without becoming long-lived roots. */
  for (allocs = 0; allocs < 100000u; allocs++) {
    lua_newtable(L);
    oldmt = tabV(L->top - 1);
    if (lj_arena_of(oldmt) != lj_arena_of(source))
      break;
    lua_pop(L, 1);
    oldmt = NULL;
  }
  assert(oldmt != NULL && allocs < 100000u);
  lua_pushliteral(L, "__index");
  lua_pushinteger(L, 11);
  lua_rawset(L, -3);
  lua_pushvalue(L, -1);
  assert(lua_setmetatable(L, base + 1) == 1);
  lua_newtable(L);
  newmt = tabV(L->top - 1);
  lua_pushliteral(L, "__index");
  lua_pushinteger(L, 22);
  lua_rawset(L, -3);
  settabV(L, &obj, source);
  /* source->metatable is now oldmt's only semantic root. The replacement at
  ** the first hook makes the second hook's exact target lease authoritative. */
  lua_remove(L, base + 2);

  memset(&ctx, 0, sizeof(ctx));
  ctx.g = g;
  ctx.source = source;
  ctx.replacement = newmt;
  ctx.source_arena = lj_arena_of(source);
  ctx.target_arena = lj_arena_of(oldmt);
  assert(ctx.source_arena != ctx.target_arena);
  ctx.source_remote0 = lj_arena_remote_active_acq(ctx.source_arena);
  ctx.target_remote0 = lj_arena_remote_active_acq(ctx.target_arena);
  ctx.smr_readers0 = gc2_smr_readers_acq(g);
  assert(lj_tab_metatable_acq(source) == oldmt);
  lj_meta_test_mt_capture_pause(obj2gco(source));
  lj_meta_test_mt_lease_pause(obj2gco(oldmt));
  assert(pthread_create(&transition, NULL,
			meta_mt_capture_transition_thread, &ctx) == 0);
  assert(lj_meta_lookuptv(L, &out, &obj, MM_index) == &out);
  assert(pthread_join(transition, NULL) == 0);
  assert(la_load32_acq(&ctx.observed) == 1);
  assert(la_load32_acq(&ctx.target_observed) == 1);
  /* The first lookup linearizes at the captured old metatable; the next sees
  ** the release-published replacement. Neither can cross an incarnation. */
  assert((tvisint(&out) && intV(&out) == 11) ||
	 (tvisnum(&out) && numV(&out) == 11.0));
  assert(lj_meta_lookuptv(L, &out, &obj, MM_index) == &out);
  assert((tvisint(&out) && intV(&out) == 22) ||
	 (tvisnum(&out) && numV(&out) == 22.0));
  lua_settop(L, base);
}

static void test_meta_tset_admission_retries(lua_State *L, global_State *g)
{
  TValue obj, key, val, out;
  GCfunc *mmfn;
  GCtab *t;
  GCstr *s;
  int base = lua_gettop(L);

  /* Existing-key assignment must retry the semantic root gate, then resolve a
  ** fresh current slot instead of returning a pre-retry vector pointer. */
  lua_newtable(L);
  t = tabV(L->top - 1);
  lua_pushliteral(L, "tset existing retry");
  s = strV(L->top - 1);
  lua_pushboolean(L, 0);
  lua_rawset(L, -3);
  settabV(L, &obj, t);
  setstrV(L, &key, s);
  setboolV(&val, 1);
  lj_gc2_test_stack_admission_retry_once(obj2gco(t));
  assert(lj_meta_tsettv_pair(L, &obj, &key, &val) != NULL);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert(lj_tab_gettv_forjit(L, t, &key, &out) == &out && tvistrue(&out));
  lua_settop(L, base);

  /* A missing collectable key is rooted and retried before any new-key or
  ** __newindex decision, then inserted exactly once. */
  lua_newtable(L);
  t = tabV(L->top - 1);
  lua_pushliteral(L, "tset missing retry");
  s = strV(L->top - 1);
  settabV(L, &obj, t);
  setstrV(L, &key, s);
  setboolV(&val, 1);
  lj_gc2_test_stack_admission_retry_once(obj2gco(s));
  assert(lj_meta_tsettv_pair(L, &obj, &key, &val) != NULL);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert(lj_tab_gettv_forjit(L, t, &key, &out) == &out && tvistrue(&out));
  lua_settop(L, base);

  /* Function-valued __newindex dispatch must survive a transient result lease
  ** without duplicating the metamethod call. This lookup goes through the same
  ** lease-aware copied-value path as ordinary helper-backed table reads. */
  assert(luaL_dostring(L,
    "local sink, proxy, calls = {}, {}, 0\n"
    "local mm = function(_, k, v)\n"
    "  calls = calls + 1; sink[k] = v\n"
    "end\n"
    "setmetatable(proxy, { __newindex = mm })\n"
    "return proxy, sink, function() return calls end, mm\n") == LUA_OK);
  assert(tvisfunc(L->top - 1));
  mmfn = funcV(L->top - 1);
  lua_pushliteral(L, "tset newindex retry");
  lua_pushinteger(L, 37);
  lj_gc2_test_stack_admission_retry_once(obj2gco(mmfn));
  lua_settable(L, base + 1);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  lua_getfield(L, base + 1, "tset newindex retry");
  assert(tvisnil(L->top - 1));
  lua_pop(L, 1);
  lua_getfield(L, base + 2, "tset newindex retry");
  assert(lua_isnumber(L, -1) && lua_tointeger(L, -1) == 37);
  lua_pop(L, 1);
  lua_pushvalue(L, base + 3);
  lua_call(L, 0, 1);
  assert(lua_isnumber(L, -1) && lua_tointeger(L, -1) == 1);
  lua_settop(L, base);
  UNUSED(g);
}

static void test_stack_admission_tristate(lua_State *L, global_State *g,
					   TGState *tg)
{
  TGState extra_tg;
  TGState *old_tg = lj_thr_get_tg();
  lua_State *owner_L, *busy;
  TValue *fake, *saved_base;
  GCtab *target, *mismatch, *env_target, *saved_env;
  uint64_t scan0, dirty0, handoff0, claims0, requeues0;
  uint64_t grey_pushed0;
  uint32_t pending0, n_threads0, round, smr0, cycle0;
  int base = lua_gettop(L);

  /* A live foreign owner first turns the grey thread into counted NEEDSCAN
  ** work. Native cur_L makes the eventual owner snapshot conservative and
  ** authoritative, so the exact target is admitted from the widened tail. */
  owner_L = lua_newthread(L);
  assert(owner_L != NULL);
  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  target = tabV(busy->top - 1);
  lua_newtable(L);
  mismatch = tabV(L->top - 1);
  fake = busy->top;
  assert(fake < tvref(busy->maxstack));
  setgcVraw(fake, obj2gco(mismatch), LJ_TFUNC);

  lj_tg_init_thread(g, &extra_tg, owner_L, 1);
  extra_tg.tid = tg->tid + 11000u;
  if (extra_tg.tid == 0 || extra_tg.tid == LJ_THREAD_GCSCAN)
    extra_tg.tid = 11000u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  owner_L->tg_hint = &extra_tg;
  busy->tg_hint = &extra_tg;
  lj_state_owner_rel(owner_L, extra_tg.tid);
  lj_state_owner_rel(busy, extra_tg.tid);
  lj_tg_store_cur_L(&extra_tg, busy);
  /* Remove the thread from the root spine so the owner-NEEDSCAN helper reaches
  ** it through the thread registry only. Main-stack and TG references still
  ** provide semantic reachability, but that helper does not traverse them. */
  lj_state_thread_registry_publish(g, busy);
  assert(lj_gc_unlink_root_obj(g, obj2gco(busy)) == LJ_GC_ROOT_UNLINKED);

  flush_and_drain(g, tg);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  cycle0 = gc2_cycle_acq(g);
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(gc2_cycle_acq(g) != cycle0);
  pin_mark_closed_for_worker_fixture(g);
  n_threads0 = g->gc2.n_threads;
  lj_tg_attach(g, &extra_tg);
  lj_native_enter(&extra_tg);
  assert(lj_tg_find_owner(g, extra_tg.tid) == &extra_tg);
  assert(lj_tg_in_native_acq(&extra_tg) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(target)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(mismatch)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(busy)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(lj_gc2_worker_drain(g, 2) != 0);
  assert(lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN);

  scan0 = lj_state_scan_epoch_acq(busy);
  dirty0 = lj_state_scan_dirty_epoch_acq(busy);
  pending0 = gc2_thread_scan_needscan_pending_acq(g);
  assert(lj_state_scan_handoff_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert(pending0 != 0);

  /* Remove the synthetic worker handoff so a direct owner-root retry must
  ** publish a fresh counted handoff rather than merely preserve one. */
  assert(la_and8_rlx(lj_obj_gcflags_ref(obj2gco(busy)),
		     (uint8_t)~LJ_GC_NEEDSCAN) & LJ_GC_NEEDSCAN);
  assert(lj_state_scan_needscan_counted_xchg(busy, 0) == 1u);
  gc2_thread_scan_needscan_pending_dec(g);
  lj_state_scan_handoff_epoch_rel(busy, 0);
  assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0 - 1u);

  lj_gc2_test_stack_admission_retry_once(obj2gco(target));
  lj_thr_set_tg(&extra_tg);
  lj_gc2_test_scan_tg_thread_root(g, &extra_tg, busy);
  lj_thr_set_tg(old_tg);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(target)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(mismatch)) == 0);
  assert(lj_state_scan_epoch_acq(busy) == scan0);
  assert(lj_state_scan_dirty_epoch_acq(busy) == dirty0);
  assert(lj_state_scan_handoff_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert(lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0);

  /* A second retry through the counted owner queue must preserve that exact
  ** handoff rather than double-counting it or publishing completion stamps. */
  handoff0 = lj_state_scan_handoff_epoch_acq(busy);
  lj_gc2_test_stack_admission_retry_once(obj2gco(target));
  lj_thr_set_tg(&extra_tg);
  lj_gc2_test_scan_owned_needscan(g, owner_L);
  lj_thr_set_tg(old_tg);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(target)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(mismatch)) == 0);
  assert(lj_state_scan_epoch_acq(busy) == scan0);
  assert(lj_state_scan_dirty_epoch_acq(busy) == dirty0);
  assert(lj_state_scan_handoff_epoch_acq(busy) == handoff0);
  assert(lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0);

  lj_thr_set_tg(&extra_tg);
  lj_gc2_test_scan_owned_needscan(g, owner_L);
  lj_thr_set_tg(old_tg);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(target)) == 1);
  /* The function-shaped stale word names a valid table allocation. A terminal
  ** tag/header mismatch is ignored rather than retried or marked as a func. */
  assert(lj_gc2_ismarked(g, obj2gco(mismatch)) == 0);
  assert(lj_state_scan_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert(lj_state_scan_dirty_epoch_acq(busy) ==
	 lj_tg_stack_dirty_epoch_acq(&extra_tg));
  assert(lj_state_scan_handoff_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0 - 1u);

  /* A thread environment is an authoritative root outside the stack range.
  ** Its transient semantic admission must publish the same counted retry and
  ** must not let the owner snapshot stamp over the missing edge. */
  (void)lj_gc2_flush_ssb(g, &extra_tg);
  (void)lj_gc2_flush_ssb(g, tg);
  worker_drain_all(g);
  lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  saved_env = lj_state_env_acq(busy);
  lua_newtable(L);
  env_target = tabV(L->top - 1);
  lj_state_env_rel(busy, env_target);
  lua_pop(L, 1);
  cycle0 = gc2_cycle_acq(g);
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(gc2_cycle_acq(g) != cycle0);
  pin_mark_closed_for_worker_fixture(g);
  scan0 = lj_state_scan_epoch_acq(busy);
  dirty0 = lj_state_scan_dirty_epoch_acq(busy);
  assert(gc2_thread_scan_needscan_pending_acq(g) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(env_target)) == 0);
  lj_gc2_test_root_semantic_retry_once(obj2gco(env_target));
  lj_thr_set_tg(&extra_tg);
  lj_gc2_test_scan_tg_thread_root(g, &extra_tg, busy);
  lj_thr_set_tg(old_tg);
  assert(lj_gc2_test_root_semantic_retry_hits() == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(env_target)) == 0);
  assert(lj_state_scan_epoch_acq(busy) == scan0);
  assert(lj_state_scan_dirty_epoch_acq(busy) == dirty0);
  assert(lj_state_scan_handoff_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert(lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN);
  assert(gc2_thread_scan_needscan_pending_acq(g) == 1u);
  lj_thr_set_tg(&extra_tg);
  lj_gc2_test_scan_owned_needscan(g, owner_L);
  lj_thr_set_tg(old_tg);
  assert(lj_gc2_test_root_semantic_retry_hits() == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(env_target)) == 1);
  assert(lj_state_scan_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  assert(gc2_thread_scan_needscan_pending_acq(g) == 0);

  setnilV(fake);
  assert(lj_native_leave_tg(&extra_tg) == 0);
  assert(lj_tg_in_native_acq(&extra_tg) == 0);
  (void)lj_gc2_flush_ssb(g, &extra_tg);
  (void)lj_gc2_flush_ssb(g, tg);
  worker_drain_all(g);
  assert(lj_gc_linkobj(g, obj2gco(busy)) == LJ_GC_ROOT_LINKED);
  lj_state_owner_rel(owner_L, 0);
  lj_state_owner_rel(busy, 0);
  owner_L->tg_hint = tg;
  busy->tg_hint = tg;
  lj_tg_detach(g, &extra_tg);
  assert(lj_tg_flags_test_acq(&extra_tg, TGF_DEAD));
  assert(g->gc2.n_threads <= n_threads0 + 1u);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, &extra_tg);
  lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lj_state_env_rel(busy, saved_env);
  lua_settop(L, base);

  /* An ownerless state is GCSCAN-claimable. Temporarily exposing an invalid
  ** frame header drives the existing conservative frame-fallback path. The
  ** first exact-target retry must republish the thread to grey without a
  ** completion stamp; a later bounded worker quantum must claim and complete. */
  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  target = tabV(busy->top - 1);
  lua_newtable(L);
  mismatch = tabV(L->top - 1);
  fake = busy->top;
  assert(fake < tvref(busy->maxstack));
  setgcVraw(fake, obj2gco(mismatch), LJ_TFUNC);
  saved_base = busy->base;
  busy->base = busy->top;

  flush_and_drain(g, tg);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  cycle0 = gc2_cycle_acq(g);
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(gc2_cycle_acq(g) != cycle0);
  pin_mark_closed_for_worker_fixture(g);
  scan0 = lj_state_scan_epoch_acq(busy);
  dirty0 = lj_state_scan_dirty_epoch_acq(busy);
  handoff0 = lj_state_scan_handoff_epoch_acq(busy);
  claims0 = la_load64_acq(&g->gc2.thread_scan_claims);
  requeues0 = gc2_thread_scan_requeues_acq(g);
  grey_pushed0 = gc2_grey_pushed_acq(g);
  smr0 = gc2_smr_readers_acq(g);
  pending0 = gc2_thread_scan_needscan_pending_acq(g);
  assert(pending0 == 0);
  /* This hook fires after the widened stale-word classifier admitted the
  ** exact object, covering nested semantic DEAD with its outer scope held. */
  lj_gc2_test_root_semantic_retry_once(obj2gco(target));
  assert(lj_gc2_markobj(g, obj2gco(busy)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(lj_gc2_worker_drain(g, 2) == 2u);
  assert(lj_gc2_test_root_semantic_retry_hits() == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(target)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(mismatch)) == 0);
  assert(lj_state_scan_epoch_acq(busy) == scan0);
  assert(lj_state_scan_dirty_epoch_acq(busy) == dirty0);
  assert(lj_state_scan_handoff_epoch_acq(busy) == handoff0);
  assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  assert(la_load64_acq(&g->gc2.thread_scan_claims) == claims0 + 1u);
  assert(gc2_thread_scan_requeues_acq(g) == requeues0);
  assert(gc2_smr_readers_acq(g) == smr0);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0);
  assert(gc2_grey_pushed_acq(g) >= grey_pushed0 + 2u);
  assert(!lj_gc2_test_ssb_empty(g));

  /* Each public quantum may first convert an unrelated active SSB item above
  ** the direct grey retry. Use a bounded 4096-item budget rather than assuming
  ** the Chase-Lev owner can pop the retry in the very next one-item quantum. */
  for (round = 0;
       round < 64 && lj_state_scan_epoch_acq(busy) != gc2_thread_scan_cycle_acq(g);
       round++)
    assert(lj_gc2_worker_drain(g, 64) != 0);
  assert(lj_state_scan_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert(lj_gc2_test_root_semantic_retry_hits() == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(target)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(mismatch)) == 0);
  assert(lj_state_scan_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert(lj_state_scan_dirty_epoch_acq(busy) == 0);
  assert(lj_state_scan_handoff_epoch_acq(busy) == 0);
  assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  assert(la_load64_acq(&g->gc2.thread_scan_claims) == claims0 + 2u);
  assert(gc2_thread_scan_requeues_acq(g) == requeues0);
  assert(gc2_smr_readers_acq(g) == smr0);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0);
  assert(lj_state_owner_acq(busy) == 0);

  busy->base = saved_base;
  setnilV(fake);
  worker_drain_all(g);
  lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);

  /* Repeat the worker retry with a non-stack authoritative environment edge.
  ** Drop GCSCAN authority first, publish the concrete retry while registry SMR
  ** still pins its identity, then return the SMR reader count to its baseline. */
  saved_env = lj_state_env_acq(busy);
  lua_newtable(L);
  env_target = tabV(L->top - 1);
  lj_state_env_rel(busy, env_target);
  lua_pop(L, 1);
  cycle0 = gc2_cycle_acq(g);
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(gc2_cycle_acq(g) != cycle0);
  pin_mark_closed_for_worker_fixture(g);
  scan0 = lj_state_scan_epoch_acq(busy);
  dirty0 = lj_state_scan_dirty_epoch_acq(busy);
  handoff0 = lj_state_scan_handoff_epoch_acq(busy);
  claims0 = la_load64_acq(&g->gc2.thread_scan_claims);
  smr0 = gc2_smr_readers_acq(g);
  pending0 = gc2_thread_scan_needscan_pending_acq(g);
  assert(pending0 == 0);
  assert(lj_gc2_ismarked(g, obj2gco(env_target)) == 0);
  lj_gc2_test_root_semantic_retry_once(obj2gco(env_target));
  assert(lj_gc2_markobj(g, obj2gco(busy)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(lj_gc2_worker_drain(g, 2) == 2u);
  assert(lj_gc2_test_root_semantic_retry_hits() == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(env_target)) == 0);
  assert(lj_state_scan_epoch_acq(busy) == scan0);
  assert(lj_state_scan_dirty_epoch_acq(busy) == dirty0);
  assert(lj_state_scan_handoff_epoch_acq(busy) == handoff0);
  assert(la_load64_acq(&g->gc2.thread_scan_claims) == claims0 + 1u);
  assert(gc2_smr_readers_acq(g) == smr0);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0);
  for (round = 0;
       round < 64 && lj_state_scan_epoch_acq(busy) != gc2_thread_scan_cycle_acq(g);
       round++)
    assert(lj_gc2_worker_drain(g, 64) != 0);
  assert(lj_state_scan_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert(lj_gc2_test_root_semantic_retry_hits() == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(env_target)) == 1);
  assert(lj_state_scan_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert(la_load64_acq(&g->gc2.thread_scan_claims) == claims0 + 2u);
  assert(gc2_smr_readers_acq(g) == smr0);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0);
  worker_drain_all(g);
  lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lj_state_env_rel(busy, saved_env);
  lua_settop(L, base);
}
#endif

enum {
  THREAD_RELEASE_RECOVERY,
  THREAD_RELEASE_EXACT_SSB,
  THREAD_RELEASE_REGISTRY_FALLBACK
};

static void test_thread_needscan_owner_release_same_cycle(lua_State *L,
						   global_State *g,
						   TGState *tg,
						   int release_mode)
{
  lua_State *owner_L, *busy;
  TGState *old_tg = NULL;
  TGState extra_tg;
  GCtab *busy_tab;
  GCRef *ssb_next0 = NULL;
  uint64_t needscan0, requeues0, recovery0, major_roots0, claims0;
  uint32_t pending0, n_threads0, round;

  owner_L = lua_newthread(L);
  assert(owner_L != NULL);
  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  busy_tab = tabV(busy->top - 1);

  lj_tg_init_thread(g, &extra_tg, owner_L, 1);
  extra_tg.tid = tg->tid + 7000u + (uint32_t)release_mode * 1000u;
  if (extra_tg.tid == 0 || extra_tg.tid == LJ_THREAD_GCSCAN)
    extra_tg.tid = 7000u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  owner_L->tg_hint = &extra_tg;
  busy->tg_hint = &extra_tg;
  lj_state_owner_rel(owner_L, extra_tg.tid);
  lj_state_owner_rel(busy, extra_tg.tid);
  if (release_mode == THREAD_RELEASE_REGISTRY_FALLBACK)
    lj_state_thread_registry_publish(g, busy);
  /* In the hook modes these are ordinary coroutines with no semantic Lua root;
  ** the current-cycle mark and counted handoff keep busy valid until release
  ** transfers its identity. The fallback mode gives busy only the process-wide
  ** threading registry identity and deliberately bypasses the release hook. */
  lua_pop(L, 2);

  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  n_threads0 = gc2_n_threads_acq(g);
  lj_tg_attach(g, &extra_tg);
  assert(lj_tg_find_owner(g, extra_tg.tid) == &extra_tg);
  assert(gc2_n_threads_acq(g) >= n_threads0);
  needscan0 = gc2_thread_scan_needscan_acq(g);
  requeues0 = gc2_thread_scan_requeues_acq(g);
  pending0 = gc2_thread_scan_needscan_pending_acq(g);
  assert(pending0 == 0);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);

  /* Consume the thread's concrete grey item while its foreign owner is live.
  ** The failed GCSCAN claim replaces that item with one counted NEEDSCAN
  ** handoff addressed to extra_tg. */
  assert(lj_gc2_markobj(g, obj2gco(busy)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(lj_gc2_worker_drain(g, 2) != 0);
  assert(gc2_thread_scan_needscan_acq(g) == needscan0 + 1u);
  assert(gc2_thread_scan_requeues_acq(g) == requeues0 + 1u);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0 + 1u);
  assert(lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);
  assert(lj_gc2_test_ssb_empty(g));

  /* Model the production state after the initial all-root snapshot. Hook modes
  ** have no later root path; fallback mode can use only the global registry. */
  gc2_mark_root_scanned_rel(g, 1);

  /* Model normal worker exit: ownership is released before TG detach. The raw
  ** fallback mode intentionally skips the hook so registry recovery is tested
  ** independently of owner-release publication. */
  recovery0 = gc2_recovery_published_acq(g);
  major_roots0 = gc2_major_root_scans_acq(g);
  if (release_mode == THREAD_RELEASE_EXACT_SSB) {
    old_tg = lj_thr_get_tg();
    ssb_next0 = lj_tg_ssb_next_acq(&extra_tg);
    lj_thr_set_tg(&extra_tg);
  } else if (release_mode == THREAD_RELEASE_RECOVERY) {
    /* A closed metadata-reader gate is legitimate during stable sweeping.
    ** The cold exact capability must still release without waiting/aborting
    ** and retain its identity directly on the recovery plane. */
    assert(gc2_smr_readers_acq(g) == 0);
    assert(gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_OPEN);
    gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_SWEEP_STABLE);
  }
  if (release_mode == THREAD_RELEASE_REGISTRY_FALLBACK) {
    lj_state_owner_rel(owner_L, 0);
    lj_state_owner_rel(busy, 0);
  } else {
    lj_state_release(owner_L, extra_tg.tid);
    lj_state_release(busy, extra_tg.tid);
  }
  if (release_mode == THREAD_RELEASE_EXACT_SSB) {
    lj_thr_set_tg(old_tg);
    assert(lj_tg_ssb_next_acq(&extra_tg) > ssb_next0);
  } else if (release_mode == THREAD_RELEASE_RECOVERY) {
    /* A synthetic owner without matching TLS must never append main_tg's SSB. */
    assert(gc2_recovery_published_acq(g) > recovery0);
    gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);
  }
  owner_L->tg_hint = tg;
  busy->tg_hint = tg;
  lj_tg_detach(g, &extra_tg);
  assert(lj_tg_flags_test_acq(&extra_tg, TGF_DEAD));
  assert(lj_state_owner_acq(busy) == 0);

  /* Exercise only the production fixpoint driver. Hook publication retains an
  ** otherwise unlocatable coroutine; fallback mode republishes its registry
  ** identity. Ownerless traversal then takes GCSCAN, marks the stack payload
  ** and discharges the global count. */
  claims0 = la_load64_acq(&g->gc2.thread_scan_claims);
  for (round = 0; round < 64; round++)
    if (lj_gc2_fixpoint_round(g, L, ~(uint32_t)0))
      break;
  assert(round < 64);
  if (release_mode == THREAD_RELEASE_REGISTRY_FALLBACK)
    assert(gc2_major_root_scans_acq(g) > major_roots0);
  assert(la_load64_acq(&g->gc2.thread_scan_claims) > claims0);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0);
  assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  assert(lj_state_scan_handoff_epoch_acq(busy) == 0);
  assert(lj_state_scan_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 1);

  /* An exact owner-TG SSB entry intentionally pins the detached TG until its
  ** item is consumed. Reclaim terminal TG storage only after fixpoint drain. */
  (void)lj_tg_reclaim_dead(g);
  assert(lj_tg_find_owner(g, extra_tg.tid) != &extra_tg);
  lj_tg_fini_thread(g, &extra_tg);
  assert(gc2_n_threads_acq(g) == n_threads0);

  lj_gc2_cycle_to_idle(g);
}

#if defined(LJ_GC2_TEST_HELPERS)
static void test_thread_needscan_wait_paused(uint32_t stage)
{
  uint32_t spin;
  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_gc2_test_thread_needscan_paused() == stage)
      return;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"thread NEEDSCAN race hook did not pause");
}

static void test_thread_needscan_exact_count_isolation(lua_State *L,
						       global_State *g)
{
  lua_State *counted, *stale_hint;
  uint32_t pending0 = gc2_thread_scan_needscan_pending_acq(g);

  assert(pending0 == 0);
  counted = lua_newthread(L);
  assert(counted != NULL);
  stale_hint = lua_newthread(L);
  assert(stale_hint != NULL);

  /* Model one exact counted state and a distinct stale header hint. Clearing
  ** the latter must not steal the former's aggregate membership. */
  lj_state_scan_needscan_counted_store_rlx(counted, 1u);
  lj_obj_addgcflags_atomic(obj2gco(counted), LJ_GC_NEEDSCAN);
  lj_obj_addgcflags_atomic(obj2gco(stale_hint), LJ_GC_NEEDSCAN);
  gc2_thread_scan_needscan_pending_inc(g);
  assert(lj_gc2_test_thread_needscan_clear(g, stale_hint) == 1);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0 + 1u);
  assert(lj_state_scan_needscan_counted_acq(counted) == 1u);
  assert(lj_state_scan_needscan_counted_acq(stale_hint) == 0u);
  assert((lj_obj_gcflags(obj2gco(stale_hint)) & LJ_GC_NEEDSCAN) == 0);

  assert(lj_gc2_test_thread_needscan_clear(g, counted) == 1);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0);
  assert(lj_state_scan_needscan_counted_acq(counted) == 0u);
  assert((lj_obj_gcflags(obj2gco(counted)) & LJ_GC_NEEDSCAN) == 0);
  lua_pop(L, 2);
}

static void test_thread_needscan_release_race(lua_State *L, global_State *g,
					       TGState *tg, uint32_t stage)
{
  lua_State *owner_L, *busy;
  TGState extra_tg;
  GCtab *busy_tab;
  WorkerDrainCtx ctx;
  pthread_t worker;
  uint64_t recovery0, claims0;
  uint32_t pending0, n_threads0, round;

  owner_L = lua_newthread(L);
  assert(owner_L != NULL);
  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  busy_tab = tabV(busy->top - 1);

  lj_tg_init_thread(g, &extra_tg, owner_L, 1);
  extra_tg.tid = tg->tid + 12000u + stage * 1000u;
  if (!lj_thr_id_is_owner(extra_tg.tid))
    extra_tg.tid = 12000u + stage;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  owner_L->tg_hint = &extra_tg;
  busy->tg_hint = &extra_tg;
  lj_state_owner_rel(owner_L, extra_tg.tid);
  lj_state_owner_rel(busy, extra_tg.tid);
  lua_pop(L, 2);  /* No registry/TValue root may mask release publication. */

  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  n_threads0 = gc2_n_threads_acq(g);
  lj_tg_attach(g, &extra_tg);
  pending0 = gc2_thread_scan_needscan_pending_acq(g);
  assert(pending0 == 0);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(busy)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);

  lj_gc2_test_thread_needscan_pause(stage);
  ctx.g = g;
  ctx.limit = 2;
  ctx.drained = 0;
  assert(pthread_create(&worker, NULL, grey_worker_drain_thread, &ctx) == 0);
  test_thread_needscan_wait_paused(stage);
  if (stage == LJ_GC2_THREAD_NEEDSCAN_TEST_BEFORE_SET) {
    assert(gc2_thread_scan_needscan_pending_acq(g) == pending0);
    assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  } else if (stage == LJ_GC2_THREAD_NEEDSCAN_TEST_INSTALLING) {
    assert(gc2_thread_scan_needscan_pending_acq(g) == pending0);
    assert(lj_state_scan_needscan_counted_acq(busy) == 2u);
    assert(lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN);
  } else {
    assert(stage == LJ_GC2_THREAD_NEEDSCAN_TEST_AFTER_SET);
    assert(gc2_thread_scan_needscan_pending_acq(g) == pending0 + 1u);
    assert(lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN);
  }

  gc2_mark_root_scanned_rel(g, 1);
  recovery0 = gc2_recovery_published_acq(g);
  /* This synthetic owner is deliberately not the raw TLS TG. Its exact state
  ** capability therefore takes the cold path, which unconditionally retains
  ** both releases even when the racing NEEDSCAN bit is not visible yet. */
  lj_state_release(owner_L, extra_tg.tid);
  lj_state_release(busy, extra_tg.tid);
  assert(gc2_recovery_published_acq(g) > recovery0);

  lj_gc2_test_thread_needscan_release();
  assert(pthread_join(worker, NULL) == 0);
  assert(ctx.drained != 0);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0 + 1u);
  assert(lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN);

  owner_L->tg_hint = tg;
  busy->tg_hint = tg;
  lj_tg_detach(g, &extra_tg);
  claims0 = la_load64_acq(&g->gc2.thread_scan_claims);
  for (round = 0; round < 64; round++)
    if (lj_gc2_fixpoint_round(g, L, ~(uint32_t)0))
      break;
  assert(round < 64);
  assert(la_load64_acq(&g->gc2.thread_scan_claims) > claims0);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0);
  assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 1);

  (void)lj_tg_reclaim_dead(g);
  assert(lj_tg_find_owner(g, extra_tg.tid) != &extra_tg);
  lj_tg_fini_thread(g, &extra_tg);
  assert(gc2_n_threads_acq(g) == n_threads0);
  lj_gc2_cycle_to_idle(g);
}

static void test_executable_root_rescan_dedup(lua_State *L, global_State *g,
					       TGState *tg)
{
  GCfunc *fn;
  GCtab *child, *filler;
  GCRef *next0;
  int top = lua_gettop(L);

  assert(luaL_dostring(L,
    "local child = { executable_rescan_child = true }\n"
    "return function() return child end, child\n") == LUA_OK);
  fn = funcV(L->top - 2);
  child = tabV(L->top - 1);
  assert(isluafunc(fn));
  lua_newtable(L);
  filler = tabV(L->top - 1);

  settle_automatic_cycle(g);
  lj_gc2_force_major(g);
  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(fn)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(lj_gc2_test_ssb_empty(g));

  /* Model an already-set uncounted token whose preserved queue suffix drained.
  ** At an empty frontier the compatibility repair republishes it exactly once. */
  lj_obj_addgcflags_atomic(obj2gco(fn), LJ_GC_NEEDSCAN);
  next0 = lj_tg_ssb_next_acq(tg);
  lj_gc2_test_thread_root_rescan_marked_obj(g, obj2gco(fn));
  assert(lj_tg_ssb_next_acq(tg) == next0 + 1);

  /* Repeated active FUNC/PROTO root discovery must treat that bit as exact
  ** same-cycle membership while any work is visible. The former force branch
  ** cleared/set/pushed the closure again on every root snapshot. */
  next0 = lj_tg_ssb_next_acq(tg);
  lj_gc2_test_thread_root_rescan_marked_obj(g, obj2gco(fn));
  assert(lj_tg_ssb_next_acq(tg) == next0);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_ssb_empty(g));
  assert(lj_obj_gcflags(obj2gco(fn)) & LJ_GC_NEEDSCAN);

  assert(lj_gc2_markobj(g, obj2gco(filler)) == 1);
  next0 = lj_tg_ssb_next_acq(tg);
  lj_gc2_test_thread_root_rescan_marked_obj(g, obj2gco(fn));
  assert(lj_tg_ssb_next_acq(tg) == next0);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);

  lj_gc2_cycle_to_idle(g);
  lua_settop(L, top);
}
#endif

static void test_thread_needscan_idle_clear(lua_State *L, global_State *g,
					    TGState *tg, int preserve_abort)
{
  lua_State *owner_L, *busy;
  TGState extra_tg;
  GCtab *busy_tab;
  uint64_t needscan0, requeues0, aborts0;
  uint32_t pending0, n_threads0, round;

  owner_L = lua_newthread(L);
  assert(owner_L != NULL);
  busy = lua_newthread(L);
  assert(busy != NULL);
  lua_newtable(busy);
  busy_tab = tabV(busy->top - 1);

  lj_tg_init_thread(g, &extra_tg, owner_L, 1);
  extra_tg.tid = tg->tid + (preserve_abort ? 9000u : 8000u);
  if (extra_tg.tid == 0 || extra_tg.tid == LJ_THREAD_GCSCAN)
    extra_tg.tid = preserve_abort ? 9000u : 8000u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  owner_L->tg_hint = &extra_tg;
  busy->tg_hint = &extra_tg;
  lj_state_owner_rel(owner_L, extra_tg.tid);
  lj_state_owner_rel(busy, extra_tg.tid);

  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  n_threads0 = g->gc2.n_threads;
  lj_tg_attach(g, &extra_tg);
  assert(lj_tg_find_owner(g, extra_tg.tid) == &extra_tg);
  assert(g->gc2.n_threads >= n_threads0);
  needscan0 = gc2_thread_scan_needscan_acq(g);
  requeues0 = gc2_thread_scan_requeues_acq(g);
  pending0 = gc2_thread_scan_needscan_pending_acq(g);
  assert(pending0 == 0);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(busy)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(lj_gc2_worker_drain(g, 2) != 0);
  assert(gc2_thread_scan_needscan_acq(g) == needscan0 + 1u);
  assert(gc2_thread_scan_requeues_acq(g) == requeues0 + 1u);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0 + 1u);
  assert(lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN);
  assert(lj_state_scan_handoff_epoch_acq(busy) == gc2_thread_scan_cycle_acq(g));
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 0);

  /*
  ** The synthetic owner TG only exists to make the worker publish NEEDSCAN.
  ** Detach it before the idle handshake; a forced/aborted close now carries
  ** that exact handoff into the replacement cycle instead of mutating an
  ** owner-local queue after publishing IDLE.
  */
  lj_state_owner_rel(owner_L, 0);
  lj_state_owner_rel(busy, 0);
  owner_L->tg_hint = tg;
  busy->tg_hint = tg;
  lj_tg_detach(g, &extra_tg);
  assert(lj_tg_flags_test_acq(&extra_tg, TGF_DEAD));
  (void)lj_tg_reclaim_dead(g);
  assert(lj_tg_find_owner(g, extra_tg.tid) != &extra_tg);
  lj_tg_fini_thread(g, &extra_tg);

  aborts0 = gc2_preserve_abort_to_idle_acq(g);
  if (preserve_abort) {
    lj_gc2_preserve_abort_to_idle(g);
    assert(gc2_preserve_abort_to_idle_acq(g) == aborts0 + 1u);
  } else {
    lj_gc2_cycle_to_idle(g);
    assert(gc2_preserve_abort_to_idle_acq(g) == aborts0);
  }
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_thread_scan_needscan_pending_acq(g) == pending0 + 1u);
  assert(lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN);
  assert(lj_state_scan_handoff_epoch_acq(busy) != 0);

  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  for (round = 0; round < 64; round++)
    if (lj_gc2_fixpoint_round(g, L, ~(uint32_t)0))
      break;
  assert(round < 64);
  assert(gc2_thread_scan_needscan_pending_acq(g) == 0);
  assert((lj_obj_gcflags(obj2gco(busy)) & LJ_GC_NEEDSCAN) == 0);
  assert(lj_state_scan_handoff_epoch_acq(busy) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(busy_tab)) == 1);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
}

static void test_userdata(lua_State *L, global_State *g)
{
  GCtab *env, *mt;
  GCudata *ud;

  lua_newtable(L);
  env = tabV(L->top - 1);
  lua_newuserdata(L, 1);
  ud = udataV(L->top - 1);
  lua_pushvalue(L, -2);
  lua_setfenv(L, -2);
  lua_newtable(L);
  mt = tabV(L->top - 1);
  lua_setmetatable(L, -2);

  lj_gc2_mark_begin(g);
  pin_mark_closed_for_worker_fixture(g);
  assert(lj_gc2_ismarked(g, obj2gco(env)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(mt)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(ud)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  /* Every userdata body is traversable now. Drain its queued payload work to
  ** completion without depending on whether an asynchronous worker wins the
  ** individual conversion first. */
  worker_drain_all(g);
  assert(lj_gc2_test_ssb_empty(g));
  assert(lj_gc2_ismarked(g, obj2gco(env)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(mt)) == 1);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 2);
}

static int gc2_empty_finalizer(lua_State *L)
{
  UNUSED(L);
  return 0;
}

static int gc2_udata_finalized;
static int gc2_cdata_finalized;
#if LJ_HASFFI
static int gc2_cdata_order[3];
static int gc2_cdata_order_count;
static int gc2_cdata_bulk_finalized;
#endif

static int gc2_counting_finalizer(lua_State *L)
{
  UNUSED(L);
  gc2_udata_finalized++;
  return 0;
}

static int gc2_cdata_counting_finalizer(lua_State *L)
{
  UNUSED(L);
  gc2_cdata_finalized++;
  return 0;
}

static void test_lib_register_weak_value_barrier(void)
{
  static const uint8_t init[] = {
    0, 0, 1,
    LIBINIT_COPY, 2,
    (uint8_t)(LIBINIT_STRING|4), 's', 'l', 'o', 't',
    LIBINIT_SET,
    LIBINIT_END
  };
  TGState *oldtg = lj_thr_get_tg();
  lua_State *L2 = luaL_newstate();
  global_State *g2;
  TGState *tg2;
  GCtab *mod, *val;
  uint64_t weak_vals0;

  assert(L2 != NULL);
  lua_gc(L2, LUA_GCSTOP, 0);
  lua_pushcfunction(L2, luaopen_base);
  lua_call(L2, 0, 1);
  lua_pop(L2, 1);
  lua_pushcfunction(L2, luaopen_package);
  lua_call(L2, 0, 1);
  lua_pop(L2, 1);
  g2 = G(L2);
  tg2 = G2TG(g2);
  assert(tg2 != NULL);
  lj_thr_set_tg(tg2);
  assert(luaL_dostring(L2,
    "package.loaded.m8lib = nil\n"
    "m8lib = setmetatable({}, { __mode = 'v' })\n"
    "package.loaded.m8lib = m8lib\n") == LUA_OK);
  lua_newtable(L2);
  val = tabV(L2->top - 1);
  lua_getglobal(L2, "m8lib");
  mod = tabV(L2->top - 1);

  lj_gc2_mark_begin(g2);
  assert(lj_gc2_markobj(g2, obj2gco(mod)) == 1);
  flush_and_drain(g2, tg2);
  assert(weak_snapshot_has(g2, mod));
  assert(lj_gc2_ismarked(g2, obj2gco(val)) == 0);
  lua_pop(L2, 1);

  weak_vals0 = gc2_weak_values_marked_acq(g2);
  enter_weak_clear_fixture(g2, tg2);
  lj_lib_register(L2, "m8lib", init, NULL);
  assert(tabV(L2->top - 1) == mod);
  assert(lj_gc2_ismarked(g2, obj2gco(val)) == 1);
  while (lj_gc2_test_weak_drain(g2, 1) != 0)
    ;
  lua_getfield(L2, -1, "slot");
  assert(tabV(L2->top - 1) == val);
  assert(gc2_weak_values_marked_acq(g2) >= weak_vals0);
  lj_gc2_cycle_to_idle(g2);
  lua_close(L2);
  lj_thr_set_tg(oldtg);
}

#if LJ_HASFFI
static int gc2_cdata_order_finalizer(lua_State *L, int id)
{
  UNUSED(L);
  assert(gc2_cdata_order_count < 3);
  gc2_cdata_order[gc2_cdata_order_count++] = id;
  return 0;
}

static int gc2_cdata_order_finalizer_1(lua_State *L)
{
  return gc2_cdata_order_finalizer(L, 1);
}

static int gc2_cdata_order_finalizer_2(lua_State *L)
{
  return gc2_cdata_order_finalizer(L, 2);
}

static int gc2_cdata_order_finalizer_3(lua_State *L)
{
  return gc2_cdata_order_finalizer(L, 3);
}

static int gc2_cdata_bulk_finalizer(lua_State *L)
{
  UNUSED(L);
  gc2_cdata_bulk_finalized++;
  return 0;
}

static void test_ffi_loaded_weak_value_barrier(void)
{
  TGState *oldtg = lj_thr_get_tg();
  lua_State *L2 = luaL_newstate();
  global_State *g2;
  TGState *tg2;
  GCtab *loaded, *mod;
  assert(L2 != NULL);
  lua_gc(L2, LUA_GCSTOP, 0);
  lua_pushcfunction(L2, luaopen_base);
  lua_call(L2, 0, 1);
  lua_pop(L2, 1);
  lua_pushcfunction(L2, luaopen_package);
  lua_call(L2, 0, 1);
  lua_pop(L2, 1);
  g2 = G(L2);
  tg2 = G2TG(g2);
  assert(tg2 != NULL);
  lj_thr_set_tg(tg2);
  assert(luaL_dostring(L2,
    "package.loaded.ffi = nil\n"
    "setmetatable(package.loaded, { __mode = 'v' })\n") == LUA_OK);
  lua_getglobal(L2, "package");
  lua_getfield(L2, -1, "loaded");
  loaded = tabV(L2->top - 1);

  lj_gc2_mark_begin(g2);
  assert(lj_gc2_markobj(g2, obj2gco(loaded)) == 1);
  flush_and_drain(g2, tg2);
  assert(lj_gc2_test_weak_snapshot_count(g2) >= 1u);
  lua_settop(L2, 0);

  enter_weak_clear_fixture(g2, tg2);
  lua_pushcfunction(L2, luaopen_ffi);
  lua_call(L2, 0, 1);
  mod = tabV(L2->top - 1);
  assert(lj_gc2_ismarked(g2, obj2gco(mod)) == 1);
  while (lj_gc2_test_weak_drain(g2, 1) != 0)
    ;
  lua_getglobal(L2, "package");
  lua_getfield(L2, -1, "loaded");
  lua_getfield(L2, -1, "ffi");
  assert(tabV(L2->top - 1) == mod);
  lj_gc2_cycle_to_idle(g2);
  lua_close(L2);
  lj_thr_set_tg(oldtg);
}
#endif

static void test_finalizer_spawn_deferred_state(lua_State *L, global_State *g)
{
  uint64_t deferrals0 = gc2_finalizer_spawn_deferrals_acq(g);
  uint64_t releasewakes0 = gc2_finalizer_spawn_release_wakes_acq(g);

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "local th = require('threading')\n"
    "ffi.cdef('typedef struct { int x; } gc2_spawn_defer_t;')\n"
    "gc2_spawn_started = th.channel(1)\n"
    "gc2_spawn_release = th.channel(1)\n"
    "gc2_spawn_worker = nil\n"
    "do\n"
    "  local cd = ffi.gc(ffi.new('gc2_spawn_defer_t'), function()\n"
    "    gc2_spawn_worker = th.spawn(function(started, release)\n"
    "      started:send(collectgarbage('isrunning') and 'running' or 'stopped')\n"
    "      local msg, ok = release:recv(10)\n"
    "      return ok == true and msg == 'release'\n"
    "    end, gc2_spawn_started, gc2_spawn_release)\n"
    "  end)\n"
    "end\n") == LUA_OK);

  /* This fixture normally runs with automatic collection stopped. Make the
  ** logical pre-callback state explicit so the worker-side threshold check
  ** distinguishes GC2's temporary suppression from a user-requested stop. */
  lua_gc(L, LUA_GCRESTART, -1);
  assert(lj_gc_threshold_load(g) != LJ_MAX_MEM);
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  assert((gc2_finalizer_spawn_latch_acq(g) &
	  LJ_GC2_FINSPAWN_DEFERRED) != 0);
  assert(mt_live_acq(g) != 0);
  assert(mt_gc_exclusive_acq(g) == 0);
  assert(lj_gc_threshold_load(g) == LJ_MAX_MEM);
  assert(gc2_finalizer_spawn_deferrals_acq(g) > deferrals0);

  assert(luaL_dostring(L,
    "local msg, ok = gc2_spawn_started:recv(1)\n"
    "assert(ok == true and msg == 'running')\n"
    "assert(gc2_spawn_release:send('release', 1) == true)\n"
    "local joined, result = gc2_spawn_worker:join(10)\n"
    "assert(joined == true and result == true, tostring(result))\n"
    "gc2_spawn_worker = nil\n"
    "gc2_spawn_started = nil\n"
    "gc2_spawn_release = nil\n") == LUA_OK);
  assert(mt_live_acq(g) == 0);
  assert((gc2_finalizer_spawn_latch_acq(g) &
	  LJ_GC2_FINSPAWN_DEFERRED) != 0);
  assert(gc2_finalizer_spawn_release_wakes_acq(g) > releasewakes0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_finalizer_spawn_latch_acq(g) == 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
}

static void drive_udata_finalizers(lua_State *L)
{
  int i;
  for (i = 0; i < 4; i++)
    lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
}

static void push_udata_finalizer_mt(lua_State *L)
{
  lua_newtable(L);
  lua_pushcfunction(L, gc2_empty_finalizer);
  lua_setfield(L, -2, "__gc");
}

static int test_unlink_udata_object(global_State *g, GCobj *target)
{
  return lj_gc_unlink_root_obj(g, target) == LJ_GC_ROOT_UNLINKED;
}

static uint32_t finreg_udata_active_nodes(global_State *g)
{
  GC2FinRegUDataNode *node;
  uint32_t n = 0;
  for (node = gc2_finreg_udata_head_acq(g);
       node != NULL && lj_gc2_mem_registered(g, node);
       node = gc2_finreg_udata_next_acq(node))
    n += gc2_finreg_udata_active_acq(node) != 0;
  return n;
}

static uint32_t finreg_udata_active_refs(global_State *g, GCobj *target)
{
  GC2FinRegUDataNode *node;
  uint32_t n = 0;
  for (node = gc2_finreg_udata_head_acq(g);
       node != NULL && lj_gc2_mem_registered(g, node);
       node = gc2_finreg_udata_next_acq(node))
    n += gc2_finreg_udata_active_acq(node) &&
	 gc2_finreg_udata_obj_acq(node) == target;
  return n;
}

static GC2FinRegUDataNode *finreg_udata_active_node(global_State *g,
						     GCobj *target)
{
  GC2FinRegUDataNode *node;
  for (node = gc2_finreg_udata_head_acq(g);
       node != NULL && lj_gc2_mem_registered(g, node);
       node = gc2_finreg_udata_next_acq(node))
    if (gc2_finreg_udata_active_acq(node) &&
	gc2_finreg_udata_obj_acq(node) == target)
      return node;
  return NULL;
}

static void finreg_udata_mark_other_active(global_State *g, GCobj *target)
{
  GC2FinRegUDataNode *node;
  for (node = gc2_finreg_udata_head_acq(g);
       node != NULL && lj_gc2_mem_registered(g, node);
       node = gc2_finreg_udata_next_acq(node)) {
    GCobj *o;
    if (!gc2_finreg_udata_active_acq(node))
      continue;
    o = gc2_finreg_udata_obj_acq(node);
    if (o && o != target)
      (void)lj_gc2_markobj(g, o);
  }
}

static uint32_t finreg_test_small_root_state(GCobj *o)
{
  GCArena *a = lj_arena_of(o);
  uint32_t cell;
  assert(!lj_arena_ishuge(a));
  cell = lj_arena_cellof(o);
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  return lj_arena_root_state_acq(a, cell);
}

static void test_finreg_userdata_dispatch_defer_retry(lua_State *L,
						       global_State *g)
{
  GCobj *o;
  GCArena *a;
  uint32_t cell;
  uint64_t queued0, dequeued0;
  GCSize cost = 0;
  int finalized0;

  lua_settop(L, 0);
  lua_newuserdata(L, 1);
  o = obj2gco(udataV(L->top - 1));
  lua_newtable(L);
  lua_pushcfunction(L, gc2_counting_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  (void)lj_gc_flush_root_pending(g);

  lj_gc2_mark_begin(g);
  finreg_udata_mark_other_active(g, o);
  assert(lj_gc2_ismarked(g, o) == 0);
  assert(lj_gc2_finreg_udata_finalize(g, 0) != 0);
  assert(finreg_test_small_root_state(o) == LJ_ARENA_ROOT_NONE);
  assert(lj_gc2_finalizer_close_pending(g));

  a = lj_arena_of(o);
  cell = lj_arena_cellof(o);
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_NONE,
					 LJ_ARENA_ROOT_LINKING));
  queued0 = gc2_finalizer_queued_acq(g);
  dequeued0 = gc2_finalizer_dequeued_acq(g);
  finalized0 = gc2_udata_finalized;

  /* Root publication loses immediately. Dispatch must retain the exact FIFO
  ** node and callback without allocation, clearing FINREG, or charging a
  ** dequeue. */
  assert(lj_gc2_finalizer_step(L, 1, &cost) == -1);
  assert(cost == LJ_MAX_MEM);
  assert(gc2_finalizer_queued_acq(g) == queued0);
  assert(gc2_finalizer_dequeued_acq(g) == dequeued0);
  assert(lj_gc2_finalizer_close_pending(g));
  assert((lj_obj_gcflags(o) & LJ_GC_UDATA_FINREG) != 0);
  assert(gc2_udata_finalized == finalized0);
  assert(finreg_test_small_root_state(o) == LJ_ARENA_ROOT_LINKING);

  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_LINKING,
					 LJ_ARENA_ROOT_NONE));
  cost = 0;
  assert(lj_gc2_finalizer_step(L, 1, &cost) == 1);
  assert(gc2_finalizer_queued_acq(g) == queued0);
  assert(gc2_finalizer_dequeued_acq(g) == dequeued0 + 1u);
  assert(gc2_udata_finalized == finalized0 + 1);
  assert((lj_obj_gcflags(o) & LJ_GC_UDATA_FINREG) == 0);
  assert(finreg_test_small_root_state(o) == LJ_ARENA_ROOT_MEMBER);

  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

static void test_finreg_userdata_unproven_unlink_retry(lua_State *L,
							global_State *g)
{
  GCobj *o, *saved_root;
  GCstr *invalid;
  uint64_t discovered0, queued0, finalizerq0;
  int finalized0;

  lua_settop(L, 0);
  lua_newuserdata(L, 1);
  o = obj2gco(udataV(L->top - 1));
  lua_newtable(L);
  lua_pushcfunction(L, gc2_counting_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  (void)lj_gc_flush_root_pending(g);
  saved_root = lj_gc_root_acq(g);
  assert(saved_root != NULL);
  invalid = lj_str_newlit(L, "udata-finreg-unproven-root-sentinel");
  assert(finreg_udata_active_refs(g, o) == 1u);

  discovered0 = gc2_finreg_udata_discovered_acq(g);
  queued0 = gc2_finreg_udata_queued_acq(g);
  finalizerq0 = gc2_finalizer_queued_acq(g);
  finalized0 = gc2_udata_finalized;
  lj_gc2_mark_begin(g);
  finreg_udata_mark_other_active(g, o);
  assert(lj_gc2_ismarked(g, o) == 0);

  lj_gc_root_rel(g, obj2gco(invalid));
  lj_gcroot_repair_epoch_add(g);
  assert(lj_gc2_finreg_udata_finalize(g, 0) == 0);
  assert(lj_gc_root_acq(g) == NULL);
  assert(finreg_udata_active_refs(g, o) == 1u);
  assert(gc2_finreg_udata_discovered_acq(g) == discovered0);
  assert(gc2_finreg_udata_queued_acq(g) == queued0);
  assert(gc2_finalizer_queued_acq(g) == finalizerq0);
  assert((lj_obj_gcflags(o) & LJ_GC_UDATA_FINREG) != 0);
  assert((lj_obj_gcflags(o) & LJ_GC_FINALIZED) == 0);
  assert(lj_gc2_ismarked(g, o) == 1);
  assert(lj_gc2_finalizer_close_pending(g));

  lj_gc_root_rel(g, saved_root);
  lj_gcroot_repair_epoch_add(g);
  lj_gc2_cycle_to_idle(g);
  lj_gc2_mark_begin(g);
  finreg_udata_mark_other_active(g, o);
  assert(lj_gc2_ismarked(g, o) == 0);
  assert(lj_gc2_finreg_udata_finalize(g, 0) != 0);
  assert(finreg_udata_active_refs(g, o) == 0);
  assert(gc2_finreg_udata_discovered_acq(g) == discovered0 + 1u);
  assert(gc2_finreg_udata_queued_acq(g) == queued0 + 1u);
  assert(gc2_finalizer_queued_acq(g) == finalizerq0 + 1u);
  lj_gc2_finalizer_dispatch_all(L);
  assert(gc2_udata_finalized == finalized0 + 1);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

static void test_finreg_userdata_lookup_retry(lua_State *L,
					       global_State *g)
{
  GC2FinRegUDataNode *finode;
  GCobj *o;
  GCtab *mt;
  Node *mtnode;
  uint32_t mtflags;
  uint64_t clears0, discovered0, queued0, finalizerq0;
  int finalized0;
  size_t separated;

  lua_settop(L, 0);
  lua_newuserdata(L, 1);
  o = obj2gco(udataV(L->top - 1));
  lua_newtable(L);
  mt = tabV(L->top - 1);
  lua_pushcfunction(L, gc2_counting_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  (void)lj_gc_flush_root_pending(g);
  finode = finreg_udata_active_node(g, o);
  assert(finode != NULL);

  clears0 = gc2_finreg_udata_clears_acq(g);
  discovered0 = gc2_finreg_udata_discovered_acq(g);
  queued0 = gc2_finreg_udata_queued_acq(g);
  finalizerq0 = gc2_finalizer_queued_acq(g);
  finalized0 = gc2_udata_finalized;

  lj_gc2_mark_begin(g);
  finreg_udata_mark_other_active(g, o);
  assert(lj_gc2_ismarked(g, o) == 0);
  assert(lj_gc2_ismarkedmem(g, finode) == 0);
  lj_gc2_mark_to_weak(g);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  /* Model a completed WEAK root certificate immediately before finalizer
  ** discovery. A structural table transient must revoke both halves. */
  gc2_weak_root_scanned_rel(g, 1);
  gc2_weak_mark_closed_rel(g, 1);

  mtnode = lj_tab_node_acq(mt);
  assert(mtnode != NULL && mtnode != &g->nilnode);
  mtflags = lj_tab_node_hdr_flags_word_acq(mtnode);
  assert((mtflags & (uint32_t)TABNODE_FLAG_RETIRING) == 0);
  lj_tab_node_hdr_flags_or_rel(mtnode, TABNODE_FLAG_RETIRING);
  separated = lj_gc2_finreg_udata_finalize(g, 0);
  la_store32_rel(&lj_tab_node_hdrw(mtnode)->flags, mtflags);

  assert(separated == 0);
  assert(gc2_weak_root_scanned_acq(g) == 0);
  assert(gc2_weak_mark_closed_acq(g) == 0);
  assert(lj_gc2_ismarked(g, o) == 1);
  assert(lj_gc2_ismarkedmem(g, finode) == 1);
  assert(finreg_udata_active_refs(g, o) == 1u);
  assert(gc2_finreg_udata_clears_acq(g) == clears0);
  assert(gc2_finreg_udata_discovered_acq(g) == discovered0);
  assert(gc2_finreg_udata_queued_acq(g) == queued0);
  assert(gc2_finalizer_queued_acq(g) == finalizerq0);
  assert((lj_obj_gcflags(o) & LJ_GC_UDATA_FINREG) != 0);
  assert((lj_obj_gcflags(o) & LJ_GC_FINALIZED) == 0);
  assert(gc2_udata_finalized == finalized0);

  /* RETRY deliberately retained this incarnation for the cycle. Once the
  ** transient clears, the next cycle must discover and dispatch the original
  ** __gc instead of converting the retry into a negative lookup. */
  lj_gc2_cycle_to_idle(g);
  lj_gc2_mark_begin(g);
  finreg_udata_mark_other_active(g, o);
  assert(lj_gc2_ismarked(g, o) == 0);
  assert(lj_gc2_finreg_udata_finalize(g, 0) != 0);
  assert(finreg_udata_active_refs(g, o) == 0);
  assert(gc2_finreg_udata_clears_acq(g) == clears0);
  assert(gc2_finreg_udata_discovered_acq(g) == discovered0 + 1u);
  assert(gc2_finreg_udata_queued_acq(g) == queued0 + 1u);
  assert(gc2_finalizer_queued_acq(g) == finalizerq0 + 1u);
  lj_gc2_finalizer_dispatch_all(L);
  assert(gc2_udata_finalized == finalized0 + 1);
  assert(gc2_finreg_udata_clears_acq(g) == clears0 + 1u);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

static void test_finreg_userdata_active_unlink(lua_State *L, global_State *g)
{
  uint32_t active0;
  uint64_t registered0, retired0, queued0;
  GCobj *o;

  lua_settop(L, 0);
  active0 = finreg_udata_active_nodes(g);
  registered0 = gc2_finreg_udata_registered_acq(g);
  retired0 = gc2_finreg_udata_retired_nodes_acq(g);
  queued0 = gc2_finreg_udata_queued_acq(g);

  lua_newuserdata(L, 1);
  o = obj2gco(udataV(L->top - 1));
  push_udata_finalizer_mt(L);
  lua_setmetatable(L, -2);
  assert(finreg_udata_active_nodes(g) == active0 + 1u);
  assert(finreg_udata_active_refs(g, o) == 1u);
  assert(gc2_finreg_udata_registered_acq(g) ==
	 registered0 + 1u);

  lua_pushnil(L);
  lua_setmetatable(L, -2);
  assert(finreg_udata_active_nodes(g) == active0);
  assert(finreg_udata_active_refs(g, o) == 0);
  assert(gc2_finreg_udata_retired_nodes_acq(g) ==
	 retired0 + 1u);

  push_udata_finalizer_mt(L);
  lua_setmetatable(L, -2);
  assert(finreg_udata_active_nodes(g) == active0 + 1u);
  assert(finreg_udata_active_refs(g, o) == 1u);
  assert(gc2_finreg_udata_registered_acq(g) ==
	 registered0 + 2u);

  lua_pop(L, 1);
  drive_udata_finalizers(L);
  assert(finreg_udata_active_nodes(g) <= active0);
  assert(finreg_udata_active_refs(g, o) == 0);
  assert(gc2_finreg_udata_retired_nodes_acq(g) >=
	 retired0 + 2u);
  assert(gc2_finreg_udata_queued_acq(g) == queued0 + 1u);
}

static void test_finreg_userdata_telemetry(lua_State *L, global_State *g)
{
  uint64_t sets0 = gc2_finreg_udata_sets_acq(g);
  uint64_t clears0 = gc2_finreg_udata_clears_acq(g);
  uint64_t queued0 = gc2_finreg_udata_queued_acq(g);
  uint64_t registered0 = gc2_finreg_udata_registered_acq(g);
  uint64_t discovered0 = gc2_finreg_udata_discovered_acq(g);
  uint64_t forgets0 = gc2_finreg_udata_forgets_acq(g);

  lua_settop(L, 0);
  lua_newuserdata(L, 1);
  push_udata_finalizer_mt(L);
  lua_setmetatable(L, -2);
  assert(gc2_finreg_udata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_udata_registered_acq(g) ==
	 registered0 + 1u);
  lua_pushnil(L);
  lua_setmetatable(L, -2);
  assert(gc2_finreg_udata_clears_acq(g) == clears0 + 1u);
  assert(gc2_finreg_udata_forgets_acq(g) == forgets0 + 1u);
  push_udata_finalizer_mt(L);
  lua_setmetatable(L, -2);
  assert(gc2_finreg_udata_sets_acq(g) == sets0 + 2u);
  assert(gc2_finreg_udata_registered_acq(g) ==
	 registered0 + 2u);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_finreg_udata_queued_acq(g) == queued0 + 1u);
  assert(gc2_finreg_udata_clears_acq(g) == clears0 + 2u);
  assert(gc2_finreg_udata_discovered_acq(g) ==
	 discovered0 + 1u);

  lua_settop(L, 0);
  lua_newuserdata(L, 1);
  push_udata_finalizer_mt(L);
  lua_setmetatable(L, -2);
  assert(gc2_finreg_udata_sets_acq(g) == sets0 + 3u);
  assert(gc2_finreg_udata_registered_acq(g) ==
	 registered0 + 3u);
  lua_getmetatable(L, -1);
  lua_pushnil(L);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 2);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_finreg_udata_queued_acq(g) == queued0 + 1u);
  assert(gc2_finreg_udata_clears_acq(g) == clears0 + 3u);
  assert(gc2_finreg_udata_discovered_acq(g) ==
	 discovered0 + 1u);

  lua_settop(L, 0);
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_setmetatable(L, -2);
  assert(gc2_finreg_udata_sets_acq(g) == sets0 + 3u);
  assert(gc2_finreg_udata_registered_acq(g) ==
	 registered0 + 4u);
  lua_getmetatable(L, -1);
  lua_pushcfunction(L, gc2_empty_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 2);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_finreg_udata_sets_acq(g) == sets0 + 4u);
  assert(gc2_finreg_udata_queued_acq(g) == queued0 + 2u);
  assert(gc2_finreg_udata_clears_acq(g) == clears0 + 4u);
  assert(gc2_finreg_udata_registered_acq(g) ==
	 registered0 + 4u);
  assert(gc2_finreg_udata_discovered_acq(g) ==
	 discovered0 + 2u);

  lua_settop(L, 0);
  lua_newuserdata(L, 1);
  push_udata_finalizer_mt(L);
  lua_setmetatable(L, -2);
  assert(gc2_finreg_udata_sets_acq(g) == sets0 + 5u);
  assert(gc2_finreg_udata_registered_acq(g) ==
	 registered0 + 5u);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(test_unlink_udata_object(g, obj2gco(udataV(L->top - 1))));
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_finreg_udata_queued_acq(g) == queued0 + 3u);
  assert(gc2_finreg_udata_clears_acq(g) == clears0 + 5u);
  assert(gc2_finreg_udata_discovered_acq(g) ==
	 discovered0 + 3u);
}

static void test_finreg_internal_userdata_telemetry(lua_State *L,
						    global_State *g)
{
  uint64_t sets0 = gc2_finreg_udata_sets_acq(g);
  uint64_t clears0 = gc2_finreg_udata_clears_acq(g);
  uint64_t queued0 = gc2_finreg_udata_queued_acq(g);
  uint64_t registered0 = gc2_finreg_udata_registered_acq(g);
  uint64_t discovered0 = gc2_finreg_udata_discovered_acq(g);
  lua_Integer registered, immediate, discoverable, lazy;

  lua_settop(L, 0);
  lua_pushcfunction(L, gc2_empty_finalizer);
  lua_setglobal(L, "gc2_internal_udata_finalizer");
  assert(luaL_dostring(L,
    "local registered, immediate, discoverable, lazy = 0, 0, 0, 0\n"
    "do\n"
    "  local f = assert(io.tmpfile())\n"
    "  registered = registered + 1\n"
    "  immediate = immediate + 1\n"
    "  discoverable = discoverable + 1\n"
    "end\n"
    "do\n"
    "  local ok, buffer = pcall(require, 'buffer')\n"
    "  if ok and buffer and buffer.new then\n"
    "    local b = buffer.new()\n"
    "    registered = registered + 1\n"
    "    immediate = immediate + 1\n"
    "    discoverable = discoverable + 1\n"
    "  end\n"
    "end\n"
    "do\n"
    "  local ok, threading = pcall(require, 'threading')\n"
    "  if ok and threading and threading.mutex and threading.channel then\n"
    "    local m = threading.mutex()\n"
    "    registered = registered + 1\n"
    "    debug.getmetatable(m).__gc = gc2_internal_udata_finalizer\n"
    "    discoverable = discoverable + 1\n"
    "    lazy = lazy + 1\n"
    "    local ch = threading.channel(1)\n"
    "    registered = registered + 1\n"
    "    immediate = immediate + 1\n"
    "    discoverable = discoverable + 1\n"
    "  end\n"
    "end\n"
#if LJ_HASFFI
    "do\n"
    "  local ok, ffi = pcall(require, 'ffi')\n"
    "  if ok and ffi then\n"
    "    registered = registered + 1 -- ffi.C default namespace.\n"
    "    immediate = immediate + 1\n"
    "  end\n"
    "end\n"
#endif
    "return registered, immediate, discoverable, lazy\n") ==
    LUA_OK);
  registered = lua_tointeger(L, -4);
  immediate = lua_tointeger(L, -3);
  discoverable = lua_tointeger(L, -2);
  lazy = lua_tointeger(L, -1);
  lua_pop(L, 4);
  assert(registered >= discoverable);
  assert(discoverable >= 3);
  assert(gc2_finreg_udata_sets_acq(g) ==
	 sets0 + (uint64_t)immediate);
  assert(gc2_finreg_udata_registered_acq(g) ==
	 registered0 + (uint64_t)registered);

  drive_udata_finalizers(L);
  assert(gc2_finreg_udata_sets_acq(g) ==
	 sets0 + (uint64_t)(immediate + lazy));
  assert(gc2_finreg_udata_queued_acq(g) ==
	 queued0 + (uint64_t)discoverable);
  assert(gc2_finreg_udata_clears_acq(g) ==
	 clears0 + (uint64_t)discoverable);
  assert(gc2_finreg_udata_discovered_acq(g) ==
	 discovered0 + (uint64_t)discoverable);
  lua_pushnil(L);
  lua_setglobal(L, "gc2_internal_udata_finalizer");
}

static void test_finreg_userdata_inplace_finalizer_behavior(lua_State *L)
{
  int finalized0;

  lua_settop(L, 0);
  finalized0 = gc2_udata_finalized;
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_setmetatable(L, -2);
  lua_getmetatable(L, -1);
  lua_pushcfunction(L, gc2_counting_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 2);
  drive_udata_finalizers(L);
  assert(gc2_udata_finalized == finalized0 + 1);

  lua_settop(L, 0);
  finalized0 = gc2_udata_finalized;
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, gc2_counting_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_getmetatable(L, -1);
  lua_pushnil(L);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 2);
  drive_udata_finalizers(L);
  assert(gc2_udata_finalized == finalized0);
}

static void test_finreg_userdata_queue_mark(lua_State *L, global_State *g,
					    TGState *tg)
{
  GCtab *env, *mt;
  GCudata *ud;
  uint64_t queued0;

  lua_settop(L, 0);
  lua_newtable(L);
  env = tabV(L->top - 1);
  lua_newuserdata(L, 1);
  ud = udataV(L->top - 1);
  lua_pushvalue(L, 1);
  lua_setfenv(L, -2);
  push_udata_finalizer_mt(L);
  mt = tabV(L->top - 1);
  lua_setmetatable(L, -2);
  lua_settop(L, 0);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(ud)) == 0);
  queued0 = gc2_finreg_udata_queued_acq(g);
  lj_gc2_test_finreg_udata_queue(g, obj2gco(ud));
  assert(gc2_finreg_udata_queued_acq(g) == queued0 + 1u);
  assert(lj_gc2_ismarked(g, obj2gco(ud)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(env)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(mt)) == 1);
  lj_gc2_cycle_to_idle(g);
  setudataV(L, L->top++, ud);
  lua_pushnil(L);
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
}

#if LJ_HASFFI
static int test_unlink_root_object(global_State *g, GCobj *target)
{
  return lj_gc_unlink_root_obj(g, target) == LJ_GC_ROOT_UNLINKED;
}

#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
static void test_finreg_cdata_preclaim_publish_order(lua_State *L,
						     global_State *g)
{
  FinclaimPublishCtx ctx;
  pthread_t thread;
  GCcdata *cd;
  TValue fin;
  uint64_t claimed0, dispatched0;

  lua_settop(L, 0);
  cd = lj_cdata_new_(L, CTID_INT32, 4);
  setcdataV(L, L->top++, cd);
  lua_pushcfunction(L, gc2_cdata_counting_finalizer);

  memset(&ctx, 0, sizeof(ctx));
  ctx.L = L;
  ctx.g = g;
  ctx.o = obj2gco(cd);
  copyTV(L, &ctx.fin, L->top - 1);

  claimed0 = gc2_finreg_cdata_pweak_claimed_acq(g);
  dispatched0 = gc2_finreg_cdata_preclaim_dispatched_acq(g);
  lj_gc2_test_finreg_cdata_preclaim_publish_pause(g);
  assert(pthread_create(&thread, NULL, finclaim_publish_thread, &ctx) == 0);
  while (gc2_finreg_cdata_preclaim_publish_paused_acq(g) == 0)
    la_cpu_pause();

  assert(!lj_gc2_test_finreg_cdata_preclaim_take(L, g, obj2gco(cd), &fin));
  assert(gc2_finreg_cdata_pweak_claimed_acq(g) == claimed0);
  assert(gc2_finreg_cdata_preclaim_dispatched_acq(g) ==
	 dispatched0);

  gc2_finreg_cdata_preclaim_publish_release_rel(g, 1);
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.ok);
  assert(gc2_finreg_cdata_pweak_claimed_acq(g) == claimed0 + 1u);
  assert(lj_gc2_test_finreg_cdata_preclaim_take(L, g, obj2gco(cd), &fin));
  assert(tvisfunc(&fin));
  assert(gc2_finreg_cdata_preclaim_dispatched_acq(g) ==
	 dispatched0 + 1u);
  lua_settop(L, 0);
}
#endif

static uint32_t finreg_cdata_order_active_refs(global_State *g, GCobj *target)
{
  CTState *cts = ctype_ctsG(g);
  FinRegOrderNode *ord;
  uint32_t n = 0;
  if (!cts)
    return 0;
  for (ord = fin_order_head_acq(cts);
       ord != NULL && lj_gc2_mem_registered(g, ord);
       ord = fin_order_next_acq(ord))
    n += fin_order_active_acq(ord) == 1 && fin_order_obj_acq(ord) == target;
  return n;
}

static FinRegOrderNode *finreg_cdata_order_active_node(global_State *g,
						       GCobj *target)
{
  CTState *cts = ctype_ctsG(g);
  FinRegOrderNode *ord;
  if (!cts)
    return NULL;
  for (ord = fin_order_head_acq(cts);
       ord != NULL && lj_gc2_mem_registered(g, ord);
       ord = fin_order_next_acq(ord))
    if (fin_order_active_acq(ord) == 1 && fin_order_obj_acq(ord) == target)
      return ord;
  return NULL;
}

static void finreg_cdata_test_mark(global_State *g, cTValue *tv)
{
  UNUSED(g);
  UNUSED(tv);  /* The fixture keeps the C finalizer in a global Lua root. */
}

static void test_finreg_cdata_slot_retry_reopens_weak(lua_State *L,
						       global_State *g)
{
  FinRegOrderNode *ord;
  GCcdata *cd;
  GCobj *o;
  GCtab *fintab;
  Node *finnode;
  uint32_t finflags;
  uint64_t claimed0, orderq0, finalizerq0;
  int finalized0;

  lua_settop(L, 0);
  lua_pushcfunction(L, gc2_cdata_counting_finalizer);
  lua_setglobal(L, "gc2_cdata_counting_finalizer");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "return ffi.gc(ffi.new('char[?]', 8), gc2_cdata_counting_finalizer)\n") ==
    LUA_OK);
  cd = cdataV(L->top - 1);
  o = obj2gco(cd);
  (void)lj_gc_flush_root_pending(g);
  ord = finreg_cdata_order_active_node(g, o);
  assert(ord != NULL);
  fintab = fin_order_tab_acq(ord);
  assert(fintab != NULL);
  finnode = lj_tab_node_acq(fintab);
  assert(finnode != NULL && finnode != &g->nilnode);
  finflags = lj_tab_node_hdr_flags_word_acq(finnode);
  assert((finflags & (uint32_t)TABNODE_FLAG_RETIRING) == 0);

  claimed0 = gc2_finreg_cdata_order_claimed_acq(g);
  orderq0 = gc2_finreg_cdata_order_queued_acq(g);
  finalizerq0 = gc2_finalizer_queued_acq(g);
  finalized0 = gc2_cdata_finalized;
  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, o) == 0);
  lj_gc2_mark_to_weak(g);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  gc2_weak_root_scanned_rel(g, 1);
  gc2_weak_mark_closed_rel(g, 1);

  /* A transient FIN-table generation is neither MISS nor a live candidate
  ** skip. P_WEAK must reopen its exact certificate without consuming the
  ** order node, slot, flags, or callback. */
  lj_tab_node_hdr_flags_or_rel(finnode, TABNODE_FLAG_RETIRING);
  assert(lj_gc2_finreg_cdata_finalize_pweak(
	L, g, finreg_cdata_test_mark) == 0);
  la_store32_rel(&lj_tab_node_hdrw(finnode)->flags, finflags);
  assert(gc2_weak_root_scanned_acq(g) == 0);
  assert(gc2_weak_mark_closed_acq(g) == 0);
  assert(finreg_cdata_order_active_refs(g, o) == 1u);
  assert(gc2_finreg_cdata_order_claimed_acq(g) == claimed0);
  assert(gc2_finreg_cdata_order_queued_acq(g) == orderq0);
  assert(gc2_finalizer_queued_acq(g) == finalizerq0);
  assert((lj_obj_gcflags(o) & LJ_GC_CDATA_FIN) != 0);
  assert((lj_obj_gcflags(o) & LJ_GC_FINALIZED) == 0);
  assert(gc2_cdata_finalized == finalized0);

  lj_gc2_cycle_to_idle(g);
  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, o) == 0);
  assert(lj_gc2_finreg_cdata_finalize_pweak(
	L, g, finreg_cdata_test_mark) == 1u);
  assert(finreg_cdata_order_active_refs(g, o) == 0);
  assert(gc2_finreg_cdata_order_claimed_acq(g) == claimed0 + 1u);
  assert(gc2_finreg_cdata_order_queued_acq(g) == orderq0 + 1u);
  assert(gc2_finalizer_queued_acq(g) == finalizerq0 + 1u);
  lj_gc2_finalizer_dispatch_all(L);
  assert(gc2_cdata_finalized == finalized0 + 1);
  lj_gc2_cycle_to_idle(g);
  lua_pushnil(L);
  lua_setglobal(L, "gc2_cdata_counting_finalizer");
  lua_settop(L, 0);
}

static void test_finreg_cdata_unproven_unlink_retry(lua_State *L,
						     global_State *g)
{
  GCcdata *cd;
  GCobj *o, *saved_root;
  GCstr *invalid;
  uint64_t claimed0, orderq0, finalizerq0;
  int finalized0;

  lua_settop(L, 0);
  lua_pushcfunction(L, gc2_cdata_counting_finalizer);
  lua_setglobal(L, "gc2_cdata_counting_finalizer");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "return ffi.gc(ffi.new('char[?]', 8), gc2_cdata_counting_finalizer)\n") ==
    LUA_OK);
  cd = cdataV(L->top - 1);
  o = obj2gco(cd);
  (void)lj_gc_flush_root_pending(g);
  saved_root = lj_gc_root_acq(g);
  assert(saved_root != NULL);
  invalid = lj_str_newlit(L, "finreg-unproven-root-sentinel");
  assert(finreg_cdata_order_active_refs(g, o) == 1u);

  claimed0 = gc2_finreg_cdata_order_claimed_acq(g);
  orderq0 = gc2_finreg_cdata_order_queued_acq(g);
  finalizerq0 = gc2_finalizer_queued_acq(g);
  finalized0 = gc2_cdata_finalized;
  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, o) == 0);

  /* An inadmissible root entry makes target absence UNPROVEN. Discovery must
  ** restore the claimed finalizer slot and retain the active order node, not
  ** enqueue an object which may still have an intrusive root membership. */
  lj_gc_root_rel(g, obj2gco(invalid));
  lj_gcroot_repair_epoch_add(g);
  assert(lj_gc2_finreg_cdata_finalize_pweak(
	L, g, finreg_cdata_test_mark) == 0);
  assert(lj_gc_root_acq(g) == NULL);  /* Shared unlinker severed invalid head. */
  assert(finreg_cdata_order_active_refs(g, o) == 1u);
  assert(gc2_finreg_cdata_order_claimed_acq(g) == claimed0);
  assert(gc2_finreg_cdata_order_queued_acq(g) == orderq0);
  assert(gc2_finalizer_queued_acq(g) == finalizerq0);
  assert((lj_obj_gcflags(o) & LJ_GC_CDATA_FIN) != 0);
  assert((lj_obj_gcflags(o) & LJ_GC_FINALIZED) == 0);
  assert(lj_gc2_ismarked(g, o) == 1);

  /* Once a complete scan can prove membership, the same node is claimed and
  ** dispatched exactly once. */
  lj_gc_root_rel(g, saved_root);
  lj_gcroot_repair_epoch_add(g);
  lj_gc2_cycle_to_idle(g);
  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, o) == 0);
  assert(lj_gc2_finreg_cdata_finalize_pweak(
	L, g, finreg_cdata_test_mark) == 1u);
  assert(finreg_cdata_order_active_refs(g, o) == 0);
  assert(gc2_finreg_cdata_order_claimed_acq(g) == claimed0 + 1u);
  assert(gc2_finreg_cdata_order_queued_acq(g) == orderq0 + 1u);
  assert(gc2_finalizer_queued_acq(g) == finalizerq0 + 1u);
  lj_gc2_finalizer_dispatch_all(L);
  assert(gc2_cdata_finalized == finalized0 + 1);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

static void test_finreg_cdata_order_active_retire(lua_State *L, global_State *g)
{
  GCobj *live, *cleared;
  uint64_t queued0, retired0;

  lua_settop(L, 0);
  lua_pushcfunction(L, gc2_cdata_counting_finalizer);
  lua_setglobal(L, "gc2_cdata_counting_finalizer");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "return ffi.gc(ffi.new('char[?]', 8), gc2_cdata_counting_finalizer)\n") ==
    LUA_OK);
  test_post_ctstate_invalid_cdata_edge(L, g);
  live = obj2gco(cdataV(L->top - 1));
  assert(finreg_cdata_order_active_refs(g, live) == 1u);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(finreg_cdata_order_active_refs(g, live) == 1u);

  queued0 = gc2_finreg_cdata_order_queued_acq(g);
  retired0 = gc2_finreg_cdata_order_retired_acq(g);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(finreg_cdata_order_active_refs(g, live) == 0);
  assert(gc2_finreg_cdata_order_queued_acq(g) == queued0 + 1u);
  assert(gc2_finreg_cdata_order_retired_acq(g) >= retired0 + 1u);

  lua_settop(L, 0);
  retired0 = gc2_finreg_cdata_order_retired_acq(g);
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "local cd = ffi.gc(ffi.new('char[?]', 8), gc2_cdata_counting_finalizer)\n"
    "ffi.gc(cd, nil)\n"
    "return cd\n") == LUA_OK);
  cleared = obj2gco(cdataV(L->top - 1));
  assert(finreg_cdata_order_active_refs(g, cleared) == 0);
  assert(gc2_finreg_cdata_order_retired_acq(g) >= retired0 + 1u);
  retired0 = gc2_finreg_cdata_order_retired_acq(g);
  assert(!lj_gc2_finreg_cdata_pending(g));
  assert(finreg_cdata_order_active_refs(g, cleared) == 0);
  assert(gc2_finreg_cdata_order_retired_acq(g) == retired0);
  lua_settop(L, 0);
}

static void test_finreg_cdata_telemetry(lua_State *L, global_State *g)
{
  uint64_t sets0 = gc2_finreg_cdata_sets_acq(g);
  uint64_t clears0 = gc2_finreg_cdata_clears_acq(g);
  uint64_t sets1, clears1, queued1, pweak1, finalizerq1, finalizerd1, mpscd1;
  uint64_t sweepqueued1, claimed1, dispatched1;
  uint64_t orderq1, orderclaimed1, orderunlinked1, orderfallback1;
  uint64_t sets2, clears2, queued2, pweak2, finalizerq2, finalizerd2, mpscd2;
  uint64_t sweepqueued2, claimed2, dispatched2;
  uint64_t orderq2, orderclaimed2, orderfallback2;
  uint64_t pendingorder2;
  uint64_t overflow2;
  uint64_t sweepqueued0;
  int finalized0;
  const MSize finclaim_fixed_n = 4096u;
  const int bulk_n = (int)finclaim_fixed_n;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "require('ffi')\n"
    "collectgarbage('collect')\n"
    "collectgarbage('stop')\n") ==
    LUA_OK);
  assert(!lj_gc2_finreg_cdata_pending(g));
  (void)lj_gc2_finreg_cdata_finalize_close(g);
  sweepqueued0 = gc2_finreg_cdata_sweep_queued_acq(g);

  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "local cd = ffi.gc(ffi.new('char[?]', 8), function() end)\n"
    "ffi.gc(cd, nil)\n"
    "return cd\n") == LUA_OK);
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0 + 1u);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);

  sets1 = gc2_finreg_cdata_sets_acq(g);
  clears1 = gc2_finreg_cdata_clears_acq(g);
  queued1 = gc2_finreg_cdata_queued_acq(g);
  sweepqueued1 = gc2_finreg_cdata_sweep_queued_acq(g);
  pweak1 = gc2_finreg_cdata_pweak_queued_acq(g);
  finalizerq1 = gc2_finalizer_queued_acq(g);
  finalizerd1 = gc2_finalizer_dequeued_acq(g);
  mpscd1 = gc2_finalizer_mpsc_drained_acq(g);
  claimed1 = gc2_finreg_cdata_pweak_claimed_acq(g);
  dispatched1 = gc2_finreg_cdata_preclaim_dispatched_acq(g);
  finalized0 = gc2_cdata_finalized;
  lua_settop(L, 0);
  lua_pushcfunction(L, gc2_cdata_counting_finalizer);
  lua_setglobal(L, "gc2_cdata_counting_finalizer");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "do\n"
    "  local cd = ffi.gc(ffi.new('char[?]', 8), gc2_cdata_counting_finalizer)\n"
    "end\n") ==
    LUA_OK);
  assert(gc2_finreg_cdata_sets_acq(g) == sets1 + 1u);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_finreg_cdata_queued_acq(g) == queued1 + 1u);
  assert(gc2_finreg_cdata_sweep_queued_acq(g) ==
	 sweepqueued1);
  assert(gc2_finreg_cdata_pweak_queued_acq(g) == pweak1 + 1u);
  assert(gc2_finreg_cdata_pweak_claimed_acq(g) == claimed1 + 1u);
  assert(gc2_finalizer_queued_acq(g) == finalizerq1 + 1u);
  assert(gc2_finalizer_dequeued_acq(g) == finalizerd1 + 1u);
  assert(gc2_finalizer_mpsc_drained_acq(g) == mpscd1 + 1u);
  assert(gc2_finreg_cdata_preclaim_dispatched_acq(g) ==
	 dispatched1 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears1 + 1u);
  assert(gc2_cdata_finalized == finalized0 + 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_finreg_cdata_queued_acq(g) == queued1 + 1u);
  assert(gc2_finreg_cdata_sweep_queued_acq(g) ==
	 sweepqueued1);
  assert(gc2_finreg_cdata_pweak_claimed_acq(g) == claimed1 + 1u);
  assert(gc2_finalizer_queued_acq(g) == finalizerq1 + 1u);
  assert(gc2_finalizer_dequeued_acq(g) == finalizerd1 + 1u);
  assert(gc2_finalizer_mpsc_drained_acq(g) == mpscd1 + 1u);
  assert(gc2_finreg_cdata_preclaim_dispatched_acq(g) ==
	 dispatched1 + 1u);
  assert(gc2_cdata_finalized == finalized0 + 1);

  sets1 = gc2_finreg_cdata_sets_acq(g);
  clears1 = gc2_finreg_cdata_clears_acq(g);
  queued1 = gc2_finreg_cdata_queued_acq(g);
  sweepqueued1 = gc2_finreg_cdata_sweep_queued_acq(g);
  pweak1 = gc2_finreg_cdata_pweak_queued_acq(g);
  finalizerq1 = gc2_finalizer_queued_acq(g);
  finalizerd1 = gc2_finalizer_dequeued_acq(g);
  mpscd1 = gc2_finalizer_mpsc_drained_acq(g);
  claimed1 = gc2_finreg_cdata_pweak_claimed_acq(g);
  dispatched1 = gc2_finreg_cdata_preclaim_dispatched_acq(g);
  orderq1 = gc2_finreg_cdata_order_queued_acq(g);
  orderclaimed1 = gc2_finreg_cdata_order_claimed_acq(g);
  orderunlinked1 = gc2_finreg_cdata_order_unlinked_acq(g);
  orderfallback1 = gc2_finreg_cdata_order_fallbacks_acq(g);
  finalized0 = gc2_cdata_finalized;
  lua_settop(L, 0);
  lua_pushcfunction(L, gc2_cdata_counting_finalizer);
  lua_setglobal(L, "gc2_cdata_counting_finalizer");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_rootless_order_fin_t;')\n"
    "return ffi.gc(ffi.new('gc2_rootless_order_fin_t'), gc2_cdata_counting_finalizer)\n") ==
    LUA_OK);
  assert(gc2_finreg_cdata_sets_acq(g) == sets1 + 1u);
  assert(test_unlink_root_object(g, obj2gco(cdataV(L->top - 1))));
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_finreg_cdata_queued_acq(g) == queued1 + 1u);
  assert(gc2_finreg_cdata_sweep_queued_acq(g) ==
	 sweepqueued1);
  assert(gc2_finreg_cdata_pweak_queued_acq(g) == pweak1 + 1u);
  assert(gc2_finreg_cdata_pweak_claimed_acq(g) == claimed1 + 1u);
  assert(gc2_finreg_cdata_order_queued_acq(g) == orderq1 + 1u);
  assert(gc2_finreg_cdata_order_claimed_acq(g) ==
	 orderclaimed1 + 1u);
  assert(gc2_finreg_cdata_order_unlinked_acq(g) ==
	 orderunlinked1);
  assert(gc2_finreg_cdata_order_fallbacks_acq(g) ==
	 orderfallback1);
  assert(gc2_finalizer_queued_acq(g) == finalizerq1 + 1u);
  assert(gc2_finalizer_dequeued_acq(g) == finalizerd1 + 1u);
  assert(gc2_finalizer_mpsc_drained_acq(g) == mpscd1 + 1u);
  assert(gc2_finreg_cdata_preclaim_dispatched_acq(g) ==
	 dispatched1 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears1 + 1u);
  assert(gc2_cdata_finalized == finalized0 + 1);

  sets1 = gc2_finreg_cdata_sets_acq(g);
  clears1 = gc2_finreg_cdata_clears_acq(g);
  queued1 = gc2_finreg_cdata_queued_acq(g);
  finalizerq1 = gc2_finalizer_queued_acq(g);
  finalizerd1 = gc2_finalizer_dequeued_acq(g);
  mpscd1 = gc2_finalizer_mpsc_drained_acq(g);
  claimed1 = gc2_finreg_cdata_pweak_claimed_acq(g);
  dispatched1 = gc2_finreg_cdata_preclaim_dispatched_acq(g);
  finalized0 = gc2_cdata_finalized;
  lua_settop(L, 0);
  lua_pushcfunction(L, gc2_cdata_counting_finalizer);
  lua_setglobal(L, "gc2_cdata_counting_finalizer");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_preclaim_clear_fin_t;')\n"
    "return ffi.gc(ffi.new('gc2_preclaim_clear_fin_t'), gc2_cdata_counting_finalizer)\n") ==
    LUA_OK);
  {
    GCcdata *cd = cdataV(L->top - 1);
    GCobj *o = obj2gco(cd);
    TValue nilv;
    lua_getglobal(L, "gc2_cdata_counting_finalizer");
    assert(tvisfunc(L->top - 1));
    assert(lj_gc2_test_finreg_cdata_preclaim(L, g, o, L->top - 1));
    assert(test_unlink_root_object(g, o));
    lj_gc2_test_finreg_cdata_finalizer_enqueue(g, o);
    setnilV(&nilv);
    lj_cdata_setfin(L, cd, gcval(&nilv), itype(&nilv));
  }
  assert(gc2_finreg_cdata_sets_acq(g) == sets1 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears1 + 1u);
  assert(gc2_finreg_cdata_queued_acq(g) == queued1 + 1u);
  assert(gc2_finreg_cdata_pweak_claimed_acq(g) == claimed1 + 1u);
  assert(gc2_finalizer_queued_acq(g) == finalizerq1 + 1u);
  lj_gc2_finalizer_dispatch_all(L);
  assert(gc2_finalizer_dequeued_acq(g) == finalizerd1 + 1u);
  assert(gc2_finalizer_mpsc_drained_acq(g) == mpscd1 + 1u);
  assert(gc2_finreg_cdata_preclaim_dispatched_acq(g) ==
	 dispatched1 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears1 + 1u);
  assert(gc2_cdata_finalized == finalized0);
  lua_settop(L, 0);

  sets2 = gc2_finreg_cdata_sets_acq(g);
  clears2 = gc2_finreg_cdata_clears_acq(g);
  queued2 = gc2_finreg_cdata_queued_acq(g);
  sweepqueued2 = gc2_finreg_cdata_sweep_queued_acq(g);
  pweak2 = gc2_finreg_cdata_pweak_queued_acq(g);
  finalizerq2 = gc2_finalizer_queued_acq(g);
  finalizerd2 = gc2_finalizer_dequeued_acq(g);
  mpscd2 = gc2_finalizer_mpsc_drained_acq(g);
  claimed2 = gc2_finreg_cdata_pweak_claimed_acq(g);
  dispatched2 = gc2_finreg_cdata_preclaim_dispatched_acq(g);
  orderq2 = gc2_finreg_cdata_order_queued_acq(g);
  orderclaimed2 = gc2_finreg_cdata_order_claimed_acq(g);
  orderfallback2 = gc2_finreg_cdata_order_fallbacks_acq(g);
  gc2_cdata_order_count = 0;
  lua_pushcfunction(L, gc2_cdata_order_finalizer_1);
  lua_setglobal(L, "gc2_cdata_order_finalizer_1");
  lua_pushcfunction(L, gc2_cdata_order_finalizer_2);
  lua_setglobal(L, "gc2_cdata_order_finalizer_2");
  lua_pushcfunction(L, gc2_cdata_order_finalizer_3);
  lua_setglobal(L, "gc2_cdata_order_finalizer_3");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_order_fin_t;')\n"
    "do\n"
    "  local a = ffi.gc(ffi.new('gc2_order_fin_t'), gc2_cdata_order_finalizer_1)\n"
    "  local b = ffi.gc(ffi.new('gc2_order_fin_t'), gc2_cdata_order_finalizer_2)\n"
    "  local c = ffi.gc(ffi.new('gc2_order_fin_t'), gc2_cdata_order_finalizer_3)\n"
    "end\n") ==
    LUA_OK);
  assert(gc2_finreg_cdata_sets_acq(g) == sets2 + 3u);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_finreg_cdata_queued_acq(g) == queued2 + 3u);
  assert(gc2_finreg_cdata_sweep_queued_acq(g) ==
	 sweepqueued2);
  assert(gc2_finreg_cdata_pweak_queued_acq(g) == pweak2 + 3u);
  assert(gc2_finreg_cdata_pweak_claimed_acq(g) == claimed2 + 3u);
  assert(gc2_finalizer_queued_acq(g) == finalizerq2 + 3u);
  assert(gc2_finalizer_dequeued_acq(g) == finalizerd2 + 3u);
  assert(gc2_finalizer_mpsc_drained_acq(g) == mpscd2 + 3u);
  assert(gc2_finreg_cdata_preclaim_dispatched_acq(g) ==
	 dispatched2 + 3u);
  assert(gc2_finreg_cdata_order_queued_acq(g) == orderq2 + 3u);
  assert(gc2_finreg_cdata_order_claimed_acq(g) ==
	 orderclaimed2 + 3u);
  assert(gc2_finreg_cdata_order_fallbacks_acq(g) ==
	 orderfallback2);
  assert(gc2_finreg_cdata_clears_acq(g) == clears2 + 3u);
  assert(gc2_cdata_order_count == 3);
  assert(gc2_cdata_order[0] == 3);
  assert(gc2_cdata_order[1] == 2);
  assert(gc2_cdata_order[2] == 1);

#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  sets2 = gc2_finreg_cdata_sets_acq(g);
  clears2 = gc2_finreg_cdata_clears_acq(g);
  queued2 = gc2_finreg_cdata_queued_acq(g);
  sweepqueued2 = gc2_finreg_cdata_sweep_queued_acq(g);
  pweak2 = gc2_finreg_cdata_pweak_queued_acq(g);
  finalizerq2 = gc2_finalizer_queued_acq(g);
  finalizerd2 = gc2_finalizer_dequeued_acq(g);
  mpscd2 = gc2_finalizer_mpsc_drained_acq(g);
  claimed2 = gc2_finreg_cdata_pweak_claimed_acq(g);
  dispatched2 = gc2_finreg_cdata_preclaim_dispatched_acq(g);
  orderq2 = gc2_finreg_cdata_order_queued_acq(g);
  orderclaimed2 = gc2_finreg_cdata_order_claimed_acq(g);
  orderfallback2 = gc2_finreg_cdata_order_fallbacks_acq(g);
  overflow2 = gc2_finreg_cdata_preclaim_overflow_acq(g);
  gc2_cdata_order_count = 0;
  lj_gc2_test_finreg_cdata_preclaim_fail(g, 1);
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_order_fail_fin_t;')\n"
    "do\n"
    "  local a = ffi.gc(ffi.new('gc2_order_fail_fin_t'), gc2_cdata_order_finalizer_1)\n"
    "  local b = ffi.gc(ffi.new('gc2_order_fail_fin_t'), gc2_cdata_order_finalizer_2)\n"
    "  local c = ffi.gc(ffi.new('gc2_order_fail_fin_t'), gc2_cdata_order_finalizer_3)\n"
    "end\n") ==
    LUA_OK);
  assert(gc2_finreg_cdata_sets_acq(g) == sets2 + 3u);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_finreg_cdata_preclaim_test_fail_acq(g) == 0);
  assert(gc2_finreg_cdata_queued_acq(g) == queued2 + 3u);
  assert(gc2_finreg_cdata_sweep_queued_acq(g) ==
	 sweepqueued2);
  assert(gc2_finreg_cdata_pweak_queued_acq(g) == pweak2 + 3u);
  assert(gc2_finreg_cdata_pweak_claimed_acq(g) == claimed2 + 2u);
  assert(gc2_finreg_cdata_preclaim_overflow_acq(g) ==
	 overflow2 + 1u);
  assert(gc2_finalizer_queued_acq(g) == finalizerq2 + 3u);
  assert(gc2_finalizer_dequeued_acq(g) == finalizerd2 + 3u);
  assert(gc2_finalizer_mpsc_drained_acq(g) == mpscd2 + 3u);
  assert(gc2_finreg_cdata_preclaim_dispatched_acq(g) ==
	 dispatched2 + 2u);
  assert(gc2_finreg_cdata_order_queued_acq(g) == orderq2 + 3u);
  assert(gc2_finreg_cdata_order_claimed_acq(g) ==
	 orderclaimed2 + 3u);
  assert(gc2_finreg_cdata_order_fallbacks_acq(g) ==
	 orderfallback2 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears2 + 3u);
  assert(gc2_cdata_order_count == 3);
  assert(gc2_cdata_order[0] == 3);
  assert(gc2_cdata_order[1] == 2);
  assert(gc2_cdata_order[2] == 1);
#endif

  sets2 = gc2_finreg_cdata_sets_acq(g);
  clears2 = gc2_finreg_cdata_clears_acq(g);
  queued2 = gc2_finreg_cdata_queued_acq(g);
  sweepqueued2 = gc2_finreg_cdata_sweep_queued_acq(g);
  pweak2 = gc2_finreg_cdata_pweak_queued_acq(g);
  finalizerq2 = gc2_finalizer_queued_acq(g);
  finalizerd2 = gc2_finalizer_dequeued_acq(g);
  mpscd2 = gc2_finalizer_mpsc_drained_acq(g);
  claimed2 = gc2_finreg_cdata_pweak_claimed_acq(g);
  dispatched2 = gc2_finreg_cdata_preclaim_dispatched_acq(g);
  orderq2 = gc2_finreg_cdata_order_queued_acq(g);
  orderclaimed2 = gc2_finreg_cdata_order_claimed_acq(g);
  orderfallback2 = gc2_finreg_cdata_order_fallbacks_acq(g);
  gc2_cdata_order_count = 0;
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_regorder_fin_t;')\n"
    "do\n"
    "  local a = ffi.new('gc2_regorder_fin_t')\n"
    "  local b = ffi.new('gc2_regorder_fin_t')\n"
    "  local c = ffi.new('gc2_regorder_fin_t')\n"
    "  ffi.gc(b, gc2_cdata_order_finalizer_2)\n"
    "  ffi.gc(a, gc2_cdata_order_finalizer_1)\n"
    "  ffi.gc(c, gc2_cdata_order_finalizer_3)\n"
    "end\n") ==
    LUA_OK);
  assert(gc2_finreg_cdata_sets_acq(g) == sets2 + 3u);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_finreg_cdata_queued_acq(g) == queued2 + 3u);
  assert(gc2_finreg_cdata_sweep_queued_acq(g) ==
	 sweepqueued2);
  assert(gc2_finreg_cdata_pweak_queued_acq(g) == pweak2 + 3u);
  assert(gc2_finreg_cdata_pweak_claimed_acq(g) == claimed2 + 3u);
  assert(gc2_finalizer_queued_acq(g) == finalizerq2 + 3u);
  assert(gc2_finalizer_dequeued_acq(g) == finalizerd2 + 3u);
  assert(gc2_finalizer_mpsc_drained_acq(g) == mpscd2 + 3u);
  assert(gc2_finreg_cdata_preclaim_dispatched_acq(g) ==
	 dispatched2 + 3u);
  assert(gc2_finreg_cdata_order_queued_acq(g) == orderq2 + 3u);
  assert(gc2_finreg_cdata_order_claimed_acq(g) ==
	 orderclaimed2 + 3u);
  assert(gc2_finreg_cdata_order_fallbacks_acq(g) ==
	 orderfallback2);
  assert(gc2_finreg_cdata_clears_acq(g) == clears2 + 3u);
  assert(gc2_cdata_order_count == 3);
  assert(gc2_cdata_order[0] == 3);
  assert(gc2_cdata_order[1] == 1);
  assert(gc2_cdata_order[2] == 2);

  sets2 = gc2_finreg_cdata_sets_acq(g);
  clears2 = gc2_finreg_cdata_clears_acq(g);
  queued2 = gc2_finreg_cdata_queued_acq(g);
  sweepqueued2 = gc2_finreg_cdata_sweep_queued_acq(g);
  pweak2 = gc2_finreg_cdata_pweak_queued_acq(g);
  finalizerq2 = gc2_finalizer_queued_acq(g);
  finalizerd2 = gc2_finalizer_dequeued_acq(g);
  mpscd2 = gc2_finalizer_mpsc_drained_acq(g);
  claimed2 = gc2_finreg_cdata_pweak_claimed_acq(g);
  dispatched2 = gc2_finreg_cdata_preclaim_dispatched_acq(g);
  orderq2 = gc2_finreg_cdata_order_queued_acq(g);
  orderclaimed2 = gc2_finreg_cdata_order_claimed_acq(g);
  orderfallback2 = gc2_finreg_cdata_order_fallbacks_acq(g);
  gc2_cdata_order_count = 0;
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_rereg_fin_t;')\n"
    "do\n"
    "  local a = ffi.gc(ffi.new('gc2_rereg_fin_t'), gc2_cdata_order_finalizer_1)\n"
    "  local b = ffi.gc(ffi.new('gc2_rereg_fin_t'), gc2_cdata_order_finalizer_2)\n"
    "  ffi.gc(a, gc2_cdata_order_finalizer_3)\n"
    "end\n") ==
    LUA_OK);
  /* Replacing a's callback preserves one active registration; only a and b
  ** transition nil->enabled and contribute set notifications. */
  assert(gc2_finreg_cdata_sets_acq(g) == sets2 + 2u);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_finreg_cdata_queued_acq(g) == queued2 + 2u);
  assert(gc2_finreg_cdata_sweep_queued_acq(g) ==
	 sweepqueued2);
  assert(gc2_finreg_cdata_pweak_queued_acq(g) == pweak2 + 2u);
  assert(gc2_finreg_cdata_pweak_claimed_acq(g) == claimed2 + 2u);
  assert(gc2_finalizer_queued_acq(g) == finalizerq2 + 2u);
  assert(gc2_finalizer_dequeued_acq(g) == finalizerd2 + 2u);
  assert(gc2_finalizer_mpsc_drained_acq(g) == mpscd2 + 2u);
  assert(gc2_finreg_cdata_preclaim_dispatched_acq(g) ==
	 dispatched2 + 2u);
  assert(gc2_finreg_cdata_order_queued_acq(g) == orderq2 + 2u);
  assert(gc2_finreg_cdata_order_claimed_acq(g) ==
	 orderclaimed2 + 2u);
  assert(gc2_finreg_cdata_order_fallbacks_acq(g) ==
	 orderfallback2);
  assert(gc2_finreg_cdata_clears_acq(g) == clears2 + 2u);
  assert(gc2_cdata_order_count == 2);
  assert(gc2_cdata_order[0] == 3);
  assert(gc2_cdata_order[1] == 2);

  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_finreg_cdata_queued_acq(g) == queued2 + 2u);
  assert(gc2_finreg_cdata_sweep_queued_acq(g) ==
	 sweepqueued2);
  assert(gc2_finreg_cdata_pweak_claimed_acq(g) == claimed2 + 2u);
  assert(gc2_finalizer_queued_acq(g) == finalizerq2 + 2u);
  assert(gc2_finalizer_dequeued_acq(g) == finalizerd2 + 2u);
  assert(gc2_finalizer_mpsc_drained_acq(g) == mpscd2 + 2u);
  assert(gc2_finreg_cdata_preclaim_dispatched_acq(g) ==
	 dispatched2 + 2u);
  assert(gc2_cdata_order_count == 2);

  sets2 = gc2_finreg_cdata_sets_acq(g);
  clears2 = gc2_finreg_cdata_clears_acq(g);
  sweepqueued2 = gc2_finreg_cdata_sweep_queued_acq(g);
  pweak2 = gc2_finreg_cdata_pweak_queued_acq(g);
  finalizerq2 = gc2_finalizer_queued_acq(g);
  finalizerd2 = gc2_finalizer_dequeued_acq(g);
  mpscd2 = gc2_finalizer_mpsc_drained_acq(g);
  orderq2 = gc2_finreg_cdata_order_queued_acq(g);
  orderfallback2 = gc2_finreg_cdata_order_fallbacks_acq(g);
  pendingorder2 = gc2_finreg_cdata_pending_order_hits_acq(g);
  gc2_cdata_order_count = 0;
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_close_order_fin_t;')\n"
    "gc2_close_keep = {}\n"
    "gc2_close_keep[1] = ffi.gc(ffi.new('gc2_close_order_fin_t'), gc2_cdata_order_finalizer_1)\n"
    "gc2_close_keep[2] = ffi.gc(ffi.new('gc2_close_order_fin_t'), gc2_cdata_order_finalizer_2)\n"
    "gc2_close_keep[3] = ffi.gc(ffi.new('gc2_close_order_fin_t'), gc2_cdata_order_finalizer_3)\n") ==
    LUA_OK);
  assert(gc2_finreg_cdata_sets_acq(g) == sets2 + 3u);
  assert(lj_gc2_finreg_cdata_pending(g));
  assert(gc2_finreg_cdata_pending_order_hits_acq(g) ==
	 pendingorder2 + 1u);
  (void)lj_gc2_finreg_cdata_finalize_close(g);
  assert(lj_gc2_test_finalizer_queue_pending(g));
  assert(!lj_gc2_finreg_cdata_pending(g));
  assert(gc2_finreg_cdata_order_queued_acq(g) == orderq2 + 3u);
  assert(gc2_finreg_cdata_order_fallbacks_acq(g) ==
	 orderfallback2);
  assert(gc2_finreg_cdata_sweep_queued_acq(g) ==
	 sweepqueued2);
  assert(gc2_finreg_cdata_pweak_queued_acq(g) == pweak2);
  assert(gc2_finalizer_queued_acq(g) == finalizerq2 + 3u);
  lj_gc2_finalizer_dispatch_all(L);
  assert(gc2_finalizer_dequeued_acq(g) == finalizerd2 + 3u);
  assert(gc2_finalizer_mpsc_drained_acq(g) == mpscd2 + 3u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears2 + 3u);
  assert(!lj_gc2_test_finalizer_queue_pending(g));
  assert(!lj_gc2_finreg_cdata_pending(g));
  assert(gc2_cdata_order_count == 3);
  assert(gc2_cdata_order[0] == 3);
  assert(gc2_cdata_order[1] == 2);
  assert(gc2_cdata_order[2] == 1);
  lua_pushnil(L);
  lua_setglobal(L, "gc2_close_keep");

  sets2 = gc2_finreg_cdata_sets_acq(g);
  clears2 = gc2_finreg_cdata_clears_acq(g);
  sweepqueued2 = gc2_finreg_cdata_sweep_queued_acq(g);
  pweak2 = gc2_finreg_cdata_pweak_queued_acq(g);
  finalizerq2 = gc2_finalizer_queued_acq(g);
  finalizerd2 = gc2_finalizer_dequeued_acq(g);
  mpscd2 = gc2_finalizer_mpsc_drained_acq(g);
  orderq2 = gc2_finreg_cdata_order_queued_acq(g);
  orderfallback2 = gc2_finreg_cdata_order_fallbacks_acq(g);
  pendingorder2 = gc2_finreg_cdata_pending_order_hits_acq(g);
  gc2_cdata_order_count = 0;
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_close_rootless_order_fin_t;')\n"
    "return ffi.gc(ffi.new('gc2_close_rootless_order_fin_t'), gc2_cdata_order_finalizer_1)\n") ==
    LUA_OK);
  assert(gc2_finreg_cdata_sets_acq(g) == sets2 + 1u);
  assert(test_unlink_root_object(g, obj2gco(cdataV(L->top - 1))));
  assert(lj_gc2_finreg_cdata_pending(g));
  assert(gc2_finreg_cdata_pending_order_hits_acq(g) ==
	 pendingorder2 + 1u);
  lua_pop(L, 1);
  (void)lj_gc2_finreg_cdata_finalize_close(g);
  assert(lj_gc2_test_finalizer_queue_pending(g));
  assert(!lj_gc2_finreg_cdata_pending(g));
  assert(gc2_finreg_cdata_order_queued_acq(g) == orderq2 + 1u);
  assert(gc2_finreg_cdata_order_fallbacks_acq(g) ==
	 orderfallback2);
  assert(gc2_finreg_cdata_sweep_queued_acq(g) ==
	 sweepqueued2);
  assert(gc2_finreg_cdata_pweak_queued_acq(g) == pweak2);
  assert(gc2_finalizer_queued_acq(g) == finalizerq2 + 1u);
  lj_gc2_finalizer_dispatch_all(L);
  assert(gc2_finalizer_dequeued_acq(g) == finalizerd2 + 1u);
  assert(gc2_finalizer_mpsc_drained_acq(g) == mpscd2 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears2 + 1u);
  assert(!lj_gc2_test_finalizer_queue_pending(g));
  assert(!lj_gc2_finreg_cdata_pending(g));
  assert(gc2_cdata_order_count == 1);
  assert(gc2_cdata_order[0] == 1);

  sets2 = gc2_finreg_cdata_sets_acq(g);
  clears2 = gc2_finreg_cdata_clears_acq(g);
  queued2 = gc2_finreg_cdata_queued_acq(g);
  sweepqueued2 = gc2_finreg_cdata_sweep_queued_acq(g);
  pweak2 = gc2_finreg_cdata_pweak_queued_acq(g);
  finalizerq2 = gc2_finalizer_queued_acq(g);
  finalizerd2 = gc2_finalizer_dequeued_acq(g);
  mpscd2 = gc2_finalizer_mpsc_drained_acq(g);
  claimed2 = gc2_finreg_cdata_pweak_claimed_acq(g);
  dispatched2 = gc2_finreg_cdata_preclaim_dispatched_acq(g);
  orderq2 = gc2_finreg_cdata_order_queued_acq(g);
  orderclaimed2 = gc2_finreg_cdata_order_claimed_acq(g);
  orderfallback2 = gc2_finreg_cdata_order_fallbacks_acq(g);
  overflow2 = gc2_finreg_cdata_preclaim_overflow_acq(g);
  gc2_cdata_bulk_finalized = 0;
  lua_pushcfunction(L, gc2_cdata_bulk_finalizer);
  lua_setglobal(L, "gc2_cdata_bulk_finalizer");
  lua_pushinteger(L, bulk_n);
  lua_setglobal(L, "gc2_cdata_bulk_n");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_bulk_fin_t;')\n"
    "do\n"
    "  for i = 1, gc2_cdata_bulk_n do\n"
    "    ffi.gc(ffi.new('gc2_bulk_fin_t'), gc2_cdata_bulk_finalizer)\n"
    "  end\n"
    "end\n") ==
    LUA_OK);
  assert(gc2_finreg_cdata_sets_acq(g) ==
	 sets2 + (uint64_t)bulk_n);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_finreg_cdata_queued_acq(g) ==
	 queued2 + (uint64_t)bulk_n);
  assert(gc2_finreg_cdata_sweep_queued_acq(g) ==
	 sweepqueued2);
  assert(gc2_finreg_cdata_pweak_queued_acq(g) ==
	 pweak2 + (uint64_t)bulk_n);
  assert(gc2_finreg_cdata_pweak_claimed_acq(g) ==
	 claimed2 + (uint64_t)bulk_n);
  assert(gc2_finreg_cdata_preclaim_overflow_acq(g) ==
	 overflow2);
  assert(gc2_finreg_cdata_preclaim_capacity_acq(g) == finclaim_fixed_n);
  assert(gc2_finalizer_queued_acq(g) ==
	 finalizerq2 + (uint64_t)bulk_n);
  assert(gc2_finalizer_dequeued_acq(g) ==
	 finalizerd2 + (uint64_t)bulk_n);
  assert(gc2_finalizer_mpsc_drained_acq(g) ==
	 mpscd2 + (uint64_t)bulk_n);
  assert(gc2_finreg_cdata_preclaim_dispatched_acq(g) ==
	 dispatched2 + (uint64_t)bulk_n);
  assert(gc2_finreg_cdata_order_queued_acq(g) ==
	 orderq2 + (uint64_t)bulk_n);
  assert(gc2_finreg_cdata_order_claimed_acq(g) ==
	 orderclaimed2 + (uint64_t)bulk_n);
  assert(gc2_finreg_cdata_order_fallbacks_acq(g) ==
	 orderfallback2);
  assert(gc2_finreg_cdata_clears_acq(g) ==
	 clears2 + (uint64_t)bulk_n);
  assert(gc2_cdata_bulk_finalized == bulk_n);

  lua_pushnil(L);
  lua_setglobal(L, "gc2_cdata_order_finalizer_1");
  lua_pushnil(L);
  lua_setglobal(L, "gc2_cdata_order_finalizer_2");
  lua_pushnil(L);
  lua_setglobal(L, "gc2_cdata_order_finalizer_3");
  lua_pushnil(L);
  lua_setglobal(L, "gc2_cdata_bulk_finalizer");
  lua_pushnil(L);
  lua_setglobal(L, "gc2_cdata_bulk_n");
  lua_pushnil(L);
  lua_setglobal(L, "gc2_cdata_counting_finalizer");
  assert(gc2_finreg_cdata_sweep_queued_acq(g) ==
	 sweepqueued0);
}

static void test_finreg_disabled_ordered_pending(lua_State *L, global_State *g)
{
  CTState *cts;
  CTypeFinLease finlease = CTYPE_FIN_LEASE_INIT;
  TValue key;
  GCcdata *cd;
  GCtab *fin_tab;
  int rc;
  lua_settop(L, 0);
  lua_pushcfunction(L, gc2_cdata_counting_finalizer);
  lua_setglobal(L, "gc2_cdata_counting_finalizer");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } gc2_disabled_pending_fin_t;')\n"
    "gc2_disable_keep = ffi.gc(ffi.new('gc2_disabled_pending_fin_t'),\n"
    "  gc2_cdata_counting_finalizer)\n") ==
    LUA_OK);
  assert(lj_gc2_finreg_cdata_pending(g));
  cts = ctype_ctsG(g);
  assert(cts != NULL);
  lua_getglobal(L, "gc2_disable_keep");
  assert(tviscdata(L->top - 1));
  cd = cdataV(L->top - 1);
  setcdataV(L, &key, cd);
  rc = lj_ctype_fin_get(L, cts, &key, &finlease);
  assert(rc == LJ_CTYPE_FIN_FOUND);
  assert(finlease.slot != NULL && finlease.tab != NULL);
  assert(lj_ctype_fin_istab(g, finlease.tab));
  fin_tab = finlease.tab;
  lj_ctype_fin_lease_release(&finlease);
  lj_gc2_finreg_cdata_disable(g);
  assert(!lj_gc2_finreg_cdata_pending(g));
  assert(!lj_ctype_fin_istab(g, fin_tab));
  rc = lj_ctype_fin_get(L, cts, &key, &finlease);
  assert(rc == LJ_CTYPE_FIN_MISS);
  assert(finlease.slot == NULL && finlease.tab == NULL);
  lj_ctype_fin_lease_release(&finlease);
  lua_pop(L, 1);

  /* This fixture deliberately invokes the terminal-only disable operation
  ** while one ordered registration is still live. Disabled generations are
  ** intentionally invisible to both discovery and ffi.gc(cd, nil), so leaving
  ** the table disabled would manufacture an ownerless FIN bit which no normal
  ** lua_close sequence can create. Restore this exact, still-owned generation
  ** only after the disable/MISS assertions, then let the ordinary keyed clear
  ** transaction retire the slot and bit before terminal root validation. */
  assert(lj_gc2_mem_registered_known(g, fin_tab));
  assert((lj_obj_gcflags(obj2gco(cd)) & LJ_GC_CDATA_FIN) != 0);
  fin_gen_tab_enable_rel(fin_tab);
  assert(lj_ctype_fin_istab(g, fin_tab));
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.gc(gc2_disable_keep, nil)\n"
    "gc2_disable_keep = nil\n") ==
    LUA_OK);
  assert((lj_obj_gcflags(obj2gco(cd)) & LJ_GC_CDATA_FIN) == 0);
  assert(!lj_gc2_finreg_cdata_pending(g));
  lua_settop(L, 0);
}
#endif

static void test_leaf_ssb(lua_State *L, global_State *g, TGState *tg)
{
  GCstr *s;

  lua_pushliteral(L, "gc2 leaf ssb");
  s = strV(L->top - 1);
  lj_gc2_mark_begin(g);
  assert(lj_gc2_test_ssb_push(g, obj2gco(s)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(s)) == 1);
  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;

  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
#if LJ_HASJIT || LJ_HASFFI
  luaL_openlibs(L);
#endif
  /* Library registration performs real live table stores. The GC2 store gate
  ** deliberately preserves their IDLE rescan work, so settle that bootstrap
  ** frontier before fixtures take exact per-operation queue baselines. */
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  {
    LJGC2TableDescSnap desc =
      lj_gc2_tabledesc_snapshot(&g->gc2.table_rescan_desc);
    assert(desc.state == LJ_GC2_TABLEDESC_IDLE && desc.generation == 0);
  }
#if defined(LJ_GC2_TEST_HELPERS)
  test_table_topology_primitive();
  test_table_authority_saturation();
  test_table_token_pass_certificate();
#endif

#if defined(LJ_GC2_TEST_WEAK_ONLY)
  test_weak_tables(L, g, tg);
  test_weak_snapshot_growth(L, g, tg);
#if defined(LJ_GC2_TEST_HELPERS)
  test_weak_value_admission_tristate(L, g, tg);
  test_weak_frontier_admission_retries(L, g, tg);
#endif
  test_worker_weak_drain(L, g, tg);
  test_weak_snapshot_ready_publication(L, g);
  test_weak_snapshot_rejects_nonobject(L, g);
  test_weak_snapshot_transient_replays_same_index(L, g);
  test_weak_snapshot_keylock_replays_same_index(L, g, tg);
  test_weak_self_metatable_publish_barrier(L, g, tg);
  test_weak_snapshot_bridge_coverage(L, g, tg);
  test_weak_complete_bridge(L, g, tg);
  test_weak_bridge_fallback_hmask0(L, g, tg);
  test_weak_clear_marks_string_slots(L, g, tg);
#else
  test_strong_table(L, g, tg);
  test_retired_table_owner_nonsemantic(L, g, tg);
  test_retired_table_root_cycle_retries(L, g);
  test_embedded_empty_string_mark(g);
  test_grey_deque_growth(L, g, tg);
#if defined(LJ_GC2_TEST_HELPERS)
  test_grey_deque_steal_race(L, g, tg);
#endif
  test_worker_drain(L, g, tg);
  test_worker_drain_race(L, g, tg);
  test_worker_leaf_ssb(L, g, tg);
  test_fixpoint_round(L, g, tg);
  test_c_value_barrier(L, g, tg);
  test_c_table_rescan_barrier(L, g, tg);
  test_table_rescan_idle_clear(L, g, tg, 0);
  test_table_rescan_idle_clear(L, g, tg, 1);
  test_upval_needscan_preserve_abort(L, g, tg, 0);
  test_upval_needscan_preserve_abort(L, g, tg, 1);
  test_vm_upvalue_barrier(L, g, tg);
  test_vm_table_barrier(L, g, tg);
  test_vm_meta_tset_barrier(L, g, tg);
  test_capi_newindex_target_parent_barrier(L, g, tg);
  test_userdata_constructor_publish_barrier(L, g, tg);
  test_thread_constructor_env_barrier(L, g, tg);
  test_thread_spawn_constructor_child_barrier(L, g, tg);
  test_cclosure_constructor_publish_barrier(L, g, tg);
  test_lua_closure_constructor_publish_barrier(L, g, tg);
  test_proto_chunkname_publish_barrier(L, g, tg);
#if LJ_HASBUFFER
  test_buffer_decode_metatable_barrier(L, g, tg);
  test_buffer_constructor_dict_barrier(L, g, tg);
#endif
#if LJ_HASJIT
  test_jit_hotcall_root(L, g);
  test_jit_table_store_helper_barrier(L, g, tg);
  test_jit_weak_table_store_helper_barrier(L, g, tg);
  test_jit_weak_array_store_helper_barrier(L, g, tg);
  test_jit_upvalue_barrier(L, g, tg);
  test_jit_current_trace_root(L, g, tg);
  test_jit_tg_executing_trace_root(L, g, tg);
#endif
#if LJ_HASPROFILE
  test_jit_profile_registry_weak_barrier();
#endif
  test_weak_tables(L, g, tg);
  test_weak_snapshot_growth(L, g, tg);
#if defined(LJ_GC2_TEST_HELPERS)
  test_weak_value_admission_tristate(L, g, tg);
  test_weak_frontier_admission_retries(L, g, tg);
#endif
  test_worker_weak_drain(L, g, tg);
  test_weak_snapshot_ready_publication(L, g);
  test_weak_snapshot_rejects_nonobject(L, g);
  test_weak_snapshot_transient_replays_same_index(L, g);
  test_weak_snapshot_keylock_replays_same_index(L, g, tg);
  test_weak_self_metatable_publish_barrier(L, g, tg);
  test_weak_snapshot_bridge_coverage(L, g, tg);
  test_weak_complete_bridge(L, g, tg);
  test_weak_bridge_fallback_hmask0(L, g, tg);
  test_weak_clear_marks_string_slots(L, g, tg);
  test_weak_drain_uses_captured_mode(L, g, tg);
  test_weak_pre_clear_late_write_survives_drain(L, g, tg);
  test_weak_post_clear_resurrection_write(L, g, tg);
  test_vm_weak_post_clear_existing_key_write(L, g, tg);
  test_capi_rawset_weak_write_barrier(L, g, tg);
  test_weak_key_write_barrier(L, g, tg);
  test_vm_weak_key_write_barrier(L, g, tg);
  test_peer_weak_key_write_barrier(L, g, tg);
  test_vm_weak_value_hash_key_barrier(L, g, tg);
  test_vm_weak_value_array_barrier(L, g, tg);
  test_table_insert_weak_value_array_barrier(L, g, tg);
  test_capi_weak_newindex_target_write_barrier(L, g, tg);
  test_vm_weak_newindex_target_write_barrier(L, g, tg);
  test_vm_tsetm_range_barrier(L, g, tg);
  test_closure(L, g, tg);
  test_tg_thread_roots(L, g, tg);
  test_capi_collect_live_values(L);
#if LJ_HASFFI
  test_pre_ctstate_cdata_edges(L, g, tg);
#endif
  test_minor_root_scan(L, g, tg);
  test_thread(L, g, tg);
#if defined(LJ_GC2_TEST_HELPERS)
  test_table_rescan_exact_membership(L, g);
  test_table_rescan_ssb_exact_gate(L, g, tg);
  test_table_rescan_queue_admission_retry(L, g, tg);
  test_table_token_small_live_exact(L, g, tg);
  test_table_token_small_cursor_budget(L, g);
  test_table_token_request_observational_stale(L, g);
  test_table_token_huge_live_exact(L, g, tg);
  test_table_token_huge_phase_behavior(L, g, tg);
  test_table_token_huge_reclaiming_owner(g, tg);
  test_table_token_small_free_no_body(L, g);
  test_table_token_small_proof_races(L, g);
  test_table_token_small_weak_oom_progress(L, g);
  test_weak_overflow_full_oom_count_bounded(L, g);
  test_weak_record_publication_protocol(L, g);
  test_weak_overflow_headless_reservation_guard(L, g);
  test_legacy_weak_oom_requeues(L, g, tg);
  test_legacy_weak_oom_recovery_quantum(L, g, tg);
  test_tvalue_edge_status(L, g);
  test_sweep_tvalue_edge_tristate(L, g, tg);
  test_forjit_current_hash_key_lease(L, g);
  test_forjit_hash_to_array_retry(L);
  test_forjit_weak_result_lease(L, g, tg);
  test_forjit_nil_key(L);
  test_meta_metatable_capture_lease(L, g);
  test_meta_tset_admission_retries(L, g);
  test_stack_admission_tristate(L, g, tg);
  test_thread_needscan_exact_count_isolation(L, g);
#endif
  test_thread_needscan_owner_release_same_cycle(
    L, g, tg, THREAD_RELEASE_RECOVERY);
  test_thread_needscan_owner_release_same_cycle(
    L, g, tg, THREAD_RELEASE_EXACT_SSB);
  test_thread_needscan_owner_release_same_cycle(
    L, g, tg, THREAD_RELEASE_REGISTRY_FALLBACK);
#if defined(LJ_GC2_TEST_HELPERS)
  test_thread_needscan_release_race(
    L, g, tg, LJ_GC2_THREAD_NEEDSCAN_TEST_BEFORE_SET);
  test_thread_needscan_release_race(
    L, g, tg, LJ_GC2_THREAD_NEEDSCAN_TEST_AFTER_SET);
  test_thread_needscan_release_race(
    L, g, tg, LJ_GC2_THREAD_NEEDSCAN_TEST_INSTALLING);
  test_executable_root_rescan_dedup(L, g, tg);
#endif
  test_thread_needscan_idle_clear(L, g, tg, 0);
  test_thread_needscan_idle_clear(L, g, tg, 1);
  test_userdata(L, g);
  test_finreg_userdata_queue_mark(L, g, tg);
  test_finreg_userdata_active_unlink(L, g);
  test_finreg_userdata_unproven_unlink_retry(L, g);
  test_finreg_userdata_lookup_retry(L, g);
  test_finreg_userdata_dispatch_defer_retry(L, g);
  test_finreg_userdata_telemetry(L, g);
  test_finreg_internal_userdata_telemetry(L, g);
  test_finreg_userdata_inplace_finalizer_behavior(L);
  test_lib_register_weak_value_barrier();
#if LJ_HASFFI
  test_ffi_loaded_weak_value_barrier();
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  test_finreg_cdata_preclaim_publish_order(L, g);
#endif
  test_finreg_cdata_slot_retry_reopens_weak(L, g);
  test_finreg_cdata_unproven_unlink_retry(L, g);
  test_finreg_cdata_order_active_retire(L, g);
  test_finreg_cdata_telemetry(L, g);
  test_finalizer_spawn_deferred_state(L, g);
  test_finreg_disabled_ordered_pending(L, g);
#endif
  test_false_candidate_mark_admission(L, g, tg);
#if LJ_HASFFI
  test_cdata_exact_coverage_admission(L, g);
#endif
  test_huge_false_type_discharge(L, g, tg);
  test_leaf_ssb(L, g, tg);
#endif

  lua_close(L);
  printf("t-gc2-traverse OK: SSB grey traversal verified\n");
  return 0;
}
