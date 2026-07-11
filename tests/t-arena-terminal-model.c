/*
** t-arena-terminal-model.c - deterministic arena terminal-gate model.
**
** This standalone C11 model enumerates all bounded interleavings at the
** protocol's atomic linearization points.  No gate state makes a producer
** wait.  Before commit, rescue/free producers CAS in COUNT+PENDING, publish a
** bit-only intent, decrement COUNT, and return.  In committed
** SEALED-without-CLOSED, rescue CASes in COUNT only and reads the stable
** terminal state plus block bit; it publishes neither PENDING nor a mark.
** Terminal free remains a COUNT+PENDING late-bit publisher.
**
** The owner observes a whole gate word and then performs an exact CAS.  This
** deliberately gives the explorer a scheduling point between observation and
** CAS.  Pending may be cleared only by an exact zero-count CAS, and terminal
** commit is an exact {CLOSED|SEALED}->SEALED generation LP, and adoption OPEN
** is an exact SEALED->OPEN CAS after private rebuild.  A producer which wins
** either race makes the
** owner's CAS fail and forces another intent scan.  During committed apply,
** SEALED lacks CLOSED: WHITE+block1 is already live, while FREEING cannot be
** resurrected (before or after apply clears block).  An exact
** SEALED->CLOSED transition preserves the active count and any late intent.
**
** Covered protocols:
**
**   * close-before-validation, SEALED rescue intent, PENDING reconciliation,
**     and exact terminal commit;
**   * a late terminal-free bit retained through the current commit, consumed
**     by next-generation PREPSWEEP, and covered by a fresh grace;
**   * adoption under SEALED, exact OPEN publication, and stale-bit versus
**     allocator-reuse arbitration.
**
** The deliberately broken variants are model-checker self-tests.  Every one
** must yield a counterexample: validation-before-close, omitted PENDING,
** dropping a SEALED producer, non-exact final commit/open, clearing PENDING
** with an active producer, treating committed WHITE as dead, resurrecting
** committed FREEING, mutating marks during committed apply, same-cycle late
** free, and OPEN-before-rebuild.
**
** Trace letters: O owner, R rescue, T terminal-free publisher, A adopter,
** S stale-intent publisher, U allocator reuse.
**
** Build & run:
**   cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic \
**      tests/t-arena-terminal-model.c -o /tmp/t-arena-terminal-model && \
**      /tmp/t-arena-terminal-model
*/

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GATE_CLOSED UINT64_C(0x8000000000000000)
#define GATE_SEALED UINT64_C(0x4000000000000000)
#define GATE_PENDING UINT64_C(0x2000000000000000)
#define GATE_COUNT_MASK UINT64_C(0x1fffffffffffffff)
#define GATE_SEALED_ZERO (GATE_CLOSED | GATE_SEALED)

static uint64_t gate_count(uint64_t gate)
{
  return gate & GATE_COUNT_MASK;
}

static int gate_open(uint64_t gate)
{
  return (gate & (GATE_CLOSED | GATE_SEALED)) == 0;
}

static int gate_closed(uint64_t gate)
{
  return (gate & (GATE_CLOSED | GATE_SEALED)) == GATE_CLOSED;
}

static int gate_sealed(uint64_t gate)
{
  return (gate & GATE_SEALED) != 0;
}

static void gate_assert_valid(uint64_t gate)
{
  if (gate_open(gate)) assert((gate & GATE_PENDING) == 0);
  assert(gate_count(gate) <= GATE_COUNT_MASK);
}

/* One abstract CAS step. */
static int gate_cas(uint64_t *gate, uint64_t expected, uint64_t desired)
{
  if (*gate != expected) return 0;
  *gate = desired;
  return 1;
}

/* OPEN publishers may perform ordinary state/header mutation. */
static void gate_join_open(uint64_t *gate)
{
  assert(gate_open(*gate));
  assert(gate_count(*gate) != GATE_COUNT_MASK);
  *gate += 1u;
}

/* Precommit rescue and every terminal-free intent are bit-only.
** COUNT+PENDING is one CAS, so owner observation can never pass a producer
** preempted before bit publish. */
static void gate_join_intent(uint64_t *gate, int omit_pending)
{
  uint64_t old = *gate;
  assert(gate_closed(old) || gate_sealed(old));
  assert(gate_count(old) != GATE_COUNT_MASK);
  *gate = old + 1u;
  if (!omit_pending) *gate |= GATE_PENDING;
}

/* Rescue in the committed SEALED-without-CLOSED transient is counted so
** unseal cannot lose the publisher, but it does not dirty PENDING: terminal
** state and the committed block bit are its immutable result. */
static void gate_join_committed_rescue(uint64_t *gate)
{
  assert((*gate & (GATE_CLOSED | GATE_SEALED)) == GATE_SEALED);
  assert(gate_count(*gate) != GATE_COUNT_MASK);
  *gate += 1u;
}

static void gate_leave(uint64_t *gate)
{
  assert(gate_count(*gate) != 0);
  *gate -= 1u;
}

/* Execute the packed operations once with real C11 atomics.  The exhaustive
** explorers use copyable words but the same indivisible CAS/sub operations. */
static void c11_gate_smoke(void)
{
  _Atomic(uint64_t) gate = ATOMIC_VAR_INIT(0);
  uint64_t expected = 0;

  assert(atomic_is_lock_free(&gate));

  assert(atomic_compare_exchange_strong_explicit(
      &gate, &expected, GATE_CLOSED,
      memory_order_acq_rel, memory_order_acquire));
  expected = GATE_CLOSED;
  assert(atomic_compare_exchange_strong_explicit(
      &gate, &expected, GATE_SEALED_ZERO,
      memory_order_acq_rel, memory_order_acquire));
  expected = GATE_SEALED_ZERO;
  assert(atomic_compare_exchange_strong_explicit(
      &gate, &expected, GATE_SEALED_ZERO | GATE_PENDING | 1u,
      memory_order_acq_rel, memory_order_acquire));
  assert(atomic_fetch_sub_explicit(&gate, 1u, memory_order_acq_rel) ==
         (GATE_SEALED_ZERO | GATE_PENDING | 1u));
  expected = GATE_SEALED_ZERO | GATE_PENDING;
  assert(atomic_compare_exchange_strong_explicit(
      &gate, &expected, GATE_SEALED_ZERO,
      memory_order_acq_rel, memory_order_acquire));
  expected = GATE_SEALED_ZERO;
  assert(atomic_compare_exchange_strong_explicit(
      &gate, &expected, GATE_SEALED,
      memory_order_acq_rel, memory_order_acquire));
  expected = GATE_SEALED;
  assert(atomic_compare_exchange_strong_explicit(
      &gate, &expected, GATE_SEALED | 1u,
      memory_order_acq_rel, memory_order_acquire));
  assert(atomic_fetch_sub_explicit(&gate, 1u, memory_order_acq_rel) ==
         (GATE_SEALED | 1u));
  expected = GATE_SEALED;
  assert(atomic_compare_exchange_strong_explicit(
      &gate, &expected, GATE_SEALED | GATE_PENDING | 1u,
      memory_order_acq_rel, memory_order_acquire));
  assert(atomic_fetch_sub_explicit(&gate, 1u, memory_order_acq_rel) ==
         (GATE_SEALED | GATE_PENDING | 1u));
  expected = GATE_SEALED | GATE_PENDING;
  assert(atomic_compare_exchange_strong_explicit(
      &gate, &expected, GATE_SEALED,
      memory_order_acq_rel, memory_order_acquire));
  expected = GATE_SEALED;
  assert(atomic_compare_exchange_strong_explicit(
      &gate, &expected, GATE_CLOSED,
      memory_order_acq_rel, memory_order_acquire));
}

#define TRACE_CAP 96u

typedef struct ExploreStats {
  uint64_t terminals;
  uint64_t safety_failures;
  uint64_t liveness_failures;
  uint64_t truncated;
  uint64_t seal_cas_races;
  uint64_t clear_cas_races;
  uint64_t final_cas_races;
  uint64_t unseal_cas_races;
  uint64_t sealed_publications;
  uint64_t postlp_intents;
  uint64_t postlp_mark_writes;
  uint64_t committed_white_reads;
  uint64_t committed_freeing_reads;
  uint64_t lost_mark_races;
  char first_safety[TRACE_CAP];
  char first_liveness[TRACE_CAP];
} ExploreStats;

static void remember_trace(char *dst, const char *trace, unsigned depth)
{
  if (dst[0] == '\0') {
    assert(depth + 1u <= TRACE_CAP);
    memcpy(dst, trace, depth);
    dst[depth] = '\0';
  }
}

/* ----------------------------------------------------------------------- */
/* Terminal owner versus one rescue publisher. */

enum RescueVariant {
  RESCUE_GOOD = 0,
  RESCUE_CLOSE_AFTER_VALIDATE,
  RESCUE_NO_PENDING,
  RESCUE_DROP_SEALED,
  RESCUE_FINAL_NOT_EXACT,
  RESCUE_CLEAR_PENDING_ACTIVE,
  RESCUE_COMMITTED_WHITE_DEAD,
  RESCUE_COMMITTED_FREEING_LIVE,
  RESCUE_COMMITTED_MARK_WRITE
};

enum {
  RO_FIRST = 0,
  RO_SECOND,
  RO_SEAL_OBSERVE,
  RO_SEAL_CAS,
  RO_SCAN_INTENTS,
  RO_CLEAR_PENDING,
  RO_FINAL_OBSERVE,
  RO_FINAL_CAS,
  RO_APPLY,
  RO_RESET_SIDECAR,
  RO_UNSEAL_OBSERVE,
  RO_UNSEAL_CAS,
  RO_DONE
};

enum {
  RR_ENTER = 0,
  RR_PUBLISH,
  RR_READ_BLOCK,
  RR_LEAVE,
  RR_DONE
};

enum {
  RA_NONE = 0,
  RA_OPEN_FULL,
  RA_CLOSED_BIT,
  RA_SEALED_BIT,
  RA_COMMITTED,
  RA_DROPPED
};

enum {
  CELL_TERMINAL_NONE = 0,
  CELL_TERMINAL_WHITE,
  CELL_TERMINAL_FREEING
};

typedef struct RescueState {
  uint64_t gate;
  uint64_t expected;
  uint8_t owner_pc;
  uint8_t rescue_pc;
  uint8_t admission;
  uint8_t object_live;
  uint8_t rescue_bit;
  uint8_t ready_snapshot;
  uint8_t commit_done;
  uint8_t apply_done;
  uint8_t cell_terminal;
  uint8_t sidecar_state;
  uint8_t sampled_state;
  uint8_t block_bit;
  uint8_t object_freed;
  uint8_t admitted_before_commit;
  uint8_t rescue_completed;
  uint8_t rescue_result;
  uint8_t committed_read;
  uint8_t dropped;
  uint8_t cleared_active;
  uint8_t committed_mark_write;
  uint8_t mark_lost;
  uint8_t safety_bad;
  uint8_t seal_cas_race;
  uint8_t clear_cas_race;
  uint8_t final_cas_race;
  uint8_t unseal_cas_race;
} RescueState;

static int rescue_owner_enabled(const RescueState *s,
                                enum RescueVariant variant)
{
  switch (s->owner_pc) {
  case RO_FIRST:
    if (variant == RESCUE_CLOSE_AFTER_VALIDATE) return 1;
    return s->gate == 0;
  case RO_SECOND:
    if (variant != RESCUE_CLOSE_AFTER_VALIDATE) return 1;
    return s->gate == 0;
  case RO_SEAL_OBSERVE:
    return gate_closed(s->gate) && gate_count(s->gate) == 0;
  case RO_SEAL_CAS:
    return 1;
  case RO_SCAN_INTENTS:
    return gate_sealed(s->gate) && gate_count(s->gate) == 0;
  case RO_CLEAR_PENDING:
    return 1;
  case RO_FINAL_OBSERVE:
    if (variant == RESCUE_CLEAR_PENDING_ACTIVE && s->cleared_active &&
        gate_count(s->gate) != 0)
      return 0;  /* Broken owner treats its unsafe clear as successful. */
    return 1;
  case RO_FINAL_CAS:
  case RO_APPLY:
  case RO_RESET_SIDECAR:
  case RO_UNSEAL_OBSERVE:
  case RO_UNSEAL_CAS:
    return 1;
  default:
    return 0;
  }
}

static void rescue_owner_commit(RescueState *s)
{
  s->commit_done = 1;
  s->cell_terminal = (uint8_t)(s->ready_snapshot ?
      CELL_TERMINAL_FREEING : CELL_TERMINAL_WHITE);
  s->sidecar_state = s->cell_terminal;
  s->block_bit = 1;  /* Both states are allocated before bitmap apply. */
  s->owner_pc = RO_APPLY;
}

static void rescue_owner_apply(RescueState *s)
{
  assert(s->cell_terminal == CELL_TERMINAL_WHITE ||
         s->cell_terminal == CELL_TERMINAL_FREEING);
  if (s->cell_terminal == CELL_TERMINAL_FREEING) {
    s->block_bit = 0;
    s->object_freed = 1;
    s->rescue_bit = 1;  /* Committed-free marker, not a rescue mark. */
  } else {
    s->block_bit = 1;
    if (s->rescue_bit && s->committed_mark_write) s->mark_lost = 1;
    s->rescue_bit = 0;  /* preserve_marks=false live-sidecar reset. */
  }
  s->owner_pc = RO_RESET_SIDECAR;
}

static void rescue_owner_reset_sidecar(RescueState *s)
{
  s->sidecar_state = CELL_TERMINAL_WHITE;
  s->apply_done = 1;
  if (s->object_freed &&
      (s->object_live || s->admitted_before_commit))
    s->safety_bad = 1;
}

static void rescue_owner_step(RescueState *s, enum RescueVariant variant)
{
  assert(rescue_owner_enabled(s, variant));

  switch (s->owner_pc) {
  case RO_FIRST:
    if (variant == RESCUE_CLOSE_AFTER_VALIDATE) {
      s->ready_snapshot = (uint8_t)(!s->object_live && !s->rescue_bit);
    } else {
      assert(gate_cas(&s->gate, 0, GATE_CLOSED));
    }
    s->owner_pc = RO_SECOND;
    break;
  case RO_SECOND:
    if (variant == RESCUE_CLOSE_AFTER_VALIDATE) {
      assert(gate_cas(&s->gate, 0, GATE_CLOSED));
    } else {
      s->ready_snapshot = (uint8_t)(!s->object_live && !s->rescue_bit);
    }
    s->owner_pc = RO_SEAL_OBSERVE;
    break;
  case RO_SEAL_OBSERVE:
    assert(gate_count(s->gate) == 0);
    s->expected = s->gate;
    s->owner_pc = RO_SEAL_CAS;
    break;
  case RO_SEAL_CAS:
    if (gate_cas(&s->gate, s->expected,
                 s->expected | GATE_CLOSED | GATE_SEALED)) {
      s->owner_pc = RO_SCAN_INTENTS;
    } else {
      s->seal_cas_race = 1;
      s->owner_pc = RO_SEAL_OBSERVE;
    }
    break;
  case RO_SCAN_INTENTS:
    assert(gate_count(s->gate) == 0);
    if (s->rescue_bit) {
      s->rescue_bit = 0;
      s->object_live = 1;
      s->ready_snapshot = 0;
    }
    s->expected = s->gate;
    s->owner_pc = RO_CLEAR_PENDING;
    break;
  case RO_CLEAR_PENDING:
    if (variant == RESCUE_CLEAR_PENDING_ACTIVE) {
      if ((s->gate & GATE_PENDING) != 0) {
        if (gate_count(s->gate) != 0) {
          s->cleared_active = 1;
          s->clear_cas_race = 1;
        }
        s->gate &= ~GATE_PENDING;  /* Deliberately non-exact mutant. */
      }
      s->owner_pc = RO_FINAL_OBSERVE;
    } else if (s->gate != s->expected) {
      s->clear_cas_race = 1;
      s->owner_pc = RO_SCAN_INTENTS;
    } else {
      if (s->expected & GATE_PENDING)
        assert(gate_cas(&s->gate, s->expected,
                        s->expected & ~GATE_PENDING));
      s->owner_pc = RO_FINAL_OBSERVE;
    }
    break;
  case RO_FINAL_OBSERVE:
    if (s->gate != GATE_SEALED_ZERO) {
      s->owner_pc = RO_SCAN_INTENTS;
    } else {
      s->expected = GATE_SEALED_ZERO;
      s->owner_pc = RO_FINAL_CAS;
    }
    break;
  case RO_FINAL_CAS:
    if (variant == RESCUE_FINAL_NOT_EXACT) {
      if (s->gate == s->expected)
        assert(gate_cas(&s->gate, s->expected, GATE_SEALED));
      /* On mismatch, publish commit anyway without clobbering the producer's
      ** count; its later leave remains defined, but safety is lost. */
      rescue_owner_commit(s);
    } else if (gate_cas(&s->gate, s->expected, GATE_SEALED)) {
      rescue_owner_commit(s);
    } else {
      s->final_cas_race = 1;
      s->owner_pc = RO_SCAN_INTENTS;
    }
    break;
  case RO_APPLY:
    rescue_owner_apply(s);
    break;
  case RO_RESET_SIDECAR:
    rescue_owner_reset_sidecar(s);
    if (variant == RESCUE_FINAL_NOT_EXACT && s->gate != GATE_SEALED) {
      /* The mutant crossed the LP with a pre-LP producer still admitted. */
      s->owner_pc = RO_DONE;
    } else {
      s->owner_pc = RO_UNSEAL_OBSERVE;
    }
    break;
  case RO_UNSEAL_OBSERVE:
    assert((s->gate & (GATE_CLOSED | GATE_SEALED)) == GATE_SEALED);
    s->expected = s->gate;
    s->owner_pc = RO_UNSEAL_CAS;
    break;
  case RO_UNSEAL_CAS: {
    uint64_t desired = GATE_CLOSED | gate_count(s->expected) |
        (s->expected & GATE_PENDING);
    if (gate_cas(&s->gate, s->expected, desired)) {
      s->owner_pc = RO_DONE;
    } else {
      s->unseal_cas_race = 1;
      s->owner_pc = RO_UNSEAL_OBSERVE;
    }
    break;
  }
  default:
    assert(0);
  }
}

static int rescue_actor_enabled(const RescueState *s)
{
  return s->rescue_pc != RR_DONE;
}

static void rescue_actor_step(RescueState *s, enum RescueVariant variant)
{
  assert(rescue_actor_enabled(s));

  switch (s->rescue_pc) {
  case RR_ENTER:
    if ((s->gate & (GATE_CLOSED | GATE_SEALED)) == GATE_SEALED) {
      gate_join_committed_rescue(&s->gate);
      s->admission = RA_COMMITTED;
      if (s->owner_pc == RO_UNSEAL_CAS) s->unseal_cas_race = 1;
      s->rescue_pc = RR_PUBLISH;
      break;
    }
    if (variant == RESCUE_DROP_SEALED &&
        (s->gate & (GATE_CLOSED | GATE_SEALED)) == GATE_SEALED_ZERO) {
      s->admission = RA_DROPPED;
      s->dropped = 1;
      s->rescue_pc = RR_DONE;
      break;
    }
    if (gate_open(s->gate)) {
      gate_join_open(&s->gate);
      s->admission = RA_OPEN_FULL;
    } else {
      int omit = variant == RESCUE_NO_PENDING;
      int was_sealed = gate_sealed(s->gate);
      gate_join_intent(&s->gate, omit);
      s->admission = was_sealed ? RA_SEALED_BIT : RA_CLOSED_BIT;
      if (!s->commit_done) s->admitted_before_commit = 1;
      if (s->owner_pc == RO_SEAL_CAS) s->seal_cas_race = 1;
      if (s->owner_pc == RO_CLEAR_PENDING) s->clear_cas_race = 1;
      if (s->owner_pc == RO_FINAL_CAS) s->final_cas_race = 1;
    }
    s->rescue_pc = RR_PUBLISH;
    break;
  case RR_PUBLISH:
    if (s->admission == RA_COMMITTED) {
      assert(s->cell_terminal == CELL_TERMINAL_WHITE ||
             s->cell_terminal == CELL_TERMINAL_FREEING);
      if (variant == RESCUE_COMMITTED_MARK_WRITE) {
        s->rescue_bit = 1;
        s->committed_mark_write = 1;
      }
      s->sampled_state = s->sidecar_state;
      s->rescue_pc = RR_READ_BLOCK;
      break;
    } else if (s->admission == RA_OPEN_FULL) {
      s->object_live = 1;
      s->rescue_result = 1;
    } else {
      s->rescue_bit = 1;
      s->rescue_result = 1;
    }
    s->rescue_pc = RR_LEAVE;
    break;
  case RR_READ_BLOCK: {
      int live = s->block_bit &&
                 s->sampled_state == CELL_TERMINAL_WHITE;
      if (variant == RESCUE_COMMITTED_WHITE_DEAD &&
          s->sampled_state == CELL_TERMINAL_WHITE)
        live = 0;
      if (variant == RESCUE_COMMITTED_FREEING_LIVE &&
          s->sampled_state == CELL_TERMINAL_FREEING)
        live = 1;
      s->rescue_result = (uint8_t)live;
      s->committed_read = 1;
    s->rescue_pc = RR_LEAVE;
    break;
  }
  case RR_LEAVE:
    gate_leave(&s->gate);
    s->rescue_completed = 1;
    s->rescue_pc = RR_DONE;
    break;
  default:
    assert(0);
  }
}

#define RESCUE_MAX_DEPTH 36u

static void rescue_explore(RescueState state, enum RescueVariant variant,
                           ExploreStats *stats, char *trace, unsigned depth)
{
  int enabled = 0;

  gate_assert_valid(state.gate);
  if (state.object_freed && state.admitted_before_commit)
    state.safety_bad = 1;
  if (state.owner_pc == RO_DONE && state.rescue_pc == RR_DONE) {
    int semantic_dead = 0;
    if (state.committed_read) {
      int expected_live = state.cell_terminal == CELL_TERMINAL_WHITE &&
                          state.block_bit;
      if (expected_live && !state.rescue_result)
        semantic_dead = 1;
      if (!expected_live && state.rescue_result)
        state.safety_bad = 1;
      if (state.committed_mark_write)
        state.safety_bad = 1;
    }
    stats->terminals++;
    if (state.safety_bad) {
      stats->safety_failures++;
      remember_trace(stats->first_safety, trace, depth);
    }
    if (state.dropped || !state.rescue_completed || semantic_dead) {
      stats->liveness_failures++;
      remember_trace(stats->first_liveness, trace, depth);
    }
    if (state.seal_cas_race) stats->seal_cas_races++;
    if (state.clear_cas_race) stats->clear_cas_races++;
    if (state.final_cas_race) stats->final_cas_races++;
    if (state.unseal_cas_race) stats->unseal_cas_races++;
    if (state.admission == RA_SEALED_BIT)
      stats->sealed_publications++;
    if (state.admission == RA_COMMITTED) {
      stats->postlp_intents++;
      if (state.committed_mark_write) stats->postlp_mark_writes++;
      if (state.cell_terminal == CELL_TERMINAL_WHITE)
        stats->committed_white_reads++;
      if (state.cell_terminal == CELL_TERMINAL_FREEING)
        stats->committed_freeing_reads++;
    }
    if (state.mark_lost) stats->lost_mark_races++;
    return;
  }
  if (depth == RESCUE_MAX_DEPTH) {
    stats->truncated++;
    return;
  }

  if (rescue_owner_enabled(&state, variant)) {
    RescueState next = state;
    trace[depth] = 'O';
    rescue_owner_step(&next, variant);
    rescue_explore(next, variant, stats, trace, depth + 1u);
    enabled = 1;
  }
  if (rescue_actor_enabled(&state)) {
    RescueState next = state;
    trace[depth] = 'R';
    rescue_actor_step(&next, variant);
    rescue_explore(next, variant, stats, trace, depth + 1u);
    enabled = 1;
  }
  assert(enabled && "rescue model reached a non-terminal dead end");
}

static ExploreStats run_rescue_model(enum RescueVariant variant)
{
  RescueState state;
  ExploreStats stats;
  char trace[TRACE_CAP];
  int initially_live;

  memset(&stats, 0, sizeof(stats));
  for (initially_live = 0; initially_live <= 1; initially_live++) {
    memset(&state, 0, sizeof(state));
    memset(trace, 0, sizeof(trace));
    state.object_live = (uint8_t)initially_live;
    rescue_explore(state, variant, &stats, trace, 0);
  }
  assert(stats.terminals != 0);
  assert(stats.truncated == 0);
  return stats;
}

static void test_rescue_protocol(void)
{
  ExploreStats good = run_rescue_model(RESCUE_GOOD);
  ExploreStats close_late =
      run_rescue_model(RESCUE_CLOSE_AFTER_VALIDATE);
  ExploreStats no_pending = run_rescue_model(RESCUE_NO_PENDING);
  ExploreStats drop = run_rescue_model(RESCUE_DROP_SEALED);
  ExploreStats loose_final = run_rescue_model(RESCUE_FINAL_NOT_EXACT);
  ExploreStats clear_active =
      run_rescue_model(RESCUE_CLEAR_PENDING_ACTIVE);
  ExploreStats white_dead =
      run_rescue_model(RESCUE_COMMITTED_WHITE_DEAD);
  ExploreStats freeing_live =
      run_rescue_model(RESCUE_COMMITTED_FREEING_LIVE);
  ExploreStats mark_race =
      run_rescue_model(RESCUE_COMMITTED_MARK_WRITE);

  assert(good.safety_failures == 0);
  assert(good.liveness_failures == 0);
  assert(good.sealed_publications != 0);
  assert(good.seal_cas_races != 0);
  assert(good.clear_cas_races != 0);
  assert(good.final_cas_races != 0);
  assert(good.unseal_cas_races != 0);
  assert(good.postlp_intents != 0);
  assert(good.postlp_mark_writes == 0);
  assert(good.committed_white_reads != 0);
  assert(good.committed_freeing_reads != 0);
  assert(good.lost_mark_races == 0);

  assert(close_late.safety_failures != 0);
  assert(no_pending.safety_failures != 0);
  assert(drop.liveness_failures != 0);
  assert(loose_final.safety_failures != 0);
  assert(clear_active.safety_failures != 0);
  assert(white_dead.liveness_failures != 0);
  assert(freeing_live.safety_failures != 0);
  assert(mark_race.safety_failures != 0);
  assert(mark_race.postlp_mark_writes != 0);
  assert(mark_race.lost_mark_races != 0);
  assert(close_late.first_safety[0] != '\0');
  assert(no_pending.first_safety[0] != '\0');
  assert(drop.first_liveness[0] != '\0');
  assert(loose_final.first_safety[0] != '\0');
  assert(clear_active.first_safety[0] != '\0');
  assert(white_dead.first_liveness[0] != '\0');
  assert(freeing_live.first_safety[0] != '\0');
  assert(mark_race.first_safety[0] != '\0');

  printf("  rescue: %" PRIu64 " safe schedules; mutants: close-late=%" PRIu64
         " (%s), no-PENDING=%" PRIu64 " (%s), drop-SEALED=%" PRIu64
         " (%s), loose-final=%" PRIu64 " (%s), clear-active=%" PRIu64
         " (%s), WHITE-dead=%" PRIu64 " (%s), FREEING-live=%" PRIu64
         " (%s), committed-mark=%" PRIu64 " (%s)\n",
         good.terminals,
         close_late.safety_failures, close_late.first_safety,
         no_pending.safety_failures, no_pending.first_safety,
         drop.liveness_failures, drop.first_liveness,
         loose_final.safety_failures, loose_final.first_safety,
         clear_active.safety_failures, clear_active.first_safety,
         white_dead.liveness_failures, white_dead.first_liveness,
         freeing_live.safety_failures, freeing_live.first_safety,
         mark_race.safety_failures, mark_race.first_safety);
}

/* ----------------------------------------------------------------------- */
/* Late terminal free.  The intent bit survives the current terminal commit;
** only next-generation PREPSWEEP translates it to FREEING, after which a new
** grace must complete before reuse. */

enum LateVariant {
  LATE_GOOD = 0,
  LATE_SAME_CYCLE_FREE
};

enum {
  LO_SEAL_OBSERVE = 0,
  LO_SEAL_CAS,
  LO_SCAN_CURRENT,
  LO_CLEAR_CURRENT,
  LO_FINAL_OBSERVE,
  LO_FINAL_CAS,
  LO_APPLY_CURRENT,
  LO_UNSEAL_OBSERVE,
  LO_UNSEAL_CAS,
  LO_WAIT_PUBLISHER,
  LO_NEXT_SEAL_OBSERVE,
  LO_NEXT_SEAL_CAS,
  LO_NEXT_SCAN,
  LO_NEXT_CLEAR,
  LO_NEXT_GRACE,
  LO_NEXT_COMMIT,
  LO_DONE
};

enum {
  LP_ENTER = 0,
  LP_SET_BIT,
  LP_LEAVE,
  LP_DONE
};

enum {
  OBJ_ALLOCATED = 0,
  OBJ_FREEING,
  OBJ_FREED
};

typedef struct LateState {
  uint64_t gate;
  uint64_t expected;
  uint8_t owner_pc;
  uint8_t publisher_pc;
  uint8_t object_state;
  uint8_t late_bit;
  uint8_t published;
  uint8_t published_sealed;
  uint8_t published_post_lp;
  uint8_t current_commit;
  uint8_t consumed_generation;
  uint8_t fresh_grace;
  uint8_t safety_bad;
  uint8_t seal_cas_race;
  uint8_t clear_cas_race;
  uint8_t final_cas_race;
  uint8_t unseal_cas_race;
} LateState;

typedef struct LateStats {
  ExploreStats all;
  uint64_t consumed_current;
  uint64_t consumed_next;
} LateStats;

static int late_owner_enabled(const LateState *s)
{
  switch (s->owner_pc) {
  case LO_SEAL_OBSERVE:
  case LO_NEXT_SEAL_OBSERVE:
    return gate_closed(s->gate) && gate_count(s->gate) == 0;
  case LO_SEAL_CAS:
  case LO_CLEAR_CURRENT:
  case LO_FINAL_OBSERVE:
  case LO_FINAL_CAS:
  case LO_APPLY_CURRENT:
  case LO_UNSEAL_OBSERVE:
  case LO_UNSEAL_CAS:
  case LO_NEXT_SEAL_CAS:
  case LO_NEXT_CLEAR:
  case LO_NEXT_GRACE:
  case LO_NEXT_COMMIT:
    return 1;
  case LO_SCAN_CURRENT:
  case LO_NEXT_SCAN:
    return gate_sealed(s->gate) && gate_count(s->gate) == 0;
  case LO_WAIT_PUBLISHER:
    return s->publisher_pc == LP_DONE;
  default:
    return 0;
  }
}

static void late_owner_step(LateState *s, enum LateVariant variant)
{
  assert(late_owner_enabled(s));

  switch (s->owner_pc) {
  case LO_SEAL_OBSERVE:
    s->expected = s->gate;
    s->owner_pc = LO_SEAL_CAS;
    break;
  case LO_SEAL_CAS:
    if (gate_cas(&s->gate, s->expected,
                 s->expected | GATE_CLOSED | GATE_SEALED)) {
      s->owner_pc = LO_SCAN_CURRENT;
    } else {
      s->seal_cas_race = 1;
      s->owner_pc = LO_SEAL_OBSERVE;
    }
    break;
  case LO_SCAN_CURRENT:
    if (variant == LATE_SAME_CYCLE_FREE && s->late_bit &&
        s->object_state == OBJ_ALLOCATED) {
      /* Broken: the current grace predates this terminal intent. */
      s->late_bit = 0;
      s->consumed_generation = 1;
      s->object_state = OBJ_FREED;
      s->safety_bad = 1;
    }
    /* Good protocol deliberately leaves late_bit durable. */
    s->expected = s->gate;
    s->owner_pc = LO_CLEAR_CURRENT;
    break;
  case LO_CLEAR_CURRENT:
    if (s->gate != s->expected) {
      s->clear_cas_race = 1;
      s->owner_pc = LO_SCAN_CURRENT;
    } else {
      if (s->expected & GATE_PENDING)
        assert(gate_cas(&s->gate, s->expected,
                        s->expected & ~GATE_PENDING));
      s->owner_pc = LO_FINAL_OBSERVE;
    }
    break;
  case LO_FINAL_OBSERVE:
    if (s->gate != GATE_SEALED_ZERO) {
      s->owner_pc = LO_SCAN_CURRENT;
    } else {
      s->expected = GATE_SEALED_ZERO;
      s->owner_pc = LO_FINAL_CAS;
    }
    break;
  case LO_FINAL_CAS:
    if (gate_cas(&s->gate, s->expected, GATE_SEALED)) {
      s->current_commit = 1;
      s->owner_pc = LO_APPLY_CURRENT;
    } else {
      s->final_cas_race = 1;
      s->owner_pc = LO_SCAN_CURRENT;
    }
    break;
  case LO_APPLY_CURRENT:
    if (variant == LATE_SAME_CYCLE_FREE && s->late_bit &&
        s->object_state == OBJ_ALLOCATED) {
      s->late_bit = 0;
      s->consumed_generation = 1;
      s->object_state = OBJ_FREED;
      s->safety_bad = 1;
    }
    if (s->object_state == OBJ_FREED && !s->fresh_grace)
      s->safety_bad = 1;
    s->owner_pc = LO_UNSEAL_OBSERVE;
    break;
  case LO_UNSEAL_OBSERVE:
    assert((s->gate & (GATE_CLOSED | GATE_SEALED)) == GATE_SEALED);
    s->expected = s->gate;
    s->owner_pc = LO_UNSEAL_CAS;
    break;
  case LO_UNSEAL_CAS: {
    uint64_t desired = GATE_CLOSED | gate_count(s->expected) |
        (s->expected & GATE_PENDING);
    if (gate_cas(&s->gate, s->expected, desired)) {
      s->owner_pc = LO_WAIT_PUBLISHER;
    } else {
      s->unseal_cas_race = 1;
      s->owner_pc = LO_UNSEAL_OBSERVE;
    }
    break;
  }
  case LO_WAIT_PUBLISHER:
    assert(s->publisher_pc == LP_DONE);
    s->owner_pc = LO_NEXT_SEAL_OBSERVE;
    break;
  case LO_NEXT_SEAL_OBSERVE:
    s->expected = s->gate;
    s->owner_pc = LO_NEXT_SEAL_CAS;
    break;
  case LO_NEXT_SEAL_CAS:
    /* Publisher is complete, but preserve the exact-CAS formulation. */
    if (gate_cas(&s->gate, s->expected,
                 s->expected | GATE_CLOSED | GATE_SEALED)) {
      s->owner_pc = LO_NEXT_SCAN;
    } else {
      s->owner_pc = LO_NEXT_SEAL_OBSERVE;
    }
    break;
  case LO_NEXT_SCAN:
    if (s->late_bit) {
      assert(s->object_state == OBJ_ALLOCATED);
      s->late_bit = 0;
      s->consumed_generation = 2;
      s->object_state = OBJ_FREEING;
    }
    s->expected = s->gate;
    s->owner_pc = LO_NEXT_CLEAR;
    break;
  case LO_NEXT_CLEAR:
    assert(s->gate == s->expected);
    if (s->expected & GATE_PENDING)
      assert(gate_cas(&s->gate, s->expected,
                      s->expected & ~GATE_PENDING));
    s->owner_pc = LO_NEXT_GRACE;
    break;
  case LO_NEXT_GRACE:
    s->fresh_grace = 1;
    s->owner_pc = LO_NEXT_COMMIT;
    break;
  case LO_NEXT_COMMIT:
    assert(s->gate == GATE_SEALED_ZERO);
    assert(gate_cas(&s->gate, GATE_SEALED_ZERO, GATE_SEALED));
    if (s->object_state == OBJ_FREEING) {
      assert(s->fresh_grace);
      s->object_state = OBJ_FREED;
    }
    assert(gate_cas(&s->gate, GATE_SEALED, GATE_CLOSED));
    s->owner_pc = LO_DONE;
    break;
  default:
    assert(0);
  }
}

static int late_publisher_enabled(const LateState *s)
{
  return s->publisher_pc != LP_DONE;
}

static void late_publisher_step(LateState *s)
{
  assert(late_publisher_enabled(s));

  switch (s->publisher_pc) {
  case LP_ENTER:
    /* This actor represents a terminal/free request, so it starts only after
    ** close.  SEALED is admitted exactly like CLOSED. */
    assert(gate_closed(s->gate) || gate_sealed(s->gate));
    s->published_sealed = (uint8_t)gate_sealed(s->gate);
    if (s->current_commit &&
        (s->gate & (GATE_CLOSED | GATE_SEALED)) == GATE_SEALED)
      s->published_post_lp = 1;
    gate_join_intent(&s->gate, 0);
    if (s->owner_pc == LO_SEAL_CAS) s->seal_cas_race = 1;
    if (s->owner_pc == LO_CLEAR_CURRENT) s->clear_cas_race = 1;
    if (s->owner_pc == LO_FINAL_CAS) s->final_cas_race = 1;
    if (s->owner_pc == LO_UNSEAL_CAS) s->unseal_cas_race = 1;
    s->publisher_pc = LP_SET_BIT;
    break;
  case LP_SET_BIT:
    s->late_bit = 1;
    s->published = 1;
    s->publisher_pc = LP_LEAVE;
    break;
  case LP_LEAVE:
    gate_leave(&s->gate);
    s->publisher_pc = LP_DONE;
    break;
  default:
    assert(0);
  }
}

#define LATE_MAX_DEPTH 28u

static void late_explore(LateState state, enum LateVariant variant,
                         LateStats *stats, char *trace, unsigned depth)
{
  int enabled = 0;

  gate_assert_valid(state.gate);
  if (state.object_state == OBJ_FREED && state.published &&
      !state.fresh_grace)
    state.safety_bad = 1;
  if (state.owner_pc == LO_DONE && state.publisher_pc == LP_DONE) {
    stats->all.terminals++;
    assert(state.published);
    if (state.safety_bad) {
      stats->all.safety_failures++;
      remember_trace(stats->all.first_safety, trace, depth);
    }
    if (state.consumed_generation == 1) stats->consumed_current++;
    if (state.consumed_generation == 2) stats->consumed_next++;
    if (state.seal_cas_race) stats->all.seal_cas_races++;
    if (state.clear_cas_race) stats->all.clear_cas_races++;
    if (state.final_cas_race) stats->all.final_cas_races++;
    if (state.unseal_cas_race) stats->all.unseal_cas_races++;
    if (state.published_sealed) stats->all.sealed_publications++;
    if (state.published_post_lp) stats->all.postlp_intents++;
    return;
  }
  if (depth == LATE_MAX_DEPTH) {
    stats->all.truncated++;
    return;
  }

  if (late_owner_enabled(&state)) {
    LateState next = state;
    trace[depth] = 'O';
    late_owner_step(&next, variant);
    late_explore(next, variant, stats, trace, depth + 1u);
    enabled = 1;
  }
  if (late_publisher_enabled(&state)) {
    LateState next = state;
    trace[depth] = 'T';
    late_publisher_step(&next);
    late_explore(next, variant, stats, trace, depth + 1u);
    enabled = 1;
  }
  assert(enabled && "late-bit model reached a non-terminal dead end");
}

static LateStats run_late_model(enum LateVariant variant)
{
  LateState state;
  LateStats stats;
  char trace[TRACE_CAP];

  memset(&state, 0, sizeof(state));
  memset(&stats, 0, sizeof(stats));
  memset(trace, 0, sizeof(trace));
  state.gate = GATE_CLOSED;
  late_explore(state, variant, &stats, trace, 0);
  assert(stats.all.terminals != 0);
  assert(stats.all.truncated == 0);
  return stats;
}

static void test_late_protocol(void)
{
  LateStats good = run_late_model(LATE_GOOD);
  LateStats same_cycle = run_late_model(LATE_SAME_CYCLE_FREE);

  assert(good.all.safety_failures == 0);
  assert(good.consumed_current == 0);
  assert(good.consumed_next == good.all.terminals);
  assert(good.all.sealed_publications != 0);
  assert(good.all.seal_cas_races != 0);
  assert(good.all.clear_cas_races != 0);
  assert(good.all.final_cas_races != 0);
  assert(good.all.unseal_cas_races != 0);
  assert(good.all.postlp_intents != 0);
  assert(same_cycle.all.safety_failures != 0);
  assert(same_cycle.consumed_current != 0);
  assert(same_cycle.all.first_safety[0] != '\0');

  printf("  late free: %" PRIu64 " safe schedules; same-cycle mutant failed "
         "%" PRIu64 " (first %s)\n",
         good.all.terminals, same_cycle.all.safety_failures,
         same_cycle.all.first_safety);
}

/* ----------------------------------------------------------------------- */
/* Reclaimed-arena adoption and stale-bit/reuse arbitration.
**
** A stale producer admitted under CLOSED/SEALED sets a generation-old bit.
** Adoption drops that bit while the committed cell is free, crosses the clean
** generation LP, and rebuilds privately in committed SEALED.  Exact OPEN CAS
** fails if a late publisher joins; staged metadata is rolled back, SEALED is
** exactly unsealed to CLOSED with count/intent preserved, and adoption retries.
** The actor is the legal first/only free publication: logical retirement and
** this publication are one synchronous producer operation, so it must begin
** before reuse. A same-address publication which starts only after OPEN/reuse
** would be an invalid double-free and is outside this protocol model.
*/

enum AdoptVariant {
  ADOPT_GOOD = 0,
  ADOPT_OPEN_BEFORE_REBUILD,
  ADOPT_OPEN_NOT_EXACT
};

enum {
  AO_SEAL_OBSERVE = 0,
  AO_SEAL_CAS,
  AO_SCAN_INTENTS,
  AO_CLEAR_PENDING,
  AO_COMMIT_OBSERVE,
  AO_COMMIT_CAS,
  AO_DRAIN,
  AO_REBUILD,
  AO_CLEAR_RECLAIMED,
  AO_LINK,
  AO_OPEN_OBSERVE,
  AO_OPEN_CAS,
  AO_ROLLBACK,
  AO_UNSEAL_OBSERVE,
  AO_UNSEAL_CAS,
  AO_DONE
};

enum {
  SP_ENTER = 0,
  SP_PUBLISH,
  SP_LEAVE,
  SP_DONE
};

enum {
  SPA_NONE = 0,
  SPA_BIT_ONLY,
  SPA_ORDINARY
};

enum {
  CELL_OLD_COMMITTED_FREE = 1,
  CELL_NEW_ALLOCATION = 2
};

typedef struct AdoptState {
  uint64_t gate;
  uint64_t expected;
  uint8_t adopter_pc;
  uint8_t publisher_pc;
  uint8_t publisher_admission;
  uint8_t scan_return_pc;
  uint8_t drained;
  uint8_t metadata_valid;
  uint8_t reclaimed;
  uint8_t linked;
  uint8_t arena_open;
  uint8_t stale_bit;
  uint8_t stale_bit_generation;
  uint8_t cell_generation;
  uint8_t reused;
  uint8_t publisher_completed;
  uint8_t published_sealed;
  uint8_t loose_open;
  uint8_t safety_bad;
  uint8_t stale_reuse_bad;
  uint8_t seal_cas_race;
  uint8_t clear_cas_race;
  uint8_t open_cas_race;
  uint8_t commit_cas_race;
  uint8_t unseal_cas_race;
} AdoptState;

typedef struct AdoptStats {
  ExploreStats all;
  uint64_t stale_reuse_failures;
  uint64_t ordinary_after_open;
  uint64_t commit_cas_races;
  uint64_t open_cas_races;
} AdoptStats;

static int adopt_ready(const AdoptState *s)
{
  return s->drained && s->metadata_valid && !s->reclaimed && s->linked;
}

static int adopt_owner_enabled(const AdoptState *s)
{
  switch (s->adopter_pc) {
  case AO_SEAL_OBSERVE:
    return gate_closed(s->gate) && gate_count(s->gate) == 0;
  case AO_SCAN_INTENTS:
    return gate_sealed(s->gate) && gate_count(s->gate) == 0;
  case AO_SEAL_CAS:
  case AO_CLEAR_PENDING:
  case AO_COMMIT_OBSERVE:
  case AO_COMMIT_CAS:
  case AO_DRAIN:
  case AO_REBUILD:
  case AO_CLEAR_RECLAIMED:
  case AO_LINK:
  case AO_OPEN_OBSERVE:
  case AO_OPEN_CAS:
  case AO_ROLLBACK:
  case AO_UNSEAL_OBSERVE:
  case AO_UNSEAL_CAS:
    return 1;
  default:
    return 0;
  }
}

static void adopt_owner_step(AdoptState *s, enum AdoptVariant variant)
{
  assert(adopt_owner_enabled(s));

  switch (s->adopter_pc) {
  case AO_SEAL_OBSERVE:
    s->expected = s->gate;
    s->adopter_pc = AO_SEAL_CAS;
    break;
  case AO_SEAL_CAS:
    if (gate_cas(&s->gate, s->expected,
                 s->expected | GATE_CLOSED | GATE_SEALED)) {
      s->scan_return_pc = AO_COMMIT_OBSERVE;
      s->adopter_pc = AO_SCAN_INTENTS;
    } else {
      s->seal_cas_race = 1;
      s->adopter_pc = AO_SEAL_OBSERVE;
    }
    break;
  case AO_SCAN_INTENTS:
    assert(gate_count(s->gate) == 0);
    if (s->stale_bit) {
      /* The committed allocation is absent, so this is an old-generation
      ** duplicate.  Drop it before any free run becomes reusable. */
      assert(s->stale_bit_generation == CELL_OLD_COMMITTED_FREE);
      assert(s->cell_generation == CELL_OLD_COMMITTED_FREE);
      s->stale_bit = 0;
      s->stale_bit_generation = 0;
    }
    s->expected = s->gate;
    s->adopter_pc = AO_CLEAR_PENDING;
    break;
  case AO_CLEAR_PENDING:
    if (s->gate != s->expected) {
      s->clear_cas_race = 1;
      s->adopter_pc = AO_SCAN_INTENTS;
    } else {
      if (s->expected & GATE_PENDING)
        assert(gate_cas(&s->gate, s->expected,
                        s->expected & ~GATE_PENDING));
      s->adopter_pc = s->scan_return_pc;
    }
    break;
  case AO_COMMIT_OBSERVE:
    if (s->gate != GATE_SEALED_ZERO) {
      s->scan_return_pc = AO_COMMIT_OBSERVE;
      s->adopter_pc = AO_SCAN_INTENTS;
    } else {
      s->expected = GATE_SEALED_ZERO;
      s->adopter_pc = AO_COMMIT_CAS;
    }
    break;
  case AO_COMMIT_CAS:
    if (gate_cas(&s->gate, s->expected, GATE_SEALED)) {
      s->adopter_pc = (uint8_t)(
          variant == ADOPT_OPEN_BEFORE_REBUILD ? AO_OPEN_OBSERVE : AO_DRAIN);
    } else {
      s->commit_cas_race = 1;
      s->scan_return_pc = AO_COMMIT_OBSERVE;
      s->adopter_pc = AO_SCAN_INTENTS;
    }
    break;
  case AO_DRAIN:
    s->drained = 1;
    s->adopter_pc = AO_REBUILD;
    break;
  case AO_REBUILD:
    s->metadata_valid = 1;
    s->adopter_pc = AO_CLEAR_RECLAIMED;
    break;
  case AO_CLEAR_RECLAIMED:
    s->reclaimed = 0;
    s->adopter_pc = AO_LINK;
    break;
  case AO_LINK:
    s->linked = 1;
    s->adopter_pc = (uint8_t)(
        variant == ADOPT_OPEN_BEFORE_REBUILD ? AO_DONE : AO_OPEN_OBSERVE);
    break;
  case AO_OPEN_OBSERVE:
    if (s->gate != GATE_SEALED) {
      s->adopter_pc = AO_ROLLBACK;
    } else {
      s->expected = GATE_SEALED;
      s->adopter_pc = AO_OPEN_CAS;
    }
    break;
  case AO_OPEN_CAS:
    if (variant == ADOPT_OPEN_NOT_EXACT && s->gate != s->expected) {
      /* Broken: publish logical OPEN while the counted bit publisher still
      ** owns its SEALED admission.  Leave the word intact so its leave step
      ** remains defined; allocator reuse is now incorrectly enabled. */
      s->arena_open = 1;
      s->loose_open = 1;
    } else if (gate_cas(&s->gate, s->expected, 0)) {
      s->arena_open = 1;
    } else {
      s->open_cas_race = 1;
      s->adopter_pc = AO_ROLLBACK;
      break;
    }
    if (variant == ADOPT_OPEN_BEFORE_REBUILD) {
      if (!adopt_ready(s)) s->safety_bad = 1;
      s->adopter_pc = AO_DRAIN;
    } else {
      s->adopter_pc = AO_DONE;
    }
    break;
  case AO_ROLLBACK:
    s->drained = 0;
    s->metadata_valid = 0;
    s->reclaimed = 1;
    s->linked = 0;
    s->adopter_pc = AO_UNSEAL_OBSERVE;
    break;
  case AO_UNSEAL_OBSERVE:
    assert((s->gate & (GATE_CLOSED | GATE_SEALED)) == GATE_SEALED);
    s->expected = s->gate;
    s->adopter_pc = AO_UNSEAL_CAS;
    break;
  case AO_UNSEAL_CAS: {
    uint64_t desired = GATE_CLOSED | gate_count(s->expected) |
        (s->expected & GATE_PENDING);
    if (gate_cas(&s->gate, s->expected, desired)) {
      s->adopter_pc = AO_SEAL_OBSERVE;
    } else {
      s->unseal_cas_race = 1;
      s->adopter_pc = AO_UNSEAL_OBSERVE;
    }
    break;
  }
  default:
    assert(0);
  }
}

static int stale_publisher_enabled(const AdoptState *s)
{
  return s->publisher_pc != SP_DONE &&
	 !(s->publisher_pc == SP_ENTER && gate_open(s->gate));
}

static void stale_publisher_step(AdoptState *s)
{
  assert(stale_publisher_enabled(s));

  switch (s->publisher_pc) {
  case SP_ENTER:
    if (gate_open(s->gate)) {
      gate_join_open(&s->gate);
      s->publisher_admission = SPA_ORDINARY;
    } else {
      s->published_sealed = (uint8_t)gate_sealed(s->gate);
      gate_join_intent(&s->gate, 0);
      s->publisher_admission = SPA_BIT_ONLY;
      if (s->adopter_pc == AO_SEAL_CAS) s->seal_cas_race = 1;
      if (s->adopter_pc == AO_CLEAR_PENDING) s->clear_cas_race = 1;
      if (s->adopter_pc == AO_COMMIT_CAS) s->commit_cas_race = 1;
      if (s->adopter_pc == AO_OPEN_CAS) s->open_cas_race = 1;
      if (s->adopter_pc == AO_UNSEAL_CAS) s->unseal_cas_race = 1;
    }
    s->publisher_pc = SP_PUBLISH;
    break;
  case SP_PUBLISH:
    if (s->publisher_admission == SPA_BIT_ONLY) {
      /* Captured generation is OLD even if a broken open already let the
      ** allocator reuse the address. */
      s->stale_bit = 1;
      s->stale_bit_generation = CELL_OLD_COMMITTED_FREE;
      if (s->cell_generation == CELL_NEW_ALLOCATION) {
        s->stale_reuse_bad = 1;
        s->safety_bad = 1;
      }
    } else {
      /* Losing the gate race to OPEN selects the ordinary route.  Its
      ** captured OLD generation either still names committed-free space or
      ** mismatches a new allocation; in both cases it publishes no bit. */
      assert(s->publisher_admission == SPA_ORDINARY);
    }
    s->publisher_pc = SP_LEAVE;
    break;
  case SP_LEAVE:
    gate_leave(&s->gate);
    s->publisher_completed = 1;
    s->publisher_pc = SP_DONE;
    break;
  default:
    assert(0);
  }
}

static int reuse_enabled(const AdoptState *s)
{
  return s->arena_open && !s->reused;
}

static void reuse_step(AdoptState *s)
{
  assert(reuse_enabled(s));
  if (!adopt_ready(s)) s->safety_bad = 1;
  if (s->stale_bit) {
    s->stale_reuse_bad = 1;
    s->safety_bad = 1;
  }
  s->cell_generation = CELL_NEW_ALLOCATION;
  s->reused = 1;
}

#define ADOPT_MAX_DEPTH 48u

static void adopt_explore(AdoptState state, enum AdoptVariant variant,
                          AdoptStats *stats, char *trace, unsigned depth)
{
  int enabled = 0;

  gate_assert_valid(state.gate);
  /* Prune schedules in which the old generation's purported first free does
  ** not start until after adoption OPEN. Such an actor is a double-free, not a
  ** legal delayed producer; legal producers are admitted before this point. */
  if (state.publisher_pc == SP_ENTER && gate_open(state.gate))
    return;
  if (state.arena_open && !adopt_ready(&state)) state.safety_bad = 1;
  if (state.cell_generation == CELL_NEW_ALLOCATION && state.stale_bit &&
      state.stale_bit_generation == CELL_OLD_COMMITTED_FREE) {
    state.stale_reuse_bad = 1;
    state.safety_bad = 1;
  }
  if (state.adopter_pc == AO_DONE && state.publisher_pc == SP_DONE &&
      state.reused) {
    stats->all.terminals++;
    assert(state.publisher_completed);
    if (state.safety_bad) {
      stats->all.safety_failures++;
      remember_trace(stats->all.first_safety, trace, depth);
    }
    if (state.stale_reuse_bad) stats->stale_reuse_failures++;
    if (state.publisher_admission == SPA_ORDINARY)
      stats->ordinary_after_open++;
    if (state.seal_cas_race) stats->all.seal_cas_races++;
    if (state.clear_cas_race) stats->all.clear_cas_races++;
    if (state.commit_cas_race) stats->commit_cas_races++;
    if (state.open_cas_race) stats->open_cas_races++;
    if (state.commit_cas_race || state.open_cas_race)
      stats->all.final_cas_races++;
    if (state.unseal_cas_race) stats->all.unseal_cas_races++;
    if (state.published_sealed) stats->all.sealed_publications++;
    return;
  }
  if (depth == ADOPT_MAX_DEPTH) {
    stats->all.truncated++;
    return;
  }

  if (adopt_owner_enabled(&state)) {
    AdoptState next = state;
    trace[depth] = 'A';
    adopt_owner_step(&next, variant);
    adopt_explore(next, variant, stats, trace, depth + 1u);
    enabled = 1;
  }
  if (stale_publisher_enabled(&state)) {
    AdoptState next = state;
    trace[depth] = 'S';
    stale_publisher_step(&next);
    adopt_explore(next, variant, stats, trace, depth + 1u);
    enabled = 1;
  }
  if (reuse_enabled(&state)) {
    AdoptState next = state;
    trace[depth] = 'U';
    reuse_step(&next);
    adopt_explore(next, variant, stats, trace, depth + 1u);
    enabled = 1;
  }
  assert(enabled && "adoption model reached a non-terminal dead end");
}

static AdoptStats run_adopt_model(enum AdoptVariant variant)
{
  AdoptState state;
  AdoptStats stats;
  char trace[TRACE_CAP];

  memset(&state, 0, sizeof(state));
  memset(&stats, 0, sizeof(stats));
  memset(trace, 0, sizeof(trace));
  state.gate = GATE_CLOSED;
  state.reclaimed = 1;
  state.cell_generation = CELL_OLD_COMMITTED_FREE;
  adopt_explore(state, variant, &stats, trace, 0);
  assert(stats.all.terminals != 0);
  assert(stats.all.truncated == 0);
  return stats;
}

static void test_adoption_protocol(void)
{
  AdoptStats good = run_adopt_model(ADOPT_GOOD);
  AdoptStats early = run_adopt_model(ADOPT_OPEN_BEFORE_REBUILD);
  AdoptStats loose = run_adopt_model(ADOPT_OPEN_NOT_EXACT);

  assert(good.all.safety_failures == 0);
  assert(good.stale_reuse_failures == 0);
  assert(good.all.sealed_publications != 0);
  assert(good.all.seal_cas_races != 0);
  assert(good.all.clear_cas_races != 0);
  assert(good.all.final_cas_races != 0);
  assert(good.commit_cas_races != 0);
  assert(good.open_cas_races != 0);
  assert(good.all.unseal_cas_races != 0);
  assert(early.all.safety_failures != 0);
  assert(early.all.first_safety[0] != '\0');
  assert(loose.all.safety_failures != 0);
  assert(loose.stale_reuse_failures != 0);
  assert(loose.all.first_safety[0] != '\0');

  printf("  adoption/reuse: %" PRIu64 " safe schedules; early-OPEN failed "
         "%" PRIu64 " (%s); loose-OPEN failed %" PRIu64
         ", including %" PRIu64 " stale-bit/reuse schedules (%s)\n",
         good.all.terminals,
         early.all.safety_failures, early.all.first_safety,
         loose.all.safety_failures, loose.stale_reuse_failures,
         loose.all.first_safety);
}

int main(void)
{
  c11_gate_smoke();
  test_rescue_protocol();
  test_late_protocol();
  test_adoption_protocol();
  puts("arena terminal gate model tests passed");
  return 0;
}
