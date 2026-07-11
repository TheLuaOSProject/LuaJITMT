/*
** lj_tgslot.h - stable TG-slot incarnation and lifetime leases.
**
** A stable external handle is {slot, incarnation}.  The complete lifecycle
** authority is one CX16 word, so a stale handle can never borrow a reused
** slot and a reclaimer can close admission without a split-word race.
**
** This is intentionally a standalone x86-64 primitive for now.  Runtime TG
** body pointers and stable-TLS reference tracking will be wired separately.
*/
#ifndef _LJ_TGSLOT_H
#define _LJ_TGSLOT_H

#include "lj_atomic.h"

#if !defined(__x86_64__)
#error "TG-slot lifecycle tokens currently require the x86-64 CX16 contract"
#endif

#define LJ_TGSLOT_STATE_BITS 3u
#define LJ_TGSLOT_STATE_MASK \
  ((UINT64_C(1) << LJ_TGSLOT_STATE_BITS) - 1u)
#define LJ_TGSLOT_MAX_LEASES (UINT64_MAX >> LJ_TGSLOT_STATE_BITS)
#define LJ_TGSLOT_INCARNATION_NONE UINT64_C(0)

typedef enum LJTGSlotState {
  LJ_TGSLOT_EMPTY = 0,
  LJ_TGSLOT_ATTACHING = 1,
  LJ_TGSLOT_LIVE = 2,
  LJ_TGSLOT_DETACHING = 3,
  LJ_TGSLOT_RETIRED = 4,
  LJ_TGSLOT_RECLAIMING = 5,
  LJ_TGSLOT_PINNED = 6,
  LJ_TGSLOT_EXHAUSTED = 7  /* Terminal empty slot; no body exists. */
} LJTGSlotState;

typedef struct LJTGSlotToken {
  /* lo = non-wrapping incarnation; hi = lease_count << 3 | state. */
  la_u128 value;
} LJTGSlotToken;

typedef struct LJTGSlotKey {
  LJTGSlotToken *slot;
  uint64_t incarnation;
} LJTGSlotKey;

typedef struct LJTGSlotSnap {
  uint64_t incarnation;
  uint64_t lease_count;
  uint8_t state;
} LJTGSlotSnap;

typedef enum LJTGSlotResult {
  LJ_TGSLOT_EXHAUSTED_RESULT = -6,
  LJ_TGSLOT_INVALID = -5,
  LJ_TGSLOT_STALE = -4,
  LJ_TGSLOT_DENIED = -3,
  LJ_TGSLOT_BUSY = -2,
  LJ_TGSLOT_PINNED_RESULT = -1,
  LJ_TGSLOT_LOST = 0,
  LJ_TGSLOT_OK = 1
} LJTGSlotResult;

typedef char lj_tgslot_token_size_must_be_16[
  sizeof(LJTGSlotToken) == 16 ? 1 : -1];
typedef char lj_tgslot_token_align_must_be_16[
  __alignof__(LJTGSlotToken) >= 16 ? 1 : -1];

LA_INLINE uint64_t lj_tgslot_pack_hi(uint64_t lease_count, uint8_t state)
{
  return (lease_count << LJ_TGSLOT_STATE_BITS) | (uint64_t)state;
}

LA_INLINE uint64_t lj_tgslot_lease_count(uint64_t hi)
{
  return hi >> LJ_TGSLOT_STATE_BITS;
}

LA_INLINE uint8_t lj_tgslot_state(uint64_t hi)
{
  return (uint8_t)(hi & LJ_TGSLOT_STATE_MASK);
}

/*
** The owner body lease is installed by EMPTY -> ATTACHING and must remain
** until RETIRED.  This makes zero leases impossible while the slot body is
** publicly usable.  PINNED is the conservative no-reclaim state.
*/
LA_INLINE int lj_tgslot_components_valid(uint64_t incarnation,
                                          uint64_t lease_count,
                                          uint8_t state)
{
  if (state > LJ_TGSLOT_EXHAUSTED || lease_count > LJ_TGSLOT_MAX_LEASES)
    return 0;
  if (state != LJ_TGSLOT_EMPTY && state != LJ_TGSLOT_EXHAUSTED &&
      incarnation == LJ_TGSLOT_INCARNATION_NONE)
    return 0;
  switch (state) {
  case LJ_TGSLOT_EMPTY:
    return lease_count == 0;
  case LJ_TGSLOT_ATTACHING:
  case LJ_TGSLOT_LIVE:
  case LJ_TGSLOT_DETACHING:
    return lease_count != 0;
  case LJ_TGSLOT_RECLAIMING:
    return lease_count == 0;
  case LJ_TGSLOT_EXHAUSTED:
    return incarnation == UINT64_MAX && lease_count == 0;
  case LJ_TGSLOT_RETIRED:
  case LJ_TGSLOT_PINNED:
    return 1;
  default:
    return 0;
  }
}

LA_INLINE LJTGSlotSnap lj_tgslot_snap_from_words(uint64_t incarnation,
                                                  uint64_t hi)
{
  LJTGSlotSnap snap;
  snap.incarnation = incarnation;
  snap.lease_count = lj_tgslot_lease_count(hi);
  snap.state = lj_tgslot_state(hi);
  return snap;
}

/* Only valid before the containing registry slot is published. */
LA_INLINE int lj_tgslot_init_unpublished(LJTGSlotToken *slot,
                                          uint64_t incarnation,
                                          uint64_t lease_count,
                                          uint8_t state)
{
  if (!slot ||
      !lj_tgslot_components_valid(incarnation, lease_count, state))
    return 0;
  slot->value.lo = incarnation;
  slot->value.hi = lj_tgslot_pack_hi(lease_count, state);
  return 1;
}

LA_INLINE int lj_tgslot_init_empty_unpublished(LJTGSlotToken *slot,
                                                uint64_t incarnation)
{
  return lj_tgslot_init_unpublished(slot, incarnation, 0,
                                     LJ_TGSLOT_EMPTY);
}

/*
** Stable acquire snapshot without a potentially out-of-line 16-byte load.
** Overlapping 64-bit subloads and CX16 are an explicit x86-64 GCC/Clang,
** MinGW and Darwin artifact contract, matching markword/activation tokens.
*/
LA_INLINE LJTGSlotSnap lj_tgslot_snapshot(const LJTGSlotToken *slot)
{
  uint64_t hi, incarnation, again;
  do {
    hi = la_load64_acq(&slot->value.hi);
    incarnation = la_load64_acq(&slot->value.lo);
    again = la_load64_acq(&slot->value.hi);
  } while (hi != again);
  return lj_tgslot_snap_from_words(incarnation, hi);
}

LA_INLINE int lj_tgslot_snap_equal(const LJTGSlotSnap *a,
                                    const LJTGSlotSnap *b)
{
  return a->incarnation == b->incarnation &&
         a->lease_count == b->lease_count && a->state == b->state;
}

LA_INLINE int lj_tgslot_key_equal(const LJTGSlotKey *a,
                                   const LJTGSlotKey *b)
{
  return a->slot == b->slot && a->incarnation == b->incarnation;
}

LA_INLINE int lj_tgslot_key_valid(const LJTGSlotKey *key)
{
  return key && key->slot &&
         key->incarnation != LJ_TGSLOT_INCARNATION_NONE;
}

/* PINNED and terminal-empty EXHAUSTED are absorbing. */
LA_INLINE int lj_tgslot_state_edge_valid(uint8_t from, uint8_t to)
{
  if (from > LJ_TGSLOT_EXHAUSTED || to > LJ_TGSLOT_EXHAUSTED ||
      from == to)
    return 0;
  if (from == LJ_TGSLOT_PINNED || from == LJ_TGSLOT_EXHAUSTED)
    return 0;
  if (to == LJ_TGSLOT_PINNED)
    return from == LJ_TGSLOT_ATTACHING || from == LJ_TGSLOT_LIVE ||
           from == LJ_TGSLOT_DETACHING || from == LJ_TGSLOT_RETIRED;
  switch (from) {
  case LJ_TGSLOT_EMPTY:
    return to == LJ_TGSLOT_ATTACHING || to == LJ_TGSLOT_EXHAUSTED;
  case LJ_TGSLOT_ATTACHING:
    return to == LJ_TGSLOT_LIVE || to == LJ_TGSLOT_RETIRED;
  case LJ_TGSLOT_LIVE:
    return to == LJ_TGSLOT_DETACHING;
  case LJ_TGSLOT_DETACHING:
    return to == LJ_TGSLOT_RETIRED;
  case LJ_TGSLOT_RETIRED:
    return to == LJ_TGSLOT_RECLAIMING;
  case LJ_TGSLOT_RECLAIMING:
    return to == LJ_TGSLOT_EMPTY;
  default:
    return 0;
  }
}

LA_INLINE void lj_tgslot_store_observed(LJTGSlotSnap *observed,
                                         const la_u128 *value)
{
  if (observed)
    *observed = lj_tgslot_snap_from_words(value->lo, value->hi);
}

LA_INLINE int lj_tgslot_replacement_valid(const LJTGSlotSnap *before,
                                           uint64_t next_incarnation,
                                           uint64_t next_lease_count,
                                           uint8_t next_state)
{
  if (!lj_tgslot_components_valid(before->incarnation,
                                   before->lease_count, before->state) ||
      !lj_tgslot_components_valid(next_incarnation, next_lease_count,
                                   next_state))
    return 0;
  if (before->state == next_state) {
    if (before->incarnation != next_incarnation)
      return 0;
    if (next_lease_count > before->lease_count &&
        next_lease_count - before->lease_count == 1u)
      return before->state == LJ_TGSLOT_LIVE ||
             before->state == LJ_TGSLOT_DETACHING ||
             before->state == LJ_TGSLOT_ATTACHING;
    if (next_lease_count < before->lease_count &&
        before->lease_count - next_lease_count == 1u) {
      if (before->state == LJ_TGSLOT_RETIRED ||
          before->state == LJ_TGSLOT_PINNED)
        return 1;
      return before->lease_count > 1u &&
             (before->state == LJ_TGSLOT_ATTACHING ||
              before->state == LJ_TGSLOT_LIVE ||
              before->state == LJ_TGSLOT_DETACHING);
    }
    return 0;
  }
  if (next_state == LJ_TGSLOT_PINNED)
    return lj_tgslot_state_edge_valid(before->state, next_state) &&
           before->incarnation == next_incarnation &&
           before->lease_count == next_lease_count;
  if (next_state == LJ_TGSLOT_EXHAUSTED)
    return before->state == LJ_TGSLOT_EMPTY &&
           before->incarnation == UINT64_MAX &&
           next_incarnation == before->incarnation &&
           before->lease_count == 0 && next_lease_count == 0;
  if (!lj_tgslot_state_edge_valid(before->state, next_state))
    return 0;
  if (before->state == LJ_TGSLOT_EMPTY)
    return before->incarnation != UINT64_MAX &&
           next_incarnation == before->incarnation + 1u &&
           before->lease_count == 0 && next_lease_count == 1;
  return before->incarnation == next_incarnation &&
         before->lease_count == next_lease_count;
}

LA_INLINE LJTGSlotResult
lj_tgslot_try_replace(LJTGSlotToken *slot, const LJTGSlotSnap *before,
                      uint64_t next_incarnation, uint64_t next_lease_count,
                      uint8_t next_state, LJTGSlotSnap *observed)
{
  la_u128 expected, desired;
  if (!slot || !before ||
      !lj_tgslot_replacement_valid(before, next_incarnation,
                                    next_lease_count, next_state))
    return LJ_TGSLOT_INVALID;
  expected.lo = before->incarnation;
  expected.hi = lj_tgslot_pack_hi(before->lease_count, before->state);
  desired.lo = next_incarnation;
  desired.hi = lj_tgslot_pack_hi(next_lease_count, next_state);
  if (la_cas128(&slot->value, &expected, desired)) {
    if (observed)
      *observed = lj_tgslot_snap_from_words(desired.lo, desired.hi);
    return LJ_TGSLOT_OK;
  }
  lj_tgslot_store_observed(observed, &expected);
  return LJ_TGSLOT_LOST;
}

/* Saturation is a sticky, exact-CAS transition into no-reclaim policy. */
LA_INLINE LJTGSlotResult
lj_tgslot_try_pin(LJTGSlotToken *slot, const LJTGSlotSnap *before,
                  LJTGSlotSnap *observed)
{
  LJTGSlotResult result;
  if (!slot || !before)
    return LJ_TGSLOT_INVALID;
  if (before->state == LJ_TGSLOT_PINNED) {
    if (observed)
      *observed = *before;
    return LJ_TGSLOT_PINNED_RESULT;
  }
  result = lj_tgslot_try_replace(slot, before, before->incarnation,
                                 before->lease_count, LJ_TGSLOT_PINNED,
                                 observed);
  return result == LJ_TGSLOT_OK ? LJ_TGSLOT_PINNED_RESULT : result;
}

/*
** Claim one exact EMPTY incarnation.  The next non-zero incarnation and its
** owner body lease are installed atomically. Incarnation exhaustion enters a
** distinct terminal-empty state instead of allowing an ABA-producing wrap.
*/
LA_INLINE LJTGSlotResult
lj_tgslot_try_claim(LJTGSlotToken *slot, LJTGSlotKey *key,
                    LJTGSlotSnap *observed)
{
  LJTGSlotSnap before;
  LJTGSlotResult result;
  if (!slot || !key)
    return LJ_TGSLOT_INVALID;
  before = lj_tgslot_snapshot(slot);
  if (!lj_tgslot_components_valid(before.incarnation, before.lease_count,
                                   before.state))
    return LJ_TGSLOT_INVALID;
  if (before.state == LJ_TGSLOT_PINNED) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_PINNED_RESULT;
  }
  if (before.state == LJ_TGSLOT_EXHAUSTED) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_EXHAUSTED_RESULT;
  }
  if (before.state != LJ_TGSLOT_EMPTY) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_DENIED;
  }
  if (before.incarnation == UINT64_MAX) {
    result = lj_tgslot_try_replace(slot, &before, before.incarnation, 0,
                                   LJ_TGSLOT_EXHAUSTED, observed);
    return result == LJ_TGSLOT_OK ? LJ_TGSLOT_EXHAUSTED_RESULT : result;
  }
  result = lj_tgslot_try_replace(slot, &before, before.incarnation + 1u, 1,
                                 LJ_TGSLOT_ATTACHING, observed);
  if (result == LJ_TGSLOT_OK) {
    key->slot = slot;
    key->incarnation = before.incarnation + 1u;
  }
  return result;
}

LA_INLINE LJTGSlotResult
lj_tgslot_try_keyed_edge(const LJTGSlotKey *key, uint8_t from, uint8_t to,
                         int require_zero_leases, LJTGSlotSnap *observed)
{
  LJTGSlotSnap before;
  if (!lj_tgslot_key_valid(key) || !lj_tgslot_state_edge_valid(from, to))
    return LJ_TGSLOT_INVALID;
  before = lj_tgslot_snapshot(key->slot);
  if (!lj_tgslot_components_valid(before.incarnation, before.lease_count,
                                   before.state))
    return LJ_TGSLOT_INVALID;
  if (before.incarnation != key->incarnation) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_STALE;
  }
  if (before.state == LJ_TGSLOT_PINNED) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_PINNED_RESULT;
  }
  if (before.state == LJ_TGSLOT_EXHAUSTED) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_STALE;
  }
  if (before.state != from) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_DENIED;
  }
  if (require_zero_leases && before.lease_count != 0) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_BUSY;
  }
  return lj_tgslot_try_replace(key->slot, &before, before.incarnation,
                               before.lease_count, to, observed);
}

LA_INLINE LJTGSlotResult
lj_tgslot_try_publish(const LJTGSlotKey *key, LJTGSlotSnap *observed)
{
  return lj_tgslot_try_keyed_edge(key, LJ_TGSLOT_ATTACHING,
                                  LJ_TGSLOT_LIVE, 0, observed);
}

LA_INLINE LJTGSlotResult
lj_tgslot_try_detach(const LJTGSlotKey *key, LJTGSlotSnap *observed)
{
  return lj_tgslot_try_keyed_edge(key, LJ_TGSLOT_LIVE,
                                  LJ_TGSLOT_DETACHING, 0, observed);
}

LA_INLINE LJTGSlotResult
lj_tgslot_try_retire(const LJTGSlotKey *key, LJTGSlotSnap *observed)
{
  return lj_tgslot_try_keyed_edge(key, LJ_TGSLOT_DETACHING,
                                  LJ_TGSLOT_RETIRED, 0, observed);
}

/* Close a failed attachment without ever exposing the slot as LIVE. */
LA_INLINE LJTGSlotResult
lj_tgslot_try_abort_attach(const LJTGSlotKey *key, LJTGSlotSnap *observed)
{
  return lj_tgslot_try_keyed_edge(key, LJ_TGSLOT_ATTACHING,
                                  LJ_TGSLOT_RETIRED, 0, observed);
}

/* Physical body reclamation is admitted only by this exact zero-lease CAS. */
LA_INLINE LJTGSlotResult
lj_tgslot_try_begin_reclaim(const LJTGSlotKey *key, LJTGSlotSnap *observed)
{
  return lj_tgslot_try_keyed_edge(key, LJ_TGSLOT_RETIRED,
                                  LJ_TGSLOT_RECLAIMING, 1, observed);
}

/* The caller clears body/global pointers before publishing EMPTY. */
LA_INLINE LJTGSlotResult
lj_tgslot_try_finish_reclaim(const LJTGSlotKey *key, LJTGSlotSnap *observed)
{
  return lj_tgslot_try_keyed_edge(key, LJ_TGSLOT_RECLAIMING,
                                  LJ_TGSLOT_EMPTY, 1, observed);
}

LA_INLINE int lj_tgslot_borrow_state(uint8_t state)
{
  return state == LJ_TGSLOT_ATTACHING || state == LJ_TGSLOT_LIVE ||
         state == LJ_TGSLOT_DETACHING;
}

/*
** One non-waiting remote borrow attempt. The registry must publish body/global
** pointers before it links an ATTACHING slot, so helpers may safely borrow it.
** DETACHING remains borrowable for a key loaded before detach. The exact
** RETIRED CAS closes admission and either precedes or preserves every borrow.
*/
LA_INLINE LJTGSlotResult
lj_tgslot_try_borrow(const LJTGSlotKey *key, LJTGSlotSnap *observed)
{
  LJTGSlotSnap before;
  if (!lj_tgslot_key_valid(key))
    return LJ_TGSLOT_INVALID;
  before = lj_tgslot_snapshot(key->slot);
  if (!lj_tgslot_components_valid(before.incarnation, before.lease_count,
                                   before.state))
    return LJ_TGSLOT_INVALID;
  if (before.incarnation != key->incarnation) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_STALE;
  }
  if (before.state == LJ_TGSLOT_PINNED) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_PINNED_RESULT;
  }
  if (before.state == LJ_TGSLOT_EXHAUSTED) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_STALE;
  }
  if (!lj_tgslot_borrow_state(before.state)) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_DENIED;
  }
  if (before.lease_count == LJ_TGSLOT_MAX_LEASES)
    return lj_tgslot_try_pin(key->slot, &before, observed);
  return lj_tgslot_try_replace(key->slot, &before, before.incarnation,
                               before.lease_count + 1u, before.state,
                               observed);
}

/*
** Release one previously acquired lease.  The final owner body lease cannot
** be dropped before RETIRED.  Releases from PINNED reduce accounting while
** preserving its absorbing no-reclaim state.
*/
LA_INLINE LJTGSlotResult
lj_tgslot_try_release(const LJTGSlotKey *key, LJTGSlotSnap *observed)
{
  LJTGSlotSnap before;
  if (!lj_tgslot_key_valid(key))
    return LJ_TGSLOT_INVALID;
  before = lj_tgslot_snapshot(key->slot);
  if (!lj_tgslot_components_valid(before.incarnation, before.lease_count,
                                   before.state))
    return LJ_TGSLOT_INVALID;
  if (before.incarnation != key->incarnation) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_STALE;
  }
  if (before.state == LJ_TGSLOT_EXHAUSTED) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_STALE;
  }
  if (before.lease_count == 0) {
    if (observed)
      *observed = before;
    return before.state == LJ_TGSLOT_PINNED ? LJ_TGSLOT_PINNED_RESULT :
                                             LJ_TGSLOT_DENIED;
  }
  if (before.state == LJ_TGSLOT_EMPTY ||
      before.state == LJ_TGSLOT_RECLAIMING) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_DENIED;
  }
  if ((before.state == LJ_TGSLOT_ATTACHING ||
       before.state == LJ_TGSLOT_LIVE ||
       before.state == LJ_TGSLOT_DETACHING) &&
      before.lease_count == 1) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_BUSY;
  }
  return lj_tgslot_try_replace(key->slot, &before, before.incarnation,
                               before.lease_count - 1u, before.state,
                               observed);
}

#endif /* _LJ_TGSLOT_H */
