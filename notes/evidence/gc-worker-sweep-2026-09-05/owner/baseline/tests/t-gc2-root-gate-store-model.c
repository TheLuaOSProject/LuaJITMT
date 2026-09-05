/*
** t-gc2-root-gate-store-model.c - exhaustive GC2 root/store cutover model.
**
** Build & run:
**   cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -mcx16 -Isrc \
**      tests/t-gc2-root-gate-store-model.c -o /tmp/t-gc2-root-gate && \
**      /tmp/t-gc2-root-gate
**
** The model enumerates every linearization order of one scalar table-store
** publisher and one phase closer. A COMMIT state is legal only if an armed
** store has either transferred a durable token or its exact parent/key/value
** descriptor was traced and certified for that exact close generation.
*/

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_gc2token.h"

enum {
  TRACE_PARENT = 0x01u,
  TRACE_KEY = 0x02u,
  TRACE_VALUE = 0x04u,
  TRACE_FULL = TRACE_PARENT | TRACE_KEY | TRACE_VALUE,
  MODEL_MAX_DEPTH = 32,
  MODEL_TRACE_CAP = 48
};

typedef enum ModelVariant {
  MODEL_GOOD = 0,
  MODEL_NO_POST_PUBLISH_GATE,
  MODEL_PARENT_ONLY_HELP,
  MODEL_NO_COVERAGE_REQUIRED,
  MODEL_EARLY_FINISH,
  MODEL_STALE_COVERAGE,
  MODEL_VARIANT_COUNT
} ModelVariant;

typedef struct ModelState {
  LJGC2Activation activation;
  LJGC2RootDesc descriptor;
  LJGC2RootDescTicket ticket;
  LJGC2ActivationSnap closing;
  LJGC2RootDescView view;
  uint64_t traced_control;
  uint64_t traced_activation_generation;
  uint64_t commit_from_generation;
  uint32_t traced_mask;
  uint8_t publisher_pc;
  uint8_t closer_pc;
  uint8_t snapshot_active;
  uint8_t publisher_armed;
  uint8_t store_done;
  uint8_t dirty_done;
  uint8_t token_handoff;
  uint8_t owner_finished;
  uint8_t routed_from_commit;
  uint8_t commit_lost_to_pending;
  ModelVariant variant;
} ModelState;

typedef struct ModelStats {
  uint64_t states;
  uint64_t terminals;
  uint64_t failures;
  uint64_t future_store_failures;
  uint64_t post_store_failures;
  uint64_t exact_active_commit;
  uint64_t covered_store_after_commit;
  uint64_t late_unarmed_active_commit;
  uint64_t routed_from_commit;
  uint64_t commit_lost_to_pending;
  uint64_t truncated;
  unsigned shortest_failure_len;
  char shortest_failure[MODEL_TRACE_CAP];
} ModelStats;

static const uint64_t STORE_PARENT = UINT64_C(0xfff7000000001111);
static const uint64_t STORE_KEY = UINT64_C(0xfff5000000002222);
static const uint64_t STORE_VALUE = UINT64_C(0xfff6000000003333);

static LJGC2RootDescSpec table_store_spec(uint64_t parent, uint64_t key,
                                          uint64_t value)
{
  LJGC2RootDescSpec spec;
  memset(&spec, 0, sizeof(spec));
  spec.flags = LJ_GC2_ROOTDESC_F_OLD | LJ_GC2_ROOTDESC_F_NEW |
               LJ_GC2_ROOTDESC_F_AUX | LJ_GC2_ROOTDESC_F_TABLE_STORE;
  spec.old_root = parent;
  spec.new_root = key;
  spec.aux_root = value;
  return spec;
}

static LJGC2ActivationSnap gate_to(LJGC2Activation *activation,
                                    const LJGC2ActivationSnap *from,
                                    uint8_t gate)
{
  LJGC2ActivationSnap observed;
  assert(lj_gc2_activation_try_gate(activation, from, gate, &observed) ==
         LJ_GC2_TRANSITION_OK);
  return observed;
}

/* Seed a real, well-formed certificate from an earlier descriptor and close.
** It remains embedded after owner completion and is useful for the stale-
** certificate sensitivity variant. */
static void model_init(ModelState *state, ModelVariant variant)
{
  LJGC2RootDescSpec stale_spec = table_store_spec(0x11, 0x22, 0x33);
  LJGC2RootDescTicket stale_ticket;
  LJGC2RootDescView stale_view;
  LJGC2ActivationSnap open, closing;

  memset(state, 0, sizeof(*state));
  state->variant = variant;
  assert(lj_gc2_activation_init_unpublished(&state->activation, 41, 100,
                                             LJ_GC2_ACT_WEAK));
  assert(lj_gc2_rootdesc_init_unpublished(&state->descriptor, 0));
  assert(lj_gc2_rootdesc_publish(&state->descriptor, &stale_spec,
                                 &stale_ticket) == LJ_GC2_ROOTDESC_OK);
  open = lj_gc2_activation_snapshot(&state->activation);
  closing = gate_to(&state->activation, &open,
                    LJ_GC2_ROOT_GATE_CLOSING);
  assert(lj_gc2_rootdesc_snapshot(&state->descriptor, &stale_view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(lj_gc2_rootdesc_cover_after_trace(&state->descriptor, &stale_view,
                                            &state->activation, &closing) ==
         LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_finish(&state->descriptor, &stale_ticket) ==
         LJ_GC2_ROOTDESC_OK);
  (void)gate_to(&state->activation, &closing, LJ_GC2_ROOT_GATE_OPEN);
}

static int semantic_certificate_exact(const ModelState *state)
{
  LJGC2RootDescCoverage coverage;
  if (state->ticket.control == 0 || state->commit_from_generation == 0 ||
      state->traced_control != state->ticket.control ||
      state->traced_activation_generation !=
        state->commit_from_generation ||
      state->traced_mask != TRACE_FULL)
    return 0;
  coverage = lj_gc2_rootdesc_coverage_snapshot(
    (LJGC2RootDesc *)(void *)&state->descriptor);
  return coverage.descriptor_control == state->ticket.control &&
         coverage.activation_generation == state->commit_from_generation;
}

static int state_unsafe(const ModelState *state)
{
  LJGC2ActivationSnap activation =
    lj_gc2_activation_snapshot(&state->activation);
  int accounted;
  if (activation.gate != LJ_GC2_ROOT_GATE_COMMIT ||
      !state->publisher_armed)
    return 0;
  accounted = state->token_handoff || semantic_certificate_exact(state);
  return !accounted;
}

enum { PUBLISHER_DONE = 7, CLOSER_DONE = 8 };

static char publisher_step(ModelState *state)
{
  LJGC2ActivationSnap snap, observed;
  LJGC2RootDescSpec spec;
  LJGC2RootDescResult result;
  switch (state->publisher_pc) {
  case 0: /* Publish exact parent/key/value intent before sampling the gate. */
    spec = table_store_spec(STORE_PARENT, STORE_KEY, STORE_VALUE);
    result = lj_gc2_rootdesc_publish(&state->descriptor, &spec,
                                     &state->ticket);
    assert(result == LJ_GC2_ROOTDESC_OK);
    state->publisher_pc = 1;
    return 'D';
  case 1: /* Mandatory post-publication root-gate observation. */
    snap = lj_gc2_activation_snapshot(&state->activation);
    if (state->variant == MODEL_NO_POST_PUBLISH_GATE) {
      state->publisher_armed = 1;
      state->publisher_pc = 3;
    } else if (snap.gate == LJ_GC2_ROOT_GATE_OPEN ||
               snap.gate == LJ_GC2_ROOT_GATE_PENDING) {
      state->publisher_armed = 1;
      state->publisher_pc = 3;
    } else {
      state->publisher_pc = 2;
    }
    return 'G';
  case 2: /* Invalidate CLOSING/COMMIT before the store can execute. */
    snap = lj_gc2_activation_snapshot(&state->activation);
    if (snap.gate == LJ_GC2_ROOT_GATE_CLOSING ||
        snap.gate == LJ_GC2_ROOT_GATE_COMMIT) {
      uint8_t old_gate = snap.gate;
      assert(lj_gc2_activation_try_gate(&state->activation, &snap,
               LJ_GC2_ROOT_GATE_PENDING, &observed) ==
             LJ_GC2_TRANSITION_OK);
      state->routed_from_commit |= old_gate == LJ_GC2_ROOT_GATE_COMMIT;
    } else {
      assert(snap.gate == LJ_GC2_ROOT_GATE_OPEN ||
             snap.gate == LJ_GC2_ROOT_GATE_PENDING);
    }
    state->publisher_armed = 1;
    state->publisher_pc = 3;
    return 'R';
  case 3:
    state->store_done = 1;
    state->publisher_pc = 4;
    return 'S';
  case 4:
    assert(state->store_done);
    state->dirty_done = 1;
    state->publisher_pc = 5;
    return 'Y';
  case 5:
    assert(state->dirty_done);
    if (state->variant == MODEL_EARLY_FINISH) {
      assert(lj_gc2_rootdesc_finish(&state->descriptor, &state->ticket) ==
             LJ_GC2_ROOTDESC_OK);
      state->owner_finished = 1;
      state->publisher_pc = 6;
      return 'F';
    }
    state->token_handoff = 1;
    state->publisher_pc = 6;
    return 'T';
  case 6:
    if (state->variant == MODEL_EARLY_FINISH) {
      state->token_handoff = 1;
      state->publisher_pc = PUBLISHER_DONE;
      return 'T';
    }
    assert(lj_gc2_rootdesc_finish(&state->descriptor, &state->ticket) ==
           LJ_GC2_ROOTDESC_OK);
    state->owner_finished = 1;
    state->publisher_pc = PUBLISHER_DONE;
    return 'F';
  default:
    assert(0);
    return '?';
  }
}

static char closer_step(ModelState *state)
{
  LJGC2ActivationSnap snap, observed;
  LJGC2RootDescResult result;
  LJGC2RootDescSnapshotResult snapshot_result;
  LJGC2TransitionResult transition;
  switch (state->closer_pc) {
  case 0:
    snap = lj_gc2_activation_snapshot(&state->activation);
    assert(snap.gate == LJ_GC2_ROOT_GATE_OPEN ||
           snap.gate == LJ_GC2_ROOT_GATE_PENDING);
    assert(lj_gc2_activation_try_gate(&state->activation, &snap,
             LJ_GC2_ROOT_GATE_CLOSING, &state->closing) ==
           LJ_GC2_TRANSITION_OK);
    state->closer_pc = 1;
    return 'C';
  case 1:
    memset(&state->view, 0, sizeof(state->view));
    snapshot_result =
      lj_gc2_rootdesc_snapshot(&state->descriptor, &state->view);
    assert(snapshot_result == LJ_GC2_ROOTDESC_SNAPSHOT_IDLE ||
           snapshot_result == LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
    state->snapshot_active =
      snapshot_result == LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE;
    state->closer_pc = 2;
    return 'N';
  case 2:
    if (state->snapshot_active) {
      uint32_t required = LJ_GC2_ROOTDESC_F_OLD |
                          LJ_GC2_ROOTDESC_F_NEW |
                          LJ_GC2_ROOTDESC_F_AUX |
                          LJ_GC2_ROOTDESC_F_TABLE_STORE;
      assert((state->view.flags & required) == required);
      assert(state->view.old_root == STORE_PARENT);
      assert(state->view.new_root == STORE_KEY);
      assert(state->view.aux_root == STORE_VALUE);
      state->traced_control = lj_gc2_rootdesc_pack_control(
        state->view.generation, LJ_GC2_ROOTDESC_ACTIVE);
      state->traced_activation_generation = state->closing.generation;
      state->traced_mask = state->variant == MODEL_PARENT_ONLY_HELP ?
                           TRACE_PARENT : TRACE_FULL;
    }
    state->closer_pc = 3;
    return 'H';
  case 3:
    if (state->snapshot_active &&
        state->variant != MODEL_NO_COVERAGE_REQUIRED &&
        state->variant != MODEL_STALE_COVERAGE) {
      result = lj_gc2_rootdesc_cover_after_trace(
        &state->descriptor, &state->view, &state->activation,
        &state->closing);
      assert(result == LJ_GC2_ROOTDESC_OK ||
             result == LJ_GC2_ROOTDESC_BUSY ||
             result == LJ_GC2_ROOTDESC_PINNED);
      if (result == LJ_GC2_ROOTDESC_PINNED) {
        state->closer_pc = 7;
        return 'A';
      }
    }
    state->closer_pc = 4;
    return 'A';
  case 4: /* Abstract durable-token scan/pass before the close validation. */
    state->closer_pc = 5;
    return 'Q';
  case 5:
    if (state->variant == MODEL_NO_COVERAGE_REQUIRED) {
      result = LJ_GC2_ROOTDESC_OK;
    } else if (state->variant == MODEL_STALE_COVERAGE) {
      LJGC2RootDescCoverage coverage =
        lj_gc2_rootdesc_coverage_snapshot(&state->descriptor);
      result = coverage.descriptor_control != 0 ? LJ_GC2_ROOTDESC_OK :
                                                 LJ_GC2_ROOTDESC_BUSY;
    } else {
      result = lj_gc2_rootdesc_covered(&state->descriptor,
                                       &state->activation,
                                       &state->closing);
    }
    state->closer_pc = result == LJ_GC2_ROOTDESC_OK ? 6 : 7;
    return 'V';
  case 6:
    transition = lj_gc2_activation_try_gate(
      &state->activation, &state->closing, LJ_GC2_ROOT_GATE_COMMIT,
      &observed);
    if (transition == LJ_GC2_TRANSITION_OK) {
      state->commit_from_generation = state->closing.generation;
      state->closer_pc = CLOSER_DONE;
    } else {
      assert(transition == LJ_GC2_TRANSITION_LOST);
      snap = lj_gc2_activation_snapshot(&state->activation);
      state->commit_lost_to_pending |=
        snap.gate == LJ_GC2_ROOT_GATE_PENDING;
      state->closer_pc = 7;
    }
    return 'K';
  case 7:
    snap = lj_gc2_activation_snapshot(&state->activation);
    if (snap.gate != LJ_GC2_ROOT_GATE_OPEN) {
      assert(snap.gate == LJ_GC2_ROOT_GATE_CLOSING ||
             snap.gate == LJ_GC2_ROOT_GATE_PENDING ||
             snap.gate == LJ_GC2_ROOT_GATE_COMMIT);
      assert(lj_gc2_activation_try_gate(&state->activation, &snap,
               LJ_GC2_ROOT_GATE_OPEN, &observed) ==
             LJ_GC2_TRANSITION_OK);
    }
    state->closer_pc = CLOSER_DONE;
    return 'X';
  default:
    assert(0);
    return '?';
  }
}

static void collect_evidence(const ModelState *state, ModelStats *stats)
{
  LJGC2ActivationSnap activation =
    lj_gc2_activation_snapshot(&state->activation);
  uint64_t control = la_load64_acq(&state->descriptor.control);
  int active = state->ticket.control != 0 &&
               control == state->ticket.control;
  int exact = semantic_certificate_exact(state);
  if (activation.gate == LJ_GC2_ROOT_GATE_COMMIT && active && exact)
    stats->exact_active_commit++;
  if (activation.gate == LJ_GC2_ROOT_GATE_COMMIT && state->store_done &&
      exact)
    stats->covered_store_after_commit++;
  if (activation.gate == LJ_GC2_ROOT_GATE_COMMIT && active &&
      !state->publisher_armed)
    stats->late_unarmed_active_commit++;
  if (state->routed_from_commit)
    stats->routed_from_commit++;
  if (state->commit_lost_to_pending)
    stats->commit_lost_to_pending++;
}

static void model_state_copy(ModelState *to, const ModelState *from)
{
  *to = *from;
  /* RootDescView intentionally binds object identity. Rebase the pointer when
  ** the value-model state is copied to its next DFS node. */
  if (from->view.descriptor != NULL) {
    assert(from->view.descriptor == &from->descriptor);
    to->view.descriptor = &to->descriptor;
  }
}

static void explore(const ModelState *state, ModelStats *stats,
                    char *trace, unsigned depth)
{
  ModelState next;
  stats->states++;
  collect_evidence(state, stats);
  if (state_unsafe(state)) {
    stats->failures++;
    if (state->store_done)
      stats->post_store_failures++;
    else
      stats->future_store_failures++;
    if (stats->shortest_failure_len == 0 ||
        depth < stats->shortest_failure_len) {
      assert(depth + 1u < MODEL_TRACE_CAP);
      memcpy(stats->shortest_failure, trace, depth);
      stats->shortest_failure[depth] = '\0';
      stats->shortest_failure_len = depth;
    }
    return;
  }
  if (state->publisher_pc == PUBLISHER_DONE &&
      state->closer_pc == CLOSER_DONE) {
    stats->terminals++;
    return;
  }
  if (depth >= MODEL_MAX_DEPTH) {
    stats->truncated++;
    return;
  }
  if (state->publisher_pc != PUBLISHER_DONE) {
    model_state_copy(&next, state);
    trace[depth] = publisher_step(&next);
    explore(&next, stats, trace, depth + 1u);
  }
  if (state->closer_pc != CLOSER_DONE) {
    model_state_copy(&next, state);
    trace[depth] = closer_step(&next);
    explore(&next, stats, trace, depth + 1u);
  }
}

static const char *variant_name(ModelVariant variant)
{
  static const char *const names[MODEL_VARIANT_COUNT] = {
    "good", "no-post-publish-gate", "parent-only-help",
    "no-coverage-required", "early-finish", "stale-coverage"
  };
  assert((unsigned)variant < MODEL_VARIANT_COUNT);
  return names[variant];
}

static ModelStats run_model(ModelVariant variant)
{
  ModelState initial;
  ModelStats stats;
  char trace[MODEL_TRACE_CAP];
  model_init(&initial, variant);
  memset(&stats, 0, sizeof(stats));
  memset(trace, 0, sizeof(trace));
  explore(&initial, &stats, trace, 0);
  assert(stats.states != 0 && stats.terminals != 0);
  assert(stats.truncated == 0);
  printf("root-gate model %-24s states=%" PRIu64
         " terminals=%" PRIu64 " failures=%" PRIu64,
         variant_name(variant), stats.states, stats.terminals,
         stats.failures);
  if (stats.failures != 0)
    printf(" future=%" PRIu64 " post=%" PRIu64 " shortest=%s",
           stats.future_store_failures, stats.post_store_failures,
           stats.shortest_failure);
  putchar('\n');
  return stats;
}

typedef struct CoverRace {
  LJGC2RootDesc *descriptor;
  const LJGC2RootDescView *view;
  const LJGC2Activation *activation;
  const LJGC2ActivationSnap *closing;
  uint32_t ready;
  uint32_t go;
  LJGC2RootDescResult result[4];
} CoverRace;

typedef struct CoverRaceArg {
  CoverRace *race;
  unsigned index;
} CoverRaceArg;

static void *same_close_cover_thread(void *ud)
{
  CoverRaceArg *arg = (CoverRaceArg *)ud;
  CoverRace *race = arg->race;
  (void)la_add32_acqrel(&race->ready, 1);
  while (!la_load32_acq(&race->go))
    la_cpu_pause();
  race->result[arg->index] = lj_gc2_rootdesc_cover_after_trace(
    race->descriptor, race->view, race->activation, race->closing);
  return NULL;
}

static void run_same_close_cover_race(LJGC2RootDesc *descriptor,
                                      const LJGC2RootDescView *view,
                                      const LJGC2Activation *activation,
                                      const LJGC2ActivationSnap *closing)
{
  CoverRace race;
  CoverRaceArg args[4];
  pthread_t threads[4];
  unsigned i;
  memset(&race, 0, sizeof(race));
  race.descriptor = descriptor;
  race.view = view;
  race.activation = activation;
  race.closing = closing;
  for (i = 0; i < 4; i++) {
    args[i].race = &race;
    args[i].index = i;
    assert(pthread_create(&threads[i], NULL, same_close_cover_thread,
                          &args[i]) == 0);
  }
  while (la_load32_acq(&race.ready) != 4)
    la_cpu_pause();
  la_store32_rel(&race.go, 1);
  for (i = 0; i < 4; i++) {
    assert(pthread_join(threads[i], NULL) == 0);
    assert(race.result[i] == LJ_GC2_ROOTDESC_OK);
  }
}

static void test_descriptor_identity_and_full_store_shape(void)
{
  LJGC2Activation activation;
  LJGC2ActivationSnap open, closing;
  LJGC2RootDesc a, b;
  LJGC2RootDescTicket ta, tb;
  LJGC2RootDescView va, vb;
  LJGC2RootDescSpec sa = table_store_spec(0x101, 0x102, 0x103);
  LJGC2RootDescSpec sb = table_store_spec(0x201, 0x202, 0x203);

  assert(lj_gc2_activation_init_unpublished(&activation, 7, 10,
                                             LJ_GC2_ACT_WEAK));
  assert(lj_gc2_rootdesc_init_unpublished(&a, 0));
  assert(lj_gc2_rootdesc_init_unpublished(&b, 0));
  assert(lj_gc2_rootdesc_publish(&a, &sa, &ta) == LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_publish(&b, &sb, &tb) == LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_snapshot(&a, &va) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(lj_gc2_rootdesc_snapshot(&b, &vb) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(va.generation == vb.generation && va.descriptor == &a &&
         vb.descriptor == &b);
  assert(va.old_root == sa.old_root && va.new_root == sa.new_root &&
         va.aux_root == sa.aux_root);
  assert(vb.old_root != va.old_root && vb.new_root != va.new_root &&
         vb.aux_root != va.aux_root);
  open = lj_gc2_activation_snapshot(&activation);
  closing = gate_to(&activation, &open, LJ_GC2_ROOT_GATE_CLOSING);
  /* An address-only/generation-only helper mutant would accept this. */
  assert(lj_gc2_rootdesc_cover_after_trace(&a, &vb, &activation, &closing) ==
         LJ_GC2_ROOTDESC_INVALID);
  assert(lj_gc2_rootdesc_cover_after_trace(&a, &va, &activation, &closing) ==
         LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_covered(&a, &activation, &closing) ==
         LJ_GC2_ROOTDESC_OK);
}

static void test_same_close_turnover_and_pin(void)
{
  LJGC2Activation activation;
  LJGC2ActivationSnap open, closing;
  LJGC2RootDesc descriptor;
  LJGC2RootDescTicket first, second;
  LJGC2RootDescView view;
  LJGC2RootDescSpec spec = table_store_spec(1, 2, 3);

  assert(lj_gc2_activation_init_unpublished(&activation, 8, 20,
                                             LJ_GC2_ACT_WEAK));
  assert(lj_gc2_rootdesc_init_unpublished(&descriptor, 0));
  assert(lj_gc2_rootdesc_publish(&descriptor, &spec, &first) ==
         LJ_GC2_ROOTDESC_OK);
  open = lj_gc2_activation_snapshot(&activation);
  closing = gate_to(&activation, &open, LJ_GC2_ROOT_GATE_CLOSING);
  assert(lj_gc2_rootdesc_snapshot(&descriptor, &view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  /* Concurrent same-close helpers may linearize in any order; every loser
  ** observes the exact certificate and succeeds idempotently. */
  run_same_close_cover_race(&descriptor, &view, &activation, &closing);
  assert(lj_gc2_rootdesc_cover_after_trace(&descriptor, &view, &activation,
                                            &closing) == LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_finish(&descriptor, &first) == LJ_GC2_ROOTDESC_OK);

  spec.old_root = 4;
  spec.new_root = 5;
  spec.aux_root = 6;
  assert(lj_gc2_rootdesc_publish(&descriptor, &spec, &second) ==
         LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_covered(&descriptor, &activation, &closing) ==
         LJ_GC2_ROOTDESC_BUSY);
  assert(lj_gc2_rootdesc_snapshot(&descriptor, &view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(lj_gc2_rootdesc_cover_after_trace(&descriptor, &view, &activation,
                                            &closing) == LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_covered(&descriptor, &activation, &closing) ==
         LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_pin(&descriptor, second.control) ==
         LJ_GC2_ROOTDESC_PINNED);
  assert(lj_gc2_rootdesc_covered(&descriptor, &activation, &closing) ==
         LJ_GC2_ROOTDESC_PINNED);
}

static void test_malformed_winner_between_snapshot_and_cas(void)
{
  LJGC2RootDesc descriptor;
  LJGC2RootDescTicket ticket;
  LJGC2RootDescCoverage coverage;
  LJGC2RootDescSpec spec = table_store_spec(1, 2, 3);
  la_u128 stale, winner, malformed;

  assert(lj_gc2_rootdesc_init_unpublished(&descriptor, 0));
  assert(lj_gc2_rootdesc_publish(&descriptor, &spec, &ticket) ==
         LJ_GC2_ROOTDESC_OK);
  stale.lo = stale.hi = 0;
  winner = stale;
  malformed.lo = ticket.control;
  malformed.hi = 0; /* ACTIVE control paired with no activation authority. */
  assert(la_cas128(&descriptor.coverage, &winner, malformed));
  assert(lj_gc2_rootdesc_coverage_advance(&descriptor, ticket.control, 4,
                                           stale) ==
         LJ_GC2_ROOTDESC_PINNED);
  coverage = lj_gc2_rootdesc_coverage_snapshot(&descriptor);
  assert(!lj_gc2_rootdesc_coverage_valid(&coverage));
  assert(lj_gc2_rootdesc_state(la_load64_acq(&descriptor.control)) ==
         LJ_GC2_ROOTDESC_NO_RECLAIM);
}

int main(void)
{
  ModelStats good;
  ModelVariant variant;

  test_descriptor_identity_and_full_store_shape();
  test_same_close_turnover_and_pin();
  test_malformed_winner_between_snapshot_and_cas();

  good = run_model(MODEL_GOOD);
  assert(good.failures == 0);
  assert(good.exact_active_commit != 0);
  assert(good.covered_store_after_commit != 0);
  assert(good.late_unarmed_active_commit != 0);
  assert(good.routed_from_commit != 0);
  assert(good.commit_lost_to_pending != 0);

  for (variant = MODEL_NO_POST_PUBLISH_GATE;
       variant < MODEL_VARIANT_COUNT; variant++) {
    ModelStats mutant = run_model(variant);
    assert(mutant.failures != 0);
  }
  puts("GC2 root-gate/store exhaustive model passed");
  return 0;
}
