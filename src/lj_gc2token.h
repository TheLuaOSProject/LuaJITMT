/*
** lj_gc2token.h - typed, non-wrapping GC2 activation authority.
**
** The mark epoch may deliberately stay unchanged across minor collections.
** transition_generation therefore changes at every semantic phase edge and
** prevents an IDLE -> active -> IDLE cycle from becoming an ABA to barriers.
*/
#ifndef _LJ_GC2TOKEN_H
#define _LJ_GC2TOKEN_H

#include "lj_atomic.h"

#if !defined(__x86_64__)
#error "GC2 activation tokens currently require the x86-64 CX16 contract"
#endif

#define LJ_GC2_ACT_STATE_BITS 3u
#define LJ_GC2_ACT_GATE_BITS 2u
#define LJ_GC2_ACT_GATE_SHIFT LJ_GC2_ACT_STATE_BITS
#define LJ_GC2_ACT_AUTHORITY_BITS \
  (LJ_GC2_ACT_STATE_BITS + LJ_GC2_ACT_GATE_BITS)
#define LJ_GC2_ACT_STATE_MASK \
  ((UINT64_C(1) << LJ_GC2_ACT_STATE_BITS) - 1u)
#define LJ_GC2_ACT_GATE_MASK \
  ((UINT64_C(1) << LJ_GC2_ACT_GATE_BITS) - 1u)
#define LJ_GC2_ACT_MAX_GENERATION \
  (UINT64_MAX >> LJ_GC2_ACT_AUTHORITY_BITS)

typedef enum LJGC2ActivationState {
  LJ_GC2_ACT_IDLE = 0,
  LJ_GC2_ACT_PREP = 1,
  LJ_GC2_ACT_MARK = 2,
  LJ_GC2_ACT_WEAK = 3,
  LJ_GC2_ACT_SWEEP_OPEN = 4,
  LJ_GC2_ACT_SWEEP_CLOSING = 5,
  LJ_GC2_ACT_SWEEP_COMMIT = 6,
  LJ_GC2_ACT_NO_RECLAIM = 7
} LJGC2ActivationState;

/*
** Root admission is part of the same exact-CAS authority as the phase.  A
** separate pending word would leave a publisher-between-check-and-CAS hole.
*/
typedef enum LJGC2RootGateState {
  LJ_GC2_ROOT_GATE_OPEN = 0,
  LJ_GC2_ROOT_GATE_CLOSING = 1,
  LJ_GC2_ROOT_GATE_PENDING = 2,
  LJ_GC2_ROOT_GATE_COMMIT = 3
} LJGC2RootGateState;

typedef struct LJGC2Activation {
  /* lo = mark_epoch; hi = generation << 5 | gate << 3 | state. */
  la_u128 value;
} LJGC2Activation;

typedef struct LJGC2ActivationSnap {
  uint64_t mark_epoch;
  uint64_t generation;
  uint8_t state;
  uint8_t gate;
} LJGC2ActivationSnap;

typedef enum LJGC2TransitionResult {
  LJ_GC2_TRANSITION_INVALID = -2,
  LJ_GC2_TRANSITION_PINNED = -1,
  LJ_GC2_TRANSITION_SATURATED = LJ_GC2_TRANSITION_PINNED,
  LJ_GC2_TRANSITION_LOST = 0,
  LJ_GC2_TRANSITION_OK = 1
} LJGC2TransitionResult;

typedef char lj_gc2_activation_size_must_be_16[
  sizeof(LJGC2Activation) == 16 ? 1 : -1];
typedef char lj_gc2_activation_align_must_be_16[
  __alignof__(LJGC2Activation) >= 16 ? 1 : -1];

LA_INLINE uint64_t lj_gc2_activation_pack_hi(uint64_t generation,
                                              uint8_t state, uint8_t gate)
{
  return (generation << LJ_GC2_ACT_AUTHORITY_BITS) |
         ((uint64_t)gate << LJ_GC2_ACT_GATE_SHIFT) | (uint64_t)state;
}

LA_INLINE int lj_gc2_activation_components_valid(uint64_t generation,
                                                  uint8_t state, uint8_t gate)
{
  return generation <= LJ_GC2_ACT_MAX_GENERATION &&
         state <= LJ_GC2_ACT_NO_RECLAIM && gate <= LJ_GC2_ROOT_GATE_COMMIT &&
         (state != LJ_GC2_ACT_NO_RECLAIM || gate == LJ_GC2_ROOT_GATE_OPEN);
}

LA_INLINE int lj_gc2_activation_value_valid(uint64_t mark_epoch,
                                             uint64_t generation,
                                             uint8_t state, uint8_t gate)
{
  return lj_gc2_activation_components_valid(generation, state, gate) &&
         (mark_epoch != 0 || state == LJ_GC2_ACT_IDLE ||
          state == LJ_GC2_ACT_NO_RECLAIM);
}

/* Only valid before the containing global state is published. */
LA_INLINE int lj_gc2_activation_init_unpublished(LJGC2Activation *token,
                                                  uint64_t mark_epoch,
                                                  uint64_t generation,
                                                  uint8_t state)
{
  if (!lj_gc2_activation_value_valid(mark_epoch, generation, state,
                                      LJ_GC2_ROOT_GATE_OPEN))
    return 0;
  token->value.lo = mark_epoch;
  token->value.hi = lj_gc2_activation_pack_hi(generation, state,
                                               LJ_GC2_ROOT_GATE_OPEN);
  return 1;
}

/*
** Stable acquire snapshot without a potentially out-of-line 16-byte load.
** As with markwords, overlapping 64-bit subloads/CX16 are an explicit x86-64
** GCC/Clang/MinGW/Darwin compiler contract checked in target artifacts.
*/
LA_INLINE LJGC2ActivationSnap
lj_gc2_activation_snapshot(const LJGC2Activation *token)
{
  LJGC2ActivationSnap snap;
  uint64_t hi, again;
  do {
    hi = la_load64_acq(&token->value.hi);
    snap.mark_epoch = la_load64_acq(&token->value.lo);
    again = la_load64_acq(&token->value.hi);
  } while (hi != again);
  snap.generation = hi >> LJ_GC2_ACT_AUTHORITY_BITS;
  snap.state = (uint8_t)(hi & LJ_GC2_ACT_STATE_MASK);
  snap.gate = (uint8_t)((hi >> LJ_GC2_ACT_GATE_SHIFT) &
                        LJ_GC2_ACT_GATE_MASK);
  return snap;
}

LA_INLINE int lj_gc2_activation_equal(const LJGC2ActivationSnap *a,
                                       const LJGC2ActivationSnap *b)
{
  return a->mark_epoch == b->mark_epoch &&
         a->generation == b->generation && a->state == b->state &&
         a->gate == b->gate;
}

/* A close must retry after PENDING; COMMIT can still be invalidated exactly. */
LA_INLINE int lj_gc2_root_gate_edge_valid(uint8_t from, uint8_t to)
{
  if (from == to)
    return 1;
  switch (from) {
  case LJ_GC2_ROOT_GATE_OPEN:
    return to == LJ_GC2_ROOT_GATE_CLOSING;
  case LJ_GC2_ROOT_GATE_CLOSING:
    return to == LJ_GC2_ROOT_GATE_PENDING ||
           to == LJ_GC2_ROOT_GATE_COMMIT || to == LJ_GC2_ROOT_GATE_OPEN;
  case LJ_GC2_ROOT_GATE_PENDING:
    return to == LJ_GC2_ROOT_GATE_CLOSING || to == LJ_GC2_ROOT_GATE_OPEN;
  case LJ_GC2_ROOT_GATE_COMMIT:
    return to == LJ_GC2_ROOT_GATE_PENDING || to == LJ_GC2_ROOT_GATE_OPEN;
  default:
    return 0;
  }
}

/* Centralized semantic transition graph.  NO_RECLAIM is absorbing. */
LA_INLINE int lj_gc2_activation_edge_valid(uint8_t from, uint8_t to)
{
  if (to == LJ_GC2_ACT_NO_RECLAIM)
    return 1;
  switch (from) {
  case LJ_GC2_ACT_IDLE:
    return to == LJ_GC2_ACT_PREP || to == LJ_GC2_ACT_MARK;
  case LJ_GC2_ACT_PREP:
    return to == LJ_GC2_ACT_IDLE || to == LJ_GC2_ACT_MARK;
  case LJ_GC2_ACT_MARK:
    return to == LJ_GC2_ACT_IDLE || to == LJ_GC2_ACT_WEAK;
  case LJ_GC2_ACT_WEAK:
    return to == LJ_GC2_ACT_IDLE || to == LJ_GC2_ACT_SWEEP_OPEN;
  case LJ_GC2_ACT_SWEEP_OPEN:
    return to == LJ_GC2_ACT_SWEEP_CLOSING;
  case LJ_GC2_ACT_SWEEP_CLOSING:
    return to == LJ_GC2_ACT_SWEEP_OPEN || to == LJ_GC2_ACT_SWEEP_COMMIT;
  case LJ_GC2_ACT_SWEEP_COMMIT:
    return to == LJ_GC2_ACT_SWEEP_OPEN || to == LJ_GC2_ACT_IDLE;
  case LJ_GC2_ACT_NO_RECLAIM:
  default:
    return 0;
  }
}

/* Epoch selection belongs only to a new cycle's IDLE admission edge. */
LA_INLINE int lj_gc2_activation_epoch_edge_valid(uint64_t from_epoch,
                                                  uint8_t from_state,
                                                  uint64_t to_epoch,
                                                  uint8_t to_state)
{
  if (from_state == LJ_GC2_ACT_IDLE &&
      (to_state == LJ_GC2_ACT_PREP || to_state == LJ_GC2_ACT_MARK))
    return to_epoch >= from_epoch;
  return to_epoch == from_epoch;
}

/*
** Perform one exact semantic transition.  The caller supplies the authority
** it observed; a racing transition returns LOST and an updated observation.
** Saturation is sticky policy input: callers must enter conservative
** no-reclaim mode instead of wrapping the generation.
*/
LA_INLINE LJGC2TransitionResult
lj_gc2_activation_try_update(LJGC2Activation *token,
                             const LJGC2ActivationSnap *expected_snap,
                             uint64_t next_mark_epoch, uint8_t next_state,
                             uint8_t next_gate,
                             LJGC2ActivationSnap *observed)
{
  la_u128 expected, desired;
  uint64_t next_generation;
  if (!lj_gc2_activation_value_valid(expected_snap->mark_epoch,
                                      expected_snap->generation,
                                      expected_snap->state,
                                      expected_snap->gate) ||
      !lj_gc2_activation_value_valid(next_mark_epoch,
                                     expected_snap->generation, next_state,
                                     next_gate) ||
      (next_state != expected_snap->state &&
       !lj_gc2_activation_edge_valid(expected_snap->state, next_state)) ||
      !lj_gc2_root_gate_edge_valid(expected_snap->gate, next_gate) ||
      !lj_gc2_activation_epoch_edge_valid(expected_snap->mark_epoch,
                                           expected_snap->state,
                                           next_mark_epoch, next_state))
    return LJ_GC2_TRANSITION_INVALID;
  if (expected_snap->state == LJ_GC2_ACT_NO_RECLAIM) {
    if (next_state != LJ_GC2_ACT_NO_RECLAIM ||
        next_mark_epoch != expected_snap->mark_epoch ||
        next_gate != LJ_GC2_ROOT_GATE_OPEN)
      return LJ_GC2_TRANSITION_INVALID;
    if (observed)
      *observed = *expected_snap;
    return LJ_GC2_TRANSITION_PINNED;
  }
  if (next_state == expected_snap->state && next_gate == expected_snap->gate)
    return LJ_GC2_TRANSITION_INVALID;
  if (expected_snap->generation == LJ_GC2_ACT_MAX_GENERATION) {
    /* No generation remains for the requested edge.  Exact-CAS the current
    ** authority into absorbing NO_RECLAIM without changing its mark epoch. */
    expected.lo = expected_snap->mark_epoch;
    expected.hi = lj_gc2_activation_pack_hi(expected_snap->generation,
                                             expected_snap->state,
                                             expected_snap->gate);
    desired.lo = expected_snap->mark_epoch;
    desired.hi = lj_gc2_activation_pack_hi(expected_snap->generation,
                                            LJ_GC2_ACT_NO_RECLAIM,
                                            LJ_GC2_ROOT_GATE_OPEN);
    if (la_cas128(&token->value, &expected, desired)) {
      if (observed) {
        observed->mark_epoch = expected_snap->mark_epoch;
        observed->generation = expected_snap->generation;
        observed->state = LJ_GC2_ACT_NO_RECLAIM;
        observed->gate = LJ_GC2_ROOT_GATE_OPEN;
      }
      return LJ_GC2_TRANSITION_PINNED;
    }
    if (observed)
      *observed = lj_gc2_activation_snapshot(token);
    return LJ_GC2_TRANSITION_LOST;
  }
  next_generation = expected_snap->generation + 1u;
  expected.lo = expected_snap->mark_epoch;
  expected.hi = lj_gc2_activation_pack_hi(expected_snap->generation,
                                           expected_snap->state,
                                           expected_snap->gate);
  desired.lo = next_mark_epoch;
  desired.hi = lj_gc2_activation_pack_hi(next_generation, next_state,
                                          next_gate);
  if (la_cas128(&token->value, &expected, desired)) {
    if (observed) {
      observed->mark_epoch = next_mark_epoch;
      observed->generation = next_generation;
      observed->state = next_state;
      observed->gate = next_gate;
    }
    return LJ_GC2_TRANSITION_OK;
  }
  if (observed)
    *observed = lj_gc2_activation_snapshot(token);
  return LJ_GC2_TRANSITION_LOST;
}

LA_INLINE LJGC2TransitionResult
lj_gc2_activation_try_transition(LJGC2Activation *token,
                                 const LJGC2ActivationSnap *expected_snap,
                                 uint64_t next_mark_epoch, uint8_t next_state,
                                 LJGC2ActivationSnap *observed)
{
  uint8_t gate = next_state == LJ_GC2_ACT_NO_RECLAIM ?
                 LJ_GC2_ROOT_GATE_OPEN : expected_snap->gate;
  return lj_gc2_activation_try_update(token, expected_snap, next_mark_epoch,
                                      next_state, gate, observed);
}

/*
** Abandon the staged sweep authority without claiming that root admission was
** ever closed.  This is the only legal shortcut from SWEEP_OPEN to IDLE: both
** sides must use the OPEN root gate, the mark epoch is unchanged, and the
** exact CX16 authority still advances its non-wrapping generation.
**
** The runtime migration layer uses this for a legacy pre-bridge abort and for
** a legacy sweep close while the activation token is veto-only.  It must be
** deleted once SWEEP_CLOSING/SWEEP_COMMIT becomes the reclaim authority.
*/
LA_INLINE LJGC2TransitionResult
lj_gc2_activation_try_abandon_sweep_open(
  LJGC2Activation *token, const LJGC2ActivationSnap *expected_snap,
  LJGC2ActivationSnap *observed)
{
  la_u128 expected, desired;
  uint64_t next_generation;
  if (!lj_gc2_activation_value_valid(expected_snap->mark_epoch,
                                      expected_snap->generation,
                                      expected_snap->state,
                                      expected_snap->gate) ||
      expected_snap->state != LJ_GC2_ACT_SWEEP_OPEN ||
      expected_snap->gate != LJ_GC2_ROOT_GATE_OPEN)
    return LJ_GC2_TRANSITION_INVALID;
  if (expected_snap->generation == LJ_GC2_ACT_MAX_GENERATION)
    return lj_gc2_activation_try_transition(token, expected_snap,
      expected_snap->mark_epoch, LJ_GC2_ACT_NO_RECLAIM, observed);
  next_generation = expected_snap->generation + 1u;
  expected.lo = expected_snap->mark_epoch;
  expected.hi = lj_gc2_activation_pack_hi(expected_snap->generation,
                                           LJ_GC2_ACT_SWEEP_OPEN,
                                           LJ_GC2_ROOT_GATE_OPEN);
  desired.lo = expected_snap->mark_epoch;
  desired.hi = lj_gc2_activation_pack_hi(next_generation,
                                          LJ_GC2_ACT_IDLE,
                                          LJ_GC2_ROOT_GATE_OPEN);
  if (la_cas128(&token->value, &expected, desired)) {
    if (observed) {
      observed->mark_epoch = expected_snap->mark_epoch;
      observed->generation = next_generation;
      observed->state = LJ_GC2_ACT_IDLE;
      observed->gate = LJ_GC2_ROOT_GATE_OPEN;
    }
    return LJ_GC2_TRANSITION_OK;
  }
  if (observed)
    *observed = lj_gc2_activation_snapshot(token);
  return LJ_GC2_TRANSITION_LOST;
}

LA_INLINE LJGC2TransitionResult
lj_gc2_activation_try_gate(LJGC2Activation *token,
                           const LJGC2ActivationSnap *expected_snap,
                           uint8_t next_gate, LJGC2ActivationSnap *observed)
{
  return lj_gc2_activation_try_update(token, expected_snap,
                                      expected_snap->mark_epoch,
                                      expected_snap->state, next_gate,
                                      observed);
}

/* ---- Helpable table-rescan descriptor and exact per-table token ----- */

/* Descriptor publication and the durable table token share one exact
** generation namespace.  The token's two state bits leave 62 non-wrapping
** generation bits; an ACTIVE descriptor must therefore never exceed this
** same maximum. */
#define LJ_GC2_TABLE_TOKEN_STATE_BITS 2u
#define LJ_GC2_TABLE_TOKEN_STATE_MASK \
  ((UINT64_C(1) << LJ_GC2_TABLE_TOKEN_STATE_BITS) - 1u)
#define LJ_GC2_TABLE_TOKEN_MAX_GENERATION \
  (UINT64_MAX >> LJ_GC2_TABLE_TOKEN_STATE_BITS)

/* The descriptor's low word is zero while idle, one when sticky-pinned, and
** otherwise an aligned GCtab identity. The high word is a non-wrapping
** publication generation. A token must be installed before ACTIVE is cleared,
** so no publisher-owned intermediate state is required. */
typedef enum LJGC2TableDescState {
  LJ_GC2_TABLEDESC_IDLE = 0,
  LJ_GC2_TABLEDESC_ACTIVE = 1,
  LJ_GC2_TABLEDESC_PINNED = 2,
  LJ_GC2_TABLEDESC_INVALID = 3
} LJGC2TableDescState;

typedef struct LJGC2TableDesc {
  la_u128 value;
} LJGC2TableDesc;

typedef struct LJGC2TableDescSnap {
  uintptr_t table;
  uint64_t generation;
  uint8_t state;
} LJGC2TableDescSnap;

typedef struct LJGC2TableDescTicket {
  uint64_t table;
  uint64_t generation;
} LJGC2TableDescTicket;

typedef enum LJGC2TableDescResult {
  LJ_GC2_TABLEDESC_RESULT_INVALID = -2,
  LJ_GC2_TABLEDESC_RESULT_PINNED = -1,
  LJ_GC2_TABLEDESC_RESULT_BUSY = 0,
  LJ_GC2_TABLEDESC_RESULT_OK = 1
} LJGC2TableDescResult;

typedef char lj_gc2_tabledesc_size_must_be_16[
  sizeof(LJGC2TableDesc) == 16 ? 1 : -1];
typedef char lj_gc2_tabledesc_align_must_be_16[
  __alignof__(LJGC2TableDesc) >= 16 ? 1 : -1];

LA_INLINE uint8_t lj_gc2_tabledesc_state(uint64_t table)
{
  if (table == 0)
    return LJ_GC2_TABLEDESC_IDLE;
  if (table == 1)
    return LJ_GC2_TABLEDESC_PINNED;
  return (table & 15u) == 0 ? LJ_GC2_TABLEDESC_ACTIVE :
                              LJ_GC2_TABLEDESC_INVALID;
}

LA_INLINE LJGC2TableDescSnap
lj_gc2_tabledesc_snap_from_words(uint64_t table, uint64_t generation)
{
  LJGC2TableDescSnap snap;
  snap.table = (uintptr_t)table;
  snap.generation = generation;
  snap.state = lj_gc2_tabledesc_state(table);
  if ((snap.state == LJ_GC2_TABLEDESC_ACTIVE && generation == 0) ||
      (generation > LJ_GC2_TABLE_TOKEN_MAX_GENERATION &&
       (snap.state == LJ_GC2_TABLEDESC_IDLE ||
        snap.state == LJ_GC2_TABLEDESC_ACTIVE)))
    snap.state = LJ_GC2_TABLEDESC_INVALID;
  return snap;
}

/* Exact CX16 read. The all-zero comparison is a no-op for the initial value
** and necessarily fails with an acquire observation for every other value. */
LA_INLINE LJGC2TableDescSnap
lj_gc2_tabledesc_snapshot(const LJGC2TableDesc *desc)
{
  la_u128 exact, zero;
  if (!desc)
    return lj_gc2_tabledesc_snap_from_words(1, 0);
  exact.lo = exact.hi = 0;
  zero = exact;
  (void)la_cas128((la_u128 *)(void *)&desc->value, &exact, zero);
  return lj_gc2_tabledesc_snap_from_words(exact.lo, exact.hi);
}

/* Only valid before the containing global state is published. */
LA_INLINE void lj_gc2_tabledesc_init_unpublished(LJGC2TableDesc *desc,
                                                  uint64_t generation)
{
  desc->value.lo = 0;
  desc->value.hi = generation;
}

/* Fault/saturation containment only. Do not use this to resolve ordinary
** BUSY or stale tickets: a losing CAS deliberately follows the latest exact
** authority and may therefore pin a later valid generation. */
LA_INLINE LJGC2TableDescResult
lj_gc2_tabledesc_pin(LJGC2TableDesc *desc, LJGC2TableDescSnap snap,
                     LJGC2TableDescSnap *observed)
{
  la_u128 expected, desired;
  if (!desc)
    return LJ_GC2_TABLEDESC_RESULT_INVALID;
  for (;;) {
    if (snap.state == LJ_GC2_TABLEDESC_PINNED) {
      if (observed)
        *observed = snap;
      return LJ_GC2_TABLEDESC_RESULT_PINNED;
    }
    expected.lo = (uint64_t)snap.table;
    expected.hi = snap.generation;
    desired.lo = 1;
    desired.hi = snap.generation;
    if (la_cas128(&desc->value, &expected, desired)) {
      if (observed)
        *observed = lj_gc2_tabledesc_snap_from_words(desired.lo,
                                                      desired.hi);
      return LJ_GC2_TABLEDESC_RESULT_PINNED;
    }
    snap = lj_gc2_tabledesc_snap_from_words(expected.lo, expected.hi);
  }
}

/* Publish one exact identity. A racing ACTIVE descriptor is ordinary BUSY:
** callers help that exact ticket and retry. Malformed/saturated authority is
** converted to the absorbing PINNED representation without wrapping. */
LA_INLINE LJGC2TableDescResult
lj_gc2_tabledesc_try_publish(LJGC2TableDesc *desc, const void *table,
                             LJGC2TableDescTicket *ticket,
                             LJGC2TableDescSnap *observed)
{
  LJGC2TableDescSnap snap;
  la_u128 expected, desired;
  uintptr_t pointer = (uintptr_t)table;
  if (!desc || !ticket || pointer <= 1u || (pointer & 15u) != 0)
    return LJ_GC2_TABLEDESC_RESULT_INVALID;
  snap = lj_gc2_tabledesc_snapshot(desc);
  if (snap.state == LJ_GC2_TABLEDESC_PINNED) {
    if (observed)
      *observed = snap;
    return LJ_GC2_TABLEDESC_RESULT_PINNED;
  }
  if (snap.state == LJ_GC2_TABLEDESC_INVALID)
    return lj_gc2_tabledesc_pin(desc, snap, observed);
  if (snap.state == LJ_GC2_TABLEDESC_ACTIVE) {
    if (observed)
      *observed = snap;
    return LJ_GC2_TABLEDESC_RESULT_BUSY;
  }
  if (snap.generation >= LJ_GC2_TABLE_TOKEN_MAX_GENERATION)
    return lj_gc2_tabledesc_pin(desc, snap, observed);
  expected.lo = 0;
  expected.hi = snap.generation;
  desired.lo = (uint64_t)pointer;
  desired.hi = snap.generation + 1u;
  if (la_cas128(&desc->value, &expected, desired)) {
    ticket->table = desired.lo;
    ticket->generation = desired.hi;
    if (observed)
      *observed = lj_gc2_tabledesc_snap_from_words(desired.lo, desired.hi);
    return LJ_GC2_TABLEDESC_RESULT_OK;
  }
  snap = lj_gc2_tabledesc_snap_from_words(expected.lo, expected.hi);
  if (observed)
    *observed = snap;
  if (snap.state == LJ_GC2_TABLEDESC_INVALID)
    return lj_gc2_tabledesc_pin(desc, snap, observed);
  return snap.state == LJ_GC2_TABLEDESC_PINNED ?
         LJ_GC2_TABLEDESC_RESULT_PINNED : LJ_GC2_TABLEDESC_RESULT_BUSY;
}

/* Clear only the exact ACTIVE identity which has already been transferred to
** a durable token. A stale helper can never clear a later generation, even if
** allocator reuse supplies the same table address. */
LA_INLINE LJGC2TableDescResult
lj_gc2_tabledesc_finish_help(LJGC2TableDesc *desc,
                             const LJGC2TableDescTicket *ticket,
                             LJGC2TableDescSnap *observed)
{
  la_u128 expected, desired;
  LJGC2TableDescSnap snap;
  if (!desc || !ticket || ticket->table <= 1u ||
      (ticket->table & 15u) != 0)
    return LJ_GC2_TABLEDESC_RESULT_INVALID;
  if (ticket->generation == 0 ||
      ticket->generation > LJ_GC2_TABLE_TOKEN_MAX_GENERATION) {
    snap = lj_gc2_tabledesc_snapshot(desc);
    if (observed)
      *observed = snap;
    if (snap.state == LJ_GC2_TABLEDESC_INVALID)
      return lj_gc2_tabledesc_pin(desc, snap, observed);
    return snap.state == LJ_GC2_TABLEDESC_PINNED ?
           LJ_GC2_TABLEDESC_RESULT_PINNED :
           LJ_GC2_TABLEDESC_RESULT_INVALID;
  }
  expected.lo = ticket->table;
  expected.hi = ticket->generation;
  desired.lo = 0;
  desired.hi = ticket->generation;
  if (la_cas128(&desc->value, &expected, desired)) {
    if (observed)
      *observed = lj_gc2_tabledesc_snap_from_words(desired.lo, desired.hi);
    return LJ_GC2_TABLEDESC_RESULT_OK;
  }
  snap = lj_gc2_tabledesc_snap_from_words(expected.lo, expected.hi);
  if (observed)
    *observed = snap;
  if (snap.state == LJ_GC2_TABLEDESC_INVALID)
    return lj_gc2_tabledesc_pin(desc, snap, observed);
  return snap.state == LJ_GC2_TABLEDESC_PINNED ?
         LJ_GC2_TABLEDESC_RESULT_PINNED : LJ_GC2_TABLEDESC_RESULT_BUSY;
}

typedef enum LJGC2TableTokenState {
  LJ_GC2_TABLE_TOKEN_NONE = 0,
  LJ_GC2_TABLE_TOKEN_PENDING = 1,
  LJ_GC2_TABLE_TOKEN_PINNED = 2,
  LJ_GC2_TABLE_TOKEN_INVALID = 3
} LJGC2TableTokenState;

typedef struct LJGC2TableToken {
  uint64_t control;
} LJGC2TableToken;

typedef char lj_gc2_table_token_size_must_be_8[
  sizeof(LJGC2TableToken) == 8 ? 1 : -1];
typedef char lj_gc2_table_token_align_must_be_8[
  __alignof__(LJGC2TableToken) >= 8 ? 1 : -1];

typedef struct LJGC2TableTokenTicket {
  uint64_t control;
} LJGC2TableTokenTicket;

typedef enum LJGC2TableTokenResult {
  LJ_GC2_TABLE_TOKEN_RESULT_INVALID = -2,
  LJ_GC2_TABLE_TOKEN_RESULT_PINNED = -1,
  LJ_GC2_TABLE_TOKEN_RESULT_BUSY = 0,
  LJ_GC2_TABLE_TOKEN_RESULT_OK = 1
} LJGC2TableTokenResult;

LA_INLINE uint64_t lj_gc2_table_token_pack(uint64_t generation,
                                            uint8_t state)
{
  return (generation << LJ_GC2_TABLE_TOKEN_STATE_BITS) | (uint64_t)state;
}

LA_INLINE uint64_t lj_gc2_table_token_generation(uint64_t control)
{
  return control >> LJ_GC2_TABLE_TOKEN_STATE_BITS;
}

LA_INLINE uint8_t lj_gc2_table_token_state(uint64_t control)
{
  return (uint8_t)(control & LJ_GC2_TABLE_TOKEN_STATE_MASK);
}

/* Only valid before a side cell or huge header becomes reachable. Persistent
** small sidecars initialize once; cell reuse must retain this generation. */
LA_INLINE int lj_gc2_table_token_init_unpublished(LJGC2TableToken *token,
                                                   uint64_t generation)
{
  if (!token || generation > LJ_GC2_TABLE_TOKEN_MAX_GENERATION)
    return 0;
  token->control = lj_gc2_table_token_pack(generation,
                                           LJ_GC2_TABLE_TOKEN_NONE);
  return 1;
}

/* Fault/saturation containment only. Runtime contention returns BUSY through
** refresh/complete and must not call this helper to pin a newer generation. */
LA_INLINE LJGC2TableTokenResult
lj_gc2_table_token_pin(LJGC2TableToken *token, uint64_t control)
{
  uint64_t desired;
  if (!token)
    return LJ_GC2_TABLE_TOKEN_RESULT_INVALID;
  for (;;) {
    uint8_t state = lj_gc2_table_token_state(control);
    if (state == LJ_GC2_TABLE_TOKEN_PINNED)
      return LJ_GC2_TABLE_TOKEN_RESULT_PINNED;
    desired = lj_gc2_table_token_pack(
      lj_gc2_table_token_generation(control), LJ_GC2_TABLE_TOKEN_PINNED);
    if (la_cas64(&token->control, &control, desired, LA_ACQ_REL, LA_ACQ))
      return LJ_GC2_TABLE_TOKEN_RESULT_PINNED;
  }
}

/* Transfer one exact ACTIVE descriptor generation to the durable token.
** Descriptor and token generations are the same authority: an older NONE or
** PENDING value becomes PENDING(target_generation), while the exact target is
** idempotent.  In particular, NONE(target_generation) means another helper
** already completed the request and must never be recreated as PENDING.
**
** A helper delayed behind a newer generation is merely stale (BUSY).  This is
** deliberately checked before malformed-state containment so a stale helper
** cannot pin a later authority it never owned. */
LA_INLINE LJGC2TableTokenResult
lj_gc2_table_token_transfer_exact(LJGC2TableToken *token,
                                  uint64_t target_generation)
{
  uint64_t control, generation, desired;
  uint8_t state;
  if (!token || target_generation == 0 ||
      target_generation > LJ_GC2_TABLE_TOKEN_MAX_GENERATION)
    return LJ_GC2_TABLE_TOKEN_RESULT_INVALID;
  control = la_load64_acq(&token->control);
  for (;;) {
    generation = lj_gc2_table_token_generation(control);
    state = lj_gc2_table_token_state(control);
    if (state == LJ_GC2_TABLE_TOKEN_PINNED)
      return LJ_GC2_TABLE_TOKEN_RESULT_PINNED;
    if (generation > target_generation)
      return LJ_GC2_TABLE_TOKEN_RESULT_BUSY;
    if (state == LJ_GC2_TABLE_TOKEN_INVALID) {
      desired = lj_gc2_table_token_pack(generation,
                                         LJ_GC2_TABLE_TOKEN_PINNED);
      if (la_cas64(&token->control, &control, desired,
                   LA_ACQ_REL, LA_ACQ))
        return LJ_GC2_TABLE_TOKEN_RESULT_PINNED;
      continue;
    }
    if (generation == target_generation)
      return LJ_GC2_TABLE_TOKEN_RESULT_OK;
    desired = lj_gc2_table_token_pack(target_generation,
                                       LJ_GC2_TABLE_TOKEN_PENDING);
    if (la_cas64(&token->control, &control, desired,
                 LA_ACQ_REL, LA_ACQ))
      return LJ_GC2_TABLE_TOKEN_RESULT_OK;
  }
}

/* Legacy local-generation fixture primitive. Production table-rescan handoff
** must use transfer_exact(): mixing this with the shared descriptor namespace
** can recreate a completed request after its descriptor has been cleared. */
LA_INLINE LJGC2TableTokenResult
lj_gc2_table_token_refresh(LJGC2TableToken *token,
                           LJGC2TableTokenTicket *ticket)
{
  uint64_t control, generation, desired;
  uint8_t state;
  if (!token || !ticket)
    return LJ_GC2_TABLE_TOKEN_RESULT_INVALID;
  control = la_load64_acq(&token->control);
  for (;;) {
    state = lj_gc2_table_token_state(control);
    generation = lj_gc2_table_token_generation(control);
    if (state == LJ_GC2_TABLE_TOKEN_PINNED)
      return LJ_GC2_TABLE_TOKEN_RESULT_PINNED;
    if (state == LJ_GC2_TABLE_TOKEN_INVALID ||
        generation == LJ_GC2_TABLE_TOKEN_MAX_GENERATION)
      return lj_gc2_table_token_pin(token, control);
    desired = lj_gc2_table_token_pack(generation + 1u,
                                       LJ_GC2_TABLE_TOKEN_PENDING);
    if (la_cas64(&token->control, &control, desired, LA_ACQ_REL, LA_ACQ)) {
      ticket->control = desired;
      return LJ_GC2_TABLE_TOKEN_RESULT_OK;
    }
  }
}

/* Capture the exact generation a scanner intends to complete. The caller must
** acquire allocation lifetime and recheck ticket->control before reading the
** table body; this helper only classifies the persistent side token. */
LA_INLINE LJGC2TableTokenResult
lj_gc2_table_token_capture_pending(LJGC2TableToken *token,
                                   LJGC2TableTokenTicket *ticket)
{
  uint64_t control;
  uint8_t state;
  if (!token || !ticket)
    return LJ_GC2_TABLE_TOKEN_RESULT_INVALID;
  control = la_load64_acq(&token->control);
  state = lj_gc2_table_token_state(control);
  if (state == LJ_GC2_TABLE_TOKEN_PENDING) {
    ticket->control = control;
    return LJ_GC2_TABLE_TOKEN_RESULT_OK;
  }
  if (state == LJ_GC2_TABLE_TOKEN_NONE)
    return LJ_GC2_TABLE_TOKEN_RESULT_BUSY;
  if (state == LJ_GC2_TABLE_TOKEN_PINNED)
    return LJ_GC2_TABLE_TOKEN_RESULT_PINNED;
  return lj_gc2_table_token_pin(token, control);
}

/* Complete only the captured descriptor generation.  Unlike the provisional
** refresh-owned lifecycle below, exact completion preserves the generation:
** PENDING(D) -> NONE(D).  This permits D == MAX; only a subsequent descriptor
** publication attempt saturates and pins. */
LA_INLINE LJGC2TableTokenResult
lj_gc2_table_token_complete_exact(LJGC2TableToken *token,
                                  const LJGC2TableTokenTicket *ticket)
{
  uint64_t expected, generation, desired;
  uint8_t state;
  if (!token || !ticket ||
      lj_gc2_table_token_state(ticket->control) !=
        LJ_GC2_TABLE_TOKEN_PENDING)
    return LJ_GC2_TABLE_TOKEN_RESULT_INVALID;
  generation = lj_gc2_table_token_generation(ticket->control);
  if (generation == 0 ||
      generation > LJ_GC2_TABLE_TOKEN_MAX_GENERATION)
    return LJ_GC2_TABLE_TOKEN_RESULT_INVALID;
  expected = ticket->control;
  desired = lj_gc2_table_token_pack(generation, LJ_GC2_TABLE_TOKEN_NONE);
  if (la_cas64(&token->control, &expected, desired,
               LA_ACQ_REL, LA_ACQ))
    return LJ_GC2_TABLE_TOKEN_RESULT_OK;
  for (;;) {
    uint64_t observed_generation =
      lj_gc2_table_token_generation(expected);
    state = lj_gc2_table_token_state(expected);
    if (state == LJ_GC2_TABLE_TOKEN_PINNED)
      return LJ_GC2_TABLE_TOKEN_RESULT_PINNED;
    if (observed_generation > generation)
      return LJ_GC2_TABLE_TOKEN_RESULT_BUSY;
    if (state != LJ_GC2_TABLE_TOKEN_INVALID)
      return LJ_GC2_TABLE_TOKEN_RESULT_BUSY;
    desired = lj_gc2_table_token_pack(observed_generation,
                                       LJ_GC2_TABLE_TOKEN_PINNED);
    if (la_cas64(&token->control, &expected, desired,
                 LA_ACQ_REL, LA_ACQ))
      return LJ_GC2_TABLE_TOKEN_RESULT_PINNED;
  }
}

/* Legacy companion to table_token_refresh(). Production scanners must use
** complete_exact(), which retains the descriptor generation in NONE. */
LA_INLINE LJGC2TableTokenResult
lj_gc2_table_token_complete(LJGC2TableToken *token,
                            const LJGC2TableTokenTicket *ticket)
{
  uint64_t expected, generation, desired;
  if (!token || !ticket ||
      lj_gc2_table_token_state(ticket->control) !=
        LJ_GC2_TABLE_TOKEN_PENDING)
    return LJ_GC2_TABLE_TOKEN_RESULT_INVALID;
  generation = lj_gc2_table_token_generation(ticket->control);
  expected = ticket->control;
  if (generation == LJ_GC2_TABLE_TOKEN_MAX_GENERATION) {
    desired = lj_gc2_table_token_pack(generation,
                                       LJ_GC2_TABLE_TOKEN_PINNED);
    if (la_cas64(&token->control, &expected, desired, LA_ACQ_REL, LA_ACQ))
      return LJ_GC2_TABLE_TOKEN_RESULT_PINNED;
    if (lj_gc2_table_token_state(expected) == LJ_GC2_TABLE_TOKEN_INVALID)
      return lj_gc2_table_token_pin(token, expected);
    return lj_gc2_table_token_state(expected) == LJ_GC2_TABLE_TOKEN_PINNED ?
           LJ_GC2_TABLE_TOKEN_RESULT_PINNED :
           LJ_GC2_TABLE_TOKEN_RESULT_BUSY;
  }
  desired = lj_gc2_table_token_pack(generation + 1u,
                                     LJ_GC2_TABLE_TOKEN_NONE);
  if (la_cas64(&token->control, &expected, desired, LA_ACQ_REL, LA_ACQ))
    return LJ_GC2_TABLE_TOKEN_RESULT_OK;
  if (lj_gc2_table_token_state(expected) == LJ_GC2_TABLE_TOKEN_INVALID)
    return lj_gc2_table_token_pin(token, expected);
  return lj_gc2_table_token_state(expected) == LJ_GC2_TABLE_TOKEN_PINNED ?
         LJ_GC2_TABLE_TOKEN_RESULT_PINNED : LJ_GC2_TABLE_TOKEN_RESULT_BUSY;
}

/* ---- Exact table-enumeration universe authority ------------------- */

/*
** A physical table-token pass spans the stable TG spine, every owner HugeTab,
** and the global small-arena directory. Neither a cursor wrap nor the sticky
** requested-generation maximum proves that this universe stayed unchanged.
**
** Each completed physical membership change advances this exact sequence
** before its operation returns or destroys the old mapping. New identities
** are published with token NONE, while PENDING mechanically vetoes removal or
** transfer. Thus a publisher paused between its membership LP and this bump
** can expose only a tokenless identity; any later token publication is covered
** by the separate helpable table descriptor generation. This avoids an
** anonymous in-flight count which a paused owner could strand forever.
**
** PINNED is absorbing: mutations continue, but no later pass may manufacture
** reclamation authority after counter saturation or malformed ownership.
*/
#define LJ_GC2_TABLE_TOPOLOGY_OPEN	UINT64_C(0)
#define LJ_GC2_TABLE_TOPOLOGY_PINNED	UINT64_C(1)

typedef struct LJGC2TableTopology {
  /* lo = nonzero completed-mutation epoch; hi = OPEN or PINNED. */
  la_u128 value;
} LJGC2TableTopology;

typedef struct LJGC2TableTopologySnap {
  uint64_t epoch;
  uint8_t pinned;
  uint8_t valid;
} LJGC2TableTopologySnap;

typedef enum LJGC2TableTopologyResult {
  LJ_GC2_TABLE_TOPOLOGY_INVALID = -2,
  LJ_GC2_TABLE_TOPOLOGY_PINNED_RESULT = -1,
  LJ_GC2_TABLE_TOPOLOGY_OK = 1
} LJGC2TableTopologyResult;

typedef char lj_gc2_table_topology_size_must_be_16[
  sizeof(LJGC2TableTopology) == 16 ? 1 : -1];
typedef char lj_gc2_table_topology_align_must_be_16[
  __alignof__(LJGC2TableTopology) >= 16 ? 1 : -1];

/* Valid only before the containing universe is published. */
LA_INLINE int lj_gc2_table_topology_init_unpublished(
  LJGC2TableTopology *topology, uint64_t epoch)
{
  if (!topology || epoch == 0)
    return 0;
  topology->value.lo = epoch;
  topology->value.hi = LJ_GC2_TABLE_TOPOLOGY_OPEN;
  return 1;
}

/* Exact CX16 observation, matching the table descriptor's artifact contract. */
LA_INLINE LJGC2TableTopologySnap lj_gc2_table_topology_snapshot(
  const LJGC2TableTopology *topology)
{
  LJGC2TableTopologySnap snap;
  la_u128 exact, zero;
  if (!topology) {
    snap.epoch = 0;
    snap.pinned = 1;
    snap.valid = 0;
    return snap;
  }
  exact.lo = exact.hi = 0;
  zero = exact;
  (void)la_cas128((la_u128 *)(void *)&topology->value, &exact, zero);
  snap.epoch = exact.lo;
  snap.pinned = (uint8_t)(exact.hi == LJ_GC2_TABLE_TOPOLOGY_PINNED);
  snap.valid = (uint8_t)(exact.lo != 0 &&
    (exact.hi == LJ_GC2_TABLE_TOPOLOGY_OPEN || snap.pinned));
  return snap;
}

LA_INLINE int lj_gc2_table_topology_equal(
  const LJGC2TableTopologySnap *a, const LJGC2TableTopologySnap *b)
{
  return a && b && a->epoch == b->epoch && a->pinned == b->pinned &&
         a->valid == b->valid;
}

LA_INLINE int lj_gc2_table_topology_open(
  const LJGC2TableTopologySnap *snap)
{
  return snap && snap->valid && !snap->pinned;
}

/* Publish one completed physical membership change. The CAS loop is lock-free
** and contains no peer wait. Saturation pins instead of wrapping to zero. */
LA_INLINE LJGC2TableTopologyResult lj_gc2_table_topology_changed(
  LJGC2TableTopology *topology)
{
  for (;;) {
    LJGC2TableTopologySnap snap =
      lj_gc2_table_topology_snapshot(topology);
    la_u128 expected, desired;
    if (!topology || !snap.valid)
      return LJ_GC2_TABLE_TOPOLOGY_INVALID;
    if (snap.pinned)
      return LJ_GC2_TABLE_TOPOLOGY_PINNED_RESULT;
    expected.lo = snap.epoch;
    expected.hi = LJ_GC2_TABLE_TOPOLOGY_OPEN;
    desired = expected;
    if (snap.epoch == UINT64_MAX) {
      desired.hi = LJ_GC2_TABLE_TOPOLOGY_PINNED;
      if (la_cas128(&topology->value, &expected, desired))
        return LJ_GC2_TABLE_TOPOLOGY_PINNED_RESULT;
      continue;
    }
    desired.lo = snap.epoch + 1u;
    if (la_cas128(&topology->value, &expected, desired))
      return LJ_GC2_TABLE_TOPOLOGY_OK;
  }
}

/* ---- Per-TG helpable root-operation descriptor -------------------- */

#define LJ_GC2_ROOTDESC_STATE_BITS 2u
#define LJ_GC2_ROOTDESC_STATE_MASK \
  ((UINT64_C(1) << LJ_GC2_ROOTDESC_STATE_BITS) - 1u)
#define LJ_GC2_ROOTDESC_MAX_GENERATION \
  (UINT64_MAX >> LJ_GC2_ROOTDESC_STATE_BITS)

typedef enum LJGC2RootDescState {
  LJ_GC2_ROOTDESC_IDLE = 0,
  LJ_GC2_ROOTDESC_ACTIVE = 1,
  LJ_GC2_ROOTDESC_NO_RECLAIM = 2
} LJGC2RootDescState;

enum {
  LJ_GC2_ROOTDESC_F_OLD = 0x01u,
  LJ_GC2_ROOTDESC_F_NEW = 0x02u,
  LJ_GC2_ROOTDESC_F_RANGE0 = 0x04u,
  LJ_GC2_ROOTDESC_F_RANGE1 = 0x08u,
  LJ_GC2_ROOTDESC_F_MOVE_DOWN = 0x10u,
  LJ_GC2_ROOTDESC_F_MOVE_UP = 0x20u,
  LJ_GC2_ROOTDESC_F_AUX = 0x40u,
  /* A scalar table store uses OLD=parent, NEW=key and AUX=value. */
  LJ_GC2_ROOTDESC_F_TABLE_STORE = 0x80u,
  LJ_GC2_ROOTDESC_F_ALL = 0xffu
};

typedef struct LJGC2RootRange {
  void *lo;
  void *hi;
} LJGC2RootRange;

typedef struct LJGC2RootDesc {
  uint64_t control;  /* generation << 2 | LJGC2RootDescState. */
  uint32_t flags;
  uint32_t reserved;
  uint64_t old_root;  /* Raw TValue snapshot; valid when F_OLD is set. */
  uint64_t new_root;  /* Raw TValue snapshot; valid when F_NEW is set. */
  uint64_t aux_root;  /* Third raw TValue snapshot; valid when F_AUX is set. */
  /* Half-open TValue spans. RANGE1 requires an equal-sized RANGE0; MOVE_DOWN
  ** means helpers scan their merged overlap high-to-low, MOVE_UP low-to-high.
  */
  LJGC2RootRange range[2];
  /* Helpers publish {exact ACTIVE control, activation generation}. The
  ** certificate is never cleared or moved backwards; a later descriptor or
  ** close generation invalidates it by exact comparison. Keep helper-written
  ** coverage after owner-written payload to reduce control-word false sharing.
  */
  la_u128 coverage;
} __attribute__((aligned(16))) LJGC2RootDesc;

typedef struct LJGC2RootDescSpec {
  uint32_t flags;
  uint64_t old_root;
  uint64_t new_root;
  uint64_t aux_root;
  LJGC2RootRange range[2];
} LJGC2RootDescSpec;

typedef struct LJGC2RootDescView {
  const LJGC2RootDesc *descriptor;
  uint64_t generation;
  uint32_t flags;
  uint64_t old_root;
  uint64_t new_root;
  uint64_t aux_root;
  LJGC2RootRange range[2];
} LJGC2RootDescView;

typedef struct LJGC2RootDescCoverage {
  uint64_t descriptor_control;
  uint64_t activation_generation;
} LJGC2RootDescCoverage;

typedef struct LJGC2RootDescTicket {
  uint64_t control;
} LJGC2RootDescTicket;

typedef enum LJGC2RootDescResult {
  LJ_GC2_ROOTDESC_INVALID = -2,
  LJ_GC2_ROOTDESC_PINNED = -1,
  LJ_GC2_ROOTDESC_BUSY = 0,
  LJ_GC2_ROOTDESC_OK = 1
} LJGC2RootDescResult;

typedef enum LJGC2RootDescSnapshotResult {
  LJ_GC2_ROOTDESC_SNAPSHOT_NO_RECLAIM = -1,
  LJ_GC2_ROOTDESC_SNAPSHOT_IDLE = 0,
  LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE = 1,
  LJ_GC2_ROOTDESC_SNAPSHOT_RETRY = 2
} LJGC2RootDescSnapshotResult;

typedef char lj_gc2_rootdesc_size_must_be_96[
  sizeof(LJGC2RootDesc) == 96 ? 1 : -1];
typedef char lj_gc2_rootdesc_align_must_be_16[
  __alignof__(LJGC2RootDesc) >= 16 ? 1 : -1];
typedef char lj_gc2_rootdesc_coverage_align_must_be_16[
  (offsetof(LJGC2RootDesc, coverage) & 15u) == 0 ? 1 : -1];
typedef char lj_gc2_rootdesc_coverage_size_must_be_16[
  sizeof(LJGC2RootDescCoverage) == 16 ? 1 : -1];

LA_INLINE uint64_t lj_gc2_rootdesc_pack_control(uint64_t generation,
                                                 uint8_t state)
{
  return (generation << LJ_GC2_ROOTDESC_STATE_BITS) | (uint64_t)state;
}

LA_INLINE uint64_t lj_gc2_rootdesc_generation(uint64_t control)
{
  return control >> LJ_GC2_ROOTDESC_STATE_BITS;
}

LA_INLINE uint8_t lj_gc2_rootdesc_state(uint64_t control)
{
  return (uint8_t)(control & LJ_GC2_ROOTDESC_STATE_MASK);
}

LA_INLINE int lj_gc2_rootdesc_range_valid(const LJGC2RootRange *range)
{
  uintptr_t lo = (uintptr_t)range->lo;
  uintptr_t hi = (uintptr_t)range->hi;
  return lo < hi && ((lo | hi) & (sizeof(uint64_t) - 1u)) == 0;
}

LA_INLINE int lj_gc2_rootdesc_spec_valid(const LJGC2RootDescSpec *spec)
{
  uint32_t roots, direction;
  if (!spec || (spec->flags & ~LJ_GC2_ROOTDESC_F_ALL) != 0)
    return 0;
  roots = spec->flags & (LJ_GC2_ROOTDESC_F_OLD | LJ_GC2_ROOTDESC_F_NEW |
                         LJ_GC2_ROOTDESC_F_AUX |
                         LJ_GC2_ROOTDESC_F_RANGE0 |
                         LJ_GC2_ROOTDESC_F_RANGE1);
  direction = spec->flags & (LJ_GC2_ROOTDESC_F_MOVE_DOWN |
                             LJ_GC2_ROOTDESC_F_MOVE_UP);
  if (roots == 0 || direction ==
      (LJ_GC2_ROOTDESC_F_MOVE_DOWN | LJ_GC2_ROOTDESC_F_MOVE_UP))
    return 0;
  if ((spec->flags & (LJ_GC2_ROOTDESC_F_RANGE0 |
                      LJ_GC2_ROOTDESC_F_RANGE1)) != 0) {
    if (direction == 0)
      return 0;
  } else if (direction != 0) {
    return 0;
  }
  if ((spec->flags & LJ_GC2_ROOTDESC_F_RANGE1) &&
      !(spec->flags & LJ_GC2_ROOTDESC_F_RANGE0))
    return 0;
  if ((spec->flags & LJ_GC2_ROOTDESC_F_TABLE_STORE) &&
      (spec->flags & (LJ_GC2_ROOTDESC_F_OLD | LJ_GC2_ROOTDESC_F_NEW |
                      LJ_GC2_ROOTDESC_F_AUX)) !=
       (LJ_GC2_ROOTDESC_F_OLD | LJ_GC2_ROOTDESC_F_NEW |
        LJ_GC2_ROOTDESC_F_AUX))
    return 0;
  if ((spec->flags & LJ_GC2_ROOTDESC_F_RANGE0) &&
      !lj_gc2_rootdesc_range_valid(&spec->range[0]))
    return 0;
  if ((spec->flags & LJ_GC2_ROOTDESC_F_RANGE1) &&
      (!lj_gc2_rootdesc_range_valid(&spec->range[1]) ||
       (uintptr_t)spec->range[0].hi - (uintptr_t)spec->range[0].lo !=
       (uintptr_t)spec->range[1].hi - (uintptr_t)spec->range[1].lo))
    return 0;
  return 1;
}

/* Only valid before the containing TG is published. */
LA_INLINE int lj_gc2_rootdesc_init_unpublished(LJGC2RootDesc *desc,
                                                uint64_t generation)
{
  if (generation > LJ_GC2_ROOTDESC_MAX_GENERATION)
    return 0;
  la_store32_rlx(&desc->flags, 0);
  la_store32_rlx(&desc->reserved, 0);
  la_store64_rlx(&desc->old_root, 0);
  la_store64_rlx(&desc->new_root, 0);
  desc->coverage.lo = 0;
  desc->coverage.hi = 0;
  la_store64_rlx(&desc->aux_root, 0);
  la_storeptr_rlx(&desc->range[0].lo, NULL);
  la_storeptr_rlx(&desc->range[0].hi, NULL);
  la_storeptr_rlx(&desc->range[1].lo, NULL);
  la_storeptr_rlx(&desc->range[1].hi, NULL);
  la_store64_rlx(&desc->control,
      lj_gc2_rootdesc_pack_control(generation, LJ_GC2_ROOTDESC_IDLE));
  return 1;
}

LA_INLINE LJGC2RootDescResult
lj_gc2_rootdesc_pin(LJGC2RootDesc *desc, uint64_t control)
{
  uint64_t desired;
  for (;;) {
    uint8_t state = lj_gc2_rootdesc_state(control);
    if (state == LJ_GC2_ROOTDESC_NO_RECLAIM)
      return LJ_GC2_ROOTDESC_PINNED;
    desired = lj_gc2_rootdesc_pack_control(
        lj_gc2_rootdesc_generation(control), LJ_GC2_ROOTDESC_NO_RECLAIM);
    if (la_cas64(&desc->control, &control, desired, LA_ACQ_REL, LA_ACQ))
      return LJ_GC2_ROOTDESC_PINNED;
  }
}

LA_INLINE LJGC2RootDescResult
lj_gc2_rootdesc_try_activate(LJGC2RootDesc *desc, uint64_t idle,
                             uint64_t active)
{
  uint64_t expected = idle;
  if (la_cas64(&desc->control, &expected, active, LA_ACQ_REL, LA_ACQ))
    return LJ_GC2_ROOTDESC_OK;
  return lj_gc2_rootdesc_pin(desc, expected);
}

/*
** Owner-only begin.  Payload stores precede the mandatory locked CAS, which
** supplies the x86 StoreLoad edge needed before the owner samples the gate
** without allowing a concurrent NO_RECLAIM pin to be overwritten.
*/
LA_INLINE LJGC2RootDescResult
lj_gc2_rootdesc_publish(LJGC2RootDesc *desc,
                        const LJGC2RootDescSpec *spec,
                        LJGC2RootDescTicket *ticket)
{
  uint64_t control, active, generation;
  LJGC2RootDescResult activate;
  if (!desc || !ticket || !spec)
    return LJ_GC2_ROOTDESC_INVALID;
  control = la_load64_acq(&desc->control);
  if (!lj_gc2_rootdesc_spec_valid(spec))
    return lj_gc2_rootdesc_pin(desc, control);
  if (lj_gc2_rootdesc_state(control) == LJ_GC2_ROOTDESC_NO_RECLAIM)
    return LJ_GC2_ROOTDESC_PINNED;
  if (lj_gc2_rootdesc_state(control) != LJ_GC2_ROOTDESC_IDLE)
    return lj_gc2_rootdesc_pin(desc, control);
  generation = lj_gc2_rootdesc_generation(control);
  if (generation == LJ_GC2_ROOTDESC_MAX_GENERATION)
    return lj_gc2_rootdesc_pin(desc, control);

  la_store32_rlx(&desc->flags, spec->flags);
  la_store32_rlx(&desc->reserved, 0);
  la_store64_rlx(&desc->old_root, spec->old_root);
  la_store64_rlx(&desc->new_root, spec->new_root);
  la_store64_rlx(&desc->aux_root, spec->aux_root);
  la_storeptr_rlx(&desc->range[0].lo,
                  spec->flags & LJ_GC2_ROOTDESC_F_RANGE0 ?
                    spec->range[0].lo : NULL);
  la_storeptr_rlx(&desc->range[0].hi,
                  spec->flags & LJ_GC2_ROOTDESC_F_RANGE0 ?
                    spec->range[0].hi : NULL);
  la_storeptr_rlx(&desc->range[1].lo,
                  spec->flags & LJ_GC2_ROOTDESC_F_RANGE1 ?
                    spec->range[1].lo : NULL);
  la_storeptr_rlx(&desc->range[1].hi,
                  spec->flags & LJ_GC2_ROOTDESC_F_RANGE1 ?
                    spec->range[1].hi : NULL);
  active = lj_gc2_rootdesc_pack_control(generation + 1u,
                                        LJ_GC2_ROOTDESC_ACTIVE);
  activate = lj_gc2_rootdesc_try_activate(desc, control, active);
  if (activate != LJ_GC2_ROOTDESC_OK)
    return activate;
  ticket->control = active;
  return LJ_GC2_ROOTDESC_OK;
}

/*
** As with the activation snapshot, control/payload/control collection is an
** explicit x86-64 GCC/Clang/MinGW artifact contract, not a portable C11
** seqlock. NO_RECLAIM obligates the caller to pin global activation too.
*/
LA_INLINE LJGC2RootDescSnapshotResult
lj_gc2_rootdesc_snapshot(const LJGC2RootDesc *desc, LJGC2RootDescView *view)
{
  LJGC2RootDescSpec spec;
  uint64_t before, after;
  uint8_t state;
  before = la_load64_acq(&desc->control);
  state = lj_gc2_rootdesc_state(before);
  if (state == LJ_GC2_ROOTDESC_IDLE) {
    if (view) {
      view->descriptor = desc;
      view->generation = lj_gc2_rootdesc_generation(before);
    }
    return LJ_GC2_ROOTDESC_SNAPSHOT_IDLE;
  }
  if (state != LJ_GC2_ROOTDESC_ACTIVE)
    return LJ_GC2_ROOTDESC_SNAPSHOT_NO_RECLAIM;

  spec.flags = la_load32_rlx(&desc->flags);
  spec.old_root = la_load64_rlx(&desc->old_root);
  spec.new_root = la_load64_rlx(&desc->new_root);
  spec.aux_root = la_load64_rlx(&desc->aux_root);
  spec.range[0].lo = la_loadptr_rlx((void *const *)&desc->range[0].lo);
  spec.range[0].hi = la_loadptr_rlx((void *const *)&desc->range[0].hi);
  spec.range[1].lo = la_loadptr_rlx((void *const *)&desc->range[1].lo);
  spec.range[1].hi = la_loadptr_rlx((void *const *)&desc->range[1].hi);
  if (la_load32_rlx(&desc->reserved) != 0)
    return LJ_GC2_ROOTDESC_SNAPSHOT_NO_RECLAIM;
  after = la_load64_acq(&desc->control);
  if (before != after)
    return LJ_GC2_ROOTDESC_SNAPSHOT_RETRY;
  if (!lj_gc2_rootdesc_spec_valid(&spec))
    return LJ_GC2_ROOTDESC_SNAPSHOT_NO_RECLAIM;
  if (view) {
    view->descriptor = desc;
    view->generation = lj_gc2_rootdesc_generation(before);
    view->flags = spec.flags;
    view->old_root = spec.old_root;
    view->new_root = spec.new_root;
    view->aux_root = spec.aux_root;
    view->range[0] = spec.range[0];
    view->range[1] = spec.range[1];
  }
  return LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE;
}

/* Exact acquire snapshot of the helper-written CX16 coverage word. The
** compare-with-zero operation is a no-op on success and returns the exact
** current pair on failure; this avoids accepting a mixed pair when multiple
** helpers cover successive descriptor generations in one close generation. */
LA_INLINE LJGC2RootDescCoverage
lj_gc2_rootdesc_coverage_snapshot(LJGC2RootDesc *desc)
{
  LJGC2RootDescCoverage coverage;
  la_u128 expected, zero;
  expected.lo = zero.lo = 0;
  expected.hi = zero.hi = 0;
  (void)la_cas128(&desc->coverage, &expected, zero);
  coverage.descriptor_control = expected.lo;
  coverage.activation_generation = expected.hi;
  return coverage;
}

LA_INLINE int
lj_gc2_rootdesc_coverage_valid(const LJGC2RootDescCoverage *coverage)
{
  uint64_t generation;
  if (!coverage)
    return 0;
  if (coverage->descriptor_control == 0 &&
      coverage->activation_generation == 0)
    return 1;
  generation = lj_gc2_rootdesc_generation(coverage->descriptor_control);
  return coverage->activation_generation != 0 &&
         coverage->activation_generation <= LJ_GC2_ACT_MAX_GENERATION &&
         generation != 0 && generation <= LJ_GC2_ROOTDESC_MAX_GENERATION &&
         lj_gc2_rootdesc_state(coverage->descriptor_control) ==
           LJ_GC2_ROOTDESC_ACTIVE;
}

LA_INLINE int
lj_gc2_rootdesc_closing_snapshot_valid(const LJGC2ActivationSnap *closing)
{
  return closing &&
         lj_gc2_activation_value_valid(closing->mark_epoch,
                                       closing->generation, closing->state,
                                       closing->gate) &&
         closing->state != LJ_GC2_ACT_NO_RECLAIM &&
         closing->gate == LJ_GC2_ROOT_GATE_CLOSING;
}

/* Advance a helper certificate from one exact CX16 observation. A failed CAS
** replaces |expected| with the winning pair, which is validated on every loop
** iteration before it can be compared or overwritten. Keeping this primitive
** separate also lets deterministic tests inject a winner between snapshot and
** CAS without adding a production pause hook. */
LA_INLINE LJGC2RootDescResult
lj_gc2_rootdesc_coverage_advance(LJGC2RootDesc *desc, uint64_t control,
                                 uint64_t activation_generation,
                                 la_u128 expected)
{
  la_u128 desired;
  if (!desc || activation_generation == 0 ||
      activation_generation > LJ_GC2_ACT_MAX_GENERATION ||
      lj_gc2_rootdesc_state(control) != LJ_GC2_ROOTDESC_ACTIVE ||
      lj_gc2_rootdesc_generation(control) == 0 ||
      lj_gc2_rootdesc_generation(control) > LJ_GC2_ROOTDESC_MAX_GENERATION)
    return LJ_GC2_ROOTDESC_INVALID;
  desired.lo = control;
  desired.hi = activation_generation;
  for (;;) {
    LJGC2RootDescCoverage coverage;
    coverage.descriptor_control = expected.lo;
    coverage.activation_generation = expected.hi;
    if (!lj_gc2_rootdesc_coverage_valid(&coverage))
      return lj_gc2_rootdesc_pin(desc, control);
    if (expected.hi > desired.hi ||
        (expected.hi == desired.hi && expected.lo >= desired.lo)) {
      return expected.lo == desired.lo && expected.hi == desired.hi ?
             LJ_GC2_ROOTDESC_OK : LJ_GC2_ROOTDESC_BUSY;
    }
    if (la_cas128(&desc->coverage, &expected, desired))
      return LJ_GC2_ROOTDESC_OK;
  }
}

/*
** Publish helper coverage only after conservatively tracing a stable ACTIVE
** view captured under the supplied CLOSING snapshot. Coverage is monotonic by
** activation generation and then exact descriptor control, so a delayed helper
** cannot overwrite a newer close certificate. Exact descriptor and activation
** rechecks make any post-trace race a retry, never a false certificate.
*/
LA_INLINE LJGC2RootDescResult
lj_gc2_rootdesc_cover_after_trace(LJGC2RootDesc *desc,
                                  const LJGC2RootDescView *view,
                                  const LJGC2Activation *activation,
                                  const LJGC2ActivationSnap *closing)
{
  uint64_t control;
  la_u128 expected;
  LJGC2ActivationSnap current;
  LJGC2RootDescResult advance;
  if (!desc || !view || view->descriptor != desc || !activation ||
      !lj_gc2_rootdesc_closing_snapshot_valid(closing) ||
      view->generation == 0 ||
      view->generation > LJ_GC2_ROOTDESC_MAX_GENERATION)
    return LJ_GC2_ROOTDESC_INVALID;
  control = lj_gc2_rootdesc_pack_control(view->generation,
                                         LJ_GC2_ROOTDESC_ACTIVE);
  {
    uint64_t observed = la_load64_acq(&desc->control);
    if (observed != control)
      return lj_gc2_rootdesc_state(observed) == LJ_GC2_ROOTDESC_NO_RECLAIM ?
             LJ_GC2_ROOTDESC_PINNED : LJ_GC2_ROOTDESC_BUSY;
  }
  current = lj_gc2_activation_snapshot(activation);
  if (!lj_gc2_activation_equal(&current, closing))
    return current.state == LJ_GC2_ACT_NO_RECLAIM ?
           LJ_GC2_ROOTDESC_PINNED : LJ_GC2_ROOTDESC_BUSY;

  {
    LJGC2RootDescCoverage coverage =
      lj_gc2_rootdesc_coverage_snapshot(desc);
    expected.lo = coverage.descriptor_control;
    expected.hi = coverage.activation_generation;
  }
  advance = lj_gc2_rootdesc_coverage_advance(
    desc, control, closing->generation, expected);
  if (advance != LJ_GC2_ROOTDESC_OK)
    return advance;
  {
    uint64_t observed = la_load64_acq(&desc->control);
    if (observed != control)
      return lj_gc2_rootdesc_state(observed) == LJ_GC2_ROOTDESC_NO_RECLAIM ?
             LJ_GC2_ROOTDESC_PINNED : LJ_GC2_ROOTDESC_BUSY;
  }
  current = lj_gc2_activation_snapshot(activation);
  if (!lj_gc2_activation_equal(&current, closing))
    return current.state == LJ_GC2_ACT_NO_RECLAIM ?
           LJ_GC2_ROOTDESC_PINNED : LJ_GC2_ROOTDESC_BUSY;
  return LJ_GC2_ROOTDESC_OK;
}

/*
** A closer may accept IDLE directly, or ACTIVE only when a helper certificate
** names this exact descriptor and this exact CLOSING activation generation.
** It still must use the supplied CLOSING snapshot for its final COMMIT CAS;
** a publisher which appears after this observation changes that CAS to LOST.
*/
LA_INLINE LJGC2RootDescResult
lj_gc2_rootdesc_covered(LJGC2RootDesc *desc,
                        const LJGC2Activation *activation,
                        const LJGC2ActivationSnap *closing)
{
  LJGC2RootDescView view;
  LJGC2RootDescCoverage coverage;
  LJGC2ActivationSnap current;
  LJGC2RootDescSnapshotResult result;
  if (!desc || !activation ||
      !lj_gc2_rootdesc_closing_snapshot_valid(closing))
    return LJ_GC2_ROOTDESC_INVALID;
  current = lj_gc2_activation_snapshot(activation);
  if (!lj_gc2_activation_equal(&current, closing))
    return current.state == LJ_GC2_ACT_NO_RECLAIM ?
           LJ_GC2_ROOTDESC_PINNED : LJ_GC2_ROOTDESC_BUSY;
  result = lj_gc2_rootdesc_snapshot(desc, &view);
  if (result == LJ_GC2_ROOTDESC_SNAPSHOT_NO_RECLAIM)
    return LJ_GC2_ROOTDESC_PINNED;
  if (result == LJ_GC2_ROOTDESC_SNAPSHOT_RETRY)
    return LJ_GC2_ROOTDESC_BUSY;
  if (result == LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE) {
    coverage = lj_gc2_rootdesc_coverage_snapshot(desc);
    if (!lj_gc2_rootdesc_coverage_valid(&coverage))
      return lj_gc2_rootdesc_pin(desc,
        lj_gc2_rootdesc_pack_control(view.generation,
                                     LJ_GC2_ROOTDESC_ACTIVE));
    if (coverage.descriptor_control !=
        lj_gc2_rootdesc_pack_control(view.generation,
                                     LJ_GC2_ROOTDESC_ACTIVE) ||
        coverage.activation_generation != closing->generation)
      return LJ_GC2_ROOTDESC_BUSY;
    {
      uint64_t observed = la_load64_acq(&desc->control);
      if (observed != lj_gc2_rootdesc_pack_control(
                         view.generation, LJ_GC2_ROOTDESC_ACTIVE))
        return lj_gc2_rootdesc_state(observed) ==
               LJ_GC2_ROOTDESC_NO_RECLAIM ? LJ_GC2_ROOTDESC_PINNED :
                                            LJ_GC2_ROOTDESC_BUSY;
    }
  }
  current = lj_gc2_activation_snapshot(activation);
  return lj_gc2_activation_equal(&current, closing) ?
         LJ_GC2_ROOTDESC_OK :
         (current.state == LJ_GC2_ACT_NO_RECLAIM ?
          LJ_GC2_ROOTDESC_PINNED : LJ_GC2_ROOTDESC_BUSY);
}

/* Owner-only completion; exact CAS cannot erase a sticky NO_RECLAIM. */
LA_INLINE LJGC2RootDescResult
lj_gc2_rootdesc_finish(LJGC2RootDesc *desc,
                       const LJGC2RootDescTicket *ticket)
{
  uint64_t expected, idle;
  if (!desc || !ticket ||
      lj_gc2_rootdesc_state(ticket->control) != LJ_GC2_ROOTDESC_ACTIVE)
    return LJ_GC2_ROOTDESC_INVALID;
  expected = ticket->control;
  idle = lj_gc2_rootdesc_pack_control(
      lj_gc2_rootdesc_generation(expected), LJ_GC2_ROOTDESC_IDLE);
  if (la_cas64(&desc->control, &expected, idle, LA_REL, LA_RLX))
    return LJ_GC2_ROOTDESC_OK;
  return lj_gc2_rootdesc_state(expected) == LJ_GC2_ROOTDESC_NO_RECLAIM ?
         LJ_GC2_ROOTDESC_PINNED : LJ_GC2_ROOTDESC_BUSY;
}

#endif /* _LJ_GC2TOKEN_H */
