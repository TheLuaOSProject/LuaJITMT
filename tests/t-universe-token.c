/*
** t-universe-token.c - dormant exact universe admission/epoch model.
**
** Build & run:
**   LJ_CAS128_CFLAGS=-mcx16  # Use an empty value for an arm64 target.
**   cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread \
**      $LJ_CAS128_CFLAGS -Isrc \
**      tests/t-universe-token.c src/lj_universe.c \
**      -o /tmp/t-universe-token && /tmp/t-universe-token
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_universe.h"

typedef struct TestBody {
  uint64_t magic;
  uint64_t serial;
} __attribute__((aligned(16))) TestBody;

static LJUniverseResult retry_release(LJUniverseTxn *txn,
                                      LJUniverseSnap *observed)
{
  LJUniverseResult result;
  do {
    result = lj_universe_txn_release(txn, observed);
  } while (result == LJ_UNIVERSE_LOST);
  return result;
}

static LJUniverseResult retry_final_drain(LJUniverseClose *close,
                                          LJUniverseSnap *observed)
{
  LJUniverseResult result;
  do {
    result = lj_universe_close_begin_final_drain(close, observed);
  } while (result == LJ_UNIVERSE_LOST);
  return result;
}

static void claim_open(LJUniverseSlot *slot, TestBody *body,
                       LJUniverseKey *key)
{
  LJUniverseBuild build;
  LJUniverseSnap snap;
  LJUniverseResult result;
  memset(&build, 0, sizeof(build));
  do {
    result = lj_universe_try_claim(slot, &build, &snap);
  } while (result == LJ_UNIVERSE_LOST);
  assert(result == LJ_UNIVERSE_OK);
  assert(build.active && build.key.slot == slot);
  assert(snap.transaction_count == 0 &&
         snap.state == LJ_UNIVERSE_BUILDING);
  result = lj_universe_try_publish(&build, body, key, &snap);
  assert(result == LJ_UNIVERSE_OK && !build.active);
  assert(snap.state == LJ_UNIVERSE_OPEN && snap.transaction_count == 0);
  assert(lj_universe_body_snapshot(slot).body == body);
}

static void make_open(LJUniverseSlot *slot, uint64_t empty_incarnation,
                      TestBody *body, LJUniverseKey *key)
{
  assert(lj_universe_slot_init_unpublished(slot, empty_incarnation, NULL));
  claim_open(slot, body, key);
  assert(key->incarnation ==
         (empty_incarnation == 0 ? 1u : empty_incarnation));
}

static void finish_without_finalizer_transactions(LJUniverseClose *close,
                                                  uint64_t expected_epoch)
{
  LJUniverseSnap snap;
  uint64_t epoch = 0;
  LJUniverseResult result;
  do {
    result = lj_universe_close_freeze_external(close, &epoch, &snap);
  } while (result == LJ_UNIVERSE_LOST);
  assert(result == LJ_UNIVERSE_OK && epoch == expected_epoch);
  assert(snap.state == LJ_UNIVERSE_FINALIZING);
  assert(retry_final_drain(close, &snap) == LJ_UNIVERSE_OK);
  assert(snap.state == LJ_UNIVERSE_FINAL_DRAIN);
  do {
    result = lj_universe_close_seal(close, &epoch, &snap);
  } while (result == LJ_UNIVERSE_LOST);
  assert(result == LJ_UNIVERSE_OK && epoch == expected_epoch);
  assert(snap.state == LJ_UNIVERSE_SEALED);
  do {
    result = lj_universe_close_recycle(close, &snap);
  } while (result == LJ_UNIVERSE_LOST);
  assert(result == LJ_UNIVERSE_OK && !close->active);
}

static void test_layout_components_and_invalid_inputs(void)
{
  LJUniverseSlot slot;
  LJUniverseKey key;
  LJUniverseBuild build;
  LJUniverseTxn txn;
  LJUniverseClose close;
  LJUniverseSnap snap;
  unsigned char raw[sizeof(LJUniverseSlot) + 16u];
  LJUniverseSlot *misaligned = (LJUniverseSlot *)(void *)(raw + 1);

  memset(&txn, 0, sizeof(txn));
  memset(&close, 0, sizeof(close));
  memset(&build, 0, sizeof(build));
  assert(sizeof(LJUniverseToken) == 16u);
  assert(__alignof__(LJUniverseToken) >= 16u);
  assert(__alignof__(LJUniverseSlot) >= 16u);
  assert(offsetof(LJUniverseSlot, token) == 0u);
  assert(offsetof(LJUniverseSlot, body_value) % 16u == 0u);
  assert(lj_universe_components_valid(0, 0, LJ_UNIVERSE_EMPTY));
  assert(lj_universe_components_valid(UINT64_MAX, 0,
                                      LJ_UNIVERSE_EMPTY));
  assert(!lj_universe_components_valid(0, 0, LJ_UNIVERSE_BUILDING));
  assert(lj_universe_components_valid(1, 0, LJ_UNIVERSE_BUILDING));
  assert(lj_universe_components_valid(1, 0, LJ_UNIVERSE_OPEN));
  assert(lj_universe_components_valid(1, LJ_UNIVERSE_MAX_TRANSACTIONS,
                                      LJ_UNIVERSE_CLOSING));
  assert(!lj_universe_components_valid(0, 1, LJ_UNIVERSE_CLOSING));
  assert(!lj_universe_components_valid(1, 1, LJ_UNIVERSE_SEALED));
  assert(lj_universe_components_valid(UINT64_MAX, 0,
                                      LJ_UNIVERSE_EXHAUSTED));
  assert(!lj_universe_components_valid(UINT64_MAX - 1u, 0,
                                       LJ_UNIVERSE_EXHAUSTED));
  assert(!lj_universe_components_valid(1, 0,
                                       LJ_UNIVERSE_EXHAUSTED + 1u));

  assert(!lj_universe_slot_init_unpublished(NULL, 0, NULL));
  assert(!lj_universe_slot_init_unpublished(misaligned, 0, NULL));
  assert(lj_universe_slot_init_unpublished(&slot, 0, NULL));
  key.slot = misaligned;
  key.incarnation = 1;
  assert(!lj_universe_key_valid(&key));
  assert(lj_universe_try_enter(&key, &txn, NULL) == LJ_UNIVERSE_INVALID);
  assert(lj_universe_try_claim(NULL, &build, NULL) == LJ_UNIVERSE_INVALID);
  assert(lj_universe_try_claim(&slot, NULL, NULL) == LJ_UNIVERSE_INVALID);
  assert(lj_universe_try_publish(NULL, &slot, &key, NULL) ==
         LJ_UNIVERSE_INVALID);
  assert(lj_universe_try_enter(NULL, &txn, NULL) == LJ_UNIVERSE_INVALID);
  assert(lj_universe_try_close(NULL, &close, NULL) == LJ_UNIVERSE_INVALID);
  assert(lj_universe_txn_release(&txn, NULL) == LJ_UNIVERSE_INVALID);
  assert(lj_universe_close_freeze_external(&close, NULL, NULL) ==
         LJ_UNIVERSE_INVALID);

  snap = lj_universe_snapshot(NULL);
  assert(snap.incarnation == 0 && snap.transaction_count == 0 &&
         snap.state == LJ_UNIVERSE_EMPTY);
  assert(lj_universe_body_snapshot(NULL).body == NULL);
}

static void test_scalar_epochs_and_reuse(void)
{
  LJUniverseSlot slot;
  LJUniverseKey first, second, stale;
  LJUniverseBuild build;
  LJUniverseTxn a, b, c, f1, f2, denied;
  LJUniverseClose close;
  LJUniverseSnap snap;
  LJUniverseBodySnap body_snap;
  LJUniverseResult result;
  TestBody body1 = { UINT64_C(0xfeedface), 1 };
  TestBody body2 = { UINT64_C(0xcafebeef), 2 };
  uint64_t external_epoch = 0, terminal_epoch = 0;

  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  memset(&c, 0, sizeof(c));
  memset(&f1, 0, sizeof(f1));
  memset(&f2, 0, sizeof(f2));
  memset(&denied, 0, sizeof(denied));
  memset(&close, 0, sizeof(close));
  memset(&build, 0, sizeof(build));
  make_open(&slot, 0, &body1, &first);
  assert(first.incarnation == 1);

  assert(lj_universe_try_enter(&first, &a, &snap) == LJ_UNIVERSE_OK);
  assert(lj_universe_try_enter(&first, &b, &snap) == LJ_UNIVERSE_OK);
  assert(lj_universe_try_enter(&first, &c, &snap) == LJ_UNIVERSE_OK);
  assert(a.active && a.body == &body1 && a.publication_ticket == 1);
  assert(b.publication_ticket == 2 && c.publication_ticket == 3);
  assert(snap.transaction_count == 3 && snap.state == LJ_UNIVERSE_OPEN);

  do {
    result = lj_universe_try_close(&first, &close, &snap);
  } while (result == LJ_UNIVERSE_LOST);
  assert(result == LJ_UNIVERSE_OK && close.active && close.body == &body1);
  assert(snap.state == LJ_UNIVERSE_CLOSING && snap.transaction_count == 3);
  assert(lj_universe_try_enter(&first, &denied, &snap) ==
         LJ_UNIVERSE_DENIED);
  assert(!denied.active);

  /* Completion order does not change the exact frozen next-ticket epoch. */
  assert(retry_release(&c, &snap) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_freeze_external(&close, &external_epoch,
                                           &snap) == LJ_UNIVERSE_BUSY);
  assert(external_epoch == 0);
  assert(retry_release(&a, &snap) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_freeze_external(&close, &external_epoch,
                                           &snap) == LJ_UNIVERSE_BUSY);
  assert(retry_release(&b, &snap) == LJ_UNIVERSE_OK);
  do {
    result = lj_universe_close_freeze_external(&close, &external_epoch,
                                                &snap);
  } while (result == LJ_UNIVERSE_LOST);
  assert(result == LJ_UNIVERSE_OK && external_epoch == 4);
  assert(close.external_final_publication == 4);
  assert(snap.state == LJ_UNIVERSE_FINALIZING &&
         snap.transaction_count == 0);

  assert(lj_universe_try_enter(&first, &denied, NULL) ==
         LJ_UNIVERSE_DENIED);
  assert(lj_universe_try_enter_finalizer(&close, &f1, &snap) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_try_enter_finalizer(&close, &f2, &snap) ==
         LJ_UNIVERSE_OK);
  assert(f1.publication_ticket == 4 && f2.publication_ticket == 5);
  assert(f1.kind == LJ_UNIVERSE_TXN_FINALIZER && f1.body == &body1);

  assert(retry_final_drain(&close, &snap) == LJ_UNIVERSE_OK);
  assert(snap.state == LJ_UNIVERSE_FINAL_DRAIN &&
         snap.transaction_count == 2);
  assert(lj_universe_try_enter_finalizer(&close, &denied, NULL) ==
         LJ_UNIVERSE_DENIED);
  assert(lj_universe_close_seal(&close, &terminal_epoch, &snap) ==
         LJ_UNIVERSE_BUSY);
  assert(retry_release(&f2, &snap) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_seal(&close, &terminal_epoch, &snap) ==
         LJ_UNIVERSE_BUSY);
  assert(retry_release(&f1, &snap) == LJ_UNIVERSE_OK);
  do {
    result = lj_universe_close_seal(&close, &terminal_epoch, &snap);
  } while (result == LJ_UNIVERSE_LOST);
  assert(result == LJ_UNIVERSE_OK && terminal_epoch == 6);
  assert(close.terminal_final_publication == 6);
  assert(lj_universe_external_epoch_snapshot(&slot).epoch == 4);
  assert(lj_universe_terminal_epoch_snapshot(&slot).epoch == 6);
  assert(snap.state == LJ_UNIVERSE_SEALED);

  stale = first;
  do {
    result = lj_universe_close_recycle(&close, &snap);
  } while (result == LJ_UNIVERSE_LOST);
  assert(result == LJ_UNIVERSE_OK && !close.active);
  assert(snap.state == LJ_UNIVERSE_EMPTY && snap.incarnation == 2);
  body_snap = lj_universe_body_snapshot(&slot);
  assert(body_snap.body == NULL && body_snap.incarnation == 2);
  assert(la_load64_acq(&slot.next_publication) == 1);
  assert(lj_universe_external_epoch_snapshot(&slot).epoch == 0);
  assert(lj_universe_terminal_epoch_snapshot(&slot).epoch == 0);

  do {
    result = lj_universe_try_claim(&slot, &build, &snap);
  } while (result == LJ_UNIVERSE_LOST);
  assert(result == LJ_UNIVERSE_OK && build.key.incarnation == 2);
  assert(lj_universe_try_publish(&build, &body2, &second, &snap) ==
         LJ_UNIVERSE_OK);
  memset(&denied, 0, sizeof(denied));
  memset(&close, 0, sizeof(close));
  assert(lj_universe_try_enter(&stale, &denied, &snap) ==
         LJ_UNIVERSE_STALE);
  assert(lj_universe_try_close(&stale, &close, &snap) ==
         LJ_UNIVERSE_STALE);
  assert(!denied.active && !close.active);
  assert(!lj_universe_key_equal(&stale, &second));

  do {
    result = lj_universe_try_close(&second, &close, &snap);
  } while (result == LJ_UNIVERSE_LOST);
  assert(result == LJ_UNIVERSE_OK);
  finish_without_finalizer_transactions(&close, 1);
}

typedef struct BuildRacer {
  LJUniverseBuild build;
  TestBody *body;
  LJUniverseKey key;
  LJUniverseResult result;
  uint32_t *ready;
  uint32_t *go;
  int publish;
} BuildRacer;

static void *build_racer(void *ud)
{
  BuildRacer *racer = (BuildRacer *)ud;
  (void)la_add32_acqrel(racer->ready, 1);
  while (!la_load32_acq(racer->go))
    la_cpu_pause();
  if (racer->publish)
    racer->result = lj_universe_try_publish(&racer->build, racer->body,
                                             &racer->key, NULL);
  else
    racer->result = lj_universe_try_abort_build(&racer->build, NULL);
  return NULL;
}

static void test_build_abort_failure_retry_and_race(void)
{
  enum { BUILD_RACE_ROUNDS = 200 };
  LJUniverseSlot slot;
  LJUniverseBuild build, copy;
  LJUniverseKey key;
  LJUniverseSnap snap;
  LJUniverseBodySnap body_snap;
  LJUniverseResult result;
  TestBody body = { UINT64_C(0xabcddcba), 77 };
  unsigned round;

  memset(&build, 0, sizeof(build));
  assert(lj_universe_slot_init_unpublished(&slot, 0, NULL));
  assert(lj_universe_try_claim(&slot, &build, &snap) == LJ_UNIVERSE_OK);
  copy = build;
  assert(lj_universe_try_abort_build(&build, &snap) == LJ_UNIVERSE_OK);
  assert(!build.active && snap.state == LJ_UNIVERSE_EMPTY &&
         snap.incarnation == 2);
  body_snap = lj_universe_body_snapshot(&slot);
  assert(body_snap.body == NULL && body_snap.incarnation == 2);
  assert(la_load64_acq(&slot.next_publication) == 1 &&
         lj_universe_external_epoch_snapshot(&slot).epoch == 0 &&
         lj_universe_terminal_epoch_snapshot(&slot).epoch == 0);
  assert(lj_universe_try_abort_build(&copy, &snap) == LJ_UNIVERSE_STALE);
  assert(!copy.active);  /* A copied loser gains no recycle authority. */

  memset(&build, 0, sizeof(build));
  assert(lj_universe_try_claim(&slot, &build, &snap) == LJ_UNIVERSE_OK);
  copy = build;
  assert(lj_universe_try_publish(&build, &body, &key, &snap) ==
         LJ_UNIVERSE_OK);
  assert(!build.active && snap.state == LJ_UNIVERSE_OPEN);
  assert(lj_universe_try_abort_build(&copy, &snap) == LJ_UNIVERSE_DENIED);
  assert(!copy.active && lj_universe_snapshot(&slot).state ==
         LJ_UNIVERSE_OPEN);
  {
    LJUniverseClose close;
    memset(&close, 0, sizeof(close));
    assert(lj_universe_try_close(&key, &close, NULL) == LJ_UNIVERSE_OK);
    finish_without_finalizer_transactions(&close, 1);
  }

  /* Publish and abort copies race on one exact body-decision CAS. */
  for (round = 0; round < BUILD_RACE_ROUNDS; round++) {
    BuildRacer publisher, aborter;
    pthread_t publish_thread, abort_thread;
    uint32_t ready = 0, go = 0;
    unsigned winners;
    memset(&build, 0, sizeof(build));
    do {
      result = lj_universe_try_claim(&slot, &build, &snap);
    } while (result == LJ_UNIVERSE_LOST);
    assert(result == LJ_UNIVERSE_OK);
    memset(&publisher, 0, sizeof(publisher));
    memset(&aborter, 0, sizeof(aborter));
    publisher.build = build;
    publisher.body = &body;
    publisher.ready = &ready;
    publisher.go = &go;
    publisher.publish = 1;
    aborter.build = build;
    aborter.ready = &ready;
    aborter.go = &go;
    assert(pthread_create(&publish_thread, NULL, build_racer,
                          &publisher) == 0);
    assert(pthread_create(&abort_thread, NULL, build_racer, &aborter) == 0);
    while (la_load32_acq(&ready) != 2)
      la_cpu_pause();
    la_store32_rel(&go, 1);
    assert(pthread_join(publish_thread, NULL) == 0);
    assert(pthread_join(abort_thread, NULL) == 0);
    winners = (publisher.result == LJ_UNIVERSE_OK) +
              (aborter.result == LJ_UNIVERSE_OK);
    assert(winners == 1);
    if (publisher.result == LJ_UNIVERSE_OK) {
      LJUniverseClose close;
      assert(aborter.result == LJ_UNIVERSE_LOST ||
             aborter.result == LJ_UNIVERSE_DENIED ||
             aborter.result == LJ_UNIVERSE_STALE);
      assert(!aborter.build.active && publisher.key.slot == &slot);
      assert(lj_universe_snapshot(&slot).state == LJ_UNIVERSE_OPEN);
      memset(&close, 0, sizeof(close));
      assert(lj_universe_try_close(&publisher.key, &close, NULL) ==
             LJ_UNIVERSE_OK);
      finish_without_finalizer_transactions(&close, 1);
    } else {
      assert(aborter.result == LJ_UNIVERSE_OK);
      assert(publisher.result == LJ_UNIVERSE_LOST ||
             publisher.result == LJ_UNIVERSE_DENIED ||
             publisher.result == LJ_UNIVERSE_STALE);
      assert(!publisher.build.active &&
             lj_universe_snapshot(&slot).state == LJ_UNIVERSE_EMPTY);
    }
    /* The original unconsumed copy is no longer authority either. */
    result = lj_universe_try_abort_build(&build, &snap);
    assert(result == LJ_UNIVERSE_DENIED || result == LJ_UNIVERSE_STALE);
    assert(!build.active);
  }

  /* A failed-build slot remains immediately claimable under a new key. */
  memset(&build, 0, sizeof(build));
  do {
    result = lj_universe_try_claim(&slot, &build, &snap);
  } while (result == LJ_UNIVERSE_LOST);
  assert(result == LJ_UNIVERSE_OK);
  assert(lj_universe_try_publish(&build, &body, &key, &snap) ==
         LJ_UNIVERSE_OK);
  {
    LJUniverseClose close;
    memset(&close, 0, sizeof(close));
    assert(lj_universe_try_close(&key, &close, NULL) == LJ_UNIVERSE_OK);
    finish_without_finalizer_transactions(&close, 1);
  }
}

static void test_poisoned_exact_drain(void)
{
  LJUniverseSlot external_slot, final_slot;
  LJUniverseKey external_key, final_key;
  LJUniverseTxn a, b, f1, f2, denied;
  LJUniverseClose final_close;
  LJUniverseSnap snap;
  TestBody body1 = { 1, 11 }, body2 = { 2, 22 };
  uint64_t epoch;

  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  memset(&f1, 0, sizeof(f1));
  memset(&f2, 0, sizeof(f2));
  memset(&denied, 0, sizeof(denied));
  memset(&final_close, 0, sizeof(final_close));

  make_open(&external_slot, 10, &body1, &external_key);
  assert(lj_universe_try_enter(&external_key, &a, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_try_enter(&external_key, &b, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_try_poison(&external_key, &snap) == LJ_UNIVERSE_OK);
  assert(snap.state == LJ_UNIVERSE_POISONED &&
         snap.transaction_count == 2);
  assert(lj_universe_try_enter(&external_key, &denied, NULL) ==
         LJ_UNIVERSE_POISONED_RESULT);
  assert(lj_universe_try_poison(&external_key, NULL) ==
         LJ_UNIVERSE_POISONED_RESULT);
  assert(retry_release(&b, &snap) == LJ_UNIVERSE_OK);
  assert(snap.state == LJ_UNIVERSE_POISONED &&
         snap.transaction_count == 1);
  assert(retry_release(&a, &snap) == LJ_UNIVERSE_OK);
  assert(snap.state == LJ_UNIVERSE_POISONED &&
         snap.transaction_count == 0);
  assert(lj_universe_try_close(&external_key, &final_close, NULL) ==
         LJ_UNIVERSE_POISONED_RESULT);
  assert(!final_close.active);

  make_open(&final_slot, 20, &body2, &final_key);
  assert(lj_universe_try_close(&final_key, &final_close, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_close_freeze_external(&final_close, &epoch, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_try_enter_finalizer(&final_close, &f1, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_try_enter_finalizer(&final_close, &f2, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_try_poison(&final_key, &snap) == LJ_UNIVERSE_OK);
  assert(snap.state == LJ_UNIVERSE_POISONED &&
         snap.transaction_count == 2);
  assert(retry_release(&f1, &snap) == LJ_UNIVERSE_OK);
  assert(retry_release(&f2, &snap) == LJ_UNIVERSE_OK);
  assert(snap.state == LJ_UNIVERSE_POISONED &&
         snap.transaction_count == 0);
  assert(lj_universe_close_begin_final_drain(&final_close, NULL) ==
         LJ_UNIVERSE_POISONED_RESULT);
  assert(final_close.active);  /* Winner authority is retained diagnostically. */
}

#if defined(LJ_UNIVERSE_TEST_HELPERS)
enum { FORCED_POISON_LOSSES = 96 };

typedef struct PoisonLossSchedule {
  LJUniverseTxn txns[FORCED_POISON_LOSSES];
  uint32_t request;
  uint32_t acknowledged;
  unsigned hook_calls;
} PoisonLossSchedule;

static void *poison_loss_releaser(void *ud)
{
  PoisonLossSchedule *schedule = (PoisonLossSchedule *)ud;
  unsigned i;
  for (i = 0; i < FORCED_POISON_LOSSES; i++) {
    while (la_load32_acq(&schedule->request) != i + 1u)
      la_cpu_pause();
    assert(retry_release(&schedule->txns[i], NULL) == LJ_UNIVERSE_OK);
    la_store32_rel(&schedule->acknowledged, i + 1u);
  }
  return NULL;
}

static void poison_loss_hook(void *ud)
{
  PoisonLossSchedule *schedule = (PoisonLossSchedule *)ud;
  unsigned release = schedule->hook_calls++;
  if (release >= FORCED_POISON_LOSSES)
    return;
  la_store32_rel(&schedule->request, release + 1u);
  while (la_load32_acq(&schedule->acknowledged) != release + 1u)
    la_cpu_pause();
}

/*
** Each scheduled release changes the exact token after poison snapshots it,
** forcing 96 real CAS losses. Poisoning must keep retrying, preserve every
** release, then drain the detector's own anonymous transaction in POISONED.
*/
static void test_exact_poison_through_concurrent_drain(void)
{
  LJUniverseSlot slot;
  LJUniverseKey key;
  LJUniverseTxn trigger;
  LJUniverseSnap snap;
  PoisonLossSchedule schedule;
  TestBody body = { UINT64_C(0x5555aaaa), 88 };
  pthread_t releaser;
  unsigned i;

  memset(&schedule, 0, sizeof(schedule));
  memset(&trigger, 0, sizeof(trigger));
  make_open(&slot, 5000, &body, &key);
  for (i = 0; i < FORCED_POISON_LOSSES; i++)
    assert(lj_universe_try_enter(&key, &schedule.txns[i], NULL) ==
           LJ_UNIVERSE_OK);
  assert(lj_universe_snapshot(&slot).transaction_count ==
         FORCED_POISON_LOSSES);
  assert(pthread_create(&releaser, NULL, poison_loss_releaser,
                        &schedule) == 0);
  lj_universe_test_set_poison_hook(poison_loss_hook, &schedule);
  /* The next admitted transaction detects this exact body corruption. */
  slot.body_value.hi = key.incarnation + 1u;
  assert(lj_universe_try_enter(&key, &trigger, &snap) ==
         LJ_UNIVERSE_CORRUPT);
  lj_universe_test_set_poison_hook(NULL, NULL);
  assert(pthread_join(releaser, NULL) == 0);
  assert(schedule.hook_calls == FORCED_POISON_LOSSES + 1u);
  assert(!trigger.active);
  for (i = 0; i < FORCED_POISON_LOSSES; i++)
    assert(!schedule.txns[i].active);
  snap = lj_universe_snapshot(&slot);
  assert(snap.incarnation == key.incarnation &&
         snap.state == LJ_UNIVERSE_POISONED &&
         snap.transaction_count == 0);
}

typedef struct StagePause {
  uint8_t point;
  uint32_t claimed;
  uint32_t entered;
  uint32_t release;
} StagePause;

typedef struct PoisonPause {
  uint32_t claimed;
  uint32_t entered;
  uint32_t release;
} PoisonPause;

typedef struct PoisonSnapshotCall {
  LJUniverseKey key;
  LJUniverseSnap authority;
  LJUniverseResult result;
} PoisonSnapshotCall;

static void poison_pause_hook(void *ud)
{
  PoisonPause *pause = (PoisonPause *)ud;
  uint32_t expected = 0;
  if (!la_cas32(&pause->claimed, &expected, 1, LA_ACQ_REL, LA_ACQ))
    return;
  la_store32_rel(&pause->entered, 1);
  while (!la_load32_acq(&pause->release))
    la_cpu_pause();
}

static void *poison_snapshot_call_thread(void *ud)
{
  PoisonSnapshotCall *call = (PoisonSnapshotCall *)ud;
  call->result = lj_universe_test_poison_from_snapshot(
    &call->key, &call->authority);
  return NULL;
}

typedef struct StageCall {
  LJUniverseClose close;
  LJUniverseResult result;
  uint64_t epoch;
  uint32_t *ready;
  uint32_t *go;
  uint8_t operation;
} StageCall;

static void stage_pause_hook(uint8_t point, void *ud)
{
  StagePause *pause = (StagePause *)ud;
  uint32_t expected = 0;
  if (point != pause->point ||
      !la_cas32(&pause->claimed, &expected, 1, LA_ACQ_REL, LA_ACQ))
    return;
  la_store32_rel(&pause->entered, 1);
  while (!la_load32_acq(&pause->release))
    la_cpu_pause();
}

static void *stage_call_thread(void *ud)
{
  StageCall *call = (StageCall *)ud;
  if (call->ready) {
    (void)la_add32_acqrel(call->ready, 1);
    while (!la_load32_acq(call->go))
      la_cpu_pause();
  }
  if (call->operation == LJ_UNIVERSE_TEST_FREEZE_EXTERNAL)
    call->result = lj_universe_close_freeze_external(
      &call->close, &call->epoch, NULL);
  else if (call->operation == LJ_UNIVERSE_TEST_FREEZE_TERMINAL)
    call->result = lj_universe_close_seal(
      &call->close, &call->epoch, NULL);
  else
    call->result = lj_universe_close_recycle(&call->close, NULL);
  return NULL;
}

static void run_same_stage_pair(uint8_t operation, LJUniverseClose *close,
                                StageCall *a, StageCall *b)
{
  pthread_t ta, tb;
  uint32_t ready = 0, go = 0;
  unsigned winners;
  memset(a, 0, sizeof(*a));
  memset(b, 0, sizeof(*b));
  a->close = *close;
  b->close = *close;
  a->operation = operation;
  b->operation = operation;
  a->ready = &ready;
  b->ready = &ready;
  a->go = &go;
  b->go = &go;
  assert(pthread_create(&ta, NULL, stage_call_thread, a) == 0);
  assert(pthread_create(&tb, NULL, stage_call_thread, b) == 0);
  while (la_load32_acq(&ready) != 2)
    la_cpu_pause();
  la_store32_rel(&go, 1);
  assert(pthread_join(ta, NULL) == 0);
  assert(pthread_join(tb, NULL) == 0);
  winners = (a->result == LJ_UNIVERSE_OK) +
            (b->result == LJ_UNIVERSE_OK);
  assert(winners == 1);
  assert((a->result == LJ_UNIVERSE_OK ||
          a->result == LJ_UNIVERSE_LOST ||
          a->result == LJ_UNIVERSE_DENIED) &&
         (b->result == LJ_UNIVERSE_OK ||
          b->result == LJ_UNIVERSE_LOST ||
          b->result == LJ_UNIVERSE_DENIED));
}

static void test_copied_close_stage_decisions(void)
{
  LJUniverseSlot slot;
  LJUniverseKey key;
  LJUniverseClose close;
  LJUniverseSnap snap;
  StageCall a, b;
  TestBody body = { UINT64_C(0x77778888), 91 };
  uint64_t epoch;

  memset(&close, 0, sizeof(close));
  make_open(&slot, 6000, &body, &key);
  assert(lj_universe_try_close(&key, &close, NULL) == LJ_UNIVERSE_OK);
  run_same_stage_pair(LJ_UNIVERSE_TEST_FREEZE_EXTERNAL, &close, &a, &b);
  snap = lj_universe_snapshot(&slot);
  assert(snap.state == LJ_UNIVERSE_FINALIZING &&
         lj_universe_external_epoch_snapshot(&slot).epoch == 1);
  /* A pre-freeze value copy synchronizes from the authoritative marker. */
  assert(lj_universe_close_begin_final_drain(&close, &snap) ==
         LJ_UNIVERSE_OK);
  run_same_stage_pair(LJ_UNIVERSE_TEST_FREEZE_TERMINAL, &close, &a, &b);
  snap = lj_universe_snapshot(&slot);
  assert(snap.state == LJ_UNIVERSE_SEALED &&
         lj_universe_terminal_epoch_snapshot(&slot).epoch == 1);
  assert(lj_universe_close_recycle(&close, &snap) == LJ_UNIVERSE_OK);
  assert(snap.state == LJ_UNIVERSE_EMPTY);

  /* Verify the recycled successor remains fully usable. */
  claim_open(&slot, &body, &key);
  memset(&close, 0, sizeof(close));
  assert(lj_universe_try_close(&key, &close, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_freeze_external(&close, &epoch, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_close_begin_final_drain(&close, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_close_seal(&close, &epoch, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_recycle(&close, NULL) == LJ_UNIVERSE_OK);
}

static void test_irreversible_epoch_markers(void)
{
  LJUniverseSlot slot;
  LJUniverseKey key;
  LJUniverseClose close, twin;
  LJUniverseEpochSnap marker;
  StagePause pause;
  StageCall delayed;
  TestBody body = { UINT64_C(0x88889999), 97 };
  pthread_t thread;
  uint64_t epoch;

  /* A publishes the external marker, B helps and advances one stage further. */
  memset(&close, 0, sizeof(close));
  make_open(&slot, 6250, &body, &key);
  assert(lj_universe_try_close(&key, &close, NULL) == LJ_UNIVERSE_OK);
  memset(&pause, 0, sizeof(pause));
  pause.point = LJ_UNIVERSE_TEST_AFTER_EXTERNAL_MARKER;
  memset(&delayed, 0, sizeof(delayed));
  delayed.close = close;
  delayed.operation = LJ_UNIVERSE_TEST_FREEZE_EXTERNAL;
  lj_universe_test_set_stage_hook(stage_pause_hook, &pause);
  assert(pthread_create(&thread, NULL, stage_call_thread, &delayed) == 0);
  while (!la_load32_acq(&pause.entered))
    la_cpu_pause();
  twin = close;
  assert(lj_universe_close_freeze_external(&twin, &epoch, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_close_begin_final_drain(&twin, NULL) == LJ_UNIVERSE_OK);
  marker = lj_universe_external_epoch_snapshot(&slot);
  assert(marker.epoch == 1 && marker.incarnation == key.incarnation);
  la_store32_rel(&pause.release, 1);
  assert(pthread_join(thread, NULL) == 0);
  lj_universe_test_set_stage_hook(NULL, NULL);
  assert(delayed.result == LJ_UNIVERSE_LOST);
  marker = lj_universe_external_epoch_snapshot(&slot);
  assert(marker.epoch == 1 && marker.incarnation == key.incarnation &&
         lj_universe_snapshot(&slot).state == LJ_UNIVERSE_FINAL_DRAIN);
  assert(lj_universe_close_seal(&twin, &epoch, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_recycle(&twin, NULL) == LJ_UNIVERSE_OK);

  /* The terminal marker likewise survives its late publisher at SEALED. */
  memset(&close, 0, sizeof(close));
  make_open(&slot, 6350, &body, &key);
  assert(lj_universe_try_close(&key, &close, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_freeze_external(&close, &epoch, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_close_begin_final_drain(&close, NULL) == LJ_UNIVERSE_OK);
  memset(&pause, 0, sizeof(pause));
  pause.point = LJ_UNIVERSE_TEST_AFTER_TERMINAL_MARKER;
  memset(&delayed, 0, sizeof(delayed));
  delayed.close = close;
  delayed.operation = LJ_UNIVERSE_TEST_FREEZE_TERMINAL;
  lj_universe_test_set_stage_hook(stage_pause_hook, &pause);
  assert(pthread_create(&thread, NULL, stage_call_thread, &delayed) == 0);
  while (!la_load32_acq(&pause.entered))
    la_cpu_pause();
  twin = close;
  assert(lj_universe_close_seal(&twin, &epoch, NULL) == LJ_UNIVERSE_OK);
  marker = lj_universe_terminal_epoch_snapshot(&slot);
  assert(marker.epoch == 1 && marker.incarnation == key.incarnation);
  la_store32_rel(&pause.release, 1);
  assert(pthread_join(thread, NULL) == 0);
  lj_universe_test_set_stage_hook(NULL, NULL);
  assert(delayed.result == LJ_UNIVERSE_DENIED);
  marker = lj_universe_terminal_epoch_snapshot(&slot);
  assert(marker.epoch == 1 && marker.incarnation == key.incarnation &&
         lj_universe_snapshot(&slot).state == LJ_UNIVERSE_SEALED);
  assert(lj_universe_close_recycle(&twin, NULL) == LJ_UNIVERSE_OK);
}

static void test_handle_local_epoch_mismatch(void)
{
  LJUniverseSlot slot;
  LJUniverseKey key;
  LJUniverseClose close, bad;
  TestBody body = { UINT64_C(0xabcdef01), 95 };
  uint64_t epoch;

  memset(&close, 0, sizeof(close));
  make_open(&slot, 6500, &body, &key);
  assert(lj_universe_try_close(&key, &close, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_freeze_external(&close, &epoch, NULL) ==
         LJ_UNIVERSE_OK);
  bad = close;
  bad.external_final_publication++;
  assert(lj_universe_close_begin_final_drain(&bad, NULL) ==
         LJ_UNIVERSE_CORRUPT);
  assert(lj_universe_snapshot(&slot).state == LJ_UNIVERSE_FINALIZING);
  assert(lj_universe_close_begin_final_drain(&close, NULL) == LJ_UNIVERSE_OK);

  bad = close;
  bad.external_final_publication++;
  assert(lj_universe_close_seal(&bad, &epoch, NULL) ==
         LJ_UNIVERSE_CORRUPT);
  assert(lj_universe_snapshot(&slot).state == LJ_UNIVERSE_FINAL_DRAIN);
  assert(lj_universe_close_seal(&close, &epoch, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_recycle(&close, NULL) == LJ_UNIVERSE_OK);
}

static void test_poison_authority_stage_toctou(void)
{
  LJUniverseSlot slot;
  LJUniverseKey key;
  LJUniverseClose close, good;
  LJUniverseSnap snap;
  PoisonSnapshotCall stale;
  PoisonPause pause;
  TestBody body = { UINT64_C(0xabcdef02), 96 };
  pthread_t thread;
  uint64_t epoch;

  memset(&close, 0, sizeof(close));
  make_open(&slot, 6600, &body, &key);
  assert(lj_universe_try_close(&key, &close, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_freeze_external(&close, &epoch, NULL) ==
         LJ_UNIVERSE_OK);
  snap = lj_universe_snapshot(&slot);
  assert(snap.state == LJ_UNIVERSE_FINALIZING);
  memset(&stale, 0, sizeof(stale));
  stale.key = key;
  stale.authority = snap;
  memset(&pause, 0, sizeof(pause));
  lj_universe_test_set_poison_hook(poison_pause_hook, &pause);
  assert(pthread_create(&thread, NULL, poison_snapshot_call_thread,
                        &stale) == 0);
  while (!la_load32_acq(&pause.entered))
    la_cpu_pause();

  /* An old exact FINALIZING authority may not adopt FINAL_DRAIN authority. */
  good = close;
  assert(lj_universe_close_begin_final_drain(&good, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_snapshot(&slot).state == LJ_UNIVERSE_FINAL_DRAIN);
  la_store32_rel(&pause.release, 1);
  assert(pthread_join(thread, NULL) == 0);
  lj_universe_test_set_poison_hook(NULL, NULL);
  assert(stale.result == LJ_UNIVERSE_DENIED);
  assert(lj_universe_snapshot(&slot).state == LJ_UNIVERSE_FINAL_DRAIN);
  assert(lj_universe_close_seal(&good, &epoch, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_recycle(&good, NULL) == LJ_UNIVERSE_OK);
}

static void test_stale_close_copies_across_recycle(void)
{
  LJUniverseSlot slot;
  LJUniverseKey old_key, new_key;
  LJUniverseClose close, twin;
  LJUniverseTxn txn;
  LJUniverseSnap snap;
  LJUniverseEpochSnap external, terminal;
  StagePause pause;
  StageCall delayed;
  TestBody old_body = { UINT64_C(0x11112222), 92 };
  TestBody new_body = { UINT64_C(0x33334444), 93 };
  pthread_t thread;
  uint64_t epoch;

  /* External freeze copy paused after CLOSING validation. */
  memset(&close, 0, sizeof(close));
  make_open(&slot, 7000, &old_body, &old_key);
  assert(lj_universe_try_close(&old_key, &close, NULL) == LJ_UNIVERSE_OK);
  memset(&pause, 0, sizeof(pause));
  pause.point = LJ_UNIVERSE_TEST_FREEZE_EXTERNAL;
  memset(&delayed, 0, sizeof(delayed));
  delayed.close = close;
  delayed.operation = LJ_UNIVERSE_TEST_FREEZE_EXTERNAL;
  lj_universe_test_set_stage_hook(stage_pause_hook, &pause);
  assert(pthread_create(&thread, NULL, stage_call_thread, &delayed) == 0);
  while (!la_load32_acq(&pause.entered))
    la_cpu_pause();
  twin = close;
  assert(lj_universe_close_freeze_external(&twin, &epoch, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_close_begin_final_drain(&twin, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_seal(&twin, &epoch, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_recycle(&twin, NULL) == LJ_UNIVERSE_OK);
  claim_open(&slot, &new_body, &new_key);
  la_store32_rel(&pause.release, 1);
  assert(pthread_join(thread, NULL) == 0);
  lj_universe_test_set_stage_hook(NULL, NULL);
  assert(delayed.result == LJ_UNIVERSE_STALE);
  external = lj_universe_external_epoch_snapshot(&slot);
  terminal = lj_universe_terminal_epoch_snapshot(&slot);
  assert(external.epoch == 0 && terminal.epoch == 0 &&
         external.incarnation == new_key.incarnation &&
         terminal.incarnation == new_key.incarnation);
  assert(lj_universe_snapshot(&slot).state == LJ_UNIVERSE_OPEN);
  assert(lj_universe_try_poison(&old_key, &snap) == LJ_UNIVERSE_STALE);
  assert(lj_universe_snapshot(&slot).state == LJ_UNIVERSE_OPEN);
  memset(&close, 0, sizeof(close));
  assert(lj_universe_try_close(&new_key, &close, NULL) == LJ_UNIVERSE_OK);
  finish_without_finalizer_transactions(&close, 1);

  /* Terminal freeze copy paused after FINAL_DRAIN validation. */
  memset(&close, 0, sizeof(close));
  claim_open(&slot, &old_body, &old_key);
  assert(lj_universe_try_close(&old_key, &close, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_freeze_external(&close, &epoch, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_close_begin_final_drain(&close, NULL) == LJ_UNIVERSE_OK);
  memset(&pause, 0, sizeof(pause));
  pause.point = LJ_UNIVERSE_TEST_FREEZE_TERMINAL;
  memset(&delayed, 0, sizeof(delayed));
  delayed.close = close;
  delayed.operation = LJ_UNIVERSE_TEST_FREEZE_TERMINAL;
  lj_universe_test_set_stage_hook(stage_pause_hook, &pause);
  assert(pthread_create(&thread, NULL, stage_call_thread, &delayed) == 0);
  while (!la_load32_acq(&pause.entered))
    la_cpu_pause();
  twin = close;
  assert(lj_universe_close_seal(&twin, &epoch, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_recycle(&twin, NULL) == LJ_UNIVERSE_OK);
  claim_open(&slot, &new_body, &new_key);
  la_store32_rel(&pause.release, 1);
  assert(pthread_join(thread, NULL) == 0);
  lj_universe_test_set_stage_hook(NULL, NULL);
  assert(delayed.result == LJ_UNIVERSE_STALE);
  terminal = lj_universe_terminal_epoch_snapshot(&slot);
  assert(terminal.epoch == 0 &&
         terminal.incarnation == new_key.incarnation &&
         lj_universe_snapshot(&slot).state == LJ_UNIVERSE_OPEN);
  memset(&close, 0, sizeof(close));
  assert(lj_universe_try_close(&new_key, &close, NULL) == LJ_UNIVERSE_OK);
  finish_without_finalizer_transactions(&close, 1);

  /* Recycler copy cannot reset successor tickets after reuse. */
  memset(&close, 0, sizeof(close));
  claim_open(&slot, &old_body, &old_key);
  assert(lj_universe_try_close(&old_key, &close, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_freeze_external(&close, &epoch, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_close_begin_final_drain(&close, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_seal(&close, &epoch, NULL) == LJ_UNIVERSE_OK);
  memset(&pause, 0, sizeof(pause));
  pause.point = LJ_UNIVERSE_TEST_RECYCLE;
  memset(&delayed, 0, sizeof(delayed));
  delayed.close = close;
  delayed.operation = LJ_UNIVERSE_TEST_RECYCLE;
  lj_universe_test_set_stage_hook(stage_pause_hook, &pause);
  assert(pthread_create(&thread, NULL, stage_call_thread, &delayed) == 0);
  while (!la_load32_acq(&pause.entered))
    la_cpu_pause();
  twin = close;
  assert(lj_universe_close_recycle(&twin, NULL) == LJ_UNIVERSE_OK);
  claim_open(&slot, &new_body, &new_key);
  memset(&txn, 0, sizeof(txn));
  assert(lj_universe_try_enter(&new_key, &txn, NULL) == LJ_UNIVERSE_OK);
  assert(txn.publication_ticket == 1);
  assert(retry_release(&txn, NULL) == LJ_UNIVERSE_OK);
  assert(la_load64_acq(&slot.next_publication) == 2);
  la_store32_rel(&pause.release, 1);
  assert(pthread_join(thread, NULL) == 0);
  lj_universe_test_set_stage_hook(NULL, NULL);
  assert(delayed.result == LJ_UNIVERSE_STALE);
  assert(la_load64_acq(&slot.next_publication) == 2 &&
         lj_universe_snapshot(&slot).state == LJ_UNIVERSE_OPEN);
  memset(&close, 0, sizeof(close));
  assert(lj_universe_try_close(&new_key, &close, NULL) == LJ_UNIVERSE_OK);
  finish_without_finalizer_transactions(&close, 2);
}
#endif

static void test_saturation_exhaustion_and_corruption(void)
{
  LJUniverseSlot slot;
  LJUniverseKey key;
  LJUniverseBuild build;
  LJUniverseTxn txn, trigger;
  LJUniverseClose close;
  LJUniverseSnap before, after;
  LJUniverseResult result;
  TestBody body = { 3, 33 };
  uint64_t epoch = 0;

  memset(&txn, 0, sizeof(txn));
  memset(&trigger, 0, sizeof(trigger));
  memset(&close, 0, sizeof(close));
  memset(&build, 0, sizeof(build));

  assert(lj_universe_slot_init_unpublished(&slot, UINT64_MAX, NULL));
  assert(lj_universe_try_claim(&slot, &build, &after) ==
         LJ_UNIVERSE_EXHAUSTED_RESULT);
  assert(after.incarnation == UINT64_MAX &&
         after.state == LJ_UNIVERSE_EXHAUSTED &&
         after.transaction_count == 0);
  assert(lj_universe_try_claim(&slot, &build, &after) ==
         LJ_UNIVERSE_EXHAUSTED_RESULT);

  /* The final live identity recycles to reserved EMPTY/max, then exhausts. */
  make_open(&slot, UINT64_MAX - 1u, &body, &key);
  assert(key.incarnation == UINT64_MAX - 1u);
  assert(lj_universe_try_close(&key, &close, NULL) == LJ_UNIVERSE_OK);
  finish_without_finalizer_transactions(&close, 1);
  after = lj_universe_snapshot(&slot);
  assert(after.incarnation == UINT64_MAX &&
         after.state == LJ_UNIVERSE_EMPTY);
  assert(lj_universe_try_claim(&slot, &build, &after) ==
         LJ_UNIVERSE_EXHAUSTED_RESULT);
  assert(after.incarnation == UINT64_MAX &&
         after.state == LJ_UNIVERSE_EXHAUSTED);

  /* Forged live incarnation UINT64_MAX poisons without wrapping any tag. */
  assert(lj_universe_slot_init_unpublished(&slot, UINT64_MAX, NULL));
  slot.token.value.hi = lj_universe_pack_hi(0, LJ_UNIVERSE_BUILDING);
  build.key.slot = &slot;
  build.key.incarnation = UINT64_MAX;
  build.active = 1;
  assert(lj_universe_try_publish(&build, &body, &key, &after) ==
         LJ_UNIVERSE_CORRUPT);
  after = lj_universe_snapshot(&slot);
  assert(after.incarnation == UINT64_MAX &&
         after.state == LJ_UNIVERSE_POISONED &&
         lj_universe_body_snapshot(&slot).incarnation == UINT64_MAX &&
         lj_universe_external_epoch_snapshot(&slot).incarnation ==
           UINT64_MAX &&
         lj_universe_terminal_epoch_snapshot(&slot).incarnation ==
           UINT64_MAX);
  make_open(&slot, 40, &body, &key);
  slot.token.value.hi = lj_universe_pack_hi(
    LJ_UNIVERSE_MAX_TRANSACTIONS, LJ_UNIVERSE_OPEN);
  before = lj_universe_snapshot(&slot);
  assert(lj_universe_try_enter(&key, &txn, &after) ==
         LJ_UNIVERSE_SATURATED);
  assert(!txn.active && lj_universe_snap_equal(&before, &after));
  after = lj_universe_snapshot(&slot);
  assert(lj_universe_snap_equal(&before, &after));

  make_open(&slot, 50, &body, &key);
  la_store64_rel(&slot.next_publication, LJ_UNIVERSE_LAST_PUBLICATION);
  assert(lj_universe_try_enter(&key, &txn, &after) == LJ_UNIVERSE_OK);
  assert(txn.publication_ticket == LJ_UNIVERSE_LAST_PUBLICATION);
  assert(la_load64_acq(&slot.next_publication) == UINT64_MAX);
  assert(lj_universe_try_enter(&key, &trigger, &after) ==
         LJ_UNIVERSE_TICKET_EXHAUSTED);
  assert(!trigger.active && txn.active);
  after = lj_universe_snapshot(&slot);
  assert(after.state == LJ_UNIVERSE_POISONED &&
         after.transaction_count == 1);
  assert(la_load64_acq(&slot.next_publication) == UINT64_MAX);
  assert(retry_release(&txn, &after) == LJ_UNIVERSE_OK);
  assert(after.state == LJ_UNIVERSE_POISONED &&
         after.transaction_count == 0);

  make_open(&slot, 60, &body, &key);
  slot.token.value.hi = lj_universe_pack_hi(0, 15);
  assert(lj_universe_try_enter(&key, &txn, &after) ==
         LJ_UNIVERSE_CORRUPT);
  after = lj_universe_snapshot(&slot);
  assert(after.state == LJ_UNIVERSE_POISONED &&
         after.incarnation == key.incarnation);

  make_open(&slot, 70, &body, &key);
  slot.body_value.hi = key.incarnation + 1u;
  assert(lj_universe_try_enter(&key, &txn, &after) ==
         LJ_UNIVERSE_CORRUPT);
  assert(lj_universe_snapshot(&slot).state == LJ_UNIVERSE_POISONED);

  assert(lj_universe_slot_init_unpublished(&slot, 0, NULL));
  la_store64_rel(&slot.next_publication, 2);
  assert(lj_universe_try_claim(&slot, &build, &after) ==
         LJ_UNIVERSE_CORRUPT);
  /* Incarnation zero cannot be safely invented merely to encode POISONED. */
  after = lj_universe_snapshot(&slot);
  assert(after.state == LJ_UNIVERSE_EMPTY && after.incarnation == 0);

  make_open(&slot, 80, &body, &key);
  assert(lj_universe_try_close(&key, &close, NULL) == LJ_UNIVERSE_OK);
  slot.external_final_publication.lo = 99;
  result = lj_universe_close_freeze_external(&close, &epoch, &after);
  assert(result == LJ_UNIVERSE_CORRUPT);
  assert(close.active && lj_universe_snapshot(&slot).state ==
         LJ_UNIVERSE_POISONED);
}

static void test_malformed_epoch_and_recycle_proofs(void)
{
  LJUniverseSlot slot;
  LJUniverseKey key;
  LJUniverseClose close;
  LJUniverseSnap snap;
  TestBody body = { UINT64_C(0x9999aaaa), 94 };
  uint64_t epoch;

  /* A non-zero terminal marker in exact FINAL_DRAIN is corruption. */
  memset(&close, 0, sizeof(close));
  make_open(&slot, 9000, &body, &key);
  assert(lj_universe_try_close(&key, &close, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_freeze_external(&close, &epoch, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_close_begin_final_drain(&close, NULL) == LJ_UNIVERSE_OK);
  slot.terminal_final_publication.lo = 99;
  assert(lj_universe_close_seal(&close, &epoch, &snap) ==
         LJ_UNIVERSE_CORRUPT);
  assert(lj_universe_snapshot(&slot).state == LJ_UNIVERSE_POISONED);

  /* SEALED proof requires authoritative next == terminal before body clear. */
  memset(&close, 0, sizeof(close));
  make_open(&slot, 9100, &body, &key);
  assert(lj_universe_try_close(&key, &close, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_freeze_external(&close, &epoch, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_close_begin_final_drain(&close, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_seal(&close, &epoch, NULL) == LJ_UNIVERSE_OK);
  la_store64_rel(&slot.next_publication, epoch + 1u);
  assert(lj_universe_close_recycle(&close, &snap) == LJ_UNIVERSE_CORRUPT);
  assert(lj_universe_snapshot(&slot).state == LJ_UNIVERSE_POISONED &&
         lj_universe_body_snapshot(&slot).body == &body);

  /* A malformed local copy cannot poison valid authoritative SEALED data. */
  memset(&close, 0, sizeof(close));
  make_open(&slot, 9200, &body, &key);
  assert(lj_universe_try_close(&key, &close, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_freeze_external(&close, &epoch, NULL) ==
         LJ_UNIVERSE_OK);
  assert(lj_universe_close_begin_final_drain(&close, NULL) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_seal(&close, &epoch, NULL) == LJ_UNIVERSE_OK);
  close.terminal_final_publication++;
  assert(lj_universe_close_recycle(&close, &snap) == LJ_UNIVERSE_CORRUPT);
  assert(lj_universe_snapshot(&slot).state == LJ_UNIVERSE_SEALED);
  close.terminal_final_publication = 0;
  assert(lj_universe_close_recycle(&close, &snap) == LJ_UNIVERSE_OK);
  assert(snap.state == LJ_UNIVERSE_EMPTY);
}

enum { CLOSE_RACERS = 12 };

typedef struct CloseRace {
  LJUniverseKey key;
  uint32_t ready;
  uint32_t go;
} CloseRace;

typedef struct CloseRacer {
  CloseRace *race;
  LJUniverseClose close;
  LJUniverseResult result;
} CloseRacer;

static void *close_racer(void *ud)
{
  CloseRacer *racer = (CloseRacer *)ud;
  memset(&racer->close, 0, sizeof(racer->close));
  (void)la_add32_acqrel(&racer->race->ready, 1);
  while (!la_load32_acq(&racer->race->go))
    la_cpu_pause();
  racer->result = lj_universe_try_close(&racer->race->key,
                                        &racer->close, NULL);
  return NULL;
}

static void test_close_winner_contract(void)
{
  LJUniverseSlot slot;
  LJUniverseClose loser;
  LJUniverseKey key;
  LJUniverseSnap snap;
  CloseRace race;
  CloseRacer racers[CLOSE_RACERS];
  pthread_t threads[CLOSE_RACERS];
  TestBody body = { 4, 44 };
  unsigned i, winners = 0, active = 0, winner_index = 0;

  memset(&race, 0, sizeof(race));
  memset(racers, 0, sizeof(racers));
  memset(&loser, 0, sizeof(loser));
  make_open(&slot, 100, &body, &key);
  race.key = key;
  for (i = 0; i < CLOSE_RACERS; i++) {
    racers[i].race = &race;
    assert(pthread_create(&threads[i], NULL, close_racer, &racers[i]) == 0);
  }
  while (la_load32_acq(&race.ready) != CLOSE_RACERS)
    la_cpu_pause();
  la_store32_rel(&race.go, 1);
  for (i = 0; i < CLOSE_RACERS; i++) {
    assert(pthread_join(threads[i], NULL) == 0);
    if (racers[i].result == LJ_UNIVERSE_OK) {
      winners++;
      winner_index = i;
    } else {
      assert(racers[i].result == LJ_UNIVERSE_LOST ||
             racers[i].result == LJ_UNIVERSE_DENIED);
    }
    if (racers[i].close.active)
      active++;
    assert((racers[i].result == LJ_UNIVERSE_OK) ==
           (racers[i].close.active != 0));
  }
  assert(winners == 1 && active == 1);
  assert(lj_universe_snapshot(&slot).state == LJ_UNIVERSE_CLOSING);

  /* Seeing the state does not let a loser reconstruct close ownership. */
  assert(lj_universe_try_close(&key, &loser, &snap) ==
         LJ_UNIVERSE_DENIED);
  assert(!loser.active && snap.state == LJ_UNIVERSE_CLOSING);
  finish_without_finalizer_transactions(&racers[winner_index].close, 1);
}

enum { REINCARNATION_READERS = 4, REINCARNATION_ROUNDS = 750 };

typedef struct ReincarnationStress {
  LJUniverseSlot slot;
  TestBody bodies[2];
  uint32_t ready;
  uint32_t go;
  uint32_t done;
  uint64_t admitted;
  uint64_t rejected;
} ReincarnationStress;

static void *reincarnation_reader(void *ud)
{
  ReincarnationStress *stress = (ReincarnationStress *)ud;
  (void)la_add32_acqrel(&stress->ready, 1);
  while (!la_load32_acq(&stress->go))
    la_cpu_pause();
  while (!la_load32_acq(&stress->done)) {
    LJUniverseSnap snap = lj_universe_snapshot(&stress->slot);
    assert(lj_universe_components_valid(snap.incarnation,
                                        snap.transaction_count,
                                        snap.state));
    if (snap.state == LJ_UNIVERSE_OPEN) {
      LJUniverseKey key;
      LJUniverseTxn txn;
      LJUniverseResult result;
      key.slot = &stress->slot;
      key.incarnation = snap.incarnation;
      memset(&txn, 0, sizeof(txn));
      result = lj_universe_try_enter(&key, &txn, NULL);
      if (result == LJ_UNIVERSE_OK) {
        TestBody *expected = &stress->bodies[key.incarnation & 1u];
        assert(txn.body == expected);
        assert(expected->serial == (key.incarnation & 1u));
        (void)la_add64_rlx(&stress->admitted, 1);
        assert(retry_release(&txn, NULL) == LJ_UNIVERSE_OK);
      } else {
        assert(result == LJ_UNIVERSE_LOST ||
               result == LJ_UNIVERSE_STALE ||
               result == LJ_UNIVERSE_DENIED);
        (void)la_add64_rlx(&stress->rejected, 1);
      }
    }
  }
  return NULL;
}

static void close_recycle_for_reincarnation(LJUniverseKey *key)
{
  LJUniverseClose close;
  LJUniverseResult result;
  LJUniverseSnap snap;
  uint64_t epoch;
  memset(&close, 0, sizeof(close));
  do {
    result = lj_universe_try_close(key, &close, &snap);
  } while (result == LJ_UNIVERSE_LOST);
  assert(result == LJ_UNIVERSE_OK);
  do {
    result = lj_universe_close_freeze_external(&close, &epoch, &snap);
  } while (result == LJ_UNIVERSE_LOST || result == LJ_UNIVERSE_BUSY);
  assert(result == LJ_UNIVERSE_OK);
  assert(retry_final_drain(&close, &snap) == LJ_UNIVERSE_OK);
  do {
    result = lj_universe_close_seal(&close, &epoch, &snap);
  } while (result == LJ_UNIVERSE_LOST || result == LJ_UNIVERSE_BUSY);
  assert(result == LJ_UNIVERSE_OK);
  do {
    result = lj_universe_close_recycle(&close, &snap);
  } while (result == LJ_UNIVERSE_LOST);
  assert(result == LJ_UNIVERSE_OK);
}

/*
** The state/count half intentionally returns to OPEN/0 in every incarnation.
** Readers continuously snapshot that recurring hi value and try to admit the
** exact incarnation they observed while the writer performs full lifecycles.
** An incoherent historical lo/recurring-hi pair can poison a newer body; the
** exact cmpxchg16b snapshot plus keyed admission must instead yield only an
** exact success or a harmless LOST/STALE/DENIED result.
*/
static void test_snapshot_coherence_across_reincarnation(void)
{
  ReincarnationStress stress;
  LJUniverseKey key;
  LJUniverseBuild build;
  LJUniverseSnap snap;
  LJUniverseResult result;
  pthread_t readers[REINCARNATION_READERS];
  unsigned i, round;

  memset(&stress, 0, sizeof(stress));
  memset(&build, 0, sizeof(build));
  stress.bodies[0].magic = UINT64_C(0xaaaa);
  stress.bodies[0].serial = 0;
  stress.bodies[1].magic = UINT64_C(0xbbbb);
  stress.bodies[1].serial = 1;
  make_open(&stress.slot, 0, &stress.bodies[1], &key);
  for (i = 0; i < REINCARNATION_READERS; i++)
    assert(pthread_create(&readers[i], NULL, reincarnation_reader,
                          &stress) == 0);
  while (la_load32_acq(&stress.ready) != REINCARNATION_READERS)
    la_cpu_pause();
  la_store32_rel(&stress.go, 1);

  for (round = 0; round < REINCARNATION_ROUNDS; round++) {
    close_recycle_for_reincarnation(&key);
    do {
      result = lj_universe_try_claim(&stress.slot, &build, &snap);
    } while (result == LJ_UNIVERSE_LOST);
    assert(result == LJ_UNIVERSE_OK);
    result = lj_universe_try_publish(
      &build, &stress.bodies[build.key.incarnation & 1u], &key, &snap);
    assert(result == LJ_UNIVERSE_OK);
    if ((round & 15u) == 0)
      la_cpu_pause();
  }
  la_store32_rel(&stress.done, 1);
  for (i = 0; i < REINCARNATION_READERS; i++)
    assert(pthread_join(readers[i], NULL) == 0);
  assert(la_load64_acq(&stress.admitted) != 0);
  assert(lj_universe_snapshot(&stress.slot).state == LJ_UNIVERSE_OPEN);
  close_recycle_for_reincarnation(&key);
}

enum {
  STRESS_THREADS = 8,
  STRESS_ROUNDS = 25000,
  STRESS_MAX_TICKETS = STRESS_THREADS * STRESS_ROUNDS + 2
};

typedef struct TxnStress {
  LJUniverseKey key;
  TestBody *body;
  uint32_t ready;
  uint32_t go;
  uint64_t admitted;
  uint64_t released;
  uint8_t seen[STRESS_MAX_TICKETS];
} TxnStress;

static void *txn_stress_worker(void *ud)
{
  TxnStress *stress = (TxnStress *)ud;
  unsigned i;
  (void)la_add32_acqrel(&stress->ready, 1);
  while (!la_load32_acq(&stress->go))
    la_cpu_pause();
  for (i = 0; i < STRESS_ROUNDS; i++) {
    LJUniverseTxn txn;
    LJUniverseResult result;
    memset(&txn, 0, sizeof(txn));
    result = lj_universe_try_enter(&stress->key, &txn, NULL);
    if (result == LJ_UNIVERSE_LOST)
      continue;
    if (result == LJ_UNIVERSE_DENIED)
      break;
    assert(result == LJ_UNIVERSE_OK);
    assert(txn.body == stress->body &&
           stress->body->magic == UINT64_C(0x123456789abcdef0));
    assert(txn.publication_ticket < STRESS_MAX_TICKETS);
    assert(__atomic_exchange_n(&stress->seen[txn.publication_ticket], 1,
                               __ATOMIC_ACQ_REL) == 0);
    (void)la_add64_rlx(&stress->admitted, 1);
    if ((i & 31u) == 0)
      la_cpu_pause();
    assert(retry_release(&txn, NULL) == LJ_UNIVERSE_OK);
    (void)la_add64_rlx(&stress->released, 1);
  }
  return NULL;
}

static void test_transaction_close_stress(void)
{
  LJUniverseSlot slot;
  LJUniverseKey key;
  LJUniverseClose close;
  LJUniverseSnap snap;
  LJUniverseResult result;
  TxnStress stress;
  pthread_t threads[STRESS_THREADS];
  TestBody body = { UINT64_C(0x123456789abcdef0), 55 };
  uint64_t admitted, epoch;
  unsigned i;

  memset(&stress, 0, sizeof(stress));
  memset(&close, 0, sizeof(close));
  make_open(&slot, 300, &body, &key);
  stress.key = key;
  stress.body = &body;
  for (i = 0; i < STRESS_THREADS; i++)
    assert(pthread_create(&threads[i], NULL, txn_stress_worker, &stress) == 0);
  while (la_load32_acq(&stress.ready) != STRESS_THREADS)
    la_cpu_pause();
  la_store32_rel(&stress.go, 1);
  while (la_load64_acq(&stress.admitted) < STRESS_THREADS)
    la_cpu_pause();
  do {
    result = lj_universe_try_close(&key, &close, &snap);
  } while (result == LJ_UNIVERSE_LOST);
  assert(result == LJ_UNIVERSE_OK && close.active);
  assert(snap.state == LJ_UNIVERSE_CLOSING);
  for (i = 0; i < STRESS_THREADS; i++)
    assert(pthread_join(threads[i], NULL) == 0);

  admitted = la_load64_acq(&stress.admitted);
  assert(admitted >= STRESS_THREADS);
  assert(admitted == la_load64_acq(&stress.released));
  assert(lj_universe_snapshot(&slot).transaction_count == 0);
  assert(lj_universe_close_freeze_external(&close, &epoch, &snap) ==
         LJ_UNIVERSE_OK);
  assert(epoch == admitted + 1u);
  assert(retry_final_drain(&close, &snap) == LJ_UNIVERSE_OK);
  assert(lj_universe_close_seal(&close, &epoch, &snap) == LJ_UNIVERSE_OK);
  assert(epoch == admitted + 1u);
  assert(lj_universe_close_recycle(&close, &snap) == LJ_UNIVERSE_OK);
}

int main(void)
{
  test_layout_components_and_invalid_inputs();
  test_scalar_epochs_and_reuse();
  test_build_abort_failure_retry_and_race();
  test_poisoned_exact_drain();
#if defined(LJ_UNIVERSE_TEST_HELPERS)
  test_exact_poison_through_concurrent_drain();
  test_copied_close_stage_decisions();
  test_irreversible_epoch_markers();
  test_handle_local_epoch_mismatch();
  test_poison_authority_stage_toctou();
  test_stale_close_copies_across_recycle();
#endif
  test_saturation_exhaustion_and_corruption();
  test_malformed_epoch_and_recycle_proofs();
  test_close_winner_contract();
  test_snapshot_coherence_across_reincarnation();
  test_transaction_close_stress();
  puts("t-universe-token OK: exact admission, epochs, close ownership, and poison drain verified");
  return 0;
}
