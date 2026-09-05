/*
** t-tgregistry-slot.c - stable external TG registry-slot model.
**
** Build & run:
**   cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -mcx16 -Isrc \
**      tests/t-tgregistry-slot.c -o /tmp/t-tgregistry-slot && \
**      /tmp/t-tgregistry-slot
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_tgregistry.h"

typedef struct TestBody {
  uint64_t incarnation;
  uint64_t canary;
  uint32_t reclaimed;
} TestBody;

static LJTGSlotResult retry_publish(const LJTGRegistryKey *key,
                                    LJTGSlotSnap *snap)
{
  LJTGSlotResult result;
  do {
    result = lj_tgregistry_try_publish(key, snap);
  } while (result == LJ_TGSLOT_LOST);
  return result;
}

static LJTGSlotResult retry_detach(const LJTGRegistryKey *key,
                                   LJTGSlotSnap *snap)
{
  LJTGSlotResult result;
  do {
    result = lj_tgregistry_try_detach(key, snap);
  } while (result == LJ_TGSLOT_LOST);
  return result;
}

static LJTGSlotResult retry_retire(const LJTGRegistryKey *key,
                                   LJTGSlotSnap *snap)
{
  LJTGSlotResult result;
  do {
    result = lj_tgregistry_try_retire(key, snap);
  } while (result == LJ_TGSLOT_LOST);
  return result;
}

static LJTGSlotResult retry_abort(const LJTGRegistryKey *key,
                                  LJTGSlotSnap *snap)
{
  LJTGSlotResult result;
  do {
    result = lj_tgregistry_try_abort_attach(key, snap);
  } while (result == LJ_TGSLOT_LOST);
  return result;
}

static LJTGSlotResult retry_reclaim(const LJTGRegistryKey *key, void **body,
                                    LJTGSlotSnap *snap)
{
  LJTGSlotResult result;
  do {
    result = lj_tgregistry_try_reclaim(key, body, snap);
  } while (result == LJ_TGSLOT_LOST);
  return result;
}

static LJTGSlotResult retry_clear(const LJTGRegistryKey *key,
                                  LJTGSlotSnap *snap)
{
  LJTGSlotResult result;
  do {
    result = lj_tgregistry_try_clear(key, snap);
  } while (result == LJ_TGSLOT_LOST);
  return result;
}

static LJTGSlotResult retry_borrow(const LJTGRegistryKey *key,
                                   LJTGRegistryBorrow *borrow,
                                   LJTGSlotSnap *snap)
{
  LJTGSlotResult result;
  do {
    result = lj_tgregistry_try_borrow(key, borrow, snap);
  } while (result == LJ_TGSLOT_LOST);
  return result;
}

static LJTGSlotResult retry_release(LJTGRegistryBorrow *borrow,
                                    LJTGSlotSnap *snap)
{
  LJTGSlotResult result;
  do {
    result = lj_tgregistry_try_release(borrow, snap);
  } while (result == LJ_TGSLOT_LOST);
  return result;
}

static void claim_and_publish(LJTGRegistrySlot *slot, TestBody *body,
                              LJTGRegistryKey *key)
{
  LJTGSlotSnap snap;
  assert(lj_tgregistry_try_claim(slot, key, &snap) == LJ_TGSLOT_OK);
  body->incarnation = key->incarnation;
  body->canary = UINT64_C(0x7467726567697374) ^ key->incarnation;
  body->reclaimed = 0;
  assert(lj_tgregistry_try_publish_body(key, body, &snap) ==
         LJ_TGSLOT_OK);
  assert(retry_publish(key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_LIVE && snap.lease_count == 1);
}

static void test_body_publication_and_stable_chain(void)
{
  LJTGRegistrySlot tail, head;
  LJTGRegistryKey claimed, discovered;
  LJTGRegistryBorrow borrow;
  LJTGSlotSnap snap;
  TestBody body, other;
  void *snapshot = NULL;

  assert(lj_tgregistry_slot_init_unpublished(&tail, 0, NULL));
  assert(lj_tgregistry_slot_init_unpublished(&head, 10, &tail));
  assert(lj_tgregistry_slot_next_all(&head) == &tail);
  assert(lj_tgregistry_slot_next_all(&tail) == NULL);
  assert(lj_tgregistry_try_claim(&head, &claimed, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_ATTACHING && snap.lease_count == 1);

  /* The stable node may already be on the immutable spine when a fresh
  ** incarnation is claimed. Its rootless publication gap is transient BUSY,
  ** never a reason to pin the slot. */
  assert(lj_tgregistry_try_borrowable_key(&head, &discovered, &snap) ==
         LJ_TGSLOT_BUSY);
  assert(snap.state == LJ_TGSLOT_ATTACHING && snap.lease_count == 1);
  lj_tgregistry_borrow_init(&borrow);
  assert(lj_tgregistry_try_borrow(&claimed, &borrow, &snap) ==
         LJ_TGSLOT_BUSY);
  assert(!borrow.active);
  assert(snap.state == LJ_TGSLOT_ATTACHING && snap.lease_count == 1);
  assert(lj_tgregistry_try_publish(&claimed, &snap) == LJ_TGSLOT_INVALID);

  body.incarnation = claimed.incarnation;
  body.canary = UINT64_C(0x1122334455667788);
  body.reclaimed = 0;
  assert(lj_tgregistry_try_publish_body(&claimed, &body, &snap) ==
         LJ_TGSLOT_OK);
  assert(lj_tgregistry_try_publish_body(&claimed, &body, &snap) ==
         LJ_TGSLOT_OK);  /* Idempotent owner retry. */
  assert(lj_tgregistry_try_publish_body(&claimed, &other, &snap) ==
         LJ_TGSLOT_INVALID);

  /* A linked, initialized ATTACHING slot is already borrowable for catch-up. */
  assert(lj_tgregistry_try_borrowable_key(&head, &discovered, &snap) ==
         LJ_TGSLOT_OK);
  assert(lj_tgregistry_key_equal(&claimed, &discovered));
  assert(retry_borrow(&discovered, &borrow, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_ATTACHING && snap.lease_count == 2);
  assert(lj_tgregistry_try_body_snapshot(&borrow, &snapshot, &snap) ==
         LJ_TGSLOT_OK);
  assert(snapshot == &body && ((TestBody *)snapshot)->canary == body.canary);
  assert(retry_release(&borrow, &snap) == LJ_TGSLOT_OK);

  assert(retry_publish(&claimed, &snap) == LJ_TGSLOT_OK);
  assert(lj_tgregistry_try_borrowable_key(&head, &discovered, &snap) ==
         LJ_TGSLOT_OK);
  assert(lj_tgregistry_slot_next_all(&head) == &tail);
}

static void test_attach_abort_with_attaching_borrow(void)
{
  LJTGRegistrySlot slot;
  LJTGRegistryKey key;
  LJTGRegistryBorrow borrow, denied;
  LJTGSlotSnap snap;
  TestBody body;
  void *snapshot = NULL, *reclaim_body = NULL;

  assert(lj_tgregistry_slot_init_unpublished(&slot, 30, NULL));
  assert(lj_tgregistry_try_claim(&slot, &key, &snap) == LJ_TGSLOT_OK);
  body.incarnation = key.incarnation;
  body.canary = UINT64_C(0xa55aa55aa55aa55a);
  body.reclaimed = 0;
  assert(lj_tgregistry_try_publish_body(&key, &body, &snap) ==
         LJ_TGSLOT_OK);
  lj_tgregistry_borrow_init(&borrow);
  assert(retry_borrow(&key, &borrow, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_ATTACHING && snap.lease_count == 2);

  assert(retry_abort(&key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_RETIRED && snap.lease_count == 2);
  assert(lj_tgregistry_try_body_snapshot(&borrow, &snapshot, &snap) ==
         LJ_TGSLOT_OK);
  assert(snapshot == &body);
  lj_tgregistry_borrow_init(&denied);
  assert(lj_tgregistry_try_borrow(&key, &denied, &snap) ==
         LJ_TGSLOT_DENIED);
  assert(lj_tgregistry_try_reclaim(&key, &reclaim_body, &snap) ==
         LJ_TGSLOT_BUSY);
  assert(retry_release(&borrow, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_RETIRED && snap.lease_count == 1);
  assert(retry_reclaim(&key, &reclaim_body, &snap) == LJ_TGSLOT_OK);
  assert(reclaim_body == &body);
  assert(snap.state == LJ_TGSLOT_RECLAIMING && snap.lease_count == 0);
  body.reclaimed = 1;
  assert(retry_clear(&key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_EMPTY && snap.lease_count == 0);
  assert(lj_tgregistry_slot_body_acq(&slot) == NULL);
}

typedef struct ClearObserve {
  LJTGRegistrySlot *slot;
  uint32_t go;
  uint32_t saw_empty;
  uint32_t bad_body;
} ClearObserve;

static void *clear_observer(void *ud)
{
  ClearObserve *observe = (ClearObserve *)ud;
  while (!la_load32_acq(&observe->go))
    la_cpu_pause();
  for (;;) {
    LJTGSlotSnap snap = lj_tgslot_snapshot(&observe->slot->token);
    if (snap.state == LJ_TGSLOT_EMPTY) {
      LJTGRegistryBodySnap body =
        lj_tgregistry_slot_body_snapshot(observe->slot);
      if (body.body != NULL || body.incarnation != snap.incarnation)
        la_store32_rel(&observe->bad_body, 1);
      la_store32_rel(&observe->saw_empty, 1);
      return NULL;
    }
    assert(snap.state == LJ_TGSLOT_RECLAIMING);
    la_cpu_pause();
  }
}

static void test_clear_before_empty_and_stale_reuse(void)
{
  LJTGRegistrySlot slot;
  LJTGRegistryKey first, second;
  LJTGRegistryBorrow borrow;
  LJTGRegistryBodySnap captured;
  LJTGSlotSnap snap;
  TestBody reused_storage;
  ClearObserve observe;
  pthread_t observer;
  void *body = NULL;

  memset(&observe, 0, sizeof(observe));
  assert(lj_tgregistry_slot_init_unpublished(&slot, 90, NULL));
  claim_and_publish(&slot, &reused_storage, &first);
  assert(retry_detach(&first, &snap) == LJ_TGSLOT_OK);
  assert(retry_retire(&first, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_RETIRED && snap.lease_count == 1);
  assert(retry_reclaim(&first, &body, &snap) == LJ_TGSLOT_OK);
  assert(body == &reused_storage);
  captured = lj_tgregistry_slot_body_snapshot(&slot);

  observe.slot = &slot;
  assert(pthread_create(&observer, NULL, clear_observer, &observe) == 0);
  la_store32_rel(&observe.go, 1);
  assert(retry_clear(&first, &snap) == LJ_TGSLOT_OK);
  assert(pthread_join(observer, NULL) == 0);
  assert(la_load32_acq(&observe.saw_empty) == 1);
  assert(la_load32_acq(&observe.bad_body) == 0);
  assert(lj_tgregistry_try_clear_captured(&first, &captured, &snap) ==
         LJ_TGSLOT_OK);  /* A captured loser resumes after EMPTY publication. */
  assert(retry_clear(&first, &snap) == LJ_TGSLOT_OK);  /* Idempotent. */
  assert(lj_tgregistry_try_clear(&first, NULL) == LJ_TGSLOT_OK);

  /* Reuse the exact slot and body address under a fresh incarnation. */
  claim_and_publish(&slot, &reused_storage, &second);
  assert(second.slot == first.slot);
  assert(second.incarnation == first.incarnation + 1u);
  assert(reused_storage.incarnation == second.incarnation);
  lj_tgregistry_borrow_init(&borrow);
  assert(lj_tgregistry_try_borrow(&first, &borrow, &snap) ==
         LJ_TGSLOT_STALE);
  assert(lj_tgregistry_try_publish(&first, &snap) == LJ_TGSLOT_STALE);
  assert(lj_tgregistry_try_retire(&first, &snap) == LJ_TGSLOT_STALE);
  assert(lj_tgregistry_try_reclaim(&first, &body, &snap) ==
         LJ_TGSLOT_STALE);
  assert(lj_tgregistry_try_clear(&first, &snap) == LJ_TGSLOT_STALE);
}

typedef struct DelayedClear {
  LJTGRegistryKey key;
  LJTGRegistryBodySnap captured;
  LJTGSlotSnap observed;
  uint32_t ready;
  uint32_t go;
  int32_t result;
} DelayedClear;

static void *delayed_old_clearer(void *ud)
{
  DelayedClear *delayed = (DelayedClear *)ud;
  LJTGSlotSnap snap;
  assert(lj_tgregistry_key_snapshot(&delayed->key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_RECLAIMING && snap.lease_count == 0);
  delayed->captured =
    lj_tgregistry_slot_body_snapshot(delayed->key.slot);
  assert(delayed->captured.body != NULL);
  assert(delayed->captured.incarnation == delayed->key.incarnation);
  la_store32_rel(&delayed->ready, 1);
  while (!la_load32_acq(&delayed->go))
    la_cpu_pause();
  /* Resume at the exact body-CAS instruction after the lifecycle validation
  ** above. This is the vulnerable schedule the tagged body word must reject. */
  assert(!lj_tgregistry_body_cas(delayed->key.slot, &delayed->captured,
                                 NULL, delayed->key.incarnation));
  delayed->result = (int32_t)lj_tgregistry_key_snapshot(
    &delayed->key, &delayed->observed);
  return NULL;
}

static void test_delayed_clear_same_address_republish_aba(void)
{
  LJTGRegistrySlot slot;
  LJTGRegistryKey first, second;
  LJTGRegistryBorrow borrow;
  LJTGRegistryBodySnap body_snap;
  LJTGSlotSnap snap;
  TestBody reused_storage;
  DelayedClear delayed;
  pthread_t thread;
  void *body = NULL;

  memset(&delayed, 0, sizeof(delayed));
  assert(lj_tgregistry_slot_init_unpublished(&slot, 180, NULL));
  claim_and_publish(&slot, &reused_storage, &first);
  assert(retry_detach(&first, &snap) == LJ_TGSLOT_OK);
  assert(retry_retire(&first, &snap) == LJ_TGSLOT_OK);
  assert(retry_reclaim(&first, &body, &snap) == LJ_TGSLOT_OK);
  assert(body == &reused_storage);

  delayed.key = first;
  assert(pthread_create(&thread, NULL, delayed_old_clearer, &delayed) == 0);
  while (!la_load32_acq(&delayed.ready))
    la_cpu_pause();

  /* The delayed thread has captured {address, first.incarnation}. Complete the
  ** old clear, reuse the slot, then republish the exact same address under the
  ** next incarnation before allowing its stale body CAS to run. */
  assert(retry_clear(&first, &snap) == LJ_TGSLOT_OK);
  claim_and_publish(&slot, &reused_storage, &second);
  assert(second.incarnation == first.incarnation + 1u);
  la_store32_rel(&delayed.go, 1);
  assert(pthread_join(thread, NULL) == 0);
  assert(delayed.result == LJ_TGSLOT_STALE);
  assert(delayed.observed.incarnation == second.incarnation);
  assert(delayed.observed.state == LJ_TGSLOT_LIVE);
  assert(delayed.captured.body == &reused_storage);
  assert(delayed.captured.incarnation == second.incarnation);

  body_snap = lj_tgregistry_slot_body_snapshot(&slot);
  assert(body_snap.body == &reused_storage);
  assert(body_snap.incarnation == second.incarnation);
  lj_tgregistry_borrow_init(&borrow);
  assert(retry_borrow(&second, &borrow, &snap) == LJ_TGSLOT_OK);
  assert(lj_tgregistry_try_body_snapshot(&borrow, &body, &snap) ==
         LJ_TGSLOT_OK);
  assert(body == &reused_storage);
  assert(retry_release(&borrow, &snap) == LJ_TGSLOT_OK);
}

static void test_captured_clear_rejects_live_lifecycle(void)
{
  LJTGRegistrySlot slot;
  LJTGRegistryKey key;
  LJTGRegistryBodySnap captured, after;
  LJTGSlotSnap snap;
  TestBody body;

  assert(lj_tgregistry_slot_init_unpublished(&slot, 240, NULL));
  claim_and_publish(&slot, &body, &key);
  captured = lj_tgregistry_slot_body_snapshot(&slot);
  assert(lj_tgregistry_try_clear_captured(&key, &captured, &snap) ==
         LJ_TGSLOT_DENIED);
  assert(snap.state == LJ_TGSLOT_LIVE);
  after = lj_tgregistry_slot_body_snapshot(&slot);
  assert(lj_tgregistry_body_snap_equal(&captured, &after));

  assert(retry_detach(&key, &snap) == LJ_TGSLOT_OK);
  assert(lj_tgregistry_try_clear_captured(&key, &captured, &snap) ==
         LJ_TGSLOT_DENIED);
  assert(snap.state == LJ_TGSLOT_DETACHING);
  after = lj_tgregistry_slot_body_snapshot(&slot);
  assert(lj_tgregistry_body_snap_equal(&captured, &after));
}

typedef struct ClearRace {
  LJTGRegistryKey key;
  uint32_t ready;
  uint32_t go;
  int32_t result[2];
} ClearRace;

typedef struct ClearRacer {
  ClearRace *race;
  uint32_t index;
} ClearRacer;

static void *concurrent_clearer(void *ud)
{
  ClearRacer *racer = (ClearRacer *)ud;
  LJTGSlotSnap snap = lj_tgslot_snapshot(&racer->race->key.slot->token);
  assert(snap.incarnation == racer->race->key.incarnation);
  assert(snap.state == LJ_TGSLOT_RECLAIMING && snap.lease_count == 0);
  (void)la_add32_acqrel(&racer->race->ready, 1);
  while (!la_load32_acq(&racer->race->go))
    la_cpu_pause();
  racer->race->result[racer->index] =
    (int32_t)lj_tgregistry_try_clear(&racer->race->key, NULL);
  return NULL;
}

static void test_concurrent_clear_idempotence(void)
{
  LJTGRegistrySlot slot;
  LJTGRegistryKey key;
  LJTGSlotSnap snap;
  TestBody body;
  ClearRace race;
  ClearRacer racer[2];
  pthread_t thread[2];
  void *reclaim_body = NULL;
  unsigned i;

  memset(&race, 0, sizeof(race));
  assert(lj_tgregistry_slot_init_unpublished(&slot, 300, NULL));
  claim_and_publish(&slot, &body, &key);
  assert(retry_detach(&key, &snap) == LJ_TGSLOT_OK);
  assert(retry_retire(&key, &snap) == LJ_TGSLOT_OK);
  assert(retry_reclaim(&key, &reclaim_body, &snap) == LJ_TGSLOT_OK);
  assert(reclaim_body == &body);
  body.reclaimed = 1;
  race.key = key;
  for (i = 0; i < 2; i++) {
    racer[i].race = &race;
    racer[i].index = i;
    assert(pthread_create(&thread[i], NULL, concurrent_clearer,
                          &racer[i]) == 0);
  }
  while (la_load32_acq(&race.ready) != 2)
    la_cpu_pause();
  la_store32_rel(&race.go, 1);
  for (i = 0; i < 2; i++)
    assert(pthread_join(thread[i], NULL) == 0);
  if (race.result[0] != LJ_TGSLOT_OK || race.result[1] != LJ_TGSLOT_OK) {
    LJTGRegistryBodySnap failed_body =
      lj_tgregistry_slot_body_snapshot(&slot);
    LJTGSlotSnap failed_token = lj_tgslot_snapshot(&slot.token);
    fprintf(stderr,
            "concurrent clear failed: result={%d,%d} "
            "token={inc=%llu,leases=%llu,state=%u} "
            "body={ptr=%p,inc=%llu}\n",
            (int)race.result[0], (int)race.result[1],
            (unsigned long long)failed_token.incarnation,
            (unsigned long long)failed_token.lease_count,
            (unsigned)failed_token.state, failed_body.body,
            (unsigned long long)failed_body.incarnation);
  }
  assert(race.result[0] == LJ_TGSLOT_OK);
  assert(race.result[1] == LJ_TGSLOT_OK);
  snap = lj_tgslot_snapshot(&slot.token);
  assert(snap.state == LJ_TGSLOT_EMPTY && snap.lease_count == 0);
  assert(lj_tgregistry_slot_body_acq(&slot) == NULL);
}

enum { STRESS_BORROWERS = 8, STRESS_CHURN = 20000 };

typedef struct RegistryStress {
  LJTGRegistrySlot slot;
  LJTGRegistryKey key;
  TestBody body;
  uint32_t ready;
  uint32_t hold_go;
  uint32_t holding;
  uint32_t release_gate;
  uint64_t borrowed;
  uint64_t released;
  uint32_t busy_reclaims;
} RegistryStress;

static void check_stress_body(RegistryStress *stress,
                              LJTGRegistryBorrow *borrow)
{
  void *body = NULL;
  assert(lj_tgregistry_try_body_snapshot(borrow, &body, NULL) ==
         LJ_TGSLOT_OK);
  assert(body == &stress->body);
  assert(((TestBody *)body)->incarnation == stress->key.incarnation);
  assert(((TestBody *)body)->canary ==
         (UINT64_C(0x7467726567697374) ^ stress->key.incarnation));
  assert(la_load32_acq(&((TestBody *)body)->reclaimed) == 0);
}

static void *registry_borrower(void *ud)
{
  RegistryStress *stress = (RegistryStress *)ud;
  unsigned i;
  for (i = 0; i < STRESS_CHURN; i++) {
    LJTGRegistryBorrow borrow;
    lj_tgregistry_borrow_init(&borrow);
    assert(retry_borrow(&stress->key, &borrow, NULL) == LJ_TGSLOT_OK);
    check_stress_body(stress, &borrow);
    assert(retry_release(&borrow, NULL) == LJ_TGSLOT_OK);
  }
  (void)la_add32_acqrel(&stress->ready, 1);
  while (!la_load32_acq(&stress->hold_go))
    la_cpu_pause();
  {
    LJTGRegistryBorrow borrow;
    lj_tgregistry_borrow_init(&borrow);
    assert(retry_borrow(&stress->key, &borrow, NULL) == LJ_TGSLOT_OK);
    check_stress_body(stress, &borrow);
    (void)la_add64_rlx(&stress->borrowed, 1);
    (void)la_add32_acqrel(&stress->holding, 1);
    while (!la_load32_acq(&stress->release_gate))
      la_cpu_pause();
    /* The admitted lease keeps body valid after the RETIRED close LP. */
    check_stress_body(stress, &borrow);
    assert(retry_release(&borrow, NULL) == LJ_TGSLOT_OK);
    (void)la_sub32_acqrel(&stress->holding, 1);
    (void)la_add64_rlx(&stress->released, 1);
  }
  return NULL;
}

static void *registry_reclaimer(void *ud)
{
  RegistryStress *stress = (RegistryStress *)ud;
  LJTGSlotResult result;
  LJTGSlotSnap snap;
  void *body = NULL;

  while (la_load32_acq(&stress->ready) != STRESS_BORROWERS)
    la_cpu_pause();
  la_store32_rel(&stress->hold_go, 1);
  while (la_load32_acq(&stress->holding) != STRESS_BORROWERS)
    la_cpu_pause();
  assert(retry_detach(&stress->key, &snap) == LJ_TGSLOT_OK);
  assert(retry_retire(&stress->key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_RETIRED);
  assert(snap.lease_count == STRESS_BORROWERS + 1u);
  assert(lj_tgregistry_try_reclaim(&stress->key, &body, &snap) ==
         LJ_TGSLOT_BUSY);
  (void)la_add32_rlx(&stress->busy_reclaims, 1);
  la_store32_rel(&stress->release_gate, 1);
  for (;;) {
    result = lj_tgregistry_try_reclaim(&stress->key, &body, &snap);
    if (result == LJ_TGSLOT_OK)
      break;
    if (result == LJ_TGSLOT_BUSY)
      (void)la_add32_rlx(&stress->busy_reclaims, 1);
    else
      assert(result == LJ_TGSLOT_LOST);
    la_cpu_pause();
  }
  assert(body == &stress->body);
  assert(snap.state == LJ_TGSLOT_RECLAIMING && snap.lease_count == 0);
  la_store32_rel(&stress->body.reclaimed, 1);
  assert(retry_clear(&stress->key, &snap) == LJ_TGSLOT_OK);
  return NULL;
}

static void test_concurrent_borrow_retire_reclaim(void)
{
  RegistryStress stress;
  pthread_t borrowers[STRESS_BORROWERS], reclaimer;
  LJTGSlotSnap final;
  unsigned i;

  memset(&stress, 0, sizeof(stress));
  assert(lj_tgregistry_slot_init_unpublished(&stress.slot, 500, NULL));
  claim_and_publish(&stress.slot, &stress.body, &stress.key);
  for (i = 0; i < STRESS_BORROWERS; i++)
    assert(pthread_create(&borrowers[i], NULL, registry_borrower,
                          &stress) == 0);
  assert(pthread_create(&reclaimer, NULL, registry_reclaimer, &stress) == 0);
  assert(pthread_join(reclaimer, NULL) == 0);
  for (i = 0; i < STRESS_BORROWERS; i++)
    assert(pthread_join(borrowers[i], NULL) == 0);
  final = lj_tgslot_snapshot(&stress.slot.token);
  assert(final.incarnation == stress.key.incarnation);
  assert(final.state == LJ_TGSLOT_EMPTY && final.lease_count == 0);
  assert(lj_tgregistry_slot_body_acq(&stress.slot) == NULL);
  assert(la_load64_acq(&stress.borrowed) == STRESS_BORROWERS);
  assert(la_load64_acq(&stress.borrowed) ==
         la_load64_acq(&stress.released));
  assert(la_load32_acq(&stress.busy_reclaims) != 0);
}

static void test_terminal_states_fail_closed(void)
{
  LJTGRegistrySlot slot;
  LJTGRegistryKey key;
  LJTGRegistryBorrow borrow;
  LJTGSlotSnap snap;
  TestBody body;
  void *snapshot = NULL;

  assert(lj_tgslot_init_unpublished(&slot.token, UINT64_MAX, 0,
                                    LJ_TGSLOT_EXHAUSTED));
  lj_tgregistry_body_init_unpublished(&slot, NULL, UINT64_MAX);
  slot.next_all = NULL;
  assert(lj_tgregistry_try_claim(&slot, &key, &snap) ==
         LJ_TGSLOT_EXHAUSTED_RESULT);
  key.slot = &slot;
  key.incarnation = UINT64_MAX;
  lj_tgregistry_borrow_init(&borrow);
  assert(lj_tgregistry_try_borrow(&key, &borrow, &snap) ==
         LJ_TGSLOT_EXHAUSTED_RESULT);
  assert(lj_tgregistry_try_clear(&key, &snap) ==
         LJ_TGSLOT_EXHAUSTED_RESULT);

  assert(lj_tgslot_init_unpublished(&slot.token, 700,
                                    LJ_TGSLOT_MAX_LEASES,
                                    LJ_TGSLOT_LIVE));
  body.incarnation = 700;
  body.canary = 1;
  body.reclaimed = 0;
  lj_tgregistry_body_init_unpublished(&slot, &body, 700);
  key.slot = &slot;
  key.incarnation = 700;
  lj_tgregistry_borrow_init(&borrow);
  assert(lj_tgregistry_try_claim(&slot, &key, &snap) ==
         LJ_TGSLOT_DENIED);
  assert(lj_tgregistry_try_borrow(&key, &borrow, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
  assert(snap.state == LJ_TGSLOT_PINNED);
  assert(lj_tgregistry_try_claim(&slot, &key, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
  assert(lj_tgregistry_try_borrowable_key(&slot, &key, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
  assert(lj_tgregistry_try_reclaim(&key, &snapshot, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
  assert(lj_tgregistry_try_clear(&key, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
  assert(lj_tgregistry_slot_body_acq(&slot) == &body);

  /* LIVE may never be repaired by installing a body after publication. */
  assert(lj_tgslot_init_unpublished(&slot.token, 800, 1,
                                    LJ_TGSLOT_LIVE));
  lj_tgregistry_body_init_unpublished(&slot, NULL, 800);
  key.slot = &slot;
  key.incarnation = 800;
  assert(lj_tgregistry_try_publish_body(&key, &body, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
  assert(snap.state == LJ_TGSLOT_PINNED && snap.lease_count == 1);
  assert(lj_tgregistry_slot_body_acq(&slot) == NULL);

  /* An exact borrow independently applies the same fail-closed rule. */
  assert(lj_tgslot_init_unpublished(&slot.token, 801, 1,
                                    LJ_TGSLOT_LIVE));
  lj_tgregistry_body_init_unpublished(&slot, NULL, 801);
  key.incarnation = 801;
  lj_tgregistry_borrow_init(&borrow);
  assert(lj_tgregistry_try_borrow(&key, &borrow, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
  assert(snap.state == LJ_TGSLOT_PINNED && snap.lease_count == 1);
  assert(!borrow.active);
  assert(lj_tgregistry_try_borrowable_key(&slot, &key, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);

  /* A non-null body with the wrong tag is equally unsafe. */
  assert(lj_tgslot_init_unpublished(&slot.token, 850, 1,
                                    LJ_TGSLOT_LIVE));
  lj_tgregistry_body_init_unpublished(&slot, &body, 849);
  key.incarnation = 850;
  lj_tgregistry_borrow_init(&borrow);
  assert(lj_tgregistry_try_borrow(&key, &borrow, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
  assert(snap.state == LJ_TGSLOT_PINNED && snap.lease_count == 1);
  assert(!borrow.active);

  /* A bodyless RETIRED incarnation is corrupt and becomes sticky PINNED;
  ** reclaim must never turn it into reusable EMPTY. */
  assert(lj_tgslot_init_unpublished(&slot.token, 900, 1,
                                    LJ_TGSLOT_RETIRED));
  lj_tgregistry_body_init_unpublished(&slot, NULL, 900);
  key.incarnation = 900;
  assert(lj_tgregistry_try_reclaim(&key, &snapshot, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
  assert(snap.state == LJ_TGSLOT_PINNED && snap.lease_count == 1);
  assert(lj_tgregistry_try_clear(&key, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
}

static void test_held_borrow_body_mismatch_pins(void)
{
  LJTGRegistrySlot slot;
  LJTGRegistryKey key;
  LJTGRegistryBorrow borrow;
  LJTGRegistryBodySnap expected;
  LJTGSlotSnap snap;
  TestBody body, replacement;
  void *snapshot = (void *)(uintptr_t)1;

  assert(lj_tgregistry_slot_init_unpublished(&slot, 1000, NULL));
  claim_and_publish(&slot, &body, &key);
  lj_tgregistry_borrow_init(&borrow);
  assert(retry_borrow(&key, &borrow, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_LIVE && snap.lease_count == 2);

  /* Model corruption after admission: a held lease makes body immutable, so
  ** either pointer or incarnation drift must make no-reclaim sticky. */
  expected = lj_tgregistry_slot_body_snapshot(&slot);
  assert(lj_tgregistry_body_cas(&slot, &expected, &replacement,
                                key.incarnation));
  assert(lj_tgregistry_try_body_snapshot(&borrow, &snapshot, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
  assert(snapshot == NULL);
  assert(snap.state == LJ_TGSLOT_PINNED && snap.lease_count == 2);
  assert(retry_release(&borrow, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_PINNED && snap.lease_count == 1);
}

int main(void)
{
  test_body_publication_and_stable_chain();
  test_attach_abort_with_attaching_borrow();
  test_clear_before_empty_and_stale_reuse();
  test_delayed_clear_same_address_republish_aba();
  test_captured_clear_rejects_live_lifecycle();
  test_concurrent_clear_idempotence();
  test_concurrent_borrow_retire_reclaim();
  test_terminal_states_fail_closed();
  test_held_borrow_body_mismatch_pins();
  printf("t-tgregistry-slot OK: stable bodies and exact leases verified\n");
  return 0;
}
