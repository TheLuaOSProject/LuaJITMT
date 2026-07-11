/*
** t-strcanon-model.c - standalone model of the canonical string directory.
**
** The canonical directory is the union of the main intern table and the
** quarantine table.  Before logical commit, an old body may be present in
** either or both indices, but an equal fresh body must never be published.
** This model covers the Stage A/B ordering needed to establish that rule:
**
**   publish quarantine record -> publish QACTIVE -> unlink main edge
**
** A main miss must consult quarantine before installing a fresh body.
** QACTIVE and QRESCUED both reserve the same old body; rescue changes the
** lifecycle word and may re-link that same body, never an equal replacement.
**
** The first half exhaustively explores bounded interleavings of one sweep
** owner and two interners.  It also requires the model checker to find a
** counterexample when either critical rule is deliberately removed.  The
** second half exercises the acquire/release implementation with pthreads and
** C11 atomics, including a many-rescuer QACTIVE/QRESCUED race.
**
** Build & run:
**   cc -std=c11 -O2 -Wall -Wextra -Werror -pthread \
**      t-strcanon-model.c -o strcanon-model && ./strcanon-model
*/
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
  PTR_NONE = 0,
  PTR_OLD = 1,
  PTR_NEW0 = 2,
  PTR_NEW1 = 3
};

enum {
  CANON_LIVE = 0,
  CANON_CANDIDATE,
  CANON_QACTIVE,
  CANON_QRESCUED
};

/* ----------------------------------------------------------------------- */
/* Bounded interleaving explorer.  Each step below is one atomic operation
** or one decision based on a previously acquired value. */

enum {
  I_LOAD_MAIN = 0,
  I_HANDLE_MAIN,
  I_LOAD_Q,
  I_HANDLE_Q,
  I_RELINK,
  I_WAIT_PREPARED,
  I_DONE
};

typedef struct ModelInterner {
  uint8_t pc;
  uint8_t seen_main;
  uint8_t seen_q;
  uint8_t result;
} ModelInterner;

typedef struct ModelState {
  uint8_t main_body;
  uint8_t q_body;
  uint8_t canon;
  uint8_t collector_pc;
  uint8_t observed_qstates;  /* bit 0 QACTIVE, bit 1 QRESCUED. */
  uint8_t saw_gap;
  uint8_t saw_duplicate;
  ModelInterner in[2];
} ModelState;

typedef struct ModelConfig {
  int inverted_unlink;
  int consult_quarantine;
} ModelConfig;

typedef struct ModelStats {
  uint64_t terminals;
  uint64_t gap_terminals;
  uint64_t duplicate_terminals;
  uint64_t wrong_identity_terminals;
  uint64_t saw_qactive_terminals;
  uint64_t saw_qrescued_terminals;
  uint64_t saw_both_qstates_terminals;
  uint64_t truncated;
} ModelStats;

static int model_old_state(uint8_t canon)
{
  return canon == CANON_LIVE || canon == CANON_CANDIDATE ||
         canon == CANON_QACTIVE || canon == CANON_QRESCUED;
}

static void model_check_invariants(ModelState *s)
{
  int i;

  /* There is an absence gap if the not-yet-committed old identity is in
  ** neither directory.  A fresh body does not retroactively close the gap. */
  if (model_old_state(s->canon) && s->main_body != PTR_OLD &&
      s->q_body != PTR_OLD)
    s->saw_gap = 1;

  /* A fresh main identity conflicts with an acquirable quarantine identity,
  ** with the still-live old lifecycle, or with an old identity already
  ** returned to a caller. */
  if (s->main_body >= PTR_NEW0 && model_old_state(s->canon))
    s->saw_duplicate = 1;
  for (i = 0; i < 2; i++) {
    if ((s->in[i].result == PTR_OLD && s->main_body >= PTR_NEW0) ||
        (s->in[i].result >= PTR_NEW0 &&
         (s->main_body == PTR_OLD || s->q_body == PTR_OLD)))
      s->saw_duplicate = 1;
  }
  if (s->in[0].result != PTR_NONE && s->in[1].result != PTR_NONE &&
      s->in[0].result != s->in[1].result)
    s->saw_duplicate = 1;
}

static int collector_done(const ModelState *s)
{
  return s->collector_pc == 3;
}

static void model_collector_step(ModelState *s, const ModelConfig *cfg)
{
  assert(!collector_done(s));

  if (!cfg->inverted_unlink) {
    switch (s->collector_pc) {
    case 0:  /* A prepared record is visible before it becomes authoritative. */
      if (s->canon != CANON_CANDIDATE) {
        s->collector_pc = 3;
      } else {
        s->q_body = PTR_OLD;
        s->collector_pc = 1;
      }
      break;
    case 1:  /* Lifecycle CAS is the quarantine reservation point. */
      if (s->canon == CANON_CANDIDATE) {
        s->canon = CANON_QACTIVE;
        s->collector_pc = 2;
      } else {
        /* A main-table reader rescued CANDIDATE.  Cancel the prepared record
        ** and retain the linked main identity. */
        s->q_body = PTR_NONE;
        s->collector_pc = 3;
      }
      break;
    case 2:  /* Only an authoritative, discoverable Q record permits unlink. */
      if (s->main_body == PTR_OLD) s->main_body = PTR_NONE;
      s->collector_pc = 3;
      break;
    default:
      assert(0);
    }
  } else {
    /* Deliberately broken ordering used as a model-checker self-test. */
    switch (s->collector_pc) {
    case 0:
      if (s->canon != CANON_CANDIDATE) {
        s->collector_pc = 3;
      } else {
        if (s->main_body == PTR_OLD) s->main_body = PTR_NONE;
        s->collector_pc = 1;
      }
      break;
    case 1:
      s->q_body = PTR_OLD;
      s->collector_pc = 2;
      break;
    case 2:
      if (s->canon == CANON_CANDIDATE) {
        s->canon = CANON_QACTIVE;
      } else {
        s->q_body = PTR_NONE;
      }
      s->collector_pc = 3;
      break;
    default:
      assert(0);
    }
  }
}

static int model_interner_enabled(const ModelState *s, int id)
{
  const ModelInterner *in = &s->in[id];
  if (in->pc == I_DONE) return 0;
  if (in->pc != I_WAIT_PREPARED) return 1;
  /* A lookup which sees a prepared but non-authoritative record retries after
  ** the owner either arms or cancels it.  Do not enumerate scheduler-only
  ** busy-loop iterations while the owner has not advanced. */
  return s->q_body != PTR_OLD || s->main_body != PTR_NONE ||
         s->canon == CANON_QACTIVE || s->canon == CANON_QRESCUED;
}

static void model_rescue(ModelState *s, ModelInterner *in, int from_q)
{
  if (s->canon == CANON_QACTIVE) {
    s->canon = CANON_QRESCUED;
    if (from_q) s->observed_qstates |= 1u;
    in->result = PTR_OLD;
    in->pc = from_q ? I_RELINK : I_DONE;
  } else if (s->canon == CANON_QRESCUED) {
    if (from_q) s->observed_qstates |= 2u;
    in->result = PTR_OLD;
    in->pc = from_q ? I_RELINK : I_DONE;
  }
}

static void model_interner_step(ModelState *s, const ModelConfig *cfg, int id)
{
  ModelInterner *in = &s->in[id];
  uint8_t fresh = (uint8_t)(PTR_NEW0 + id);

  assert(model_interner_enabled(s, id));
  switch (in->pc) {
  case I_LOAD_MAIN:
    in->seen_main = s->main_body;
    in->pc = I_HANDLE_MAIN;
    break;

  case I_HANDLE_MAIN:
    if (in->seen_main == PTR_OLD) {
      if (s->canon == CANON_CANDIDATE) {
        s->canon = CANON_LIVE;  /* CANDIDATE -> LIVE rescue CAS. */
        in->result = PTR_OLD;
        in->pc = I_DONE;
      } else if (s->canon == CANON_QACTIVE ||
                 s->canon == CANON_QRESCUED) {
        model_rescue(s, in, 0);
      } else {
        assert(s->canon == CANON_LIVE);
        in->result = PTR_OLD;
        in->pc = I_DONE;
      }
    } else if (in->seen_main >= PTR_NEW0) {
      in->result = in->seen_main;
      in->pc = I_DONE;
    } else if (cfg->consult_quarantine) {
      in->pc = I_LOAD_Q;
    } else {
      in->seen_q = PTR_NONE;
      in->pc = I_HANDLE_Q;
    }
    break;

  case I_LOAD_Q:
    in->seen_q = s->q_body;
    in->pc = I_HANDLE_Q;
    break;

  case I_HANDLE_Q:
    if (in->seen_q == PTR_OLD) {
      if (s->canon == CANON_QACTIVE ||
          s->canon == CANON_QRESCUED) {
        model_rescue(s, in, 1);
      } else {
        /* The record was observed before its lifecycle CAS, or while it was
        ** being cancelled.  It is not authoritative yet. */
        in->pc = I_WAIT_PREPARED;
      }
    } else if (s->main_body == PTR_NONE) {
      /* Main bucket CAS is the fresh canonical linearization point. */
      s->main_body = fresh;
      in->result = fresh;
      in->pc = I_DONE;
    } else {
      /* Lost the insert CAS to a re-link or another constructor. */
      in->pc = I_LOAD_MAIN;
    }
    break;

  case I_RELINK:
    if (s->main_body == PTR_NONE) s->main_body = PTR_OLD;
    /* A fresh body here is already a uniqueness violation.  Leave it visible
    ** so the invariant checker records the counterexample. */
    in->pc = I_DONE;
    break;

  case I_WAIT_PREPARED:
    in->pc = I_LOAD_MAIN;
    break;

  default:
    assert(0);
  }
}

#define MODEL_MAX_DEPTH 32u

static void model_explore(ModelState state, const ModelConfig *cfg,
                          ModelStats *stats, unsigned depth)
{
  int i, enabled = 0;

  model_check_invariants(&state);
  if (collector_done(&state) && state.in[0].pc == I_DONE &&
      state.in[1].pc == I_DONE) {
    stats->terminals++;
    if (state.saw_gap) stats->gap_terminals++;
    if (state.saw_duplicate) stats->duplicate_terminals++;
    if (state.in[0].result != PTR_OLD || state.in[1].result != PTR_OLD)
      stats->wrong_identity_terminals++;
    if (state.observed_qstates & 1u) stats->saw_qactive_terminals++;
    if (state.observed_qstates & 2u) stats->saw_qrescued_terminals++;
    if ((state.observed_qstates & 3u) == 3u)
      stats->saw_both_qstates_terminals++;
    return;
  }

  if (depth == MODEL_MAX_DEPTH) {
    stats->truncated++;
    return;
  }

  if (!collector_done(&state)) {
    ModelState next = state;
    model_collector_step(&next, cfg);
    model_explore(next, cfg, stats, depth + 1);
    enabled = 1;
  }
  for (i = 0; i < 2; i++) {
    if (model_interner_enabled(&state, i)) {
      ModelState next = state;
      model_interner_step(&next, cfg, i);
      model_explore(next, cfg, stats, depth + 1);
      enabled = 1;
    }
  }
  assert(enabled && "model reached a non-terminal dead end");
}

static ModelStats run_model(ModelConfig cfg)
{
  ModelState state;
  ModelStats stats;
  memset(&state, 0, sizeof(state));
  memset(&stats, 0, sizeof(stats));
  state.main_body = PTR_OLD;
  state.canon = CANON_CANDIDATE;
  model_explore(state, &cfg, &stats, 0);
  assert(stats.terminals != 0);
  assert(stats.truncated == 0);
  return stats;
}

static void test_bounded_interleavings(void)
{
  ModelStats good = run_model((ModelConfig){ 0, 1 });
  ModelStats inverted = run_model((ModelConfig){ 1, 1 });
  ModelStats no_q_lookup = run_model((ModelConfig){ 0, 0 });

  assert(good.gap_terminals == 0);
  assert(good.duplicate_terminals == 0);
  assert(good.wrong_identity_terminals == 0);
  assert(good.saw_qactive_terminals != 0);
  assert(good.saw_qrescued_terminals != 0);
  assert(good.saw_both_qstates_terminals != 0);

  /* Self-tests: the explorer must detect both forbidden simplifications. */
  assert(inverted.gap_terminals != 0);
  assert(inverted.duplicate_terminals != 0);
  assert(no_q_lookup.duplicate_terminals != 0);

  printf("  exhaustive: %" PRIu64 " valid schedules; inverted ordering "
         "exposed %" PRIu64 " gap and %" PRIu64 " duplicate schedules; "
         "main-only miss exposed %" PRIu64 " duplicate schedules\n",
         good.terminals, inverted.gap_terminals,
         inverted.duplicate_terminals, no_q_lookup.duplicate_terminals);
}

/* ----------------------------------------------------------------------- */
/* Real C11 acquire/release race harness. */

typedef struct TestBarrier {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  unsigned parties;
  unsigned arrived;
  unsigned generation;
} TestBarrier;

static void test_barrier_init(TestBarrier *b, unsigned parties)
{
  memset(b, 0, sizeof(*b));
  b->parties = parties;
  assert(pthread_mutex_init(&b->mutex, NULL) == 0);
  assert(pthread_cond_init(&b->cond, NULL) == 0);
}

static void test_barrier_wait(TestBarrier *b)
{
  unsigned generation;
  assert(pthread_mutex_lock(&b->mutex) == 0);
  generation = b->generation;
  if (++b->arrived == b->parties) {
    b->arrived = 0;
    b->generation++;
    assert(pthread_cond_broadcast(&b->cond) == 0);
  } else {
    do {
      assert(pthread_cond_wait(&b->cond, &b->mutex) == 0);
    } while (generation == b->generation);
  }
  assert(pthread_mutex_unlock(&b->mutex) == 0);
}

static void test_barrier_destroy(TestBarrier *b)
{
  assert(pthread_cond_destroy(&b->cond) == 0);
  assert(pthread_mutex_destroy(&b->mutex) == 0);
}

typedef struct ConcurrentDirectory {
  _Atomic(uintptr_t) main_body;
  _Atomic(uintptr_t) q_body;
  _Atomic(unsigned) canon;
  _Atomic(uint64_t) main_misses;
  _Atomic(uint64_t) q_hits;
  _Atomic(uint64_t) qactive_wins;
  _Atomic(uint64_t) qrescued_hits;
  _Atomic(uint64_t) unlinks;
} ConcurrentDirectory;

static void concurrent_init(ConcurrentDirectory *d)
{
  atomic_init(&d->main_body, PTR_NONE);
  atomic_init(&d->q_body, PTR_NONE);
  atomic_init(&d->canon, CANON_LIVE);
  atomic_init(&d->main_misses, 0);
  atomic_init(&d->q_hits, 0);
  atomic_init(&d->qactive_wins, 0);
  atomic_init(&d->qrescued_hits, 0);
  atomic_init(&d->unlinks, 0);
}

static uint32_t mix32(uint32_t x)
{
  x ^= x >> 16;
  x *= UINT32_C(0x7feb352d);
  x ^= x >> 15;
  x *= UINT32_C(0x846ca68b);
  return x ^ (x >> 16);
}

static void race_pause(unsigned round, unsigned role, unsigned phase)
{
  uint32_t x = mix32((uint32_t)round * UINT32_C(0x9e3779b9) ^
                     (uint32_t)role * UINT32_C(0x85ebca6b) ^ phase);
  unsigned i;
  for (i = 0; i < (x & 15u); i++) atomic_signal_fence(memory_order_seq_cst);
  if ((x & 63u) == 0) sched_yield();
}

static uintptr_t concurrent_intern(ConcurrentDirectory *d, unsigned id,
                                   unsigned round)
{
  uintptr_t fresh = (uintptr_t)(PTR_NEW0 + (id & 1u));
  unsigned attempts;

  for (attempts = 0; attempts < 256; attempts++) {
    uintptr_t main_body =
      atomic_load_explicit(&d->main_body, memory_order_acquire);
    race_pause(round, id + 1u, attempts * 4u);
    if (main_body == PTR_OLD) {
      for (;;) {
        unsigned canon = atomic_load_explicit(&d->canon,
                                              memory_order_acquire);
        if (canon == CANON_LIVE) return PTR_OLD;
        if (canon == CANON_CANDIDATE) {
          unsigned expected = CANON_CANDIDATE;
          if (atomic_compare_exchange_strong_explicit(
                &d->canon, &expected, CANON_LIVE,
                memory_order_acq_rel, memory_order_acquire))
            return PTR_OLD;
          continue;
        }
        if (canon == CANON_QACTIVE) {
          unsigned expected = CANON_QACTIVE;
          if (atomic_compare_exchange_strong_explicit(
                &d->canon, &expected, CANON_QRESCUED,
                memory_order_acq_rel, memory_order_acquire)) {
            atomic_fetch_add_explicit(&d->qactive_wins, 1,
                                      memory_order_relaxed);
            return PTR_OLD;
          }
          continue;
        }
        assert(canon == CANON_QRESCUED);
        atomic_fetch_add_explicit(&d->qrescued_hits, 1,
                                  memory_order_relaxed);
        return PTR_OLD;
      }
    }
    if (main_body >= PTR_NEW0) return main_body;

    atomic_fetch_add_explicit(&d->main_misses, 1, memory_order_relaxed);
    {
      uintptr_t q_body =
        atomic_load_explicit(&d->q_body, memory_order_acquire);
      race_pause(round, id + 1u, attempts * 4u + 1u);
      if (q_body == PTR_OLD) {
        unsigned canon;
        atomic_fetch_add_explicit(&d->q_hits, 1, memory_order_relaxed);
        canon = atomic_load_explicit(&d->canon, memory_order_acquire);
        if (canon == CANON_QACTIVE) {
          unsigned expected = CANON_QACTIVE;
          if (atomic_compare_exchange_strong_explicit(
                &d->canon, &expected, CANON_QRESCUED,
                memory_order_acq_rel, memory_order_acquire)) {
            atomic_fetch_add_explicit(&d->qactive_wins, 1,
                                      memory_order_relaxed);
            canon = CANON_QRESCUED;
          } else {
            canon = expected;
          }
        }
        if (canon == CANON_QRESCUED) {
          uintptr_t expected_main = PTR_NONE;
          atomic_fetch_add_explicit(&d->qrescued_hits, 1,
                                    memory_order_relaxed);
          if (!atomic_compare_exchange_strong_explicit(
                &d->main_body, &expected_main, PTR_OLD,
                memory_order_acq_rel, memory_order_acquire))
            assert(expected_main == PTR_OLD);
          return PTR_OLD;
        }
        /* Prepared, non-authoritative record: retry until arming/cancel. */
        assert(canon == CANON_CANDIDATE || canon == CANON_LIVE);
        continue;
      }
    }

    /* Correct publication ordering makes this CAS fail or unreachable while
    ** the old lifecycle is acquirable.  It remains in the model so a missing
    ** quarantine lookup would have an observable fresh-identity outcome. */
    {
      uintptr_t expected_main = PTR_NONE;
      if (atomic_compare_exchange_strong_explicit(
            &d->main_body, &expected_main, fresh,
            memory_order_acq_rel, memory_order_acquire))
        return fresh;
    }
  }
  assert(0 && "bounded canonical lookup failed to make progress");
  return PTR_NONE;
}

static void concurrent_reserve_unlink(ConcurrentDirectory *d, unsigned round)
{
  unsigned expected_canon;
  uintptr_t expected_main;

  race_pause(round, 0, 0);
  atomic_store_explicit(&d->q_body, PTR_OLD, memory_order_release);
  race_pause(round, 0, 1);
  expected_canon = CANON_CANDIDATE;
  if (!atomic_compare_exchange_strong_explicit(
        &d->canon, &expected_canon, CANON_QACTIVE,
        memory_order_acq_rel, memory_order_acquire)) {
    assert(expected_canon == CANON_LIVE);
    atomic_store_explicit(&d->q_body, PTR_NONE, memory_order_release);
    return;
  }

  race_pause(round, 0, 2);
  expected_main = PTR_OLD;
  assert(atomic_compare_exchange_strong_explicit(
    &d->main_body, &expected_main, PTR_NONE,
    memory_order_acq_rel, memory_order_acquire));
  atomic_fetch_add_explicit(&d->unlinks, 1, memory_order_relaxed);
}

#define RESCUER_THREADS 8u

typedef struct RescueRace {
  ConcurrentDirectory dir;
  TestBarrier start;
  uintptr_t result[RESCUER_THREADS];
} RescueRace;

typedef struct RescueArg {
  RescueRace *race;
  unsigned id;
} RescueArg;

static void *rescue_worker(void *opaque)
{
  RescueArg *arg = (RescueArg *)opaque;
  test_barrier_wait(&arg->race->start);
  arg->race->result[arg->id] =
    concurrent_intern(&arg->race->dir, arg->id, arg->id + 1u);
  return NULL;
}

static void test_rescue_race(void)
{
  RescueRace race;
  RescueArg args[RESCUER_THREADS];
  pthread_t threads[RESCUER_THREADS];
  unsigned i;

  memset(&race, 0, sizeof(race));
  concurrent_init(&race.dir);
  atomic_store_explicit(&race.dir.main_body, PTR_NONE, memory_order_relaxed);
  atomic_store_explicit(&race.dir.q_body, PTR_OLD, memory_order_relaxed);
  atomic_store_explicit(&race.dir.canon, CANON_QACTIVE,
                        memory_order_relaxed);
  test_barrier_init(&race.start, RESCUER_THREADS + 1u);

  for (i = 0; i < RESCUER_THREADS; i++) {
    args[i].race = &race;
    args[i].id = i;
    assert(pthread_create(&threads[i], NULL, rescue_worker, &args[i]) == 0);
  }
  test_barrier_wait(&race.start);
  for (i = 0; i < RESCUER_THREADS; i++) {
    assert(pthread_join(threads[i], NULL) == 0);
    assert(race.result[i] == PTR_OLD);
  }
  assert(atomic_load_explicit(&race.dir.canon, memory_order_acquire) ==
         CANON_QRESCUED);
  assert(atomic_load_explicit(&race.dir.qactive_wins,
                              memory_order_relaxed) == 1);
  assert(atomic_load_explicit(&race.dir.main_body, memory_order_acquire) ==
         PTR_OLD);

  /* QRESCUED remains independently authoritative after the main edge is
  ** detached again: it returns and re-links the same body. */
  atomic_store_explicit(&race.dir.main_body, PTR_NONE, memory_order_release);
  assert(concurrent_intern(&race.dir, 0, 0x1234u) == PTR_OLD);
  assert(atomic_load_explicit(&race.dir.main_body, memory_order_acquire) ==
         PTR_OLD);
  assert(atomic_load_explicit(&race.dir.qrescued_hits,
                              memory_order_relaxed) != 0);
  test_barrier_destroy(&race.start);
}

#define STRESS_ROUNDS 12000u

typedef struct StressRace {
  ConcurrentDirectory dir;
  TestBarrier start;
  TestBarrier finish;
  _Atomic(uintptr_t) result[2];
} StressRace;

typedef struct StressArg {
  StressRace *race;
  unsigned role;  /* 0 collector, 1/2 interners. */
} StressArg;

static void *stress_worker(void *opaque)
{
  StressArg *arg = (StressArg *)opaque;
  unsigned round;
  for (round = 0; round < STRESS_ROUNDS; round++) {
    test_barrier_wait(&arg->race->start);
    if (arg->role == 0) {
      concurrent_reserve_unlink(&arg->race->dir, round);
    } else {
      uintptr_t result = concurrent_intern(&arg->race->dir,
                                           arg->role - 1u, round);
      atomic_store_explicit(&arg->race->result[arg->role - 1u], result,
                            memory_order_relaxed);
    }
    test_barrier_wait(&arg->race->finish);
  }
  return NULL;
}

static void test_atomic_stress(void)
{
  StressRace race;
  StressArg args[3];
  pthread_t threads[3];
  unsigned i, round;

  memset(&race, 0, sizeof(race));
  concurrent_init(&race.dir);
  atomic_init(&race.result[0], PTR_NONE);
  atomic_init(&race.result[1], PTR_NONE);
  test_barrier_init(&race.start, 4);
  test_barrier_init(&race.finish, 4);
  for (i = 0; i < 3; i++) {
    args[i].race = &race;
    args[i].role = i;
    assert(pthread_create(&threads[i], NULL, stress_worker, &args[i]) == 0);
  }

  for (round = 0; round < STRESS_ROUNDS; round++) {
    /* The two barriers keep reset outside every protocol operation. */
    atomic_store_explicit(&race.dir.main_body, PTR_OLD,
                          memory_order_relaxed);
    atomic_store_explicit(&race.dir.q_body, PTR_NONE, memory_order_relaxed);
    atomic_store_explicit(&race.dir.canon, CANON_CANDIDATE,
                          memory_order_relaxed);
    atomic_store_explicit(&race.result[0], PTR_NONE, memory_order_relaxed);
    atomic_store_explicit(&race.result[1], PTR_NONE, memory_order_relaxed);
    test_barrier_wait(&race.start);
    test_barrier_wait(&race.finish);

    assert(atomic_load_explicit(&race.result[0], memory_order_relaxed) ==
           PTR_OLD);
    assert(atomic_load_explicit(&race.result[1], memory_order_relaxed) ==
           PTR_OLD);
    assert(atomic_load_explicit(&race.dir.main_body, memory_order_acquire) <
           PTR_NEW0);
    if (atomic_load_explicit(&race.dir.q_body, memory_order_acquire) ==
        PTR_OLD) {
      unsigned canon = atomic_load_explicit(&race.dir.canon,
                                             memory_order_acquire);
      assert(canon == CANON_QACTIVE || canon == CANON_QRESCUED);
    }
  }

  for (i = 0; i < 3; i++) assert(pthread_join(threads[i], NULL) == 0);
  test_barrier_destroy(&race.finish);
  test_barrier_destroy(&race.start);

  printf("  atomics: %u rounds, %" PRIu64 " ordered unlinks, %" PRIu64
         " main misses, %" PRIu64 " quarantine hits\n",
         STRESS_ROUNDS,
         atomic_load_explicit(&race.dir.unlinks, memory_order_relaxed),
         atomic_load_explicit(&race.dir.main_misses, memory_order_relaxed),
         atomic_load_explicit(&race.dir.q_hits, memory_order_relaxed));
}

int main(void)
{
  test_bounded_interleavings();
  test_rescue_race();
  test_atomic_stress();
  printf("t-strcanon-model OK: quarantine-before-unlink, miss consultation, "
         "and QACTIVE/QRESCUED identity verified\n");
  return 0;
}
