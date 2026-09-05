/*
** lj_universe.h - dormant exact universe admission primitive.
**
** A process-stable slot is named by {slot, incarnation}.  One CX16 word
** atomically combines the complete universe state and its publication
** transaction count.  The body publication and monotonically increasing
** publication-ticket fields are protected by that exact authority.
**
** This module is intentionally not integrated into lua_newstate, API entry,
** attach/detach, or lua_close yet.  Its slots must currently be allocated by
** a safe control-plane caller and remain address-stable for their process
** lifetime.  The bounded process slab which will supply them is a later
** integration slice.
*/
#ifndef _LJ_UNIVERSE_H
#define _LJ_UNIVERSE_H

#include "lj_atomic.h"

#if !defined(__x86_64__)
#error "Universe admission tokens currently require the x86-64 CX16 contract"
#endif

#define LJ_UNIVERSE_STATE_BITS 4u
#define LJ_UNIVERSE_STATE_MASK \
  ((UINT64_C(1) << LJ_UNIVERSE_STATE_BITS) - 1u)
#define LJ_UNIVERSE_MAX_TRANSACTIONS \
  (UINT64_MAX >> LJ_UNIVERSE_STATE_BITS)
#define LJ_UNIVERSE_INCARNATION_NONE UINT64_C(0)
#define LJ_UNIVERSE_FIRST_PUBLICATION UINT64_C(1)
/* next_publication == UINT64_MAX is the sticky exhausted sentinel. */
#define LJ_UNIVERSE_LAST_PUBLICATION (UINT64_MAX - UINT64_C(1))

typedef enum LJUniverseState {
  LJ_UNIVERSE_EMPTY = 0,
  LJ_UNIVERSE_BUILDING = 1,
  LJ_UNIVERSE_OPEN = 2,
  LJ_UNIVERSE_CLOSING = 3,
  LJ_UNIVERSE_FINALIZING = 4,
  LJ_UNIVERSE_FINAL_DRAIN = 5,
  LJ_UNIVERSE_SEALED = 6,
  LJ_UNIVERSE_POISONED = 7,
  LJ_UNIVERSE_EXHAUSTED = 8
} LJUniverseState;

typedef enum LJUniverseResult {
  LJ_UNIVERSE_CORRUPT = -10,
  LJ_UNIVERSE_TICKET_EXHAUSTED = -9,
  LJ_UNIVERSE_EXHAUSTED_RESULT = -8,
  LJ_UNIVERSE_SATURATED = -7,
  LJ_UNIVERSE_INVALID = -6,
  LJ_UNIVERSE_STALE = -5,
  LJ_UNIVERSE_DENIED = -4,
  LJ_UNIVERSE_BUSY = -3,
  LJ_UNIVERSE_POISONED_RESULT = -2,
  LJ_UNIVERSE_LOST = 0,
  LJ_UNIVERSE_OK = 1
} LJUniverseResult;

typedef enum LJUniverseTxnKind {
  LJ_UNIVERSE_TXN_NONE = 0,
  LJ_UNIVERSE_TXN_EXTERNAL = 1,
  LJ_UNIVERSE_TXN_FINALIZER = 2
} LJUniverseTxnKind;

typedef struct LJUniverseToken {
  /* lo = non-wrapping incarnation; hi = transaction_count << 4 | state. */
  la_u128 value;
} LJUniverseToken;

typedef struct LJUniverseSlot LJUniverseSlot;

struct LJUniverseSlot {
  LJUniverseToken token;
  /* lo = body pointer; hi = exact incarnation owning the pointer. */
  la_u128 body_value;
  /* The next non-zero ticket. UINT64_MAX is exhausted and never increments. */
  uint64_t next_publication;
  /* lo = frozen next-ticket epoch (zero if absent), hi = exact incarnation. */
  la_u128 external_final_publication;
  la_u128 terminal_final_publication;
  /* Immutable after a process registry release-publishes this stable slot. */
  LJUniverseSlot *next_all;
};

typedef struct LJUniverseKey {
  LJUniverseSlot *slot;
  uint64_t incarnation;
} LJUniverseKey;

/*
** Linear private-construction authority.  It is created only by a successful
** EMPTY->BUILDING claim and consumed by exactly one publish or abort winner.
** An LJUniverseKey is not externally available until publish succeeds.
*/
typedef struct LJUniverseBuild {
  LJUniverseKey key;
  uint8_t active;
} LJUniverseBuild;

typedef struct LJUniverseSnap {
  uint64_t incarnation;
  uint64_t transaction_count;
  uint8_t state;
} LJUniverseSnap;

typedef struct LJUniverseBodySnap {
  void *body;
  uint64_t incarnation;
} LJUniverseBodySnap;

typedef struct LJUniverseEpochSnap {
  uint64_t epoch;
  uint64_t incarnation;
} LJUniverseEpochSnap;

/* Linear handle: an active value must not be copied and is released once. */
typedef struct LJUniverseTxn {
  LJUniverseKey key;
  void *body;
  uint64_t publication_ticket;
  uint8_t kind;
  uint8_t active;
} LJUniverseTxn;

/*
** Linear winner-only close authority.  Only LJ_UNIVERSE_OK from
** lj_universe_try_close() creates an active handle.  LOST, DENIED, STALE,
** POISONED, and all validation failures leave the output inactive.  A close
** loser may therefore never infer authority from the observed CLOSING state.
*/
typedef struct LJUniverseClose {
  LJUniverseKey key;
  void *body;
  uint64_t external_final_publication;
  uint64_t terminal_final_publication;
  uint8_t active;
} LJUniverseClose;

typedef char lj_universe_token_size_must_be_16[
  sizeof(LJUniverseToken) == 16 ? 1 : -1];
typedef char lj_universe_token_align_must_be_16[
  __alignof__(LJUniverseToken) >= 16 ? 1 : -1];
typedef char lj_universe_token_must_be_first[
  offsetof(LJUniverseSlot, token) == 0 ? 1 : -1];
typedef char lj_universe_slot_align_must_cover_token[
  __alignof__(LJUniverseSlot) >= __alignof__(LJUniverseToken) ? 1 : -1];
typedef char lj_universe_body_must_be_16_aligned[
  offsetof(LJUniverseSlot, body_value) % 16u == 0 ? 1 : -1];
typedef char lj_universe_external_epoch_must_be_16_aligned[
  offsetof(LJUniverseSlot, external_final_publication) % 16u == 0 ? 1 : -1];
typedef char lj_universe_terminal_epoch_must_be_16_aligned[
  offsetof(LJUniverseSlot, terminal_final_publication) % 16u == 0 ? 1 : -1];
typedef char lj_universe_pointer_must_fit_body_word[
  sizeof(void *) <= sizeof(uint64_t) ? 1 : -1];

LA_INLINE uint64_t lj_universe_pack_hi(uint64_t transaction_count,
                                       uint8_t state)
{
  return (transaction_count << LJ_UNIVERSE_STATE_BITS) | (uint64_t)state;
}

LA_INLINE uint64_t lj_universe_transaction_count(uint64_t hi)
{
  return hi >> LJ_UNIVERSE_STATE_BITS;
}

LA_INLINE uint8_t lj_universe_state(uint64_t hi)
{
  return (uint8_t)(hi & LJ_UNIVERSE_STATE_MASK);
}

LA_INLINE LJUniverseSnap lj_universe_snap_from_words(uint64_t incarnation,
                                                      uint64_t hi)
{
  LJUniverseSnap snap;
  snap.incarnation = incarnation;
  snap.transaction_count = lj_universe_transaction_count(hi);
  snap.state = lj_universe_state(hi);
  return snap;
}

/*
** Exact lock-free snapshot.  A hi/lo/hi subload is insufficient here because
** state/count hi values intentionally recur after a complete slot lifecycle:
** it could return a historical incarnation paired with that recurring hi.
** cmpxchg16b with an all-zero comparison linearizes an exact read.  It writes
** nothing for every claimed slot; on the canonical all-zero initial slot it
** writes the same all-zero value.  la_cas128 supplies ACQ_REL/ACQUIRE ordering.
*/
LA_INLINE LJUniverseSnap lj_universe_snapshot(const LJUniverseSlot *slot)
{
  la_u128 exact, compare_zero;
  LJUniverseSnap empty;
  if (!slot) {
    empty.incarnation = 0;
    empty.transaction_count = 0;
    empty.state = LJ_UNIVERSE_EMPTY;
    return empty;
  }
  exact.lo = 0;
  exact.hi = 0;
  compare_zero = exact;
  (void)la_cas128((la_u128 *)(void *)&slot->token.value, &exact,
                  compare_zero);
  return lj_universe_snap_from_words(exact.lo, exact.hi);
}

LA_INLINE LJUniverseBodySnap
lj_universe_body_snap_from_words(uint64_t pointer, uint64_t incarnation)
{
  LJUniverseBodySnap snap;
  snap.body = (void *)(uintptr_t)pointer;
  snap.incarnation = incarnation;
  return snap;
}

/* Body incarnation changes on reuse, so hi/lo/hi rejects a split pair. */
LA_INLINE LJUniverseBodySnap
lj_universe_body_snapshot(const LJUniverseSlot *slot)
{
  uint64_t incarnation, pointer, again;
  if (!slot)
    return lj_universe_body_snap_from_words(0, 0);
  do {
    incarnation = la_load64_acq(&slot->body_value.hi);
    pointer = la_load64_acq(&slot->body_value.lo);
    again = la_load64_acq(&slot->body_value.hi);
  } while (incarnation != again);
  return lj_universe_body_snap_from_words(pointer, incarnation);
}

/* Exact CX16 snapshot: epoch zero recurs when one incarnation is recycled. */
LA_INLINE LJUniverseEpochSnap
lj_universe_epoch_snapshot(const la_u128 *field)
{
  la_u128 exact, compare_zero;
  LJUniverseEpochSnap snap;
  if (!field) {
    snap.epoch = 0;
    snap.incarnation = 0;
    return snap;
  }
  exact.lo = 0;
  exact.hi = 0;
  compare_zero = exact;
  (void)la_cas128((la_u128 *)(void *)field, &exact, compare_zero);
  snap.epoch = exact.lo;
  snap.incarnation = exact.hi;
  return snap;
}

LA_INLINE LJUniverseEpochSnap
lj_universe_external_epoch_snapshot(const LJUniverseSlot *slot)
{
  return slot ? lj_universe_epoch_snapshot(
    &slot->external_final_publication) : lj_universe_epoch_snapshot(NULL);
}

LA_INLINE LJUniverseEpochSnap
lj_universe_terminal_epoch_snapshot(const LJUniverseSlot *slot)
{
  return slot ? lj_universe_epoch_snapshot(
    &slot->terminal_final_publication) : lj_universe_epoch_snapshot(NULL);
}

LA_INLINE int lj_universe_snap_equal(const LJUniverseSnap *a,
                                     const LJUniverseSnap *b)
{
  return a && b && a->incarnation == b->incarnation &&
         a->transaction_count == b->transaction_count &&
         a->state == b->state;
}

LA_INLINE int lj_universe_body_snap_equal(const LJUniverseBodySnap *a,
                                          const LJUniverseBodySnap *b)
{
  return a && b && a->body == b->body &&
         a->incarnation == b->incarnation;
}

LA_INLINE int lj_universe_key_equal(const LJUniverseKey *a,
                                    const LJUniverseKey *b)
{
  return a && b && a->slot == b->slot &&
         a->incarnation == b->incarnation;
}

LA_INLINE int lj_universe_key_valid(const LJUniverseKey *key)
{
  return key && key->slot &&
         ((uintptr_t)key->slot & (uintptr_t)15u) == 0 &&
         key->incarnation != LJ_UNIVERSE_INCARNATION_NONE;
}

int lj_universe_components_valid(uint64_t incarnation,
                                 uint64_t transaction_count,
                                 uint8_t state);

/* Valid only before the process registry publishes the stable slot. */
int lj_universe_slot_init_unpublished(LJUniverseSlot *slot,
                                      uint64_t empty_incarnation,
                                      LJUniverseSlot *next_all);

/* Claim EMPTY under a new incarnation and return linear BUILDING authority. */
LJUniverseResult lj_universe_try_claim(LJUniverseSlot *slot,
                                       LJUniverseBuild *build,
                                       LJUniverseSnap *observed);

/* Publish exact {body, incarnation}; success consumes build and returns key. */
LJUniverseResult lj_universe_try_publish(LJUniverseBuild *build, void *body,
                                         LJUniverseKey *key,
                                         LJUniverseSnap *observed);

/*
** Consume a failed private construction. The exact body-decision CAS advances
** the null-body incarnation before BUILDING->EMPTY, so a publish/abort race
** has one winner and the losing copied handle gains no recycle authority.
*/
LJUniverseResult lj_universe_try_abort_build(LJUniverseBuild *build,
                                             LJUniverseSnap *observed);

/* One OPEN transaction; success returns an active linear transaction handle. */
LJUniverseResult lj_universe_try_enter(const LJUniverseKey *key,
                                       LJUniverseTxn *txn,
                                       LJUniverseSnap *observed);

/* Privileged transaction; the active winner-only close handle is mandatory. */
LJUniverseResult lj_universe_try_enter_finalizer(
  const LJUniverseClose *close, LJUniverseTxn *txn,
  LJUniverseSnap *observed);

/* Exact decrement. POISONED preserves its absorbing state while draining. */
LJUniverseResult lj_universe_txn_release(LJUniverseTxn *txn,
                                         LJUniverseSnap *observed);

/* OPEN->CLOSING close LP. Only OK grants active close-owner authority. */
LJUniverseResult lj_universe_try_close(const LJUniverseKey *key,
                                       LJUniverseClose *close,
                                       LJUniverseSnap *observed);

/* Require CLOSING/0, freeze the external epoch, then enter FINALIZING. */
LJUniverseResult lj_universe_close_freeze_external(
  LJUniverseClose *close, uint64_t *epoch, LJUniverseSnap *observed);

/* FINALIZING/N->FINAL_DRAIN/N closes privileged finalizer admission. */
LJUniverseResult lj_universe_close_begin_final_drain(
  LJUniverseClose *close, LJUniverseSnap *observed);

/* Require FINAL_DRAIN/0, freeze the terminal epoch, then publish SEALED. */
LJUniverseResult lj_universe_close_seal(LJUniverseClose *close,
                                        uint64_t *epoch,
                                        LJUniverseSnap *observed);

/*
** Terminal-proof-only operation.  The caller must already have destroyed the
** body under sole close authority.  This clears its exact publication and
** recycles SEALED/N to EMPTY/N+1.  For N == UINT64_MAX-1 this publishes
** EMPTY/UINT64_MAX; the subsequent claim exact-transitions it to EXHAUSTED.
*/
LJUniverseResult lj_universe_close_recycle(LJUniverseClose *close,
                                           LJUniverseSnap *observed);

/* Fail-closed diagnostic transition; never grants close/free authority. */
LJUniverseResult lj_universe_try_poison(const LJUniverseKey *key,
                                        LJUniverseSnap *observed);

#if defined(LJ_UNIVERSE_TEST_HELPERS)
typedef void (*LJUniversePoisonTestHook)(void *ud);
/* Called after each exact poison snapshot and before its CAS, for schedules. */
void lj_universe_test_set_poison_hook(LJUniversePoisonTestHook hook,
                                      void *ud);
typedef enum LJUniverseStageTestPoint {
  LJ_UNIVERSE_TEST_FREEZE_EXTERNAL = 1,
  LJ_UNIVERSE_TEST_FREEZE_TERMINAL = 2,
  LJ_UNIVERSE_TEST_RECYCLE = 3,
  LJ_UNIVERSE_TEST_AFTER_EXTERNAL_MARKER = 4,
  LJ_UNIVERSE_TEST_AFTER_TERMINAL_MARKER = 5
} LJUniverseStageTestPoint;
typedef void (*LJUniverseStageTestHook)(uint8_t point, void *ud);
/* Called at the named pre-decision or post-marker scheduling point. */
void lj_universe_test_set_stage_hook(LJUniverseStageTestHook hook, void *ud);
/* Exact old-snapshot poison schedule; never part of the production API. */
LJUniverseResult lj_universe_test_poison_from_snapshot(
  const LJUniverseKey *key, const LJUniverseSnap *authority);
#endif

#endif /* _LJ_UNIVERSE_H */
