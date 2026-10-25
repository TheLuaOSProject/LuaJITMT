/* Test-only measurements of exact lease and table-rescan credit operations. */
#include <assert.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_tab.h"
#include "lj_thr.h"

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

enum { SCALAR_CONTENTION_READS = 4000, SCALAR_CONTENTION_RETRIES = 16,
       SCALAR_CONTENTION_SAMPLES =
         SCALAR_CONTENTION_READS * SCALAR_CONTENTION_RETRIES };

typedef struct ScalarContention {
  lua_State *L;
  cTValue *tabroot, *keyroot;
  TValue *outroot;
  uint32_t start, done, resize_active;
  uint32_t successes, refusals, caller_retries, exhausted;
  uint32_t overlap_attempts;
  uint64_t success_ns[SCALAR_CONTENTION_SAMPLES];
  uint64_t refusal_ns[SCALAR_CONTENTION_SAMPLES];
  uint32_t success_samples, refusal_samples;
} ScalarContention;

#if defined(__APPLE__)
static mach_timebase_info_data_t scalar_timebase;
#endif
static uint32_t scalar_force_resize;

static uint64_t scalar_now_ns(void)
{
#if defined(__APPLE__)
  return (uint64_t)(((__uint128_t)mach_absolute_time() *
                     scalar_timebase.numer) / scalar_timebase.denom);
#else
  struct timespec now;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
         (uint64_t)now.tv_nsec;
#endif
}

static int scalar_u64_cmp(const void *a, const void *b)
{
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}

static uint64_t scalar_percentile_sorted(const uint64_t *samples,
                                         uint32_t count,
                                         uint32_t percentile)
{
  uint32_t index;
  assert(count != 0 && percentile <= 100);
  index = (uint32_t)((((uint64_t)count - 1u) * percentile) / 100u);
  return samples[index];
}

/* This deliberate resize occurs after the old scalar value has been copied
** but before its generation recheck. The held vector leases must make the
** retired allocation safe until the helper refuses and releases them. */
static void scalar_resize_at_result(lua_State *L, GCtab *t, uint32_t stage)
{
  if (stage == LJ_TAB_SCALAR_TEST_RESULT && scalar_force_resize) {
    scalar_force_resize = 0;
    lj_tab_resize(L, t, 256, 6);
    assert(lj_tab_array_retired_head_acq(G(L)) != NULL);
  }
}

static void *scalar_contention_reader(void *arg)
{
  ScalarContention *c = (ScalarContention *)arg;
  uint32_t logical, attempt;
  assert(lj_threading_attach(c->L));
  setintV(c->outroot, -9);
  while (!la_load32_acq(&c->start)) la_cpu_pause();
  for (logical = 0; logical < SCALAR_CONTENTION_READS; logical++) {
    int found = 0;
    for (attempt = 0; attempt < SCALAR_CONTENTION_RETRIES; attempt++) {
      setintV(c->outroot, -9);
      if (la_load32_acq(&c->resize_active)) c->overlap_attempts++;
      uint64_t start = scalar_now_ns();
      int ok = lj_tab_getscalar_rooted_try(c->L, c->tabroot, c->keyroot,
                                           c->outroot);
      uint64_t elapsed = scalar_now_ns() - start;
      if (ok) {
        assert(numberVnum(c->outroot) == 2.5);
        assert(c->success_samples < SCALAR_CONTENTION_SAMPLES);
        c->success_ns[c->success_samples++] = elapsed;
        c->successes++;
        found = 1;
        break;
      }
      assert(tvisint(c->outroot) && intV(c->outroot) == -9);
      assert(c->refusal_samples < SCALAR_CONTENTION_SAMPLES);
      c->refusal_ns[c->refusal_samples++] = elapsed;
      c->refusals++;
      if (attempt + 1u < SCALAR_CONTENTION_RETRIES)
        c->caller_retries++;
    }
    if (!found) c->exhausted++;
  }
  la_store32_rel(&c->done, 1);
  lj_threading_detach(c->L, 1);
  return NULL;
}

/* Measure individual bounded helper attempts while a second Lua actor
** repeatedly publishes and retires array generations. Refusal is a completed
** try outcome; only the fixture's caller-level retry is retried. */
static void scalar_resize_retirement_contention(void)
{
  lua_State *L = luaL_newstate(), *reader;
  GCtab *t;
  TValue *tabroot, *keyroot;
  ScalarContention c;
  pthread_t thread;
  LJTabScalarProgressMetrics tabm;
  LJGC2TestProgressMetrics gcm;
  uint32_t resize_rounds = 0;
  uint64_t s50 = 0, s95 = 0, s99 = 0, smax = 0;
  uint64_t r50 = 0, r95 = 0, r99 = 0, rmax = 0;
  assert(L);
#if defined(__APPLE__)
  assert(mach_timebase_info(&scalar_timebase) == KERN_SUCCESS &&
         scalar_timebase.denom != 0);
#endif
  luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF));
  lj_gc_threshold_store(G(L), UINT64_MAX / 2u);
  lj_gc2_hard_store(G(L), UINT64_MAX / 2u);
  lj_gc2_trigger_store(G(L), UINT64_MAX / 2u);
  reader = lua_newthread(L);  /* Retain a real owner for the reader pthread. */
  lua_createtable(L, 32, 8);
  t = tabV(L->top - 1);
  lua_pushnumber(L, 2.5); lua_rawseti(L, -2, 1);
  lj_tab_resize(L, t, 64, 7);  /* Move the live array out of colocated storage. */
  lua_pushinteger(L, 1);
  lua_pushnumber(L, -9);
  tabroot = L->top - 3; keyroot = L->top - 2;

  /* One forced overlap proves the exact stale-generation refusal, retired
  ** vector lifetime through the held lease, and successful subsequent retry. */
  lj_tab_test_scalar_progress_metrics_reset();
  scalar_force_resize = 1;
  lj_tab_test_set_scalar_rooted_try_hook(scalar_resize_at_result);
  assert(!lj_tab_getscalar_rooted_try(L, tabroot, keyroot, L->top - 1));
  lj_tab_test_set_scalar_rooted_try_hook(NULL);
  assert(numberVnum(L->top - 1) == -9 && scalar_force_resize == 0);
  assert(lj_tab_array_retired_head_acq(G(L)) != NULL);
  setnumV(L->top - 1, -9);
  assert(lj_tab_getscalar_rooted_try(L, tabroot, keyroot, L->top - 1));
  assert(numberVnum(L->top - 1) == 2.5);
  lj_tab_test_scalar_progress_metrics_read(&tabm);
  assert(tabm.calls == 2 && tabm.hits == 1 && tabm.refusals == 1);
  printf("resize overlap: attempts=%llu hits=%llu refusals=%llu caller-retries=1 retired-vector=queued\n",
    (unsigned long long)tabm.calls, (unsigned long long)tabm.hits,
    (unsigned long long)tabm.refusals);

  memset(&c, 0, sizeof(c));
  c.L = reader;
  {
    int table_index = (int)(tabroot - L->base) + 1;
    lua_pushvalue(L, table_index);
    lua_xmove(L, reader, 1);
    lua_pushinteger(reader, 1);
    lua_pushnumber(reader, -9);
    /* lua_pushvalue may relocate the main stack. Recompute roots afterwards. */
    tabroot = L->base + table_index - 1;
    keyroot = tabroot + 1;
  }
  c.tabroot = reader->base; c.keyroot = reader->base + 1;
  c.outroot = reader->base + 2;
  lj_tab_test_scalar_progress_metrics_reset();
  lj_gc2_test_progress_metrics_reset();
  assert(pthread_create(&thread, NULL, scalar_contention_reader, &c) == 0);
  la_store32_rel(&c.resize_active, 1);
  la_store32_rel(&c.start, 1);
  while (!la_load32_acq(&c.done) && resize_rounds < 512u) {
    uint32_t asize = (resize_rounds & 1u) ? 128u : 256u;
    lj_tab_resize(L, t, asize, 7);
    resize_rounds++;
  }
  la_store32_rel(&c.resize_active, 0);
  assert(pthread_join(thread, NULL) == 0 && la_load32_acq(&c.done));
  assert(resize_rounds != 0);
  lj_tab_test_scalar_progress_metrics_read(&tabm);
  lj_gc2_test_progress_metrics_read(&gcm);
  assert(c.successes + c.exhausted == SCALAR_CONTENTION_READS);
  assert(c.overlap_attempts != 0);
  assert(c.success_samples == c.successes && c.refusal_samples == c.refusals);
  assert(tabm.calls == (uint64_t)c.successes + c.refusals);
  assert(tabm.hits == c.successes && tabm.refusals == c.refusals);
  assert(gcm.lease_rescue_retry == 0);
  assert(lj_tab_array_retired_head_acq(G(L)) != NULL);
  if (c.success_samples) {
    qsort(c.success_ns, c.success_samples, sizeof(*c.success_ns), scalar_u64_cmp);
    s50 = scalar_percentile_sorted(c.success_ns, c.success_samples, 50);
    s95 = scalar_percentile_sorted(c.success_ns, c.success_samples, 95);
    s99 = scalar_percentile_sorted(c.success_ns, c.success_samples, 99);
    smax = c.success_ns[c.success_samples - 1u];
  }
  if (c.refusal_samples) {
    qsort(c.refusal_ns, c.refusal_samples, sizeof(*c.refusal_ns), scalar_u64_cmp);
    r50 = scalar_percentile_sorted(c.refusal_ns, c.refusal_samples, 50);
    r95 = scalar_percentile_sorted(c.refusal_ns, c.refusal_samples, 95);
    r99 = scalar_percentile_sorted(c.refusal_ns, c.refusal_samples, 99);
    rmax = c.refusal_ns[c.refusal_samples - 1u];
  }
  printf("resize/retirement contention: reads=%u logical-hits=%u exhausted=%u attempts=%llu hits=%llu refusals=%llu caller-retries=%u retry-budget=%u resizes=%u lease-rescue-retries=%llu\n",
    SCALAR_CONTENTION_READS, c.successes, c.exhausted,
    (unsigned long long)tabm.calls,
    (unsigned long long)tabm.hits, (unsigned long long)tabm.refusals,
    c.caller_retries, SCALAR_CONTENTION_RETRIES, resize_rounds,
    (unsigned long long)gcm.lease_rescue_retry);
  printf("  resize-window overlap attempts=%u leases=%llu admitted=%llu refused=%llu rescue-retries=%llu\n",
    c.overlap_attempts, (unsigned long long)gcm.lease_attempts,
    (unsigned long long)gcm.lease_admitted,
    (unsigned long long)gcm.lease_refused,
    (unsigned long long)gcm.lease_rescue_retry);
  printf("  raw instrumented try ns: success p50/p95/p99/max=%llu/%llu/%llu/%llu refusal p50/p95/p99/max=%llu/%llu/%llu/%llu\n",
    (unsigned long long)s50, (unsigned long long)s95,
    (unsigned long long)s99, (unsigned long long)smax,
    (unsigned long long)r50, (unsigned long long)r95,
    (unsigned long long)r99, (unsigned long long)rmax);
  lua_close(L);
}

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
  scalar_resize_retirement_contention();
  independent_progress_with_stalled_publisher();
  pending_credit_measurement();
  puts("lockless progress metrics OK");
  return 0;
}
