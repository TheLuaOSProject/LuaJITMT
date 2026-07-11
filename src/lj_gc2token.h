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
#define LJ_GC2_ACT_STATE_MASK ((UINT64_C(1) << LJ_GC2_ACT_STATE_BITS) - 1u)
#define LJ_GC2_ACT_MAX_GENERATION (UINT64_MAX >> LJ_GC2_ACT_STATE_BITS)

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

typedef struct LJGC2Activation {
  la_u128 value;  /* lo = mark_epoch, hi = generation << 3 | state. */
} LJGC2Activation;

typedef struct LJGC2ActivationSnap {
  uint64_t mark_epoch;
  uint64_t generation;
  uint8_t state;
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
                                              uint8_t state)
{
  return (generation << LJ_GC2_ACT_STATE_BITS) | (uint64_t)state;
}

LA_INLINE int lj_gc2_activation_components_valid(uint64_t generation,
                                                  uint8_t state)
{
  return generation <= LJ_GC2_ACT_MAX_GENERATION &&
         state <= LJ_GC2_ACT_NO_RECLAIM;
}

LA_INLINE int lj_gc2_activation_value_valid(uint64_t mark_epoch,
                                             uint64_t generation,
                                             uint8_t state)
{
  return lj_gc2_activation_components_valid(generation, state) &&
         (mark_epoch != 0 || state == LJ_GC2_ACT_IDLE ||
          state == LJ_GC2_ACT_NO_RECLAIM);
}

/* Only valid before the containing global state is published. */
LA_INLINE int lj_gc2_activation_init_unpublished(LJGC2Activation *token,
                                                  uint64_t mark_epoch,
                                                  uint64_t generation,
                                                  uint8_t state)
{
  if (!lj_gc2_activation_value_valid(mark_epoch, generation, state))
    return 0;
  token->value.lo = mark_epoch;
  token->value.hi = lj_gc2_activation_pack_hi(generation, state);
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
  snap.generation = hi >> LJ_GC2_ACT_STATE_BITS;
  snap.state = (uint8_t)(hi & LJ_GC2_ACT_STATE_MASK);
  return snap;
}

LA_INLINE int lj_gc2_activation_equal(const LJGC2ActivationSnap *a,
                                       const LJGC2ActivationSnap *b)
{
  return a->mark_epoch == b->mark_epoch &&
         a->generation == b->generation && a->state == b->state;
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
lj_gc2_activation_try_transition(LJGC2Activation *token,
                                 const LJGC2ActivationSnap *expected_snap,
                                 uint64_t next_mark_epoch, uint8_t next_state,
                                 LJGC2ActivationSnap *observed)
{
  la_u128 expected, desired;
  uint64_t next_generation;
  if (!lj_gc2_activation_value_valid(expected_snap->mark_epoch,
                                      expected_snap->generation,
                                      expected_snap->state) ||
      !lj_gc2_activation_value_valid(next_mark_epoch,
                                     expected_snap->generation, next_state) ||
      !lj_gc2_activation_edge_valid(expected_snap->state, next_state) ||
      !lj_gc2_activation_epoch_edge_valid(expected_snap->mark_epoch,
                                           expected_snap->state,
                                           next_mark_epoch, next_state))
    return LJ_GC2_TRANSITION_INVALID;
  if (expected_snap->state == LJ_GC2_ACT_NO_RECLAIM) {
    if (next_state != LJ_GC2_ACT_NO_RECLAIM ||
        next_mark_epoch != expected_snap->mark_epoch)
      return LJ_GC2_TRANSITION_INVALID;
    if (observed)
      *observed = *expected_snap;
    return LJ_GC2_TRANSITION_PINNED;
  }
  if (expected_snap->generation == LJ_GC2_ACT_MAX_GENERATION) {
    /* No generation remains for the requested edge.  Exact-CAS the current
    ** authority into absorbing NO_RECLAIM without changing its mark epoch. */
    expected.lo = expected_snap->mark_epoch;
    expected.hi = lj_gc2_activation_pack_hi(expected_snap->generation,
                                             expected_snap->state);
    desired.lo = expected_snap->mark_epoch;
    desired.hi = lj_gc2_activation_pack_hi(expected_snap->generation,
                                            LJ_GC2_ACT_NO_RECLAIM);
    if (la_cas128(&token->value, &expected, desired)) {
      if (observed) {
        observed->mark_epoch = expected_snap->mark_epoch;
        observed->generation = expected_snap->generation;
        observed->state = LJ_GC2_ACT_NO_RECLAIM;
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
                                           expected_snap->state);
  desired.lo = next_mark_epoch;
  desired.hi = lj_gc2_activation_pack_hi(next_generation, next_state);
  if (la_cas128(&token->value, &expected, desired)) {
    if (observed) {
      observed->mark_epoch = next_mark_epoch;
      observed->generation = next_generation;
      observed->state = next_state;
    }
    return LJ_GC2_TRANSITION_OK;
  }
  if (observed)
    *observed = lj_gc2_activation_snapshot(token);
  return LJ_GC2_TRANSITION_LOST;
}

#endif /* _LJ_GC2TOKEN_H */
