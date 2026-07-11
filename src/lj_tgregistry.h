/*
** lj_tgregistry.h - stable external TG registry slots.
**
** Registry nodes are allocated once and remain address-stable until VM
** shutdown.  A node may host many TG bodies over its lifetime, but every use
** is named by {node, incarnation}.  lj_tgslot.h owns the complete admission
** and lease word; body is an immutable-per-incarnation publication protected
** by those leases.  next_all is initialized before the node is linked into an
** external registry and is never changed afterward.
**
** This header deliberately does not integrate with TGState or the runtime TG
** list.  It is the standalone lifetime primitive used by that later wiring.
*/
#ifndef _LJ_TGREGISTRY_H
#define _LJ_TGREGISTRY_H

#include "lj_tgslot.h"

typedef struct LJTGRegistrySlot LJTGRegistrySlot;

typedef struct LJTGRegistryBodySnap {
  void *body;
  uint64_t incarnation;
} LJTGRegistryBodySnap;

struct LJTGRegistrySlot {
  LJTGSlotToken token;          /* Exact incarnation/state/lease authority. */
  /* lo = body pointer, hi = the exact incarnation which owns that pointer. */
  la_u128 body_value;
  LJTGRegistrySlot *next_all;   /* Immutable after registry publication. */
};

typedef struct LJTGRegistryKey {
  LJTGRegistrySlot *slot;
  uint64_t incarnation;
} LJTGRegistryKey;

/* A borrow handle is linear: initialize once, do not copy while active, and
** release exactly once.  The cached body is protected by the held token lease.
*/
typedef struct LJTGRegistryBorrow {
  LJTGRegistryKey key;
  void *body;
  uint8_t active;
} LJTGRegistryBorrow;

typedef char lj_tgregistry_token_must_be_first[
  offsetof(LJTGRegistrySlot, token) == 0 ? 1 : -1];
typedef char lj_tgregistry_slot_align_must_cover_token[
  __alignof__(LJTGRegistrySlot) >= __alignof__(LJTGSlotToken) ? 1 : -1];
typedef char lj_tgregistry_body_must_be_16_aligned[
  offsetof(LJTGRegistrySlot, body_value) % 16u == 0 ? 1 : -1];
typedef char lj_tgregistry_pointer_must_fit_body_word[
  sizeof(void *) <= sizeof(uint64_t) ? 1 : -1];

LA_INLINE la_u128 lj_tgregistry_body_words(void *body, uint64_t incarnation)
{
  la_u128 words;
  words.lo = (uint64_t)(uintptr_t)body;
  words.hi = incarnation;
  return words;
}

LA_INLINE LJTGRegistryBodySnap
lj_tgregistry_body_snap_from_words(uint64_t pointer, uint64_t incarnation)
{
  LJTGRegistryBodySnap snap;
  snap.body = (void *)(uintptr_t)pointer;
  snap.incarnation = incarnation;
  return snap;
}

/* Stable acquire snapshot without an out-of-line 16-byte load. Publishing a
** new incarnation changes hi, so hi/lo/hi rejects a split old/new pair. Clear
** changes only the naturally atomic pointer half and either complete value is
** a valid snapshot for that same incarnation.
*/
LA_INLINE LJTGRegistryBodySnap
lj_tgregistry_slot_body_snapshot(const LJTGRegistrySlot *slot)
{
  uint64_t incarnation, pointer, again;
  if (!slot)
    return lj_tgregistry_body_snap_from_words(0, 0);
  do {
    incarnation = la_load64_acq(&slot->body_value.hi);
    pointer = la_load64_acq(&slot->body_value.lo);
    again = la_load64_acq(&slot->body_value.hi);
  } while (incarnation != again);
  return lj_tgregistry_body_snap_from_words(pointer, incarnation);
}

LA_INLINE int
lj_tgregistry_body_snap_equal(const LJTGRegistryBodySnap *a,
                              const LJTGRegistryBodySnap *b)
{
  return a && b && a->body == b->body &&
         a->incarnation == b->incarnation;
}

LA_INLINE int
lj_tgregistry_body_snap_is(const LJTGRegistryBodySnap *snap, void *body,
                           uint64_t incarnation)
{
  return snap && snap->body == body && snap->incarnation == incarnation;
}

LA_INLINE int
lj_tgregistry_body_is_published(const LJTGRegistryBodySnap *snap,
                                const LJTGRegistryKey *key)
{
  return key && key->slot &&
         key->incarnation != LJ_TGSLOT_INCARNATION_NONE && snap &&
         snap->body != NULL &&
         snap->incarnation == key->incarnation;
}

LA_INLINE int
lj_tgregistry_body_is_unpublished(const LJTGRegistryBodySnap *snap,
                                  const LJTGRegistryKey *key)
{
  return key && key->slot &&
         key->incarnation != LJ_TGSLOT_INCARNATION_NONE && snap &&
         snap->body == NULL &&
         snap->incarnation == key->incarnation - 1u;
}

/* This low-level CAS is exposed only so lifecycle helpers and deterministic
** schedule tests can resume at the exact body linearization point. Production
** callers must first validate the keyed token state; body_value alone is not
** reclamation authority.
*/
LA_INLINE int
lj_tgregistry_body_cas(LJTGRegistrySlot *slot,
                       LJTGRegistryBodySnap *expected,
                       void *body, uint64_t incarnation)
{
  la_u128 words, desired;
  if (!slot || !expected)
    return 0;
  words = lj_tgregistry_body_words(expected->body, expected->incarnation);
  desired = lj_tgregistry_body_words(body, incarnation);
  if (la_cas128(&slot->body_value, &words, desired)) {
    expected->body = body;
    expected->incarnation = incarnation;
    return 1;
  }
  *expected = lj_tgregistry_body_snap_from_words(words.lo, words.hi);
  return 0;
}

/* Test/bootstrap-only direct initialization, before either word is published. */
LA_INLINE void
lj_tgregistry_body_init_unpublished(LJTGRegistrySlot *slot, void *body,
                                    uint64_t incarnation)
{
  if (slot)
    slot->body_value = lj_tgregistry_body_words(body, incarnation);
}

/* Valid only before the node is release-published by its owning registry. */
LA_INLINE int
lj_tgregistry_slot_init_unpublished(LJTGRegistrySlot *slot,
                                    uint64_t empty_incarnation,
                                    LJTGRegistrySlot *next_all)
{
  if (!slot ||
      !lj_tgslot_init_empty_unpublished(&slot->token,
                                        empty_incarnation))
    return 0;
  lj_tgregistry_body_init_unpublished(slot, NULL, empty_incarnation);
  slot->next_all = next_all;
  return 1;
}

LA_INLINE LJTGRegistrySlot *
lj_tgregistry_slot_next_all(const LJTGRegistrySlot *slot)
{
  return slot ? (LJTGRegistrySlot *)
    la_loadptr_acq((void *const *)&slot->next_all) : NULL;
}

LA_INLINE void *lj_tgregistry_slot_body_acq(const LJTGRegistrySlot *slot)
{
  return lj_tgregistry_slot_body_snapshot(slot).body;
}

LA_INLINE int lj_tgregistry_key_valid(const LJTGRegistryKey *key)
{
  return key && key->slot &&
         key->incarnation != LJ_TGSLOT_INCARNATION_NONE;
}

LA_INLINE int lj_tgregistry_key_equal(const LJTGRegistryKey *a,
                                      const LJTGRegistryKey *b)
{
  return a && b && a->slot == b->slot &&
         a->incarnation == b->incarnation;
}

LA_INLINE LJTGSlotKey
lj_tgregistry_token_key(const LJTGRegistryKey *key)
{
  LJTGSlotKey token_key;
  token_key.slot = key && key->slot ? &key->slot->token : NULL;
  token_key.incarnation = key ? key->incarnation : 0;
  return token_key;
}

LA_INLINE LJTGSlotResult
lj_tgregistry_key_snapshot(const LJTGRegistryKey *key,
                           LJTGSlotSnap *snap)
{
  LJTGSlotSnap value;
  if (!lj_tgregistry_key_valid(key))
    return LJ_TGSLOT_INVALID;
  value = lj_tgslot_snapshot(&key->slot->token);
  if (!lj_tgslot_components_valid(value.incarnation, value.lease_count,
                                  value.state))
    return LJ_TGSLOT_INVALID;
  if (snap)
    *snap = value;
  if (value.incarnation != key->incarnation)
    return LJ_TGSLOT_STALE;
  if (value.state == LJ_TGSLOT_PINNED)
    return LJ_TGSLOT_PINNED_RESULT;
  if (value.state == LJ_TGSLOT_EXHAUSTED)
    return LJ_TGSLOT_EXHAUSTED_RESULT;
  return LJ_TGSLOT_OK;
}

/* Claim publishes ATTACHING with its owner lease, but no body. The claim owner
** initializes the body privately and release-publishes it before linking a new
** stable slot into the external registry. A reused slot is already linked, so
** scanners see its canonical-unpublished gap as BUSY. Once initialized, an
** ATTACHING body is borrowable for attach catch-up/root scans before LIVE.
*/
LA_INLINE LJTGSlotResult
lj_tgregistry_try_claim(LJTGRegistrySlot *slot, LJTGRegistryKey *key,
                        LJTGSlotSnap *observed)
{
  LJTGRegistryBodySnap body;
  LJTGSlotSnap before;
  LJTGSlotKey token_key;
  LJTGSlotResult result;
  if (!slot || !key)
    return LJ_TGSLOT_INVALID;
  before = lj_tgslot_snapshot(&slot->token);
  if (!lj_tgslot_components_valid(before.incarnation, before.lease_count,
                                  before.state))
    return LJ_TGSLOT_INVALID;
  if (before.state == LJ_TGSLOT_PINNED) {
    if (observed) *observed = before;
    return LJ_TGSLOT_PINNED_RESULT;
  }
  if (before.state == LJ_TGSLOT_EXHAUSTED) {
    if (observed) *observed = before;
    body = lj_tgregistry_slot_body_snapshot(slot);
    return lj_tgregistry_body_snap_is(&body, NULL, before.incarnation) ?
           LJ_TGSLOT_EXHAUSTED_RESULT : LJ_TGSLOT_INVALID;
  }
  if (before.state == LJ_TGSLOT_EMPTY) {
    body = lj_tgregistry_slot_body_snapshot(slot);
    if (!lj_tgregistry_body_snap_is(&body, NULL, before.incarnation))
      return LJ_TGSLOT_INVALID;  /* EMPTY must name its exact null body. */
  }
  result = lj_tgslot_try_claim(&slot->token, &token_key, observed);
  if (result == LJ_TGSLOT_OK) {
    key->slot = slot;
    key->incarnation = token_key.incarnation;
  }
  return result;
}

/* The claim owner is the sole ATTACHING terminal actor: publish_body,
** publish and abort_attach for one key are serialized by that owner.  This is
** the only body write before reclaim.  A retry with the same pointer is
** idempotent; a different pointer fails closed.
*/
LA_INLINE LJTGSlotResult
lj_tgregistry_try_publish_body(const LJTGRegistryKey *key, void *body,
                               LJTGSlotSnap *observed)
{
  LJTGRegistryBodySnap actual, expected;
  LJTGSlotSnap before, after;
  LJTGSlotResult status;
  if (!body)
    return LJ_TGSLOT_INVALID;
  before.incarnation = after.incarnation = 0;
  before.lease_count = after.lease_count = 0;
  before.state = after.state = LJ_TGSLOT_EMPTY;
  status = lj_tgregistry_key_snapshot(key, &before);
  if (status != LJ_TGSLOT_OK)
    return status;
  if (before.state != LJ_TGSLOT_ATTACHING &&
      before.state != LJ_TGSLOT_LIVE) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_DENIED;
  }
  actual = lj_tgregistry_slot_body_snapshot(key->slot);
  if (before.state == LJ_TGSLOT_LIVE) {
    /* LIVE is already the body-publication contract. It can only accept an
    ** idempotent retry; filling a missing LIVE body would hide corruption. */
    if (!lj_tgregistry_body_snap_is(&actual, body, key->incarnation)) {
      if (!lj_tgregistry_body_is_published(&actual, key))
        return lj_tgslot_try_pin(&key->slot->token, &before, observed);
      return LJ_TGSLOT_INVALID;
    }
    if (observed)
      *observed = before;
    return LJ_TGSLOT_OK;
  }
  if (!lj_tgregistry_body_snap_is(&actual, body, key->incarnation)) {
    expected.body = NULL;
    expected.incarnation = key->incarnation - 1u;
    if (!lj_tgregistry_body_cas(key->slot, &expected, body,
                               key->incarnation) &&
        !lj_tgregistry_body_snap_is(&expected, body, key->incarnation)) {
      if (lj_tgregistry_body_is_published(&expected, key))
        return LJ_TGSLOT_INVALID;
      if (expected.incarnation == key->incarnation ||
          expected.incarnation == key->incarnation - 1u)
        return lj_tgslot_try_pin(&key->slot->token, &before, observed);
      return LJ_TGSLOT_STALE;
    }
  }
  status = lj_tgregistry_key_snapshot(key, &after);
  if (observed)
    *observed = after;
  if (status != LJ_TGSLOT_OK)
    return status;
  return after.state == LJ_TGSLOT_ATTACHING ||
         after.state == LJ_TGSLOT_LIVE ? LJ_TGSLOT_OK : LJ_TGSLOT_DENIED;
}

/* Publish LIVE only after the body release store.  The token's release CAS is
** the registry publication edge observed by a successful exact borrow.
*/
LA_INLINE LJTGSlotResult
lj_tgregistry_try_publish(const LJTGRegistryKey *key,
                          LJTGSlotSnap *observed)
{
  LJTGRegistryBodySnap body;
  LJTGSlotKey token_key;
  LJTGSlotSnap before;
  LJTGSlotResult status = lj_tgregistry_key_snapshot(key, &before);
  if (status != LJ_TGSLOT_OK)
    return status;
  if (before.state == LJ_TGSLOT_LIVE) {
    if (observed)
      *observed = before;
    body = lj_tgregistry_slot_body_snapshot(key->slot);
    return lj_tgregistry_body_is_published(&body, key) ? LJ_TGSLOT_OK :
      lj_tgslot_try_pin(&key->slot->token, &before, observed);
  }
  if (before.state != LJ_TGSLOT_ATTACHING) {
    if (observed)
      *observed = before;
    return LJ_TGSLOT_DENIED;
  }
  body = lj_tgregistry_slot_body_snapshot(key->slot);
  if (!lj_tgregistry_body_is_published(&body, key)) {
    if (lj_tgregistry_body_is_unpublished(&body, key))
      return LJ_TGSLOT_INVALID;
    return lj_tgslot_try_pin(&key->slot->token, &before, observed);
  }
  token_key = lj_tgregistry_token_key(key);
  return lj_tgslot_try_publish(&token_key, observed);
}

/* Take a non-owning key snapshot from a stable registry node. It is only a
** lookup result: callers must still borrow before reading body state. An
** initialized ATTACHING body is deliberately visible to attach catch-up/root
** scanners, and DETACHING stays visible until the exact RETIRED
** admission-close LP. A reused node is already linked while its next
** ATTACHING body is still canonical-unpublished; that rootless gap is BUSY,
** not corruption.
*/
LA_INLINE LJTGSlotResult
lj_tgregistry_try_borrowable_key(LJTGRegistrySlot *slot,
                                 LJTGRegistryKey *key,
                                 LJTGSlotSnap *observed)
{
  LJTGRegistryBodySnap body;
  LJTGSlotSnap before, after;
  if (!slot || !key)
    return LJ_TGSLOT_INVALID;
  before = lj_tgslot_snapshot(&slot->token);
  if (!lj_tgslot_components_valid(before.incarnation, before.lease_count,
                                  before.state))
    return LJ_TGSLOT_INVALID;
  if (before.state == LJ_TGSLOT_PINNED) {
    if (observed) *observed = before;
    return LJ_TGSLOT_PINNED_RESULT;
  }
  if (before.state == LJ_TGSLOT_EXHAUSTED) {
    if (observed) *observed = before;
    return LJ_TGSLOT_EXHAUSTED_RESULT;
  }
  body = lj_tgregistry_slot_body_snapshot(slot);
  if (!lj_tgslot_borrow_state(before.state)) {
    if (observed) *observed = before;
    return LJ_TGSLOT_DENIED;
  }
  if (body.body == NULL || body.incarnation != before.incarnation) {
    if (before.state == LJ_TGSLOT_ATTACHING && body.body == NULL &&
        body.incarnation == before.incarnation - 1u) {
      after = lj_tgslot_snapshot(&slot->token);
      if (observed)
        *observed = after;
      return lj_tgslot_snap_equal(&before, &after) ? LJ_TGSLOT_BUSY :
                                                    LJ_TGSLOT_LOST;
    }
    return lj_tgslot_try_pin(&slot->token, &before, observed);
  }
  after = lj_tgslot_snapshot(&slot->token);
  if (observed)
    *observed = after;
  if (!lj_tgslot_snap_equal(&before, &after))
    return LJ_TGSLOT_LOST;
  key->slot = slot;
  key->incarnation = before.incarnation;
  return LJ_TGSLOT_OK;
}

LA_INLINE void lj_tgregistry_borrow_init(LJTGRegistryBorrow *borrow)
{
  if (borrow) {
    borrow->key.slot = NULL;
    borrow->key.incarnation = 0;
    borrow->body = NULL;
    borrow->active = 0;
  }
}

/* One exact nonwaiting borrow attempt. ATTACHING is admitted only after body
** publication, allowing catch-up/root scans on a linked attaching slot. A key
** may still borrow during DETACHING; RETIRED is the exact admission-close LP.
*/
LA_INLINE LJTGSlotResult
lj_tgregistry_try_borrow(const LJTGRegistryKey *key,
                         LJTGRegistryBorrow *borrow,
                         LJTGSlotSnap *observed)
{
  LJTGRegistryBodySnap body;
  LJTGSlotSnap before;
  LJTGSlotResult status;
  if (!borrow || borrow->active)
    return LJ_TGSLOT_INVALID;
  status = lj_tgregistry_key_snapshot(key, &before);
  if (status != LJ_TGSLOT_OK)
    return status;
  if (!lj_tgslot_borrow_state(before.state)) {
    if (observed) *observed = before;
    return LJ_TGSLOT_DENIED;
  }
  body = lj_tgregistry_slot_body_snapshot(key->slot);
  if (!lj_tgregistry_body_is_published(&body, key)) {
    /* ATTACHING legitimately precedes body publication. Its canonical
    ** not-yet-published value still names the previous EMPTY incarnation and
    ** is a bounded BUSY result, not corruption. Every other mismatch
    ** under the unchanged borrowable token becomes sticky no-reclaim. */
    if (before.state == LJ_TGSLOT_ATTACHING &&
        lj_tgregistry_body_is_unpublished(&body, key)) {
      if (observed)
        *observed = before;
      return LJ_TGSLOT_BUSY;
    }
    return lj_tgslot_try_pin(&key->slot->token, &before, observed);
  }
  if (before.lease_count == LJ_TGSLOT_MAX_LEASES)
    return lj_tgslot_try_pin(&key->slot->token, &before, observed);
  status = lj_tgslot_try_replace(&key->slot->token, &before,
                                 before.incarnation,
                                 before.lease_count + 1u,
                                 before.state, observed);
  if (status == LJ_TGSLOT_OK) {
    borrow->key = *key;
    borrow->body = body.body;
    borrow->active = 1;
  }
  return status;
}

/* Snapshot the immutable body while the borrow lease is held.  RETIRED is
** valid for an admitted borrower; RECLAIMING/EMPTY are impossible until its
** release.  PINNED and any invariant mismatch fail closed.
*/
LA_INLINE LJTGSlotResult
lj_tgregistry_try_body_snapshot(const LJTGRegistryBorrow *borrow,
                                void **bodyp, LJTGSlotSnap *observed)
{
  LJTGRegistryBodySnap body;
  LJTGSlotSnap snap;
  LJTGSlotResult status;
  if (bodyp)
    *bodyp = NULL;
  if (!borrow || !borrow->active || !borrow->body)
    return LJ_TGSLOT_INVALID;
  snap.incarnation = 0;
  snap.lease_count = 0;
  snap.state = LJ_TGSLOT_EMPTY;
  status = lj_tgregistry_key_snapshot(&borrow->key, &snap);
  if (observed)
    *observed = snap;
  if (status != LJ_TGSLOT_OK)
    return status;
  if (snap.state != LJ_TGSLOT_ATTACHING &&
      snap.state != LJ_TGSLOT_LIVE &&
      snap.state != LJ_TGSLOT_DETACHING &&
      snap.state != LJ_TGSLOT_RETIRED)
    return LJ_TGSLOT_DENIED;
  body = lj_tgregistry_slot_body_snapshot(borrow->key.slot);
  if (!body.body || body.body != borrow->body ||
      body.incarnation != borrow->key.incarnation)
    return lj_tgslot_try_pin(&borrow->key.slot->token, &snap, observed);
  if (bodyp)
    *bodyp = body.body;
  return LJ_TGSLOT_OK;
}

LA_INLINE LJTGSlotResult
lj_tgregistry_try_release(LJTGRegistryBorrow *borrow,
                          LJTGSlotSnap *observed)
{
  LJTGSlotKey token_key;
  LJTGSlotResult result;
  if (!borrow || !borrow->active)
    return LJ_TGSLOT_INVALID;
  token_key = lj_tgregistry_token_key(&borrow->key);
  result = lj_tgslot_try_release(&token_key, observed);
  if (result == LJ_TGSLOT_OK)
    lj_tgregistry_borrow_init(borrow);
  return result;
}

LA_INLINE LJTGSlotResult
lj_tgregistry_try_detach(const LJTGRegistryKey *key,
                         LJTGSlotSnap *observed)
{
  LJTGSlotKey token_key;
  LJTGSlotResult status = lj_tgregistry_key_snapshot(key, NULL);
  if (status != LJ_TGSLOT_OK)
    return status;
  token_key = lj_tgregistry_token_key(key);
  return lj_tgslot_try_detach(&token_key, observed);
}

LA_INLINE LJTGSlotResult
lj_tgregistry_try_retire(const LJTGRegistryKey *key,
                         LJTGSlotSnap *observed)
{
  LJTGSlotKey token_key;
  LJTGSlotResult status = lj_tgregistry_key_snapshot(key, NULL);
  if (status != LJ_TGSLOT_OK)
    return status;
  token_key = lj_tgregistry_token_key(key);
  return lj_tgslot_try_retire(&token_key, observed);
}

LA_INLINE LJTGSlotResult
lj_tgregistry_try_abort_attach(const LJTGRegistryKey *key,
                               LJTGSlotSnap *observed)
{
  LJTGSlotKey token_key;
  LJTGSlotResult status = lj_tgregistry_key_snapshot(key, NULL);
  if (status != LJ_TGSLOT_OK)
    return status;
  token_key = lj_tgregistry_token_key(key);
  return lj_tgslot_try_abort_attach(&token_key, observed);
}

/* RETIRED closes admission while retaining the owner lease.  Borrow releases
** drain the count to one.  This exact compound edge consumes that final owner
** lease and enters RECLAIMING atomically, so a duplicate owner release can
** never consume a remote borrow and RETIRED/count-zero is never exposed.
*/
LA_INLINE LJTGSlotResult
lj_tgregistry_try_reclaim(const LJTGRegistryKey *key, void **bodyp,
                          LJTGSlotSnap *observed)
{
  LJTGRegistryBodySnap body;
  LJTGSlotSnap before;
  LJTGSlotKey token_key;
  LJTGSlotResult status;
  if (bodyp)
    *bodyp = NULL;
  status = lj_tgregistry_key_snapshot(key, &before);
  if (status != LJ_TGSLOT_OK)
    return status;
  if (before.state != LJ_TGSLOT_RETIRED) {
    if (observed) *observed = before;
    return LJ_TGSLOT_DENIED;
  }
  body = lj_tgregistry_slot_body_snapshot(key->slot);
  if (!body.body || body.incarnation != key->incarnation) {
    /* A bodyless RETIRED slot violates the registry publication contract.
    ** Make the failure sticky before any actor can expose reusable EMPTY. */
    return lj_tgslot_try_pin(&key->slot->token, &before, observed);
  }
  token_key = lj_tgregistry_token_key(key);
  status = lj_tgslot_try_begin_reclaim(&token_key, observed);
  if (status != LJ_TGSLOT_OK)
    return status;
  if (bodyp)
    *bodyp = body.body;
  return LJ_TGSLOT_OK;
}

LA_INLINE LJTGSlotResult
lj_tgregistry_try_finish_clear(const LJTGRegistryKey *key,
                               LJTGSlotSnap *observed)
{
  LJTGRegistryBodySnap body;
  LJTGSlotSnap edge_observed;
  LJTGSlotKey token_key;
  LJTGSlotResult status;
  if (!lj_tgregistry_key_valid(key))
    return LJ_TGSLOT_INVALID;
  body = lj_tgregistry_slot_body_snapshot(key->slot);
  if (body.incarnation != key->incarnation) {
    status = lj_tgregistry_key_snapshot(key, observed);
    return status == LJ_TGSLOT_OK ? LJ_TGSLOT_INVALID : status;
  }
  if (body.body != NULL)
    return LJ_TGSLOT_INVALID;
  edge_observed = lj_tgslot_snapshot(&key->slot->token);
  token_key = lj_tgregistry_token_key(key);
  status = lj_tgslot_try_finish_reclaim(&token_key, &edge_observed);
  if (observed)
    *observed = edge_observed;
  if (status == LJ_TGSLOT_OK)
    return status;
  if (edge_observed.incarnation != key->incarnation)
    return LJ_TGSLOT_STALE;
  if (edge_observed.state == LJ_TGSLOT_EMPTY &&
      edge_observed.lease_count == 0) {
    body = lj_tgregistry_slot_body_snapshot(key->slot);
    return lj_tgregistry_body_snap_is(&body, NULL, key->incarnation) ?
           LJ_TGSLOT_OK : LJ_TGSLOT_INVALID;
  }
  return status;
}

/* Complete a clear after the caller captured the exact body. Revalidate the
** exact RECLAIMING/0 lifecycle before touching body_value. A winner may still
** clear, publish EMPTY and reuse the slot immediately after that snapshot; the
** tagged body CAS then loses against {same pointer, new incarnation} instead
** of erasing the new body.
*/
LA_INLINE LJTGSlotResult
lj_tgregistry_try_clear_captured(const LJTGRegistryKey *key,
                                 const LJTGRegistryBodySnap *captured,
                                 LJTGSlotSnap *observed)
{
  LJTGRegistryBodySnap expected;
  LJTGSlotSnap before;
  LJTGSlotResult status;
  if (!lj_tgregistry_key_valid(key) || !captured || !captured->body ||
      captured->incarnation != key->incarnation)
    return LJ_TGSLOT_INVALID;
  status = lj_tgregistry_key_snapshot(key, &before);
  if (status != LJ_TGSLOT_OK)
    return status;
  if (before.state == LJ_TGSLOT_EMPTY && before.lease_count == 0)
    return lj_tgregistry_try_finish_clear(key, observed);
  if (before.state != LJ_TGSLOT_RECLAIMING || before.lease_count != 0) {
    if (observed) *observed = before;
    return LJ_TGSLOT_DENIED;
  }
  expected = *captured;
  if (!lj_tgregistry_body_cas(key->slot, &expected, NULL,
                              key->incarnation)) {
    if (expected.incarnation != key->incarnation) {
      status = lj_tgregistry_key_snapshot(key, observed);
      return status == LJ_TGSLOT_OK ? LJ_TGSLOT_INVALID : status;
    }
    if (expected.body != NULL)
      return LJ_TGSLOT_INVALID;
  }
  return lj_tgregistry_try_finish_clear(key, observed);
}

/* The reclaimer destroys/frees its body after try_reclaim succeeds, then calls
** clear. Body is exact-CASed to {NULL, incarnation} before EMPTY is published.
** Concurrent same-incarnation helpers remain idempotent, while a delayed old
** helper cannot clear a reused slot even when the body address is identical.
*/
LA_INLINE LJTGSlotResult
lj_tgregistry_try_clear(const LJTGRegistryKey *key,
                        LJTGSlotSnap *observed)
{
  LJTGRegistryBodySnap body;
  LJTGSlotSnap before;
  LJTGSlotResult status = lj_tgregistry_key_snapshot(key, &before);
  if (status != LJ_TGSLOT_OK)
    return status;
  body = lj_tgregistry_slot_body_snapshot(key->slot);
  if (before.state == LJ_TGSLOT_EMPTY) {
    if (observed) *observed = before;
    return lj_tgregistry_body_snap_is(&body, NULL, key->incarnation) ?
           LJ_TGSLOT_OK : LJ_TGSLOT_INVALID;
  }
  if (before.state != LJ_TGSLOT_RECLAIMING || before.lease_count != 0) {
    if (observed) *observed = before;
    return LJ_TGSLOT_DENIED;
  }
  if (body.incarnation != key->incarnation)
    return LJ_TGSLOT_INVALID;
  if (body.body == NULL)
    return lj_tgregistry_try_finish_clear(key, observed);
  return lj_tgregistry_try_clear_captured(key, &body, observed);
}

#endif /* _LJ_TGREGISTRY_H */
