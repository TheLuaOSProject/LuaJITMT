#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/*
** Focused test for the staged parked GC2 worker scheduler.
*/

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)
#include <signal.h>
#include <ucontext.h>
#endif

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lib/test_sleep.h"
#include "lib/tg_stopreq_fixture_helpers.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_dispatch.h"
#include "lj_safepoint.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"


static uint32_t worker_start_create_pause;
static uint32_t worker_start_create_paused;
static uint32_t worker_start_create_release;
static global_State *worker_start_expect_no_traces_g;
static uint32_t worker_join_fail_once;

static void test_publish_sweep_phase(global_State *g)
{
  LJGC2ActivationSnap idle, mark, weak, sweep;
  uint64_t epoch;
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  idle = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(idle.state == LJ_GC2_ACT_IDLE);
  assert(idle.gate == LJ_GC2_ROOT_GATE_OPEN);
  epoch = idle.mark_epoch == UINT64_MAX ? UINT64_MAX : idle.mark_epoch + 1u;
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &idle, epoch,
           LJ_GC2_ACT_MARK, &mark) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &mark, epoch,
           LJ_GC2_ACT_WEAK, &weak) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &weak, epoch,
           LJ_GC2_ACT_SWEEP_OPEN, &sweep) == LJ_GC2_TRANSITION_OK);
  gc2_phase_rel(g, LJ_GC2_SWEEP);
  /* Synthetic SWEEP skips the real root driver, so model its completed
  ** prerequisite explicitly before publishing READY below. It also skips the
  ** real WEAK->SWEEP edge, which initializes the bounded string subphase. */
  gc2_sweep_root_scanned_rel(g, 1);
  lj_str_gc2_sweep_begin(g, 0);
  assert(!lj_gc2_activation_reclaim_veto(g));
}

static void test_reset_sweep_phase(global_State *g)
{
  LJGC2ActivationSnap sweep, idle;
  assert(gc2_phase_xchg_acqrel(g, LJ_GC2_IDLE) == LJ_GC2_SWEEP);
  gc2_sweep_root_scanned_rel(g, 0);
  sweep = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(sweep.state == LJ_GC2_ACT_SWEEP_OPEN);
  assert(sweep.gate == LJ_GC2_ROOT_GATE_OPEN);
  assert(lj_gc2_activation_try_abandon_sweep_open(&g->gc2.activation,
           &sweep, &idle) == LJ_GC2_TRANSITION_OK);
  lj_str_gc2_sweep_abort(g);
  assert(idle.state == LJ_GC2_ACT_IDLE);
  assert(!lj_gc2_activation_reclaim_veto(g));
}

#if defined(__linux__) && defined(__x86_64__)
/*
** The production loop deliberately has no test hook in its wake/park fast
** path. Linux/x86-64 trap-flag stepping lets this fixture stop the real worker
** at the exact observable instruction window instead: the forced zero-drain
** busy counter has advanced, while the following park counter has not.
*/
#define WORKER_PARK_TRACE_SIGNAL SIGUSR2
#define WORKER_PARK_TRACE_FLAG 0x100u

static global_State *worker_park_trace_g;
static uint64_t worker_park_busy_base;
static uint64_t worker_park_parks_base;
static uint32_t worker_park_trace_armed;
static uint32_t worker_park_trace_seen;
static uint32_t worker_park_trace_release;

static void worker_park_trace_flag(void *vctx, int enable)
{
  ucontext_t *ctx = (ucontext_t *)vctx;
  if (enable)
    ctx->uc_mcontext.gregs[REG_EFL] |= WORKER_PARK_TRACE_FLAG;
  else
    ctx->uc_mcontext.gregs[REG_EFL] &= ~WORKER_PARK_TRACE_FLAG;
}

static void worker_park_trace_start(int sig, siginfo_t *info, void *vctx)
{
  UNUSED(sig); UNUSED(info);
  if (la_load32_acq(&worker_park_trace_armed))
    worker_park_trace_flag(vctx, 1);
}

static void worker_park_trace_step(int sig, siginfo_t *info, void *vctx)
{
  global_State *g;
  uint64_t busy, parks, busy_base, parks_base;
  UNUSED(sig); UNUSED(info);

  if (!la_load32_acq(&worker_park_trace_armed)) {
    worker_park_trace_flag(vctx, 0);
    return;
  }
  g = (global_State *)la_loadptr_acq((void *const *)&worker_park_trace_g);
  if (!g)
    return;
  busy_base = la_load64_acq(&worker_park_busy_base);
  parks_base = la_load64_acq(&worker_park_parks_base);
  busy = gc2_worker_busy_retries_acq(g);
  parks = gc2_worker_parks_acq(g);
  if (busy == busy_base + 1u && parks == parks_base) {
    /* Resume without further single-step traps after the publisher commits. */
    worker_park_trace_flag(vctx, 0);
    la_store32_rel(&worker_park_trace_seen, 1);
    while (!la_load32_acq(&worker_park_trace_release))
      __asm__ __volatile__("pause" ::: "memory");
  }
}
#endif

static uint32_t scheduler_trace_count(global_State *g)
{
#if LJ_HASJIT
  jit_State *J = G2J(g);
  TraceNo i;
  uint32_t n = 0;
  for (i = 1; i < trace_sizetrace_acq(J); i++)
    if (traceref(J, i) != NULL)
      n++;
  return n;
#else
  UNUSED(g);
  return 0;
#endif
}

extern int __real_pthread_create(pthread_t *thread,
				 const pthread_attr_t *attr,
				 void *(*start_routine)(void *), void *arg);
extern int __real_pthread_join(pthread_t thread, void **retval);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
			  void *(*start_routine)(void *), void *arg)
{
  uint32_t expect = 1;
  if (worker_start_expect_no_traces_g)
    assert(scheduler_trace_count(worker_start_expect_no_traces_g) == 0);
  if (la_cas32(&worker_start_create_pause, &expect, 2, LA_ACQ_REL, LA_ACQ)) {
    la_store32_rel(&worker_start_create_paused, 1);
    while (la_load32_acq(&worker_start_create_release) == 0)
      sleep_ns(100000L);
    la_store32_rel(&worker_start_create_paused, 0);
  }
  return __real_pthread_create(thread, attr, start_routine, arg);
}

int __wrap_pthread_join(pthread_t thread, void **retval)
{
  uint32_t expect = 1;
  if (la_cas32(&worker_join_fail_once, &expect, 0, LA_ACQ_REL, LA_ACQ))
    return EINVAL;
  return __real_pthread_join(thread, retval);
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
    if (lj_gc2_test_ssb_empty(g))
      return 1;
    sleep_ns(1000000L);
  }
  return 0;
}

static int wait_gc2_counter_at_least(global_State *g,
				     uint64_t (*load)(global_State *),
				     uint64_t target)
{
  int i;
  for (i = 0; i < 1000; i++) {
    if (load(g) >= target)
      return 1;
    sleep_ns(1000000L);
  }
  return 0;
}

static void publish_manual(global_State *g, TGState *tg, uint32_t actions)
{
  uint64_t epoch = la_load64_rlx(&g->gc2.hs_epoch) + 1u;
  la_store32_rel(&g->gc2.hs_actions, actions);
  la_store32_rel(&g->gc2.hs_pending, 1);  /* 05 section 5.4.2. */
  la_store64_rel(&g->gc2.hs_epoch, epoch);  /* 05 section 5.4.2. */
  la_store32_rel(&tg->reqmask, actions);  /* 05 section 5.4.2. */
  la_store32_rel(&tg->poll, 1);  /* 05 section 5.4.2 signal word. */
}

static lua_State *finalizer_expected_L;
static int finalizer_count;
static int finalizer_order[3];

typedef struct FinalizerOwnerHold {
  global_State *g;
  TGState *wait_tg;
  TGState *tg;
  uint32_t tid;
  uint32_t actor;
  uint32_t entered;
  uint32_t saw_native;
} FinalizerOwnerHold;

typedef struct WorkerStartStopReq {
  global_State *g;
  TGState *tg;
  uint32_t saw_native;
  uint32_t published;
} WorkerStartStopReq;

typedef struct NoTLSWorkerDrain {
  global_State *g;
  TGState *saw_tg;
  uint32_t drained;
} NoTLSWorkerDrain;

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

static void *finalizer_owner_hold_thread(void *arg)
{
  FinalizerOwnerHold *ctx = (FinalizerOwnerHold *)arg;
  TGState *saved_tg = lj_thr_get_tg();
  TGState *tg = ctx->tg;
  int i;

  lj_tg_init_thread(ctx->g, tg, NULL, 0);
  lj_tg_tid_rel(tg, ctx->tid);
  lj_tg_derive_prng(ctx->g, tg, ctx->tid);
  lj_thr_set_tg(tg);
  lj_tg_attach(ctx->g, tg);
  ctx->actor = lj_thr_actor_current();
  lj_gc2_test_finalizer_enter(ctx->g);
  la_store32_rel(&ctx->entered, 1);
  for (i = 0; i < 10000; i++) {
    if (lj_tg_in_native_acq(ctx->wait_tg)) {
      la_store32_rel(&ctx->saw_native, 1);
      break;
    }
    sleep_ns(100000L);
  }
  lj_gc2_test_finalizer_leave(ctx->g);
  lj_tg_detach(ctx->g, tg);
  lj_thr_set_tg(saved_tg);
  return NULL;
}

static void *no_tls_worker_drain_thread(void *arg)
{
  NoTLSWorkerDrain *ctx = (NoTLSWorkerDrain *)arg;
  lj_thr_set_tg(NULL);
  ctx->saw_tg = lj_thr_get_tg();
  ctx->drained = lj_gc2_worker_drain(ctx->g, LJ_GC2_WORKER_DRAIN_BATCH);
  return NULL;
}

static void *worker_start_stopreq_thread(void *arg)
{
  WorkerStartStopReq *ctx = (WorkerStartStopReq *)arg;
  int i;
  for (i = 0; i < 1000; i++) {
    if (la_load32_acq(&worker_start_create_paused) &&
	lj_tg_in_native_acq(ctx->tg)) {
      la_store32_rel(&ctx->saw_native, 1);
      break;
    }
    sleep_ns(100000L);
  }
  assert(la_load32_acq(&ctx->saw_native) == 1);
  assert(lj_safepoint_handshake(ctx->g, LJ_GC2_HS_STOPREQ) >= 1u);
  la_store32_rel(&ctx->published, 1);
  la_store32_rel(&worker_start_create_release, 1);
  return NULL;
}

static int arena_list_contains(GCArena *a, GCArena *needle)
{
  while (a) {
    if (a == needle)
      return 1;
    a = lj_arena_next_acq(a);
  }
  return 0;
}

static int scheduler_tg_registered(global_State *g, TGState *target)
{
  TGState *tg;
  if (!g || !target)
    return 0;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg))
    if (tg == target)
      return 1;
  return 0;
}

static void mark_worker_tgs_sweep_ready(global_State *g, uint32_t sweep_cycle)
{
  uint32_t i;
  for (i = 0; i < LJ_GC2_WORKER_MAX; i++) {
    TGState *worker = gc2_worker_tg_acq(g, i);
    if (!worker)
      continue;
    worker->alloc.prepare_epoch = sweep_cycle;
    worker->alloc.sweep_epoch = sweep_cycle;
  }
}

static int unlink_root_object(global_State *g, GCobj *needle)
{
  GCRef *p = &g->gc.root;
  (void)lj_gc_flush_root_pending(g);
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
  GC2SSBNode *node;
  uint64_t async0, busy0, parks0, wakes0;
  uint32_t wake;

  assert(gc2_n_workers_acq(g) == 2);
  parks0 = gc2_worker_parks_acq(g);
  if (parks0 < 2u)
    assert(wait_gc2_counter_at_least(g, gc2_worker_parks_acq, 2u));
  parks0 = gc2_worker_parks_acq(g);

  busy0 = gc2_worker_busy_retries_acq(g);
  wakes0 = gc2_worker_wakes_acq(g);
  gc2_worker_active_rel(g, 1);
  la_store32_rel(&g->gc2.phase, LJ_GC2_MARK);
  lj_gc2_test_worker_wake(g);
  assert(gc2_worker_wakes_acq(g) > wakes0);
  assert(wait_gc2_counter_at_least(g, gc2_worker_busy_retries_acq, busy0 + 2u));

  /* Both workers consumed the published wake and lost worker_active. Install
  ** one counted SSB item, then wait until both have committed to parking on
  ** the unchanged worker_wake sequence. Releasing only worker_active models
  ** gc2_worker_release(): its futex notification must not be the sole path to
  ** retrying visible active-phase work. */
  node = (GC2SSBNode *)malloc(sizeof(GC2SSBNode));
  assert(node != NULL);
  node->pad = TG_GC2_SSB_DYNAMIC;
  lj_gc2_ssb_owner_rel(node, NULL);
  lj_gc2_ssb_count_rel(node, 1);
  lj_gc2_ssb_remembered_rel(node, 0);
  setgcrefnull(node->slot[0]);
  lj_gc2_ssb_next_rel(node, NULL);
  assert(gc2_ssb_head_acq(g) == NULL);
  assert(gc2_ssb_drain_acq(g) == NULL);
  gc2_ssb_head_store_rlx(g, node);
  async0 = gc2_worker_async_progress_acq(g);
  wake = gc2_worker_wake_acq(g);
  assert(wait_gc2_counter_at_least(g, gc2_worker_parks_acq, parks0 + 2u));
  gc2_worker_active_rel(g, 0);
  la_futex_wake(&g->gc2.worker_active, 0x7fffffff);
  assert(wait_gc2_counter_at_least(g, gc2_worker_async_progress_acq,
				   async0 + 1u));
  assert(gc2_worker_wake_acq(g) == wake);
  assert(lj_gc2_test_ssb_empty(g));
  la_store32_rel(&g->gc2.phase, LJ_GC2_IDLE);
}

static void test_worker_retirement_uses_embedded_links(global_State *g)
{
  TGState blocker, *worker0, *worker1, *head, *next;
  TGState *saved_tg = lj_thr_get_tg();
  GCSize total0;
  uint32_t tid = lj_thr_newid();

  assert(gc2_n_workers_acq(g) == 2);
  worker0 = gc2_worker_tg_acq(g, 0);
  worker1 = gc2_worker_tg_acq(g, 1);
  assert(worker0 != NULL && worker1 != NULL && worker0 != worker1);

  /* Keep n_threads above the registry's sole-owner reclamation boundary while
  ** the parked workers detach. Their lifetime queue must use the worker TGs'
  ** embedded links; allocating queue nodes via mainthread(g) here would drive
  ** the main allocator from a foreign controller in the production case. */
  lj_tg_init_thread(g, &blocker, NULL, 0);
  lj_tg_tid_rel(&blocker, tid);
  lj_tg_derive_prng(g, &blocker, tid);
  lj_native_enter(&blocker);
  lj_tg_attach(g, &blocker);
  assert(lj_tg_worker_retire_next_acq(&blocker) == NULL);

  lj_gc2_worker_stop(g);
  assert(gc2_n_workers_acq(g) == 0);
  assert(gc2_worker_tg_acq(g, 0) == NULL);
  assert(gc2_worker_tg_acq(g, 1) == NULL);
  head = (TGState *)gc2_worker_tg_retired_acq(g);
  assert(head == worker0 || head == worker1);
  next = lj_tg_worker_retire_next_acq(head);
  assert(next == (head == worker0 ? worker1 : worker0));
  assert(lj_tg_worker_retire_next_acq(next) == NULL);

  lj_tg_detach(g, &blocker);
  lj_gc2_worker_stop(g);  /* Unlink registry nodes, then free retired workers. */
  assert(gc2_worker_tg_retired_acq(g) == NULL);
  lj_tg_fini_thread(g, &blocker);
  lj_thr_set_tg(saved_tg);

  total0 = lj_gc_total_load(g);
  assert(lj_gc2_workers_set(g, 2) == 1);
  assert(gc2_n_workers_acq(g) == 2);
  /* Runtime-only pool records must not enter the Lua allocator/accounting.
  ** A foreign gcworkers() controller cannot own the main TG allocator. */
  assert(lj_gc_total_load(g) == total0);
}

static void test_worker_join_failure_retains_lifetime(global_State *g)
{
  LJThr *thread0;
  TGState *tg0;

  assert(gc2_n_workers_acq(g) == 2);
  thread0 = (LJThr *)gc2_worker_thread_acq(g, 0);
  tg0 = gc2_worker_tg_acq(g, 0);
  assert(thread0 != NULL && tg0 != NULL);

  la_store32_rel(&worker_join_fail_once, 1);
  assert(lj_gc2_workers_set(g, 0) == 0);
  assert(la_load32_acq(&worker_join_fail_once) == 0);
  assert(gc2_worker_stop_acq(g) == 1);
  assert(gc2_n_workers_acq(g) == 1);
  assert(gc2_worker_thread_acq(g, 0) == thread0);
  assert(gc2_worker_tg_acq(g, 0) == tg0);
  assert(scheduler_tg_registered(g, tg0));

  /* Retrying the control operation performs the real join, then and only then
  ** releases the retained TG slot. */
  assert(lj_gc2_workers_set(g, 0) == 1);
  assert(gc2_n_workers_acq(g) == 0);
  assert(gc2_worker_thread_acq(g, 0) == NULL);
  assert(gc2_worker_tg_acq(g, 0) == NULL);
  assert(lj_gc2_workers_set(g, 2) == 1);
}

static void test_multistate_terminal_tg_reclaim(lua_State *outer_L,
                                                global_State *outer_g)
{
  lua_State *L2;
  global_State *g2;
  TGState *outer_tg = lj_thr_get_tg();
  TGState *worker0, *worker1, *deferred;
  GCobj *outer_pending;
  uint32_t tid;

  assert(outer_tg != NULL && outer_tg->gl == outer_g);
  assert(gc2_n_workers_acq(outer_g) == 2);
  assert(lj_gc2_workers_set(outer_g, 0) == 1);

  /* Seed an owner-private chain in universe 1. Closing universe 2 must not
  ** mistake raw TLS for one of g2's pending-root producers. */
  (void)lj_gc_flush_root_pending(outer_g);
  lua_newtable(outer_L);
  outer_pending = lj_tg_gcroot_pending_acq(outer_tg);
  assert(outer_pending != NULL);

  L2 = luaL_newstate();
  assert(L2 != NULL);
  g2 = G(L2);
  assert(lj_thr_get_tg() == outer_tg);
  assert(outer_tg->gl != g2);

  assert(lj_gc2_workers_set(g2, 2) == 1);
  worker0 = gc2_worker_tg_acq(g2, 0);
  worker1 = gc2_worker_tg_acq(g2, 1);
  assert(worker0 != NULL && worker1 != NULL && worker0 != worker1);
  lj_gc2_worker_stop(g2);
  assert(gc2_n_workers_acq(g2) == 0);
  assert(scheduler_tg_registered(g2, worker0));
  assert(scheduler_tg_registered(g2, worker1));
  assert(gc2_worker_tg_retired_acq(g2) != NULL);

  /* Model a threading.thread userdata which relinquished its last external
  ** reference while its TG remained registry-owned. */
  deferred = lj_mem_newt(L2, sizeof(TGState), TGState);
  lj_tg_init_thread(g2, deferred, NULL, 1);
  lj_tg_flags_or_rlx(deferred, TGF_LUA_ALLOC|TGF_DEFER_FREE);
  tid = lj_thr_newid();
  lj_tg_tid_rel(deferred, tid);
  lj_tg_derive_prng(g2, deferred, tid);
  lj_tg_attach(g2, deferred);
  lj_tg_detach(g2, deferred);
  (void)lj_tg_ssb_refs_add(deferred, 1);
  assert(scheduler_tg_registered(g2, deferred));
  assert(lj_tg_reclaim_dead(g2) == 0);  /* Raw TLS still belongs to g1. */

  mt_shutdown_rel(g2, 1);
  assert(lj_tg_reclaim_dead_terminal(g2) == 2u);
  assert(!scheduler_tg_registered(g2, worker0));
  assert(!scheduler_tg_registered(g2, worker1));
  assert(scheduler_tg_registered(g2, deferred));
  assert(lj_tg_ssb_refs_sub(deferred, 1) == 1u);
  assert(lj_tg_reclaim_dead_terminal(g2) == 1u);
  assert(!scheduler_tg_registered(g2, deferred));
  assert(lj_tg_gcroot_pending_acq(outer_tg) == outer_pending);

  /* Registry unlink leaves unflagged worker storage to the embedded retire
  ** list. The idempotent stopped-pool pass now finalizes and frees it. */
  lj_gc2_worker_stop(g2);
  assert(gc2_worker_tg_retired_acq(g2) == NULL);
  lua_close(L2);
  assert(lj_thr_get_tg() == outer_tg);
  assert(lj_tg_gcroot_pending_acq(outer_tg) == outer_pending);

  (void)lj_gc_flush_root_pending(outer_g);
  lua_pop(outer_L, 1);
  assert(lj_gc2_workers_set(outer_g, 2) == 1);
}

static void test_bounded_ssb_private_remainder(global_State *g)
{
  enum { NNODES = 64 };
  GC2SSBNode *head = NULL;
  uint32_t i;

  assert(lj_gc2_workers_set(g, 0) == 1);
  assert(gc2_n_workers_acq(g) == 0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_ssb_head_acq(g) == NULL);
  assert(gc2_ssb_drain_acq(g) == NULL);
  assert(lj_gc2_test_ssb_empty(g));

  for (i = 0; i < NNODES; i++) {
    GC2SSBNode *node = (GC2SSBNode *)malloc(sizeof(GC2SSBNode));
    assert(node != NULL);
    node->pad = TG_GC2_SSB_DYNAMIC;
    lj_gc2_ssb_owner_rel(node, NULL);
    lj_gc2_ssb_count_rel(node, 1);
    setgcrefnull(node->slot[0]);
    lj_gc2_ssb_next_rel(node, head);
    head = node;
  }
  gc2_ssb_head_store_rlx(g, head);
  la_store32_rel(&g->gc2.phase, LJ_GC2_MARK);

  assert(lj_gc2_worker_drain(g, 1) == 1);
  /* A bounded drain owns the untouched suffix directly. Re-publishing the
  ** suffix to the MPSC head makes limit=1 perform a full tail walk per item
  ** (quadratic for a large queue) and lets weak-close handshakes amplify it. */
  assert(gc2_ssb_head_acq(g) == NULL);
  assert(gc2_ssb_drain_acq(g) != NULL);
  assert(!lj_gc2_test_ssb_empty(g));
  for (i = 1; i < NNODES; i++)
    assert(lj_gc2_worker_drain(g, 1) == 1);
  assert(gc2_ssb_head_acq(g) == NULL);
  assert(gc2_ssb_drain_acq(g) == NULL);
  assert(lj_gc2_test_ssb_empty(g));

  la_store32_rel(&g->gc2.phase, LJ_GC2_IDLE);
  assert(lj_gc2_workers_set(g, 2) == 1);
  assert(gc2_n_workers_acq(g) == 2);
}

static void test_worker_wake_between_snapshot_and_park(lua_State *L,
						global_State *g)
{
#if defined(__linux__) && defined(__x86_64__)
  struct sigaction start_sa, step_sa, old_start_sa, old_step_sa;
  sigset_t trace_set, old_mask;
  LJThr *worker;
  GCobj *obj;
  uint64_t parks0, busy0, drained0, published_wake = 0;
  int i, caught = 0, drained_without_rescue = 0, queue_drained = 0;
  int object_unlinked = 0, worker_started = 0;

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_finalizer_mpsc_acq(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) == NULL);
  assert(lj_gc2_workers_set(g, 0) == 1);
  assert(gc2_n_workers_acq(g) == 0);

  lua_settop(L, 0);
  lua_newtable(L);
  obj = obj2gco(tabV(L->top - 1));
  object_unlinked = unlink_root_object(g, obj);
  assert(object_unlinked);

  sigemptyset(&trace_set);
  sigaddset(&trace_set, WORKER_PARK_TRACE_SIGNAL);
  sigaddset(&trace_set, SIGTRAP);
  assert(pthread_sigmask(SIG_UNBLOCK, &trace_set, &old_mask) == 0);

  memset(&step_sa, 0, sizeof(step_sa));
  step_sa.sa_sigaction = worker_park_trace_step;
  sigemptyset(&step_sa.sa_mask);
  step_sa.sa_flags = SA_SIGINFO;
  assert(sigaction(SIGTRAP, &step_sa, &old_step_sa) == 0);

  memset(&start_sa, 0, sizeof(start_sa));
  start_sa.sa_sigaction = worker_park_trace_start;
  sigemptyset(&start_sa.sa_mask);
  start_sa.sa_flags = SA_SIGINFO;
  assert(sigaction(WORKER_PARK_TRACE_SIGNAL, &start_sa, &old_start_sa) == 0);

  la_storeptr_rel((void **)&worker_park_trace_g, g);
  la_store32_rel(&worker_park_trace_armed, 0);
  la_store32_rel(&worker_park_trace_seen, 0);
  la_store32_rel(&worker_park_trace_release, 0);

  parks0 = gc2_worker_parks_acq(g);
  assert(lj_gc2_workers_set(g, 1) == 1);
  worker_started = 1;
  assert(gc2_n_workers_acq(g) == 1);
  assert(wait_gc2_counter_at_least(g, gc2_worker_parks_acq, parks0 + 1u));

  /*
  ** Leave the real worker's drain with zero progress, then single-step from
  ** its interrupted futex return. The first busy/park counter skew is exactly
  ** after the iteration's pre-drain wake snapshot and before its park
  ** decision. Publishing in that skew caught the former late-snapshot bug:
  ** the worker used to snapshot the new sequence and sleep on queued work.
  */
  parks0 = gc2_worker_parks_acq(g);
  busy0 = gc2_worker_busy_retries_acq(g);
  la_store64_rel(&worker_park_busy_base, busy0);
  la_store64_rel(&worker_park_parks_base, parks0);
  gc2_worker_active_rel(g, 1);
  la_store32_rel(&g->gc2.phase, LJ_GC2_MARK);
  la_store32_rel(&worker_park_trace_armed, 1);

  worker = (LJThr *)gc2_worker_thread_acq(g, 0);
  assert(worker != NULL);
  assert(pthread_kill(worker->handle, WORKER_PARK_TRACE_SIGNAL) == 0);
  for (i = 0; i < 10000; i++) {
    if (la_load32_acq(&worker_park_trace_seen)) {
      caught = 1;
      break;
    }
    sleep_ns(100000L);
  }

  if (caught) {
    assert(gc2_worker_busy_retries_acq(g) == busy0 + 1u);
    assert(gc2_worker_parks_acq(g) == parks0);
    drained0 = gc2_finalizer_mpsc_drained_acq(g);
    lj_gc2_test_finalizer_enqueue(g, obj);
    assert(gc2_finalizer_mpsc_acq(g) != NULL);
    published_wake = gc2_worker_wake_acq(g);
  }

  /* Release both the synthetic drain owner and the stopped instruction. */
  la_store32_rel(&g->gc2.phase, LJ_GC2_IDLE);
  gc2_worker_active_rel(g, 0);
  la_store32_rel(&worker_park_trace_armed, 0);
  la_store32_rel(&worker_park_trace_release, 1);

  if (caught) {
    for (i = 0; i < 5000; i++) {
      if (gc2_finalizer_mpsc_drained_acq(g) >= drained0 + 1u) {
        drained_without_rescue = 1;
        break;
      }
      sleep_ns(100000L);
    }
    /* Keep cleanup bounded even when testing a deliberately broken binary. */
    if (!drained_without_rescue) {
      assert(gc2_worker_wake_acq(g) == published_wake);
      lj_gc2_test_worker_wake(g);
      queue_drained = wait_gc2_counter_at_least(
	 g, gc2_finalizer_mpsc_drained_acq, drained0 + 1u);
    } else {
      /* No later wake is allowed to mask the publication under test. */
      assert(gc2_worker_wake_acq(g) == published_wake);
      queue_drained = 1;
    }
  }

  if (worker_started)
    assert(lj_gc2_workers_set(g, 0) == 1);
  assert(sigaction(WORKER_PARK_TRACE_SIGNAL, &old_start_sa, NULL) == 0);
  assert(sigaction(SIGTRAP, &old_step_sa, NULL) == 0);
  assert(pthread_sigmask(SIG_SETMASK, &old_mask, NULL) == 0);
  la_storeptr_rel((void **)&worker_park_trace_g, NULL);

  if (caught && queue_drained) {
    assert(gc2_finalizer_mpsc_acq(g) == NULL);
    assert(lj_gc2_test_finalizer_dequeue(g) == obj);
    assert(lj_gc2_test_finalizer_dequeue(g) == NULL);
  }
  if (object_unlinked)
    relink_root_object(g, obj);
  lua_settop(L, 0);
  assert(lj_gc2_workers_set(g, 2) == 1);
  assert(gc2_n_workers_acq(g) == 2);

  assert(caught);  /* Deterministic instruction-window interception. */
  assert(queue_drained);
  assert(drained_without_rescue);  /* The publication wake was not lost. */
#else
  /* Exact instruction-window stepping is a Linux/x86-64 test technique. */
  UNUSED(L); UNUSED(g);
  fprintf(stderr,
	  "t-gc2-worker-scheduler: wake-before-park window test skipped\n");
#endif
}

static void test_worker_finalizer_mpsc_drain(lua_State *L, global_State *g)
{
  GCobj *a, *b;
  uint64_t drained0, runs0, async0;

  lua_settop(L, 0);
  assert(gc2_n_workers_acq(g) == 2);
  assert(gc2_finalizer_mpsc_acq(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) == NULL);

  lua_newtable(L);
  a = obj2gco(tabV(L->top - 1));
  lua_newtable(L);
  b = obj2gco(tabV(L->top - 1));
  assert(unlink_root_object(g, a));
  assert(unlink_root_object(g, b));

  drained0 = gc2_finalizer_mpsc_drained_acq(g);
  runs0 = gc2_worker_runs_acq(g);
  async0 = gc2_worker_async_progress_acq(g);

  gc2_worker_active_rel(g, 1);
  lj_gc2_test_finalizer_enqueue(g, a);
  lj_gc2_test_finalizer_enqueue(g, b);
  assert(wait_gc2_counter_at_least(g, gc2_finalizer_mpsc_drained_acq,
				   drained0 + 2u));
  assert(gc2_finalizer_mpsc_acq(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) != NULL);
  assert(gc2_worker_runs_acq(g) > runs0);
  assert(wait_gc2_counter_at_least(g, gc2_worker_async_progress_acq, async0 + 2u));
  assert(gc2_worker_active_acq(g) == 1);
  gc2_worker_active_rel(g, 0);
  assert(gc2_worker_active_acq(g) == 0);

  assert(lj_gc2_test_finalizer_dequeue(g) == a);
  assert(lj_gc2_test_finalizer_dequeue(g) == b);
  assert(lj_gc2_test_finalizer_dequeue(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) == NULL);

  relink_root_object(g, b);
  relink_root_object(g, a);
  lua_settop(L, 0);
}

static void test_worker_finalizer_sweep_mpsc_drain(lua_State *L,
						   global_State *g)
{
  GCobj *a, *b;
  uint64_t drained0, runs0, async0;

  lua_settop(L, 0);
  assert(gc2_n_workers_acq(g) == 2);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_IDLE);
  assert(gc2_finalizer_mpsc_acq(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) == NULL);

  lua_newtable(L);
  a = obj2gco(tabV(L->top - 1));
  lua_newtable(L);
  b = obj2gco(tabV(L->top - 1));
  assert(unlink_root_object(g, a));
  assert(unlink_root_object(g, b));

  drained0 = gc2_finalizer_mpsc_drained_acq(g);
  runs0 = gc2_worker_runs_acq(g);
  async0 = gc2_worker_async_progress_acq(g);

  test_publish_sweep_phase(g);
  lj_gc2_test_finalizer_enqueue(g, a);
  lj_gc2_test_finalizer_enqueue(g, b);
  assert(wait_gc2_counter_at_least(g, gc2_finalizer_mpsc_drained_acq,
				   drained0 + 2u));
  assert(gc2_finalizer_mpsc_acq(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) != NULL);
  assert(gc2_worker_runs_acq(g) > runs0);
  assert(wait_gc2_counter_at_least(g, gc2_worker_async_progress_acq,
				   async0 + 2u));
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_SWEEP);

  test_reset_sweep_phase(g);
  assert(lj_gc2_test_finalizer_dequeue(g) == a);
  assert(lj_gc2_test_finalizer_dequeue(g) == b);
  assert(lj_gc2_test_finalizer_dequeue(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) == NULL);

  relink_root_object(g, b);
  relink_root_object(g, a);
  lua_settop(L, 0);
}

static void test_worker_finalizer_uses_physical_actor(lua_State *L,
						      global_State *g)
{
  NoTLSWorkerDrain ctx;
  pthread_t thread;
  GCobj *a, *b;
  uint64_t drained0;

  assert(lj_gc2_workers_set(g, 0) == 1);
  assert(gc2_n_workers_acq(g) == 0);
  lua_settop(L, 0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_finalizer_mpsc_acq(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) == NULL);

  lua_newtable(L);
  a = obj2gco(tabV(L->top - 1));
  lua_newtable(L);
  b = obj2gco(tabV(L->top - 1));
  assert(unlink_root_object(g, a));
  assert(unlink_root_object(g, b));

  drained0 = gc2_finalizer_mpsc_drained_acq(g);
  lj_gc2_test_finalizer_enqueue(g, a);
  lj_gc2_test_finalizer_enqueue(g, b);
  assert(gc2_finalizer_mpsc_acq(g) != NULL);
  assert(gc2_finalizer_tail_acq(g) == NULL);

  ctx.g = g;
  ctx.saw_tg = (TGState *)g;
  ctx.drained = 1;
  assert(pthread_create(&thread, NULL, no_tls_worker_drain_thread,
			&ctx) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.saw_tg == NULL);
  /* TLS-less helpers no longer alias one pseudo owner. Actor admission gives
  ** this pthread a unique physical FIFO capability, so it may drain without
  ** borrowing or impersonating a Lua TG. */
  assert(ctx.drained == 2u);
  assert(gc2_finalizer_mpsc_drained_acq(g) == drained0 + 2u);
  assert(gc2_finalizer_mpsc_acq(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) != NULL);
  assert(lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH) == 0u);
  assert(lj_gc2_test_finalizer_dequeue(g) == a);
  assert(lj_gc2_test_finalizer_dequeue(g) == b);
  assert(lj_gc2_test_finalizer_dequeue(g) == NULL);

  relink_root_object(g, b);
  relink_root_object(g, a);
  lua_settop(L, 0);

  assert(lj_gc2_workers_set(g, 2) == 1);
  assert(gc2_n_workers_acq(g) == 2);
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

  assert(gc2_n_workers_acq(g) == 2);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_IDLE);
  assert(gc2_finalizer_mpsc_acq(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) == NULL);

  push_scheduler_udata(L, 1);
  push_scheduler_udata(L, 2);
  push_scheduler_udata(L, 3);

  queued0 = gc2_finalizer_queued_acq(g);
  drained0 = gc2_finalizer_mpsc_drained_acq(g);
  dequeued0 = gc2_finalizer_dequeued_acq(g);
  async0 = gc2_worker_async_progress_acq(g);

  separated = lj_gc2_finreg_udata_finalize(g, 1);
  assert(separated >= 3u);
  assert(gc2_finalizer_queued_acq(g) == queued0 + 3u);
  assert(wait_gc2_counter_at_least(g, gc2_finalizer_mpsc_drained_acq,
				   drained0 + 3u));
  assert(gc2_finalizer_mpsc_acq(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) != NULL);
  assert(wait_gc2_counter_at_least(g, gc2_worker_async_progress_acq, async0 + 3u));

  lj_gc2_finalizer_dispatch_all(L);
  assert(gc2_finalizer_dequeued_acq(g) == dequeued0 + 3u);
  assert(gc2_finalizer_tail_acq(g) == NULL);
  assert(finalizer_count == 3);
  assert(finalizer_order[0] == 3);
  assert(finalizer_order[1] == 2);
  assert(finalizer_order[2] == 1);

  lua_settop(L, 0);
  lj_gc2_finreg_udata_finalize(g, 1);
  lj_gc2_finalizer_dispatch_all(L);
  assert(finalizer_count == 3);
  finalizer_expected_L = NULL;
}

static void test_finalizer_dispatch_all_waits_native(lua_State *L,
						     global_State *g,
						     TGState *tg)
{
  FinalizerOwnerHold hold;
  pthread_t thread;
  uint64_t queued0, drained0, dequeued0;
  size_t separated;
  int i;

  lua_settop(L, 0);
  finalizer_expected_L = L;
  finalizer_count = 0;
  finalizer_order[0] = finalizer_order[1] = finalizer_order[2] = 0;

  assert(gc2_n_workers_acq(g) == 2);
  assert(gc2_finalizer_mpsc_acq(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) == NULL);

  push_scheduler_udata(L, 1);

  queued0 = gc2_finalizer_queued_acq(g);
  drained0 = gc2_finalizer_mpsc_drained_acq(g);
  dequeued0 = gc2_finalizer_dequeued_acq(g);
  separated = lj_gc2_finreg_udata_finalize(g, 1);
  assert(separated >= 1u);
  assert(gc2_finalizer_queued_acq(g) == queued0 + 1u);
  assert(wait_gc2_counter_at_least(g, gc2_finalizer_mpsc_drained_acq,
				   drained0 + 1u));
  assert(gc2_finalizer_mpsc_acq(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) != NULL);

  hold.g = g;
  hold.wait_tg = tg;
  hold.tid = lj_tg_tid_acq(tg) + 6000u;
  if (hold.tid == 0 || hold.tid == LJ_THREAD_GCSCAN)
    hold.tid = 6000u;
  hold.tg = lj_mem_newt(L, sizeof(TGState), TGState);
  hold.entered = 0;
  hold.saw_native = 0;

  assert(lj_state_owner_acq(L) == lj_tg_tid_acq(tg));
  assert(pthread_create(&thread, NULL, finalizer_owner_hold_thread,
			&hold) == 0);
  for (i = 0; i < 1000 && la_load32_acq(&hold.entered) == 0; i++)
    sleep_ns(1000000L);
  assert(la_load32_acq(&hold.entered) == 1);
  assert(gc2_finalizer_owner_acq(g) == hold.actor);

  lj_gc2_finalizer_dispatch_all(L);
  assert(pthread_join(thread, NULL) == 0);
  assert(la_load32_acq(&hold.saw_native) == 1);
  assert(gc2_finalizer_dequeued_acq(g) == dequeued0 + 1u);
  assert(gc2_finalizer_tail_acq(g) == NULL);
  assert(finalizer_count == 1);
  assert(finalizer_order[0] == 1);

  lua_settop(L, 0);
  lj_gc2_finreg_udata_finalize(g, 1);
  lj_gc2_finalizer_dispatch_all(L);
  assert(finalizer_count == 1);
  finalizer_expected_L = NULL;

  assert(lj_gc2_workers_set(g, 0) == 1);
  assert(!scheduler_tg_registered(g, hold.tg));
  lj_tg_fini_thread(g, hold.tg);
  lj_mem_freet(g, hold.tg);
  assert(lj_gc2_workers_set(g, 2) == 1);
}

static void heat_pre_worker_trace(lua_State *L, global_State *g)
{
  int status;
  luaL_openlibs(L);
  status = luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local util = require('jit.util')\n"
    "local function hot(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for i = 1, 20 do assert(hot(80) == 3240) end\n"
    "assert(util.traceinfo(1), 'pre-worker loop did not trace')\n");
  if (status != 0)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(status == 0);
  assert(scheduler_trace_count(g) > 0);
}

static void test_worker_start_l_flushes_prestart_traces(lua_State *L,
							global_State *g)
{
  uint32_t actions = 0;
  assert(lj_gc2_workers_set(g, 0) == 1);
  assert(gc2_n_workers_acq(g) == 0);

  heat_pre_worker_trace(L, g);
  worker_start_expect_no_traces_g = g;
  assert(lj_gc2_workers_set_l(L, 1, &actions) == 1);
  worker_start_expect_no_traces_g = NULL;
  assert(actions == 0);
  assert(gc2_n_workers_acq(g) == 1);
  assert(scheduler_trace_count(g) == 0);
  assert(lj_gc2_workers_set(g, 0) == 1);
  assert(gc2_n_workers_acq(g) == 0);
  assert(lj_gc2_workers_set(g, 2) == 1);
  assert(gc2_n_workers_acq(g) == 2);
}

static void test_finalizer_owner_leave_rewakes_worker(lua_State *L,
						      global_State *g)
{
  GCobj *a, *b;
  uint64_t drained0, parks0, wakes0;

  assert(lj_gc2_workers_set(g, 1) == 1);
  assert(gc2_n_workers_acq(g) == 1);
  lua_settop(L, 0);
  assert(gc2_finalizer_mpsc_acq(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) == NULL);

  lua_newtable(L);
  a = obj2gco(tabV(L->top - 1));
  lua_newtable(L);
  b = obj2gco(tabV(L->top - 1));
  assert(unlink_root_object(g, a));
  assert(unlink_root_object(g, b));

  drained0 = gc2_finalizer_mpsc_drained_acq(g);
  parks0 = gc2_worker_parks_acq(g);
  wakes0 = gc2_worker_wakes_acq(g);
  lj_gc2_test_finalizer_enter(g);
  lj_gc2_test_finalizer_enqueue(g, a);
  lj_gc2_test_finalizer_enqueue(g, b);
  assert(wait_gc2_counter_at_least(g, gc2_worker_parks_acq, parks0 + 1u));
  assert(gc2_finalizer_mpsc_drained_acq(g) == drained0);
  assert(gc2_finalizer_mpsc_acq(g) != NULL);
  assert(gc2_finalizer_tail_acq(g) == NULL);
  lj_gc2_test_finalizer_leave(g);
  assert(wait_gc2_counter_at_least(g, gc2_worker_wakes_acq, wakes0 + 2u));
  assert(wait_gc2_counter_at_least(g, gc2_finalizer_mpsc_drained_acq,
				   drained0 + 2u));
  assert(gc2_finalizer_mpsc_acq(g) == NULL);
  assert(gc2_finalizer_tail_acq(g) != NULL);

  assert(lj_gc2_test_finalizer_dequeue(g) == a);
  assert(lj_gc2_test_finalizer_dequeue(g) == b);
  assert(lj_gc2_test_finalizer_dequeue(g) == NULL);
  relink_root_object(g, b);
  relink_root_object(g, a);
  lua_settop(L, 0);

  assert(lj_gc2_workers_set(g, 2) == 1);
  assert(gc2_n_workers_acq(g) == 2);
}

static void test_worker_stop_l_delivers_stopreq(lua_State *L, global_State *g,
						TGState *tg)
{
  uint32_t actions = 0;

  assert(lj_gc2_workers_set(g, 1) == 1);
  assert(gc2_n_workers_acq(g) == 1);
  publish_manual(g, tg, LJ_GC2_HS_STOPREQ);
  assert(lj_gc2_workers_set_l(L, 0, &actions) == 1);
  assert((actions & LJ_GC2_HS_STOPREQ) != 0);
  assert(ljt_tg_has_stopreq(tg));
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(gc2_n_workers_acq(g) == 0);
  assert(gc2_worker_thread_acq(g, 0) == NULL);
  assert(la_load32_acq(&g->gc2.hs_pending) == 0);
  /* A returned-but-unchecked STOP action retains its one-shot VM dispatch edge
  ** until an L-aware check consumes it. This fixture deliberately inspects the
  ** action instead of throwing, so cancel that edge explicitly for teardown. */
  assert(lj_tg_poll_acq(tg) == 1);
  assert(lj_tg_reqmask_acq(tg) == 0);
  lj_tg_poll_rel(tg, 0);
  ljt_tg_clear_stopreq(tg);
  assert(lj_gc2_workers_set(g, 2) == 1);
  assert(gc2_n_workers_acq(g) == 2);
}

static void test_worker_start_l_native_stopreq(lua_State *L, global_State *g,
					       TGState *tg)
{
  WorkerStartStopReq ctx;
  pthread_t thread;
  uint32_t actions = 0;

  assert(lj_gc2_workers_set(g, 0) == 1);
  assert(gc2_n_workers_acq(g) == 0);

  ljt_tg_set_stopreq(tg);
  assert(lj_gc2_workers_set_l(L, 1, &actions) == 1);
  assert(actions == 0);
  assert(gc2_n_workers_acq(g) == 1);
  ljt_tg_clear_stopreq(tg);
  assert(lj_gc2_workers_set(g, 0) == 1);
  assert(gc2_n_workers_acq(g) == 0);

  ctx.g = g;
  ctx.tg = tg;
  ctx.saw_native = 0;
  ctx.published = 0;
  la_store32_rel(&worker_start_create_pause, 0);
  la_store32_rel(&worker_start_create_paused, 0);
  la_store32_rel(&worker_start_create_release, 0);

  assert(pthread_create(&thread, NULL, worker_start_stopreq_thread,
			&ctx) == 0);
  la_store32_rel(&worker_start_create_pause, 1);
  assert(lj_gc2_workers_set_l(L, 1, &actions) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(la_load32_acq(&ctx.saw_native) == 1);
  assert(la_load32_acq(&ctx.published) == 1);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(ljt_tg_has_stopreq(tg));
  assert(gc2_n_workers_acq(g) == 0);
  assert(gc2_worker_thread_acq(g, 0) == NULL);
  assert(la_load32_acq(&worker_start_create_paused) == 0);
  assert((actions & LJ_GC2_HS_STOPREQ) != 0);
  assert(lj_tg_poll_acq(tg) == 1);
  assert(lj_tg_reqmask_acq(tg) == 0);
  lj_tg_poll_rel(tg, 0);
  ljt_tg_clear_stopreq(tg);
  assert(lj_gc2_workers_set(g, 2) == 1);
  assert(gc2_n_workers_acq(g) == 2);
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

  wakes0 = gc2_worker_wakes_acq(g);
  lj_gc2_mark_begin(g);
  assert(gc2_worker_wakes_acq(g) > wakes0);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);

  async0 = gc2_worker_async_progress_acq(g);
  runs0 = gc2_worker_runs_acq(g);
  ssb0 = gc2_worker_ssb_converted_acq(g);
  grey0 = gc2_worker_grey_drained_acq(g);

  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  /* Setup/store rescans may share or fill this suffix and publish it before
  ** the explicit flush. The semantic assertion is eventual graph progress. */
  (void)lj_gc2_flush_ssb(g, tg);
  assert(wait_until_marked(g, obj2gco(grandchild)));
  assert(wait_ssb_empty(g));
  assert(gc2_worker_async_progress_acq(g) > async0);
  assert(gc2_worker_runs_acq(g) > runs0);
  assert(gc2_worker_ssb_converted_acq(g) > ssb0);
  assert(gc2_worker_grey_drained_acq(g) >= grey0 + 3u);
  assert(gc2_worker_active_acq(g) == 0);

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 3);
}

static void test_async_weak(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *weak, *key, *val;
  uint64_t async0, worker_weak0, clears0;
  int i;

  lua_settop(L, 0);
  make_weak_table(L, &weak, &key, &val);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  (void)lj_gc2_flush_ssb(g, tg);
  /* Snapshot readiness is published at weak-table discovery, before the same
  ** bounded traversal reaches its strong key. Wait for both independent
  ** effects; the snapshot counter alone is not a traversal-complete fence. */
  for (i = 0; i < 1000 &&
       (lj_gc2_test_weak_snapshot_count(g) == 0 ||
	lj_gc2_ismarked(g, obj2gco(key)) != 1); i++)
    sleep_ns(1000000L);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  async0 = gc2_worker_async_progress_acq(g);
  worker_weak0 = gc2_worker_weak_drained_acq(g);
  clears0 = gc2_weak_clear_tables_acq(g);
  lj_gc2_mark_to_weak(g);
  /*
  ** This fixture isolates parked worker weak-snapshot draining, not owner root
  ** closure. The value remains on the C test's Lua stack so a real weak-close
  ** root scan would correctly mark it and keep the weak-value entry alive.
  ** Publish the closed-frontier precondition that lj_gc2_weak_drain() requires
  ** and wake a worker to verify the async clear path itself.
  */
  gc2_weak_mark_closed_rel(g, 1);
  lj_gc2_test_worker_wake(g);
  for (i = 0; i < 1000 && !weak_entry_is_nil(L, weak, key); i++)
    sleep_ns(1000000L);
  assert(weak_entry_is_nil(L, weak, key));
  assert(gc2_worker_async_progress_acq(g) > async0);
  assert(gc2_worker_weak_drained_acq(g) > worker_weak0);
  assert(gc2_weak_clear_tables_acq(g) > clears0);
  assert(gc2_worker_active_acq(g) == 0);

  lj_gc2_cycle_to_idle(g);
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
  assert(g->gc2.n_threads == 4);

  extra_plain = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64, 0);
  extra_trav = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			      LJ_AF_TRAVERSABLE);
  assert(extra_plain != NULL);
  assert(extra_trav != NULL);
  extra_plain_a = lj_arena_of(extra_plain);
  extra_trav_a = lj_arena_of(extra_trav);
  lj_native_enter(tg);

  lj_gc2_test_finalizer_enter(g);
  assert(lj_gc2_test_finalizer_pending(g));
  g->gc2.cycle++;
  sweep_cycle = g->gc2.cycle;
  test_publish_sweep_phase(g);
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_PLAIN);
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  lj_arena_alloc_restore_sweep_kind(&extra_tg.alloc, LJ_ARENAK_PLAIN);
  extra_tg.alloc.prepare_epoch = sweep_cycle;
  lj_gc2_sweep_bridge_ready(g);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_PLAIN],
			     extra_plain_a));
  assert(arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     extra_trav_a));
  swept_a = extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE];
  assert(swept_a != NULL);

  async0 = gc2_worker_async_progress_acq(g);
  arenas0 = gc2_sweep_owner_arenas_acq(g);
  blocks0 = gc2_finalizer_sweep_blocks_acq(g);
  wakes0 = gc2_worker_wakes_acq(g);
  for (i = 0; i < 1000 &&
	      gc2_finalizer_sweep_blocks_acq(g) == blocks0; i++) {
    lj_gc2_test_worker_wake(g);
    sleep_ns(1000000L);
  }
  assert(gc2_worker_wakes_acq(g) > wakes0);
  assert(gc2_finalizer_sweep_blocks_acq(g) > blocks0);
  assert(gc2_sweep_owner_arenas_acq(g) == arenas0);
  assert(gc2_worker_async_progress_acq(g) == async0);
  assert(arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     swept_a));
  lj_gc2_test_finalizer_leave(g);
  assert(!lj_gc2_test_finalizer_pending(g));
  wakes0 = gc2_worker_wakes_acq(g);
  for (i = 0; i < 1000 &&
	      gc2_sweep_owner_arenas_acq(g) == arenas0; i++) {
    lj_gc2_test_worker_wake(g);
    sleep_ns(1000000L);
  }
  assert(gc2_worker_wakes_acq(g) > wakes0);
  assert(gc2_sweep_owner_arenas_acq(g) > arenas0);
  assert(gc2_worker_async_progress_acq(g) > async0);
  assert(!arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			      swept_a));
  /* Terminal commit keeps the arena CLOSED on the reclaimed stack. Owner
  ** allocation adopts it lazily only when reusable space is requested. */
  assert(arena_list_contains(lj_arena_alloc_reclaimed_head(
	&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE), swept_a) ||
         arena_list_contains(lj_arena_alloc_empty_reclaimed_head(
           &extra_tg.alloc), swept_a));
  assert((lj_arena_flags_acq(swept_a) & LJ_AF_RECLAIMED) != 0);
  assert((extra_plain_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert((swept_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert(swept_a->hdr.sweep_epoch == sweep_cycle);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_SWEEP);
  assert(!lj_gc2_sweep_pending(g));

  /* Keep the later close-boundary assertion synthetic and explicit. */
  gc2_sweep_bridge_ready_rel(g, 0);
  lj_arena_alloc_restore_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  /* Synthetic boundary: main TG had no prepared work. */
  tg->alloc.prepare_epoch = sweep_cycle;
  tg->alloc.sweep_epoch = sweep_cycle;
  mark_worker_tgs_sweep_ready(g, sweep_cycle);
  assert(lj_gc2_sweep_to_idle(g) == 0);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_SWEEP);
  async0 = gc2_worker_async_progress_acq(g);
  lj_gc2_sweep_bridge_ready(g);
  for (i = 0; i < 1000 && la_load32_acq(&g->gc2.phase) == LJ_GC2_SWEEP; i++) {
    lj_gc2_test_worker_wake(g);
    (void)lj_safepoint_poll(L);
    sleep_ns(1000000L);
  }
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_IDLE);
  assert(gc2_worker_async_progress_acq(g) > async0);

  lj_gc2_worker_stop(g);
  assert(gc2_worker_thread_acq(g, 0) == NULL);
  assert(gc2_worker_thread_acq(g, 1) == NULL);
  assert(gc2_n_workers_acq(g) == 0);
  assert(gc2_worker_exited_acq(g) == 2);
  assert(la_load32_acq(&g->gc2.n_threads) == 2);

  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(lj_tg_reclaim_dead(g) >= 1u);
  lj_tg_fini_thread(g, &extra_tg);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;

  assert(L != NULL);
  /* lua_newstate() performs guarded table stores whose IDLE remembered/rescan
  ** work is intentionally durable. Drain it with a real cycle so the worker
  ** contention case below owns the only synthetic SSB item it measures. */
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);

  assert(lj_gc2_workers_set(g, 2) == 1);
  assert(gc2_worker_thread_acq(g, 0) != NULL);
  assert(gc2_worker_thread_acq(g, 1) != NULL);
  assert(gc2_n_workers_acq(g) == 2);
  assert(gc2_worker_started_acq(g) == 2);
  assert(la_load32_acq(&g->gc2.n_threads) == 3);

  test_two_worker_contention(g);
  test_worker_retirement_uses_embedded_links(g);
  test_worker_join_failure_retains_lifetime(g);
  test_multistate_terminal_tg_reclaim(L, g);
  test_bounded_ssb_private_remainder(g);
  test_worker_wake_between_snapshot_and_park(L, g);
  test_worker_finalizer_mpsc_drain(L, g);
  test_worker_finalizer_sweep_mpsc_drain(L, g);
  test_worker_finalizer_uses_physical_actor(L, g);
  test_worker_real_finalizer_dispatch(L, g);
  test_finalizer_dispatch_all_waits_native(L, g, tg);
  test_finalizer_owner_leave_rewakes_worker(L, g);
  test_worker_stop_l_delivers_stopreq(L, g, tg);
  test_worker_start_l_native_stopreq(L, g, tg);
  test_async_mark(L, g, tg);
  test_async_weak(L, g, tg);
  test_async_sweep_and_stop(L, g, tg);
  test_worker_start_l_flushes_prestart_traces(L, g);

  lua_close(L);
  printf("t-gc2-worker-scheduler OK: parked worker wake/drain/finalizer handoff verified\n");
  return 0;
}
