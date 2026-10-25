/* Test-only measurements of exact lease and table-rescan credit operations. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_tab.h"

#if !defined(LJ_GC2_TEST_HELPERS) || !defined(LJ_TAB_TEST_HELPERS)
#error "progress metrics fixture requires GC2 and table test helpers"
#endif

typedef struct Reclaim {
  global_State *g;
  uint32_t done;
} Reclaim;

typedef struct CreditWorker {
  global_State *g;
  GCtab *t;
  uint32_t *start;
  uint32_t loops;
  uint32_t failures;
} CreditWorker;

typedef struct PausedPublisher {
  global_State *g;
  GCtab *t;
  int installed;
} PausedPublisher;

static void *reclaim_main(void *arg)
{
  Reclaim *r = (Reclaim *)arg;
  (void)lj_gc2_reclaim_retired(r->g, lj_gc2_retire_epoch(r->g) + 1u);
  la_store32_rel(&r->done, 1);
  return NULL;
}

static void *credit_worker(void *arg)
{
  CreditWorker *w = (CreditWorker *)arg;
  while (!la_load32_acq(w->start)) la_cpu_pause();
  for (uint32_t i = 0; i < w->loops; i++) {
    if (!lj_gc2_test_table_rescan_set(w->g, w->t) ||
        !lj_gc2_test_table_rescan_clear(w->g, w->t))
      w->failures++;
  }
  return NULL;
}

static void *paused_publisher_main(void *arg)
{
  PausedPublisher *p = (PausedPublisher *)arg;
  p->installed = lj_gc2_test_table_rescan_set(p->g, p->t);
  return NULL;
}

static void bounded_scalar_during_stalled_reclaimer(void)
{
  lua_State *L = luaL_newstate();
  TValue out;
  TValue *tabroot, *keyroot;
  Reclaim r;
  pthread_t thread;
  struct timespec delay = { 0, 1000000 };
  LJTabScalarProgressMetrics tabm;
  LJGC2TestProgressMetrics gcm;
  uint32_t waits, i;
  int hits = 0;
  assert(L);
  luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF));
  lua_createtable(L, 1, 0);
  lua_pushnumber(L, 2.5); lua_rawseti(L, -2, 1);
  lua_pushinteger(L, 1);
  tabroot = L->top - 2; keyroot = L->top - 1;
  setintV(&out, -9);
  r.g = G(L); r.done = 0;
  assert(gc2_phase_acq(r.g) == LJ_GC2_IDLE);
  assert(gc2_smr_readers_acq(r.g) == 0);
  lj_gc2_test_idle_reclaim_pause_after_jit_quiescence();
  assert(pthread_create(&thread, NULL, reclaim_main, &r) == 0);
  for (i = 0; i < 5000 && !lj_gc2_test_idle_reclaim_paused(); i++) {
    assert(!la_load32_acq(&r.done)); nanosleep(&delay, NULL);
  }
  assert(lj_gc2_test_idle_reclaim_paused());
  assert(gc2_smr_reclaiming_acq(r.g) == LJ_GC2_SMR_META_EXCLUSIVE);

  lj_tab_test_scalar_progress_metrics_reset();
  lj_gc2_test_progress_metrics_reset();
  lj_tab_test_reset_wait_l_calls();
  lj_tab_test_reset_wait_no_l_calls();
  waits = lj_tab_test_wait_l_calls();
  alarm(5);
  for (i = 0; i < 1000; i++) {
    setintV(&out, -9);
    hits += lj_tab_getscalar_rooted_try(L, tabroot, keyroot, &out);
    assert(numberVnum(&out) == 2.5);
  }
  /* A valid but missing key is a bounded refusal: it still releases the
  ** table lease and preserves the caller's output while reclamation is held. */
  setintV((TValue *)keyroot, 2);
  setintV(&out, -9);
  assert(!lj_tab_getscalar_rooted_try(L, tabroot, keyroot, &out));
  assert(numberVnum(&out) == -9);
  alarm(0);
  assert(hits == 1000);
  assert(lj_tab_test_wait_l_calls() == waits);
  assert(lj_tab_test_wait_no_l_calls() == 0);
  assert(lj_gc2_test_idle_reclaim_paused() && !la_load32_acq(&r.done));
  lj_tab_test_scalar_progress_metrics_read(&tabm);
  lj_gc2_test_progress_metrics_read(&gcm);
  assert(tabm.calls == 1001 && tabm.hits == 1000 && tabm.refusals == 1);
  assert(gcm.lease_attempts >= 1001 && gcm.lease_admitted >= 1001);
  assert(gcm.lease_refused == 0 && gcm.lease_rescue_retry == 0);
  printf("stalled-reclaimer scalar: calls=%llu hits=%llu table-refusals=%llu leases=%llu admitted=%llu lease-refusals=%llu waits=0\n",
    (unsigned long long)tabm.calls, (unsigned long long)tabm.hits,
    (unsigned long long)tabm.refusals,
    (unsigned long long)gcm.lease_attempts,
    (unsigned long long)gcm.lease_admitted,
    (unsigned long long)gcm.lease_refused);
  lj_gc2_test_idle_reclaim_release();
  assert(pthread_join(thread, NULL) == 0 && la_load32_acq(&r.done));
  lua_close(L);
}

static void pending_credit_measurement(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  LJGC2TestProgressMetrics m;
  uint32_t before;
  enum { NWORKER = 4, LOOPS = 10000 };
  CreditWorker workers[NWORKER];
  pthread_t threads[NWORKER];
  uint32_t start = 0, i;
  struct timespec begin, end;
  uint64_t elapsed_ns;
  assert(L);
  lua_createtable(L, 0, 0);
  t = tabV(L->top - 1);
  lj_gc2_test_progress_metrics_reset();
  before = gc2_table_rescan_pending_acq(G(L));
  assert(lj_gc2_test_table_rescan_set(G(L), t) == 1);
  assert(lj_gc2_test_table_rescan_set(G(L), t) == 0);
  assert(gc2_table_rescan_pending_acq(G(L)) == before + 1u);
  assert(lj_gc2_test_table_rescan_clear(G(L), t) == 1);
  assert(gc2_table_rescan_pending_acq(G(L)) == before);
  lj_gc2_test_progress_metrics_read(&m);
  assert(m.pending_reservations == 2);
  assert(m.pending_reservation_conflicts == 1);
  assert(m.pending_credit_adds == 2 && m.pending_credit_subs == 2);
  printf("rescan credit: reserves=%llu conflicts=%llu adds=%llu subs=%llu retries=%llu pending=0\n",
    (unsigned long long)m.pending_reservations,
    (unsigned long long)m.pending_reservation_conflicts,
    (unsigned long long)m.pending_credit_adds,
    (unsigned long long)m.pending_credit_subs,
    (unsigned long long)m.pending_cas_retries);

  for (i = 0; i < NWORKER; i++) {
    lua_createtable(L, 0, 0);
    workers[i].g = G(L); workers[i].t = tabV(L->top - 1);
    workers[i].start = &start; workers[i].loops = LOOPS;
    workers[i].failures = 0;
  }
  before = gc2_table_rescan_pending_acq(G(L));
  lj_gc2_test_progress_metrics_reset();
  for (i = 0; i < NWORKER; i++)
    assert(pthread_create(&threads[i], NULL, credit_worker, &workers[i]) == 0);
  assert(clock_gettime(CLOCK_MONOTONIC, &begin) == 0);
  la_store32_rel(&start, 1);
  for (i = 0; i < NWORKER; i++)
    assert(pthread_join(threads[i], NULL) == 0 && workers[i].failures == 0);
  assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
  elapsed_ns = (uint64_t)(end.tv_sec - begin.tv_sec) * UINT64_C(1000000000);
  if (end.tv_nsec >= begin.tv_nsec)
    elapsed_ns += (uint64_t)(end.tv_nsec - begin.tv_nsec);
  else
    elapsed_ns -= (uint64_t)(begin.tv_nsec - end.tv_nsec);
  assert(gc2_table_rescan_pending_acq(G(L)) == before);
  lj_gc2_test_progress_metrics_read(&m);
  assert(m.pending_reservations == (uint64_t)NWORKER * LOOPS);
  assert(m.pending_credit_adds == (uint64_t)NWORKER * LOOPS);
  assert(m.pending_credit_subs == (uint64_t)NWORKER * LOOPS);
  assert(m.pending_reservation_conflicts == 0);
  printf("rescan contention: workers=%u rounds=%u credit-add/sub=%llu/%llu elapsed-ns=%llu ops/s=%llu\n",
    NWORKER, LOOPS,
    (unsigned long long)m.pending_credit_adds,
    (unsigned long long)m.pending_credit_subs,
    (unsigned long long)elapsed_ns,
    (unsigned long long)(((uint64_t)NWORKER * LOOPS * 1000000000u) /
                         (elapsed_ns ? elapsed_ns : 1u)));
  lua_close(L);
}

static void independent_progress_with_stalled_publisher(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  GCtab *blocked, *independent;
  PausedPublisher p;
  pthread_t thread;
  LJGC2TestProgressMetrics m;
  uint32_t before, i;
  assert(L);
  lua_createtable(L, 0, 0);
  blocked = tabV(L->top - 1);
  lua_createtable(L, 0, 0);
  independent = tabV(L->top - 1);
  g = G(L); before = gc2_table_rescan_pending_acq(g);
  lj_gc2_test_progress_metrics_reset();
  p.g = g; p.t = blocked; p.installed = 0;
  lj_gc2_test_table_rescan_pause(LJ_GC2_TABLE_RESCAN_TEST_INSTALLING);
  assert(pthread_create(&thread, NULL, paused_publisher_main, &p) == 0);
  for (i = 0; i < 1000000u &&
       lj_gc2_test_table_rescan_paused() !=
         LJ_GC2_TABLE_RESCAN_TEST_INSTALLING; i++)
    la_cpu_pause();
  assert(lj_gc2_test_table_rescan_paused() ==
         LJ_GC2_TABLE_RESCAN_TEST_INSTALLING);
  assert(gc2_table_rescan_pending_acq(g) == before + 1u);
  assert(lj_gc2_test_table_rescan_clear(g, blocked) == 0);
  assert(lj_tab_gc2_rescan_state_acq(blocked) == LJ_TAB_RESCAN_CANCELLED);

  /* This operation completes while the first publisher is deliberately
  ** stalled inside INSTALLING. Its credit must not depend on helping it. */
  alarm(5);
  assert(lj_gc2_test_table_rescan_set(g, independent) == 1);
  assert(lj_gc2_test_table_rescan_clear(g, independent) == 1);
  alarm(0);
  assert(lj_gc2_test_table_rescan_paused() ==
         LJ_GC2_TABLE_RESCAN_TEST_INSTALLING);
  lj_gc2_test_table_rescan_release();
  assert(pthread_join(thread, NULL) == 0 && p.installed == 1);
  assert(lj_tab_gc2_rescan_state_acq(blocked) == LJ_TAB_RESCAN_NONE);
  assert(gc2_table_rescan_pending_acq(g) == before);
  lj_gc2_test_progress_metrics_read(&m);
  assert(m.pending_reservations == 2);
  assert(m.pending_credit_adds == 2 && m.pending_credit_subs == 2);
  printf("stalled-publisher independent-table: reservations=%llu adds/subs=%llu/%llu pending-restored=1\n",
    (unsigned long long)m.pending_reservations,
    (unsigned long long)m.pending_credit_adds,
    (unsigned long long)m.pending_credit_subs);
  lua_close(L);
}

int main(void)
{
  bounded_scalar_during_stalled_reclaimer();
  independent_progress_with_stalled_publisher();
  pending_credit_measurement();
  puts("lockless progress metrics OK");
  return 0;
}
