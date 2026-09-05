/*
** lj_universe.c - dormant exact universe admission primitive.
*/
#include "lj_universe.h"

#if defined(LJ_UNIVERSE_TEST_HELPERS)
static LJUniversePoisonTestHook universe_poison_test_hook;
static void *universe_poison_test_ud;
static LJUniverseStageTestHook universe_stage_test_hook;
static void *universe_stage_test_ud;

void lj_universe_test_set_poison_hook(LJUniversePoisonTestHook hook,
                                      void *ud)
{
  universe_poison_test_ud = ud;
  universe_poison_test_hook = hook;
}

void lj_universe_test_set_stage_hook(LJUniverseStageTestHook hook, void *ud)
{
  universe_stage_test_ud = ud;
  universe_stage_test_hook = hook;
}
#endif

static la_u128 universe_token_words(uint64_t incarnation,
                                    uint64_t transaction_count,
                                    uint8_t state)
{
  la_u128 words;
  words.lo = incarnation;
  words.hi = lj_universe_pack_hi(transaction_count, state);
  return words;
}

static la_u128 universe_body_words(void *body, uint64_t incarnation)
{
  la_u128 words;
  words.lo = (uint64_t)(uintptr_t)body;
  words.hi = incarnation;
  return words;
}

static la_u128 universe_epoch_words(uint64_t epoch, uint64_t incarnation)
{
  la_u128 words;
  words.lo = epoch;
  words.hi = incarnation;
  return words;
}

static int universe_epoch_is(const la_u128 *field, uint64_t epoch,
                             uint64_t incarnation)
{
  LJUniverseEpochSnap snap = lj_universe_epoch_snapshot(field);
  return snap.epoch == epoch && snap.incarnation == incarnation;
}

static int universe_successor(uint64_t incarnation, uint64_t *successor)
{
  if (incarnation == UINT64_MAX)
    return 0;
  if (successor) *successor = incarnation + 1u;
  return 1;
}

static void universe_store_observed(LJUniverseSnap *observed,
                                    const la_u128 *words)
{
  if (observed)
    *observed = lj_universe_snap_from_words(words->lo, words->hi);
}

static int universe_slot_aligned(const LJUniverseSlot *slot)
{
  return slot && ((uintptr_t)slot & (uintptr_t)15u) == 0;
}

int lj_universe_components_valid(uint64_t incarnation,
                                 uint64_t transaction_count,
                                 uint8_t state)
{
  if (state > LJ_UNIVERSE_EXHAUSTED ||
      transaction_count > LJ_UNIVERSE_MAX_TRANSACTIONS)
    return 0;
  switch (state) {
  case LJ_UNIVERSE_EMPTY:
    return transaction_count == 0;
  case LJ_UNIVERSE_BUILDING:
    return incarnation != LJ_UNIVERSE_INCARNATION_NONE &&
           incarnation != UINT64_MAX &&
           transaction_count == 0;
  case LJ_UNIVERSE_OPEN:
  case LJ_UNIVERSE_CLOSING:
  case LJ_UNIVERSE_FINALIZING:
  case LJ_UNIVERSE_FINAL_DRAIN:
    return incarnation != LJ_UNIVERSE_INCARNATION_NONE &&
           incarnation != UINT64_MAX;
  case LJ_UNIVERSE_POISONED:
    return incarnation != LJ_UNIVERSE_INCARNATION_NONE;
  case LJ_UNIVERSE_SEALED:
    return incarnation != LJ_UNIVERSE_INCARNATION_NONE &&
           incarnation != UINT64_MAX &&
           transaction_count == 0;
  case LJ_UNIVERSE_EXHAUSTED:
    return incarnation == UINT64_MAX && transaction_count == 0;
  default:
    return 0;
  }
}

static LJUniverseResult universe_try_replace(
  LJUniverseSlot *slot, const LJUniverseSnap *before,
  uint64_t next_incarnation, uint64_t next_transaction_count,
  uint8_t next_state, LJUniverseSnap *observed)
{
  la_u128 expected, desired;
  if (!before)
    return LJ_UNIVERSE_INVALID;
  if (!universe_slot_aligned(slot) ||
      !lj_universe_components_valid(next_incarnation,
                                    next_transaction_count, next_state)) {
    if (observed) *observed = *before;
    return LJ_UNIVERSE_INVALID;
  }
  expected = universe_token_words(before->incarnation,
                                  before->transaction_count, before->state);
  desired = universe_token_words(next_incarnation,
                                 next_transaction_count, next_state);
  if (la_cas128(&slot->token.value, &expected, desired)) {
    universe_store_observed(observed, &desired);
    return LJ_UNIVERSE_OK;
  }
  universe_store_observed(observed, &expected);
  return LJ_UNIVERSE_LOST;
}

static LJUniverseResult universe_poison_snapshot(
  LJUniverseSlot *slot, const LJUniverseSnap *before,
  LJUniverseSnap *observed)
{
  if (!universe_slot_aligned(slot) || !before)
    return LJ_UNIVERSE_INVALID;
  if (before->incarnation == LJ_UNIVERSE_INCARNATION_NONE) {
    if (observed) *observed = *before;
    return LJ_UNIVERSE_CORRUPT;
  }
  if (before->state == LJ_UNIVERSE_POISONED) {
    if (observed) *observed = *before;
    return LJ_UNIVERSE_POISONED_RESULT;
  }
  if (before->state == LJ_UNIVERSE_EXHAUSTED) {
    if (observed) *observed = *before;
    return LJ_UNIVERSE_EXHAUSTED_RESULT;
  }
  return universe_try_replace(slot, before, before->incarnation,
                              before->transaction_count,
                              LJ_UNIVERSE_POISONED, observed);
}

/*
** Finish the fail-closed transition for one exact incarnation. CAS losers
** retry while that incarnation remains current, but never follow a recycled
** slot and poison a later legitimate universe.
*/
#define UNIVERSE_STATE_BIT(state) ((uint16_t)(1u << (uint8_t)(state)))

static LJUniverseResult universe_poison_authority(
  LJUniverseSlot *slot, const LJUniverseSnap *authority,
  uint16_t allowed_states, int count_may_change, LJUniverseSnap *observed)
{
  LJUniverseSnap before, after;
  LJUniverseResult result;
  if (!universe_slot_aligned(slot) || !authority)
    return LJ_UNIVERSE_INVALID;
  before = *authority;
  if (before.incarnation != authority->incarnation) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_STALE;
  }
  for (;;) {
    if (before.incarnation != authority->incarnation) {
      if (observed) *observed = before;
      return LJ_UNIVERSE_STALE;
    }
    if (before.state == LJ_UNIVERSE_POISONED) {
      if (observed) *observed = before;
      return LJ_UNIVERSE_POISONED_RESULT;
    }
    if (before.state > LJ_UNIVERSE_STATE_MASK ||
        !(allowed_states & UNIVERSE_STATE_BIT(before.state))) {
      if (observed) *observed = before;
      return LJ_UNIVERSE_DENIED;
    }
    if (!count_may_change &&
        (before.state != authority->state ||
         before.transaction_count != authority->transaction_count)) {
      if (observed) *observed = before;
      return LJ_UNIVERSE_DENIED;
    }
#if defined(LJ_UNIVERSE_TEST_HELPERS)
    if (universe_poison_test_hook)
      universe_poison_test_hook(universe_poison_test_ud);
#endif
    result = universe_poison_snapshot(slot, &before, &after);
    if (result != LJ_UNIVERSE_LOST) {
      if (observed) *observed = after;
      return result;
    }
    la_cpu_pause();
    before = after;
  }
}

#if defined(LJ_UNIVERSE_TEST_HELPERS)
LJUniverseResult lj_universe_test_poison_from_snapshot(
  const LJUniverseKey *key, const LJUniverseSnap *authority)
{
  if (!lj_universe_key_valid(key) || !authority ||
      authority->incarnation != key->incarnation ||
      authority->state > LJ_UNIVERSE_STATE_MASK)
    return LJ_UNIVERSE_INVALID;
  return universe_poison_authority(
    key->slot, authority, UNIVERSE_STATE_BIT(authority->state), 0, NULL);
}
#endif

static int universe_body_is(const LJUniverseSlot *slot, void *body,
                            uint64_t incarnation)
{
  LJUniverseBodySnap snap = lj_universe_body_snapshot(slot);
  return snap.body == body && snap.incarnation == incarnation;
}

static int universe_live_body_is(const LJUniverseSlot *slot,
                                 uint64_t incarnation, void **body)
{
  LJUniverseBodySnap snap = lj_universe_body_snapshot(slot);
  if (!snap.body || snap.incarnation != incarnation)
    return 0;
  if (body) *body = snap.body;
  return 1;
}

int lj_universe_slot_init_unpublished(LJUniverseSlot *slot,
                                      uint64_t empty_incarnation,
                                      LJUniverseSlot *next_all)
{
  if (!universe_slot_aligned(slot))
    return 0;
  slot->token.value = universe_token_words(empty_incarnation, 0,
                                           LJ_UNIVERSE_EMPTY);
  slot->body_value = universe_body_words(NULL, empty_incarnation);
  slot->next_publication = LJ_UNIVERSE_FIRST_PUBLICATION;
  slot->external_final_publication = universe_epoch_words(
    0, empty_incarnation);
  slot->terminal_final_publication = universe_epoch_words(
    0, empty_incarnation);
  slot->next_all = next_all;
  return 1;
}

LJUniverseResult lj_universe_try_claim(LJUniverseSlot *slot,
                                       LJUniverseBuild *build,
                                       LJUniverseSnap *observed)
{
  LJUniverseSnap before;
  LJUniverseResult result;
  if (!universe_slot_aligned(slot) || !build || build->active)
    return LJ_UNIVERSE_INVALID;
  before = lj_universe_snapshot(slot);
  if (!lj_universe_components_valid(before.incarnation,
                                     before.transaction_count,
                                     before.state)) {
    (void)universe_poison_authority(
      slot, &before, UNIVERSE_STATE_BIT(before.state), 0, NULL);
    if (observed) *observed = before;
    return LJ_UNIVERSE_CORRUPT;
  }
  if (before.state == LJ_UNIVERSE_POISONED) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_POISONED_RESULT;
  }
  if (before.state == LJ_UNIVERSE_EXHAUSTED) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_EXHAUSTED_RESULT;
  }
  if (before.state != LJ_UNIVERSE_EMPTY) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_DENIED;
  }
  if (!universe_body_is(slot, NULL, before.incarnation) ||
      la_load64_acq(&slot->next_publication) !=
        LJ_UNIVERSE_FIRST_PUBLICATION ||
      !universe_epoch_is(&slot->external_final_publication, 0,
                         before.incarnation) ||
      !universe_epoch_is(&slot->terminal_final_publication, 0,
                         before.incarnation)) {
    LJUniverseSnap recheck = lj_universe_snapshot(slot);
    if (!lj_universe_snap_equal(&before, &recheck)) {
      if (observed) *observed = recheck;
      return LJ_UNIVERSE_LOST;
    }
    result = universe_poison_authority(
      slot, &before, UNIVERSE_STATE_BIT(LJ_UNIVERSE_EMPTY), 0, &recheck);
    if (result == LJ_UNIVERSE_STALE) {
      if (observed) *observed = recheck;
      return LJ_UNIVERSE_LOST;
    }
    if (observed) *observed = before;
    return LJ_UNIVERSE_CORRUPT;
  }
  if (before.incarnation == UINT64_MAX) {
    result = universe_try_replace(slot, &before, before.incarnation, 0,
                                  LJ_UNIVERSE_EXHAUSTED, observed);
    return result == LJ_UNIVERSE_OK ? LJ_UNIVERSE_EXHAUSTED_RESULT : result;
  }
  {
    uint64_t claim_incarnation = before.incarnation == 0 ?
      UINT64_C(1) : before.incarnation;
    result = universe_try_replace(slot, &before, claim_incarnation, 0,
                                LJ_UNIVERSE_BUILDING, observed);
    if (result == LJ_UNIVERSE_OK) {
      la_u128 expected, desired;
      LJUniverseSnap claim_authority;
      claim_authority.incarnation = claim_incarnation;
      claim_authority.transaction_count = 0;
      claim_authority.state = LJ_UNIVERSE_BUILDING;
      expected = universe_body_words(NULL, before.incarnation);
      desired = universe_body_words(NULL, claim_incarnation);
      if (before.incarnation != claim_incarnation &&
          !la_cas128(&slot->body_value, &expected, desired) &&
          (expected.lo != desired.lo || expected.hi != desired.hi)) {
        (void)universe_poison_authority(
          slot, &claim_authority,
          UNIVERSE_STATE_BIT(LJ_UNIVERSE_BUILDING), 0, NULL);
        return LJ_UNIVERSE_CORRUPT;
      }
      expected = universe_epoch_words(0, before.incarnation);
      desired = universe_epoch_words(0, claim_incarnation);
      if (before.incarnation != claim_incarnation &&
          !la_cas128(&slot->external_final_publication, &expected, desired) &&
          (expected.lo != desired.lo || expected.hi != desired.hi)) {
        (void)universe_poison_authority(
          slot, &claim_authority,
          UNIVERSE_STATE_BIT(LJ_UNIVERSE_BUILDING), 0, NULL);
        return LJ_UNIVERSE_CORRUPT;
      }
      expected = universe_epoch_words(0, before.incarnation);
      if (before.incarnation != claim_incarnation &&
          !la_cas128(&slot->terminal_final_publication, &expected, desired) &&
          (expected.lo != desired.lo || expected.hi != desired.hi)) {
        (void)universe_poison_authority(
          slot, &claim_authority,
          UNIVERSE_STATE_BIT(LJ_UNIVERSE_BUILDING), 0, NULL);
        return LJ_UNIVERSE_CORRUPT;
      }
      build->key.slot = slot;
      build->key.incarnation = claim_incarnation;
      build->active = 1;
    }
  }
  return result;
}

static void universe_build_consume(LJUniverseBuild *build)
{
  build->active = 0;
  build->key.slot = NULL;
  build->key.incarnation = 0;
}

static LJUniverseResult universe_build_validate(LJUniverseBuild *build,
                                                LJUniverseSnap *before,
                                                LJUniverseSnap *observed)
{
  if (!build || !build->active || !lj_universe_key_valid(&build->key))
    return LJ_UNIVERSE_INVALID;
  *before = lj_universe_snapshot(build->key.slot);
  if (before->incarnation != build->key.incarnation) {
    if (observed) *observed = *before;
    universe_build_consume(build);
    return LJ_UNIVERSE_STALE;
  }
  if (!lj_universe_components_valid(before->incarnation,
                                     before->transaction_count,
                                     before->state)) {
    (void)universe_poison_authority(
      build->key.slot, before, UNIVERSE_STATE_BIT(before->state), 0, NULL);
    if (observed) *observed = *before;
    universe_build_consume(build);
    return LJ_UNIVERSE_CORRUPT;
  }
  if (before->state == LJ_UNIVERSE_POISONED) {
    if (observed) *observed = *before;
    universe_build_consume(build);
    return LJ_UNIVERSE_POISONED_RESULT;
  }
  if (before->state != LJ_UNIVERSE_BUILDING) {
    if (observed) *observed = *before;
    universe_build_consume(build);
    return LJ_UNIVERSE_DENIED;
  }
  if (before->transaction_count != 0) {
    (void)universe_poison_authority(
      build->key.slot, before,
      UNIVERSE_STATE_BIT(LJ_UNIVERSE_BUILDING), 0, NULL);
    if (observed) *observed = *before;
    universe_build_consume(build);
    return LJ_UNIVERSE_CORRUPT;
  }
  return LJ_UNIVERSE_OK;
}

static int universe_build_side_valid(const LJUniverseBuild *build)
{
  return la_load64_acq(&build->key.slot->next_publication) ==
           LJ_UNIVERSE_FIRST_PUBLICATION &&
         universe_epoch_is(
           &build->key.slot->external_final_publication, 0,
           build->key.incarnation) &&
         universe_epoch_is(
           &build->key.slot->terminal_final_publication, 0,
           build->key.incarnation);
}

/* A failed body-decision CAS normally means a copied builder already won. */
static LJUniverseResult universe_build_decision_lost(
  LJUniverseBuild *build, const la_u128 *actual,
  const LJUniverseSnap *authority, LJUniverseSnap *observed)
{
  LJUniverseSnap token = lj_universe_snapshot(build->key.slot);
  LJUniverseBodySnap body = lj_universe_body_snap_from_words(actual->lo,
                                                             actual->hi);
  uint64_t incarnation = build->key.incarnation;
  uint64_t successor = 0;
  LJUniverseResult result;
  if (!universe_successor(incarnation, &successor))
    result = LJ_UNIVERSE_CORRUPT;
  else if (token.incarnation != incarnation)
    result = LJ_UNIVERSE_STALE;
  else if (token.state == LJ_UNIVERSE_POISONED)
    result = LJ_UNIVERSE_POISONED_RESULT;
  else if ((body.body != NULL && body.incarnation == incarnation) ||
           (body.body == NULL && body.incarnation == successor)) {
    /* Non-NULL/current is publish; NULL/successor is the abort marker. */
    result = token.state == LJ_UNIVERSE_BUILDING ?
      LJ_UNIVERSE_LOST : LJ_UNIVERSE_DENIED;
  } else {
    (void)universe_poison_authority(
      build->key.slot, authority,
      UNIVERSE_STATE_BIT(LJ_UNIVERSE_BUILDING), 0, NULL);
    result = LJ_UNIVERSE_CORRUPT;
  }
  if (observed) *observed = token;
  universe_build_consume(build);
  return result;
}

LJUniverseResult lj_universe_try_publish(LJUniverseBuild *build, void *body,
                                         LJUniverseKey *key,
                                         LJUniverseSnap *observed)
{
  LJUniverseSnap before;
  LJUniverseBodySnap body_before;
  la_u128 expected, desired;
  uint64_t successor;
  LJUniverseResult result;
  if (!body || !key)
    return LJ_UNIVERSE_INVALID;
  result = universe_build_validate(build, &before, observed);
  if (result != LJ_UNIVERSE_OK)
    return result;
  if (!universe_successor(build->key.incarnation, &successor)) {
    universe_build_consume(build);
    return LJ_UNIVERSE_CORRUPT;
  }
  if (!universe_build_side_valid(build)) {
    body_before = lj_universe_body_snapshot(build->key.slot);
    if ((body_before.body != NULL &&
         body_before.incarnation == build->key.incarnation) ||
        (body_before.body == NULL &&
         body_before.incarnation == successor)) {
      expected = universe_body_words(body_before.body,
                                     body_before.incarnation);
      return universe_build_decision_lost(
        build, &expected, &before, observed);
    }
    (void)universe_poison_authority(
      build->key.slot, &before,
      UNIVERSE_STATE_BIT(LJ_UNIVERSE_BUILDING), 0, NULL);
    universe_build_consume(build);
    return LJ_UNIVERSE_CORRUPT;
  }
  body_before = lj_universe_body_snapshot(build->key.slot);
  if (body_before.body != NULL ||
      body_before.incarnation != build->key.incarnation) {
    if ((body_before.body != NULL &&
         body_before.incarnation == build->key.incarnation) ||
        (body_before.body == NULL &&
         body_before.incarnation == successor)) {
      expected = universe_body_words(body_before.body,
                                     body_before.incarnation);
      return universe_build_decision_lost(
        build, &expected, &before, observed);
    }
    (void)universe_poison_authority(
      build->key.slot, &before,
      UNIVERSE_STATE_BIT(LJ_UNIVERSE_BUILDING), 0, NULL);
    if (observed) *observed = before;
    universe_build_consume(build);
    return LJ_UNIVERSE_CORRUPT;
  }
  expected = universe_body_words(NULL, body_before.incarnation);
  desired = universe_body_words(body, build->key.incarnation);
  if (!la_cas128(&build->key.slot->body_value, &expected, desired))
    return universe_build_decision_lost(
      build, &expected, &before, observed);
  result = universe_try_replace(build->key.slot, &before,
                                build->key.incarnation, 0,
                                LJ_UNIVERSE_OPEN, observed);
  if (result == LJ_UNIVERSE_OK) {
    *key = build->key;
    universe_build_consume(build);
  } else {
    (void)universe_poison_authority(
      build->key.slot, &before,
      UNIVERSE_STATE_BIT(LJ_UNIVERSE_BUILDING), 0, NULL);
    universe_build_consume(build);
    result = LJ_UNIVERSE_CORRUPT;
  }
  return result;
}

LJUniverseResult lj_universe_try_abort_build(LJUniverseBuild *build,
                                             LJUniverseSnap *observed)
{
  LJUniverseSnap before;
  LJUniverseBodySnap body_before;
  LJUniverseResult result;
  la_u128 expected, desired;
  uint64_t successor;
  result = universe_build_validate(build, &before, observed);
  if (result != LJ_UNIVERSE_OK)
    return result;
  if (!universe_successor(build->key.incarnation, &successor)) {
    universe_build_consume(build);
    return LJ_UNIVERSE_CORRUPT;
  }
  if (!universe_build_side_valid(build)) {
    body_before = lj_universe_body_snapshot(build->key.slot);
    if ((body_before.body != NULL &&
         body_before.incarnation == build->key.incarnation) ||
        (body_before.body == NULL &&
         body_before.incarnation == successor)) {
      expected = universe_body_words(body_before.body,
                                     body_before.incarnation);
      return universe_build_decision_lost(
        build, &expected, &before, observed);
    }
    (void)universe_poison_authority(
      build->key.slot, &before,
      UNIVERSE_STATE_BIT(LJ_UNIVERSE_BUILDING), 0, NULL);
    universe_build_consume(build);
    return LJ_UNIVERSE_CORRUPT;
  }
  body_before = lj_universe_body_snapshot(build->key.slot);
  if (body_before.body != NULL ||
      body_before.incarnation != build->key.incarnation) {
    if ((body_before.body != NULL &&
         body_before.incarnation == build->key.incarnation) ||
        (body_before.body == NULL &&
         body_before.incarnation == successor)) {
      expected = universe_body_words(body_before.body,
                                     body_before.incarnation);
      return universe_build_decision_lost(
        build, &expected, &before, observed);
    }
    (void)universe_poison_authority(
      build->key.slot, &before,
      UNIVERSE_STATE_BIT(LJ_UNIVERSE_BUILDING), 0, NULL);
    if (observed) *observed = before;
    universe_build_consume(build);
    return LJ_UNIVERSE_CORRUPT;
  }
  expected = universe_body_words(NULL, build->key.incarnation);
  desired = universe_body_words(NULL, successor);
  if (!la_cas128(&build->key.slot->body_value, &expected, desired))
    return universe_build_decision_lost(
      build, &expected, &before, observed);
  {
    la_u128 epoch_expected = universe_epoch_words(0,
                                                  build->key.incarnation);
    la_u128 epoch_desired = universe_epoch_words(0, successor);
    if (!la_cas128(&build->key.slot->external_final_publication,
                   &epoch_expected, epoch_desired)) {
      (void)universe_poison_authority(
        build->key.slot, &before,
        UNIVERSE_STATE_BIT(LJ_UNIVERSE_BUILDING), 0, NULL);
      universe_build_consume(build);
      return LJ_UNIVERSE_CORRUPT;
    }
    epoch_expected = universe_epoch_words(0, build->key.incarnation);
    if (!la_cas128(&build->key.slot->terminal_final_publication,
                   &epoch_expected, epoch_desired)) {
      (void)universe_poison_authority(
        build->key.slot, &before,
        UNIVERSE_STATE_BIT(LJ_UNIVERSE_BUILDING), 0, NULL);
      universe_build_consume(build);
      return LJ_UNIVERSE_CORRUPT;
    }
  }
  la_store64_rel(&build->key.slot->next_publication,
                 LJ_UNIVERSE_FIRST_PUBLICATION);
  result = universe_try_replace(build->key.slot, &before,
                                successor, 0,
                                LJ_UNIVERSE_EMPTY, observed);
  if (result != LJ_UNIVERSE_OK) {
    (void)universe_poison_authority(
      build->key.slot, &before,
      UNIVERSE_STATE_BIT(LJ_UNIVERSE_BUILDING), 0, NULL);
    result = LJ_UNIVERSE_CORRUPT;
  }
  universe_build_consume(build);
  return result;
}

static LJUniverseResult universe_reserve_ticket(LJUniverseSlot *slot,
                                                uint64_t *ticket)
{
  uint64_t before, expected;
  if (!slot || !ticket)
    return LJ_UNIVERSE_INVALID;
  before = la_load64_acq(&slot->next_publication);
  for (;;) {
    if (before == 0)
      return LJ_UNIVERSE_CORRUPT;
    if (before == UINT64_MAX)
      return LJ_UNIVERSE_TICKET_EXHAUSTED;
    expected = before;
    if (la_cas64(&slot->next_publication, &expected, before + 1u,
                 LA_ACQ_REL, LA_ACQ)) {
      *ticket = before;
      return LJ_UNIVERSE_OK;
    }
    before = expected;
  }
}

static int universe_release_state_for_kind(uint8_t kind, uint8_t state)
{
  if (state == LJ_UNIVERSE_POISONED)
    return 1;
  if (kind == LJ_UNIVERSE_TXN_EXTERNAL)
    return state == LJ_UNIVERSE_OPEN || state == LJ_UNIVERSE_CLOSING;
  if (kind == LJ_UNIVERSE_TXN_FINALIZER)
    return state == LJ_UNIVERSE_FINALIZING ||
           state == LJ_UNIVERSE_FINAL_DRAIN;
  return 0;
}

static LJUniverseResult universe_release_key(const LJUniverseKey *key,
                                             uint8_t kind,
                                             LJUniverseSnap *observed)
{
  LJUniverseSnap before;
  LJUniverseResult result;
  if (!lj_universe_key_valid(key) ||
      (kind != LJ_UNIVERSE_TXN_EXTERNAL &&
       kind != LJ_UNIVERSE_TXN_FINALIZER))
    return LJ_UNIVERSE_INVALID;
  before = lj_universe_snapshot(key->slot);
  if (before.incarnation != key->incarnation) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_STALE;
  }
  if (!lj_universe_components_valid(before.incarnation,
                                     before.transaction_count,
                                     before.state)) {
    (void)universe_poison_authority(
      key->slot, &before, UNIVERSE_STATE_BIT(before.state), 1, NULL);
    if (observed) *observed = before;
    return LJ_UNIVERSE_CORRUPT;
  }
  if (!universe_release_state_for_kind(kind, before.state)) {
    if (observed) *observed = before;
    return before.state == LJ_UNIVERSE_POISONED ?
      LJ_UNIVERSE_POISONED_RESULT : LJ_UNIVERSE_DENIED;
  }
  if (before.transaction_count == 0) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_CORRUPT;
  }
  result = universe_try_replace(key->slot, &before, before.incarnation,
                                before.transaction_count - 1u,
                                before.state, observed);
  if (result == LJ_UNIVERSE_OK && before.transaction_count == 1u)
    la_fence_acq();
  return result;
}

static LJUniverseResult universe_try_enter_kind(
  const LJUniverseKey *key, const LJUniverseClose *close,
  uint8_t wanted_state, uint8_t kind, LJUniverseTxn *txn,
  LJUniverseSnap *observed)
{
  LJUniverseSnap before, after;
  LJUniverseResult result, release_result;
  void *body;
  uint64_t ticket;
  if (!lj_universe_key_valid(key) || !txn || txn->active ||
      (kind == LJ_UNIVERSE_TXN_FINALIZER &&
       (!close || !close->active ||
        !lj_universe_key_equal(key, &close->key))))
    return LJ_UNIVERSE_INVALID;
  before = lj_universe_snapshot(key->slot);
  if (before.incarnation != key->incarnation) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_STALE;
  }
  if (!lj_universe_components_valid(before.incarnation,
                                     before.transaction_count,
                                     before.state)) {
    (void)universe_poison_authority(
      key->slot, &before, UNIVERSE_STATE_BIT(before.state), 0, NULL);
    if (observed) *observed = before;
    return LJ_UNIVERSE_CORRUPT;
  }
  if (before.state == LJ_UNIVERSE_POISONED) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_POISONED_RESULT;
  }
  if (before.state != wanted_state) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_DENIED;
  }
  if (before.transaction_count == LJ_UNIVERSE_MAX_TRANSACTIONS) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_SATURATED;
  }
  result = universe_try_replace(key->slot, &before, before.incarnation,
                                before.transaction_count + 1u,
                                before.state, &after);
  if (result != LJ_UNIVERSE_OK) {
    if (observed) *observed = after;
    return result;
  }
  if (!universe_live_body_is(key->slot, key->incarnation, &body) ||
      (close && body != close->body)) {
    (void)universe_poison_authority(
      key->slot, &after,
      kind == LJ_UNIVERSE_TXN_EXTERNAL ?
        (UNIVERSE_STATE_BIT(LJ_UNIVERSE_OPEN) |
         UNIVERSE_STATE_BIT(LJ_UNIVERSE_CLOSING)) :
        (UNIVERSE_STATE_BIT(LJ_UNIVERSE_FINALIZING) |
         UNIVERSE_STATE_BIT(LJ_UNIVERSE_FINAL_DRAIN)),
      1, NULL);
    do {
      release_result = universe_release_key(key, kind, &after);
    } while (release_result == LJ_UNIVERSE_LOST);
    if (observed) *observed = after;
    return LJ_UNIVERSE_CORRUPT;
  }
  result = universe_reserve_ticket(key->slot, &ticket);
  if (result != LJ_UNIVERSE_OK) {
    (void)universe_poison_authority(
      key->slot, &after,
      kind == LJ_UNIVERSE_TXN_EXTERNAL ?
        (UNIVERSE_STATE_BIT(LJ_UNIVERSE_OPEN) |
         UNIVERSE_STATE_BIT(LJ_UNIVERSE_CLOSING)) :
        (UNIVERSE_STATE_BIT(LJ_UNIVERSE_FINALIZING) |
         UNIVERSE_STATE_BIT(LJ_UNIVERSE_FINAL_DRAIN)),
      1, NULL);
    do {
      release_result = universe_release_key(key, kind, &after);
    } while (release_result == LJ_UNIVERSE_LOST);
    if (observed) *observed = after;
    return result;
  }
  txn->key = *key;
  txn->body = body;
  txn->publication_ticket = ticket;
  txn->kind = kind;
  txn->active = 1;
  if (observed) *observed = after;
  return LJ_UNIVERSE_OK;
}

LJUniverseResult lj_universe_try_enter(const LJUniverseKey *key,
                                       LJUniverseTxn *txn,
                                       LJUniverseSnap *observed)
{
  return universe_try_enter_kind(key, NULL, LJ_UNIVERSE_OPEN,
                                 LJ_UNIVERSE_TXN_EXTERNAL, txn, observed);
}

LJUniverseResult lj_universe_try_enter_finalizer(
  const LJUniverseClose *close, LJUniverseTxn *txn,
  LJUniverseSnap *observed)
{
  if (!close)
    return LJ_UNIVERSE_INVALID;
  return universe_try_enter_kind(&close->key, close,
                                 LJ_UNIVERSE_FINALIZING,
                                 LJ_UNIVERSE_TXN_FINALIZER, txn, observed);
}

LJUniverseResult lj_universe_txn_release(LJUniverseTxn *txn,
                                         LJUniverseSnap *observed)
{
  LJUniverseResult result;
  if (!txn || !txn->active || txn->publication_ticket == 0)
    return LJ_UNIVERSE_INVALID;
  result = universe_release_key(&txn->key, txn->kind, observed);
  if (result == LJ_UNIVERSE_OK) {
    txn->active = 0;
    txn->kind = LJ_UNIVERSE_TXN_NONE;
    txn->body = NULL;
    txn->publication_ticket = 0;
  }
  return result;
}

LJUniverseResult lj_universe_try_close(const LJUniverseKey *key,
                                       LJUniverseClose *close,
                                       LJUniverseSnap *observed)
{
  LJUniverseSnap before, after;
  LJUniverseResult result;
  void *body;
  if (!lj_universe_key_valid(key) || !close || close->active)
    return LJ_UNIVERSE_INVALID;
  before = lj_universe_snapshot(key->slot);
  if (before.incarnation != key->incarnation) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_STALE;
  }
  if (!lj_universe_components_valid(before.incarnation,
                                     before.transaction_count,
                                     before.state)) {
    (void)universe_poison_authority(
      key->slot, &before, UNIVERSE_STATE_BIT(before.state), 0, NULL);
    if (observed) *observed = before;
    return LJ_UNIVERSE_CORRUPT;
  }
  if (before.state == LJ_UNIVERSE_POISONED) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_POISONED_RESULT;
  }
  if (before.state != LJ_UNIVERSE_OPEN) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_DENIED;
  }
  result = universe_try_replace(key->slot, &before, before.incarnation,
                                before.transaction_count,
                                LJ_UNIVERSE_CLOSING, &after);
  if (result == LJ_UNIVERSE_OK) {
    if (!universe_live_body_is(key->slot, key->incarnation, &body)) {
      (void)universe_poison_authority(
        key->slot, &after, UNIVERSE_STATE_BIT(LJ_UNIVERSE_CLOSING), 1,
        NULL);
      if (observed) *observed = after;
      return LJ_UNIVERSE_CORRUPT;
    }
    close->key = *key;
    close->body = body;
    close->external_final_publication = 0;
    close->terminal_final_publication = 0;
    close->active = 1;
  }
  if (observed) *observed = after;
  return result;
}

static LJUniverseResult universe_close_validate(
  LJUniverseClose *close, uint8_t state, LJUniverseSnap *before,
  LJUniverseSnap *observed)
{
  if (!close || !close->active || !lj_universe_key_valid(&close->key) ||
      !close->body)
    return LJ_UNIVERSE_INVALID;
  *before = lj_universe_snapshot(close->key.slot);
  if (before->incarnation != close->key.incarnation) {
    if (observed) *observed = *before;
    return LJ_UNIVERSE_STALE;
  }
  if (!lj_universe_components_valid(before->incarnation,
                                     before->transaction_count,
                                     before->state)) {
    (void)universe_poison_authority(
      close->key.slot, before, UNIVERSE_STATE_BIT(before->state), 1, NULL);
    if (observed) *observed = *before;
    return LJ_UNIVERSE_CORRUPT;
  }
  if (before->state == LJ_UNIVERSE_POISONED) {
    if (observed) *observed = *before;
    return LJ_UNIVERSE_POISONED_RESULT;
  }
  if (before->state != state) {
    if (observed) *observed = *before;
    return LJ_UNIVERSE_DENIED;
  }
  if (!universe_body_is(close->key.slot, close->body,
                        close->key.incarnation)) {
    LJUniverseBodySnap body = lj_universe_body_snapshot(close->key.slot);
    LJUniverseSnap recheck = lj_universe_snapshot(close->key.slot);
    uint64_t successor = close->key.incarnation;
    (void)universe_successor(close->key.incarnation, &successor);
    if (state == LJ_UNIVERSE_SEALED && body.body == NULL &&
        body.incarnation == successor) {
      if (observed) *observed = recheck;
      return LJ_UNIVERSE_LOST;
    }
    if (!lj_universe_snap_equal(before, &recheck)) {
      if (observed) *observed = recheck;
      return recheck.incarnation == close->key.incarnation ?
        LJ_UNIVERSE_LOST : LJ_UNIVERSE_STALE;
    }
    (void)universe_poison_authority(
      close->key.slot, before, UNIVERSE_STATE_BIT(before->state), 1, NULL);
    if (observed) *observed = *before;
    return LJ_UNIVERSE_CORRUPT;
  }
  return LJ_UNIVERSE_OK;
}

static int universe_epoch_snap_is(const LJUniverseEpochSnap *snap,
                                  uint64_t epoch, uint64_t incarnation)
{
  return snap && snap->epoch == epoch && snap->incarnation == incarnation;
}

/*
** The epoch marker is the exact per-stage decision. A copied close handle
** which sees the identical marker loses harmlessly; a stale incarnation can
** never match the reset successor tag. Malformed side data poisons only while
** the complete original stage token still holds.
*/
static LJUniverseResult universe_try_mark_epoch(
  LJUniverseClose *close, la_u128 *field, uint64_t next,
  const LJUniverseSnap *stage, uint8_t completed_state,
  LJUniverseSnap *observed)
{
  la_u128 expected = universe_epoch_words(0, close->key.incarnation);
  la_u128 desired = universe_epoch_words(next, close->key.incarnation);
  LJUniverseEpochSnap actual;
  LJUniverseSnap current;
  if (!field || !stage || next == 0)
    return LJ_UNIVERSE_CORRUPT;
  if (la_cas128(field, &expected, desired)) {
#if defined(LJ_UNIVERSE_TEST_HELPERS)
    if (universe_stage_test_hook)
      universe_stage_test_hook(
        completed_state == LJ_UNIVERSE_FINALIZING ?
          LJ_UNIVERSE_TEST_AFTER_EXTERNAL_MARKER :
          LJ_UNIVERSE_TEST_AFTER_TERMINAL_MARKER,
        universe_stage_test_ud);
#endif
    return LJ_UNIVERSE_OK;
  }
  actual.epoch = expected.lo;
  actual.incarnation = expected.hi;
  current = lj_universe_snapshot(close->key.slot);
  if (observed) *observed = current;
  if (current.incarnation != close->key.incarnation)
    return LJ_UNIVERSE_STALE;
  if (current.state == LJ_UNIVERSE_POISONED)
    return LJ_UNIVERSE_POISONED_RESULT;
  if (current.state != stage->state) {
    /* Includes a legal completed/later stage and recycle-in-progress. */
    return current.state == completed_state &&
           universe_epoch_snap_is(&actual, next,
                                  close->key.incarnation) ?
      LJ_UNIVERSE_DENIED : LJ_UNIVERSE_LOST;
  }
  if (!lj_universe_snap_equal(&current, stage))
    return LJ_UNIVERSE_LOST;
  if (universe_epoch_snap_is(&actual, next, close->key.incarnation))
    return LJ_UNIVERSE_OK;  /* Help the suspended identical-marker winner. */
  (void)universe_poison_authority(
    close->key.slot, stage, UNIVERSE_STATE_BIT(stage->state), 0, NULL);
  return LJ_UNIVERSE_CORRUPT;
}

static LJUniverseResult universe_close_sync_external(
  LJUniverseClose *close, uint64_t *epoch)
{
  LJUniverseEpochSnap snap = lj_universe_external_epoch_snapshot(
    close->key.slot);
  if (snap.incarnation != close->key.incarnation || snap.epoch == 0)
    return LJ_UNIVERSE_CORRUPT;
  if (close->external_final_publication != 0 &&
      close->external_final_publication != snap.epoch)
    return LJ_UNIVERSE_INVALID;
  close->external_final_publication = snap.epoch;
  if (epoch) *epoch = snap.epoch;
  return LJ_UNIVERSE_OK;
}

static LJUniverseResult universe_close_sync_terminal(
  LJUniverseClose *close, uint64_t *epoch)
{
  LJUniverseEpochSnap snap = lj_universe_terminal_epoch_snapshot(
    close->key.slot);
  if (snap.incarnation != close->key.incarnation || snap.epoch == 0)
    return LJ_UNIVERSE_CORRUPT;
  if (close->terminal_final_publication != 0 &&
      close->terminal_final_publication != snap.epoch)
    return LJ_UNIVERSE_INVALID;
  close->terminal_final_publication = snap.epoch;
  if (epoch) *epoch = snap.epoch;
  return LJ_UNIVERSE_OK;
}

LJUniverseResult lj_universe_close_freeze_external(
  LJUniverseClose *close, uint64_t *epoch, LJUniverseSnap *observed)
{
  LJUniverseSnap before, after;
  LJUniverseResult result;
  uint64_t next;
  result = universe_close_validate(close, LJ_UNIVERSE_CLOSING, &before,
                                   observed);
  if (result != LJ_UNIVERSE_OK)
    return result;
  if (before.transaction_count != 0) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_BUSY;
  }
  next = la_load64_acq(&close->key.slot->next_publication);
  if (next == 0) {
    (void)universe_poison_authority(
      close->key.slot, &before, UNIVERSE_STATE_BIT(LJ_UNIVERSE_CLOSING), 0,
      NULL);
    return LJ_UNIVERSE_CORRUPT;
  }
#if defined(LJ_UNIVERSE_TEST_HELPERS)
  if (universe_stage_test_hook)
    universe_stage_test_hook(LJ_UNIVERSE_TEST_FREEZE_EXTERNAL,
                             universe_stage_test_ud);
#endif
  result = universe_try_mark_epoch(
    close, &close->key.slot->external_final_publication, next, &before,
    LJ_UNIVERSE_FINALIZING, observed);
  if (result != LJ_UNIVERSE_OK) {
    if ((result == LJ_UNIVERSE_LOST || result == LJ_UNIVERSE_DENIED) &&
        universe_close_sync_external(close, epoch) == LJ_UNIVERSE_OK)
      return result;
    return result;
  }
  result = universe_try_replace(close->key.slot, &before,
                                before.incarnation, 0,
                                LJ_UNIVERSE_FINALIZING, &after);
  if (result != LJ_UNIVERSE_OK) {
    if (observed) *observed = after;
    if (after.incarnation != close->key.incarnation)
      return LJ_UNIVERSE_STALE;
    if (after.state == LJ_UNIVERSE_POISONED)
      return LJ_UNIVERSE_POISONED_RESULT;
    if (after.state == LJ_UNIVERSE_FINALIZING) {
      (void)universe_close_sync_external(close, epoch);
      return LJ_UNIVERSE_DENIED;
    }
    return LJ_UNIVERSE_LOST;
  }
  close->external_final_publication = next;
  if (epoch) *epoch = next;
  if (observed) *observed = after;
  return LJ_UNIVERSE_OK;
}

LJUniverseResult lj_universe_close_begin_final_drain(
  LJUniverseClose *close, LJUniverseSnap *observed)
{
  LJUniverseSnap before;
  LJUniverseResult result = universe_close_validate(
    close, LJ_UNIVERSE_FINALIZING, &before, observed);
  if (result != LJ_UNIVERSE_OK)
    return result;
  result = universe_close_sync_external(close, NULL);
  if (result != LJ_UNIVERSE_OK) {
    if (result == LJ_UNIVERSE_INVALID)
      return LJ_UNIVERSE_CORRUPT;
    LJUniverseSnap recheck = lj_universe_snapshot(close->key.slot);
    if (!lj_universe_snap_equal(&before, &recheck)) {
      if (observed) *observed = recheck;
      return recheck.incarnation == close->key.incarnation ?
        LJ_UNIVERSE_LOST : LJ_UNIVERSE_STALE;
    }
    (void)universe_poison_authority(
      close->key.slot, &before,
      UNIVERSE_STATE_BIT(LJ_UNIVERSE_FINALIZING), 1, NULL);
    return result;
  }
  return universe_try_replace(close->key.slot, &before,
                              before.incarnation,
                              before.transaction_count,
                              LJ_UNIVERSE_FINAL_DRAIN, observed);
}

LJUniverseResult lj_universe_close_seal(LJUniverseClose *close,
                                        uint64_t *epoch,
                                        LJUniverseSnap *observed)
{
  LJUniverseSnap before, after;
  LJUniverseResult result;
  uint64_t next;
  result = universe_close_validate(close, LJ_UNIVERSE_FINAL_DRAIN, &before,
                                   observed);
  if (result != LJ_UNIVERSE_OK)
    return result;
  if (before.transaction_count != 0) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_BUSY;
  }
  next = la_load64_acq(&close->key.slot->next_publication);
  result = universe_close_sync_external(close, NULL);
  if (result == LJ_UNIVERSE_INVALID)
    return LJ_UNIVERSE_CORRUPT;
  if (result != LJ_UNIVERSE_OK || next == 0 ||
      next < close->external_final_publication) {
    LJUniverseSnap recheck = lj_universe_snapshot(close->key.slot);
    if (!lj_universe_snap_equal(&before, &recheck)) {
      if (observed) *observed = recheck;
      return recheck.incarnation == close->key.incarnation ?
        LJ_UNIVERSE_LOST : LJ_UNIVERSE_STALE;
    }
    (void)universe_poison_authority(
      close->key.slot, &before,
      UNIVERSE_STATE_BIT(LJ_UNIVERSE_FINAL_DRAIN), 0, NULL);
    return LJ_UNIVERSE_CORRUPT;
  }
#if defined(LJ_UNIVERSE_TEST_HELPERS)
  if (universe_stage_test_hook)
    universe_stage_test_hook(LJ_UNIVERSE_TEST_FREEZE_TERMINAL,
                             universe_stage_test_ud);
#endif
  result = universe_try_mark_epoch(
    close, &close->key.slot->terminal_final_publication, next, &before,
    LJ_UNIVERSE_SEALED, observed);
  if (result != LJ_UNIVERSE_OK) {
    if ((result == LJ_UNIVERSE_LOST || result == LJ_UNIVERSE_DENIED) &&
        universe_close_sync_terminal(close, epoch) == LJ_UNIVERSE_OK)
      return result;
    return result;
  }
  result = universe_try_replace(close->key.slot, &before,
                                before.incarnation, 0,
                                LJ_UNIVERSE_SEALED, &after);
  if (result != LJ_UNIVERSE_OK) {
    if (observed) *observed = after;
    if (after.incarnation != close->key.incarnation)
      return LJ_UNIVERSE_STALE;
    if (after.state == LJ_UNIVERSE_POISONED)
      return LJ_UNIVERSE_POISONED_RESULT;
    if (after.state == LJ_UNIVERSE_SEALED) {
      (void)universe_close_sync_terminal(close, epoch);
      return LJ_UNIVERSE_DENIED;
    }
    return LJ_UNIVERSE_LOST;
  }
  close->terminal_final_publication = next;
  if (epoch) *epoch = next;
  if (observed) *observed = after;
  return LJ_UNIVERSE_OK;
}

LJUniverseResult lj_universe_close_recycle(LJUniverseClose *close,
                                           LJUniverseSnap *observed)
{
  LJUniverseSnap before, after;
  LJUniverseEpochSnap external, terminal;
  LJUniverseBodySnap actual_body;
  LJUniverseSlot *slot;
  LJUniverseResult result;
  la_u128 expected, desired;
  uint8_t terminal_state;
  uint64_t successor, next;
  if (!close || !close->active || !close->key.slot)
    return LJ_UNIVERSE_INVALID;
  slot = close->key.slot;
  result = universe_close_validate(close, LJ_UNIVERSE_SEALED, &before,
                                   observed);
  if (result != LJ_UNIVERSE_OK)
    return result;
  successor = close->key.incarnation;
  terminal_state = LJ_UNIVERSE_EXHAUSTED;
  if (universe_successor(close->key.incarnation, &successor))
    terminal_state = LJ_UNIVERSE_EMPTY;
  external = lj_universe_external_epoch_snapshot(slot);
  terminal = lj_universe_terminal_epoch_snapshot(slot);
  next = la_load64_acq(&slot->next_publication);
  if (before.transaction_count != 0 || external.epoch == 0 ||
      external.incarnation != close->key.incarnation ||
      terminal.epoch == 0 ||
      terminal.incarnation != close->key.incarnation ||
      terminal.epoch < external.epoch || next != terminal.epoch) {
    LJUniverseBodySnap body = lj_universe_body_snapshot(slot);
    LJUniverseSnap recheck = lj_universe_snapshot(slot);
    if (body.body == NULL && body.incarnation == successor) {
      if (observed) *observed = recheck;
      return recheck.incarnation == close->key.incarnation ?
        LJ_UNIVERSE_LOST : LJ_UNIVERSE_STALE;
    }
    if (!lj_universe_snap_equal(&before, &recheck)) {
      if (observed) *observed = recheck;
      return recheck.incarnation == close->key.incarnation ?
        LJ_UNIVERSE_LOST : LJ_UNIVERSE_STALE;
    }
    (void)universe_poison_authority(
      slot, &before, UNIVERSE_STATE_BIT(LJ_UNIVERSE_SEALED), 0, NULL);
    return LJ_UNIVERSE_CORRUPT;
  }
  if ((close->external_final_publication != 0 &&
       close->external_final_publication != external.epoch) ||
      (close->terminal_final_publication != 0 &&
       close->terminal_final_publication != terminal.epoch))
    return LJ_UNIVERSE_CORRUPT;
  close->external_final_publication = external.epoch;
  close->terminal_final_publication = terminal.epoch;
#if defined(LJ_UNIVERSE_TEST_HELPERS)
  if (universe_stage_test_hook)
    universe_stage_test_hook(LJ_UNIVERSE_TEST_RECYCLE,
                             universe_stage_test_ud);
#endif
  expected = universe_body_words(close->body, close->key.incarnation);
  desired = universe_body_words(NULL, successor);
  if (!la_cas128(&slot->body_value, &expected, desired)) {
    actual_body = lj_universe_body_snap_from_words(expected.lo, expected.hi);
    after = lj_universe_snapshot(slot);
    if (observed) *observed = after;
    if (actual_body.body == NULL &&
        actual_body.incarnation == successor)
      return after.incarnation == close->key.incarnation ?
        LJ_UNIVERSE_LOST : LJ_UNIVERSE_STALE;
    if (!lj_universe_snap_equal(&before, &after))
      return after.incarnation == close->key.incarnation ?
        LJ_UNIVERSE_LOST : LJ_UNIVERSE_STALE;
    (void)universe_poison_authority(
      slot, &before, UNIVERSE_STATE_BIT(LJ_UNIVERSE_SEALED), 0, NULL);
    return LJ_UNIVERSE_CORRUPT;
  }
  {
    LJUniverseEpochSnap ext_recheck =
      lj_universe_external_epoch_snapshot(slot);
    LJUniverseEpochSnap term_recheck =
      lj_universe_terminal_epoch_snapshot(slot);
    uint64_t next_recheck = la_load64_acq(
      &slot->next_publication);
    if (!universe_epoch_snap_is(&ext_recheck, external.epoch,
                                close->key.incarnation) ||
        !universe_epoch_snap_is(&term_recheck, terminal.epoch,
                                close->key.incarnation) ||
        next_recheck != terminal.epoch) {
      (void)universe_poison_authority(
        slot, &before, UNIVERSE_STATE_BIT(LJ_UNIVERSE_SEALED), 0, NULL);
      return LJ_UNIVERSE_CORRUPT;
    }
  }
  expected = universe_epoch_words(external.epoch,
                                  close->key.incarnation);
  desired = universe_epoch_words(0, successor);
  if (!la_cas128(&slot->external_final_publication,
                 &expected, desired)) {
    (void)universe_poison_authority(
      slot, &before, UNIVERSE_STATE_BIT(LJ_UNIVERSE_SEALED), 0, NULL);
    return LJ_UNIVERSE_CORRUPT;
  }
  expected = universe_epoch_words(terminal.epoch,
                                  close->key.incarnation);
  if (!la_cas128(&slot->terminal_final_publication,
                 &expected, desired)) {
    (void)universe_poison_authority(
      slot, &before, UNIVERSE_STATE_BIT(LJ_UNIVERSE_SEALED), 0, NULL);
    return LJ_UNIVERSE_CORRUPT;
  }
  la_store64_rel(&slot->next_publication,
                 LJ_UNIVERSE_FIRST_PUBLICATION);
  result = universe_try_replace(slot, &before,
                                successor, 0,
                                terminal_state, &after);
  if (result != LJ_UNIVERSE_OK) {
    if (observed) *observed = after;
    if (after.incarnation != close->key.incarnation)
      return LJ_UNIVERSE_STALE;
    if (after.state == LJ_UNIVERSE_POISONED)
      return LJ_UNIVERSE_POISONED_RESULT;
    return LJ_UNIVERSE_LOST;
  }
  close->active = 0;
  close->body = NULL;
  if (observed) *observed = after;
  return LJ_UNIVERSE_OK;
}

LJUniverseResult lj_universe_try_poison(const LJUniverseKey *key,
                                        LJUniverseSnap *observed)
{
  LJUniverseSnap before;
  if (!lj_universe_key_valid(key))
    return LJ_UNIVERSE_INVALID;
  before = lj_universe_snapshot(key->slot);
  if (before.incarnation != key->incarnation) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_STALE;
  }
  if (before.state == LJ_UNIVERSE_EMPTY ||
      before.state == LJ_UNIVERSE_BUILDING ||
      before.state == LJ_UNIVERSE_EXHAUSTED) {
    if (observed) *observed = before;
    return LJ_UNIVERSE_DENIED;
  }
  return universe_poison_snapshot(key->slot, &before, observed);
}
