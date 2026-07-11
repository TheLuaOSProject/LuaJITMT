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
  LJ_GC2_ROOTDESC_F_ALL = 0x3fu
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
  /* Half-open TValue spans. RANGE1 requires an equal-sized RANGE0; MOVE_DOWN
  ** means helpers scan their merged overlap high-to-low, MOVE_UP low-to-high.
  */
  LJGC2RootRange range[2];
} __attribute__((aligned(16))) LJGC2RootDesc;

typedef struct LJGC2RootDescSpec {
  uint32_t flags;
  uint64_t old_root;
  uint64_t new_root;
  LJGC2RootRange range[2];
} LJGC2RootDescSpec;

typedef struct LJGC2RootDescView {
  uint64_t generation;
  uint32_t flags;
  uint64_t old_root;
  uint64_t new_root;
  LJGC2RootRange range[2];
} LJGC2RootDescView;

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

typedef char lj_gc2_rootdesc_size_must_be_64[
  sizeof(LJGC2RootDesc) == 64 ? 1 : -1];
typedef char lj_gc2_rootdesc_align_must_be_16[
  __alignof__(LJGC2RootDesc) >= 16 ? 1 : -1];

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
    if (view)
      view->generation = lj_gc2_rootdesc_generation(before);
    return LJ_GC2_ROOTDESC_SNAPSHOT_IDLE;
  }
  if (state != LJ_GC2_ROOTDESC_ACTIVE)
    return LJ_GC2_ROOTDESC_SNAPSHOT_NO_RECLAIM;

  spec.flags = la_load32_rlx(&desc->flags);
  spec.old_root = la_load64_rlx(&desc->old_root);
  spec.new_root = la_load64_rlx(&desc->new_root);
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
    view->generation = lj_gc2_rootdesc_generation(before);
    view->flags = spec.flags;
    view->old_root = spec.old_root;
    view->new_root = spec.new_root;
    view->range[0] = spec.range[0];
    view->range[1] = spec.range[1];
  }
  return LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE;
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
