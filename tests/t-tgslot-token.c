/*
** t-tgslot-token.c - standalone stable TG-slot lifecycle/lease model.
**
** Build & run:
**   cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -mcx16 -Isrc \
**      tests/t-tgslot-token.c -o /tmp/t-tgslot-token && \
**      /tmp/t-tgslot-token
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_tgslot.h"

static LJTGSlotResult retry_edge(LJTGSlotResult (*edge)(
                                   const LJTGSlotKey *, LJTGSlotSnap *),
                                 const LJTGSlotKey *key,
                                 LJTGSlotSnap *observed)
{
  LJTGSlotResult result;
  do {
    result = edge(key, observed);
  } while (result == LJ_TGSLOT_LOST);
  return result;
}

static void make_live(LJTGSlotToken *slot, uint64_t empty_incarnation,
                      LJTGSlotKey *key)
{
  LJTGSlotSnap snap;
  assert(lj_tgslot_init_empty_unpublished(slot, empty_incarnation));
  assert(lj_tgslot_try_claim(slot, key, &snap) == LJ_TGSLOT_OK);
  assert(snap.incarnation == empty_incarnation + 1u);
  assert(snap.lease_count == 1);
  assert(snap.state == LJ_TGSLOT_ATTACHING);
  assert(retry_edge(lj_tgslot_try_publish, key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_LIVE);
}

static void test_scalar_lifecycle_and_reuse(void)
{
  LJTGSlotToken slot;
  LJTGSlotKey first, second;
  LJTGSlotSnap before, snap;

  make_live(&slot, 0, &first);
  assert(first.slot == &slot);
  assert(first.incarnation == 1);
  before = lj_tgslot_snapshot(&slot);
  assert(lj_tgslot_try_borrow(&first, &snap) == LJ_TGSLOT_OK);
  assert(snap.lease_count == 2 && snap.state == LJ_TGSLOT_LIVE);

  /* A delayed close cannot erase a lease admitted after its snapshot. */
  assert(lj_tgslot_try_replace(&slot, &before, before.incarnation,
                               before.lease_count, LJ_TGSLOT_DETACHING,
                               &snap) == LJ_TGSLOT_LOST);
  assert(snap.lease_count == 2 && snap.state == LJ_TGSLOT_LIVE);
  assert(retry_edge(lj_tgslot_try_detach, &first, &snap) == LJ_TGSLOT_OK);
  assert(snap.lease_count == 2 && snap.state == LJ_TGSLOT_DETACHING);
  assert(lj_tgslot_try_release(&first, &snap) == LJ_TGSLOT_OK);
  assert(snap.lease_count == 1 && snap.state == LJ_TGSLOT_DETACHING);
  assert(retry_edge(lj_tgslot_try_retire, &first, &snap) == LJ_TGSLOT_OK);
  assert(snap.lease_count == 1 && snap.state == LJ_TGSLOT_RETIRED);
  assert(lj_tgslot_try_begin_reclaim(&first, &snap) == LJ_TGSLOT_BUSY);
  assert(lj_tgslot_try_release(&first, &snap) == LJ_TGSLOT_OK);
  assert(snap.lease_count == 0 && snap.state == LJ_TGSLOT_RETIRED);
  assert(retry_edge(lj_tgslot_try_begin_reclaim, &first, &snap) ==
         LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_RECLAIMING);
  assert(retry_edge(lj_tgslot_try_finish_reclaim, &first, &snap) ==
         LJ_TGSLOT_OK);
  assert(snap.incarnation == 1 && snap.state == LJ_TGSLOT_EMPTY);

  /* The same address is reusable, but never under the old stable key. */
  assert(lj_tgslot_try_claim(&slot, &second, &snap) == LJ_TGSLOT_OK);
  assert(second.slot == first.slot);
  assert(second.incarnation == first.incarnation + 1u);
  assert(!lj_tgslot_key_equal(&first, &second));
  assert(lj_tgslot_try_borrow(&first, &snap) == LJ_TGSLOT_STALE);
  assert(lj_tgslot_try_release(&first, &snap) == LJ_TGSLOT_STALE);
  assert(lj_tgslot_try_publish(&first, &snap) == LJ_TGSLOT_STALE);
  assert(retry_edge(lj_tgslot_try_publish, &second, &snap) == LJ_TGSLOT_OK);
  assert(lj_tgslot_try_borrow(&first, &snap) == LJ_TGSLOT_STALE);
}

static void test_borrow_retire_linearization(void)
{
  LJTGSlotToken slot;
  LJTGSlotKey key;
  LJTGSlotSnap snap;

  make_live(&slot, 40, &key);
  assert(retry_edge(lj_tgslot_try_detach, &key, NULL) == LJ_TGSLOT_OK);

  /* Borrow-before-retire is retained in RETIRED's exact lease count. */
  assert(lj_tgslot_try_borrow(&key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_DETACHING && snap.lease_count == 2);
  assert(retry_edge(lj_tgslot_try_retire, &key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_RETIRED && snap.lease_count == 2);

  /* Retire-before-borrow closes admission, even while leases remain. */
  assert(lj_tgslot_try_borrow(&key, &snap) == LJ_TGSLOT_DENIED);
  assert(lj_tgslot_try_begin_reclaim(&key, &snap) == LJ_TGSLOT_BUSY);
  assert(lj_tgslot_try_release(&key, &snap) == LJ_TGSLOT_OK);
  assert(lj_tgslot_try_release(&key, &snap) == LJ_TGSLOT_OK);
  assert(retry_edge(lj_tgslot_try_begin_reclaim, &key, NULL) ==
         LJ_TGSLOT_OK);
  assert(retry_edge(lj_tgslot_try_finish_reclaim, &key, NULL) ==
         LJ_TGSLOT_OK);
}

static void test_attaching_borrow_and_abort(void)
{
  LJTGSlotToken slot;
  LJTGSlotKey key;
  LJTGSlotSnap snap;

  assert(lj_tgslot_init_empty_unpublished(&slot, 80));
  assert(lj_tgslot_try_claim(&slot, &key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_ATTACHING && snap.lease_count == 1);
  assert(lj_tgslot_try_borrow(&key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_ATTACHING && snap.lease_count == 2);
  assert(retry_edge(lj_tgslot_try_abort_attach, &key, &snap) ==
         LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_RETIRED && snap.lease_count == 2);
  assert(lj_tgslot_try_borrow(&key, &snap) == LJ_TGSLOT_DENIED);
  assert(lj_tgslot_try_release(&key, &snap) == LJ_TGSLOT_OK);
  assert(lj_tgslot_try_release(&key, &snap) == LJ_TGSLOT_OK);
  assert(retry_edge(lj_tgslot_try_begin_reclaim, &key, NULL) ==
         LJ_TGSLOT_OK);
  assert(retry_edge(lj_tgslot_try_finish_reclaim, &key, NULL) ==
         LJ_TGSLOT_OK);
}

static int expected_edge(unsigned from, unsigned to)
{
  if (from > LJ_TGSLOT_EXHAUSTED || to > LJ_TGSLOT_EXHAUSTED ||
      from == to || from == LJ_TGSLOT_PINNED ||
      from == LJ_TGSLOT_EXHAUSTED)
    return 0;
  if (to == LJ_TGSLOT_PINNED)
    return from >= LJ_TGSLOT_ATTACHING && from <= LJ_TGSLOT_RETIRED;
  if (from == LJ_TGSLOT_EMPTY && to == LJ_TGSLOT_EXHAUSTED)
    return 1;
  if (from == LJ_TGSLOT_ATTACHING && to == LJ_TGSLOT_RETIRED)
    return 1;
  return to == from + 1u ||
         (from == LJ_TGSLOT_RECLAIMING && to == LJ_TGSLOT_EMPTY);
}

static void test_invalid_edges_and_owner_lease(void)
{
  LJTGSlotToken slot;
  LJTGSlotKey key, bad;
  LJTGSlotSnap snap;
  unsigned from, to;

  for (from = 0; from <= LJ_TGSLOT_EXHAUSTED + 1u; from++) {
    for (to = 0; to <= LJ_TGSLOT_EXHAUSTED + 1u; to++)
      assert(lj_tgslot_state_edge_valid((uint8_t)from, (uint8_t)to) ==
             expected_edge(from, to));
  }

  assert(!lj_tgslot_init_unpublished(&slot, 0, 1, LJ_TGSLOT_LIVE));
  assert(!lj_tgslot_init_unpublished(&slot, 1, 0, LJ_TGSLOT_LIVE));
  assert(!lj_tgslot_init_unpublished(&slot, 1, 1,
                                      LJ_TGSLOT_RECLAIMING));
  assert(!lj_tgslot_init_unpublished(&slot, 1, 0,
                                      LJ_TGSLOT_EXHAUSTED + 1u));
  assert(!lj_tgslot_init_unpublished(NULL, 0, 0, LJ_TGSLOT_EMPTY));
  assert(lj_tgslot_init_empty_unpublished(&slot, 5));
  snap = lj_tgslot_snapshot(&slot);
  assert(lj_tgslot_try_pin(&slot, &snap, NULL) == LJ_TGSLOT_INVALID);
  assert(lj_tgslot_snapshot(&slot).state == LJ_TGSLOT_EMPTY);

  make_live(&slot, 100, &key);
  snap = lj_tgslot_snapshot(&slot);
  assert(lj_tgslot_try_replace(&slot, &snap, snap.incarnation, 0,
                               LJ_TGSLOT_EMPTY, NULL) == LJ_TGSLOT_INVALID);
  assert(lj_tgslot_try_replace(&slot, &snap, snap.incarnation + 1u,
                               snap.lease_count, LJ_TGSLOT_LIVE, NULL) ==
         LJ_TGSLOT_INVALID);
  assert(lj_tgslot_try_publish(&key, &snap) == LJ_TGSLOT_DENIED);
  assert(lj_tgslot_try_retire(&key, &snap) == LJ_TGSLOT_DENIED);
  assert(lj_tgslot_try_begin_reclaim(&key, &snap) == LJ_TGSLOT_DENIED);
  assert(lj_tgslot_try_finish_reclaim(&key, &snap) == LJ_TGSLOT_DENIED);

  /* The last body lease cannot disappear from an exposed slot. */
  assert(lj_tgslot_try_release(&key, &snap) == LJ_TGSLOT_BUSY);
  assert(snap.state == LJ_TGSLOT_LIVE && snap.lease_count == 1);

  bad = key;
  bad.incarnation = 0;
  assert(lj_tgslot_try_borrow(&bad, NULL) == LJ_TGSLOT_INVALID);
  assert(lj_tgslot_try_release(&bad, NULL) == LJ_TGSLOT_INVALID);
  assert(lj_tgslot_try_keyed_edge(&key, LJ_TGSLOT_LIVE,
                                  LJ_TGSLOT_RETIRED, 0, NULL) ==
         LJ_TGSLOT_INVALID);
}

static void test_sticky_saturation(void)
{
  LJTGSlotToken slot;
  LJTGSlotKey key;
  LJTGSlotSnap snap;

  /* Reusing UINT64_MAX would recreate an old key. The body is already gone,
  ** so exhaustion is terminal-empty rather than a live-body PINNED lease. */
  assert(lj_tgslot_init_empty_unpublished(&slot, UINT64_MAX));
  assert(lj_tgslot_try_claim(&slot, &key, &snap) ==
         LJ_TGSLOT_EXHAUSTED_RESULT);
  assert(snap.incarnation == UINT64_MAX && snap.lease_count == 0);
  assert(snap.state == LJ_TGSLOT_EXHAUSTED);
  assert(lj_tgslot_try_claim(&slot, &key, &snap) ==
         LJ_TGSLOT_EXHAUSTED_RESULT);
  key.slot = &slot;
  key.incarnation = UINT64_MAX;
  assert(lj_tgslot_try_borrow(&key, &snap) == LJ_TGSLOT_STALE);
  assert(lj_tgslot_try_release(&key, &snap) == LJ_TGSLOT_STALE);

  /* Lease count exhaustion is also sticky and never wraps to zero. */
  assert(lj_tgslot_init_unpublished(&slot, 9, LJ_TGSLOT_MAX_LEASES,
                                     LJ_TGSLOT_LIVE));
  key.slot = &slot;
  key.incarnation = 9;
  assert(lj_tgslot_try_borrow(&key, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
  assert(snap.incarnation == 9);
  assert(snap.lease_count == LJ_TGSLOT_MAX_LEASES);
  assert(snap.state == LJ_TGSLOT_PINNED);
  assert(lj_tgslot_try_detach(&key, &snap) == LJ_TGSLOT_PINNED_RESULT);
  assert(lj_tgslot_try_borrow(&key, &snap) == LJ_TGSLOT_PINNED_RESULT);

  /* Accounting can drain, but no release can unpin or admit reclamation. */
  assert(lj_tgslot_try_release(&key, &snap) == LJ_TGSLOT_OK);
  assert(snap.lease_count == LJ_TGSLOT_MAX_LEASES - 1u);
  assert(snap.state == LJ_TGSLOT_PINNED);
  assert(lj_tgslot_try_begin_reclaim(&key, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
}

enum { BORROWER_THREADS = 8, BORROWER_ROUNDS = 50000 };

typedef struct SlotStress {
  LJTGSlotToken slot;
  LJTGSlotKey key;
  uint32_t ready;
  uint32_t go;
  uint32_t release_gate;
  uint32_t holding;
  uint64_t borrowed;
  uint64_t released;
  uint32_t busy_reclaims;
} SlotStress;

static void *borrower_thread(void *ud)
{
  SlotStress *stress = (SlotStress *)ud;
  unsigned i;
  (void)la_add32_acqrel(&stress->ready, 1);
  while (!la_load32_acq(&stress->go))
    la_cpu_pause();
  for (i = 0; i < BORROWER_ROUNDS; i++) {
    LJTGSlotResult result = lj_tgslot_try_borrow(&stress->key, NULL);
    if (result == LJ_TGSLOT_LOST)
      continue;
    if (result == LJ_TGSLOT_DENIED)
      break;
    assert(result == LJ_TGSLOT_OK);
    (void)la_add64_rlx(&stress->borrowed, 1);
    (void)la_add32_acqrel(&stress->holding, 1);
    while (!la_load32_acq(&stress->release_gate))
      la_cpu_pause();
    do {
      result = lj_tgslot_try_release(&stress->key, NULL);
    } while (result == LJ_TGSLOT_LOST);
    assert(result == LJ_TGSLOT_OK);
    (void)la_sub32_acqrel(&stress->holding, 1);
    (void)la_add64_rlx(&stress->released, 1);
  }
  return NULL;
}

static void *reclaimer_thread(void *ud)
{
  SlotStress *stress = (SlotStress *)ud;
  LJTGSlotResult result;
  (void)la_add32_acqrel(&stress->ready, 1);
  while (!la_load32_acq(&stress->go))
    la_cpu_pause();
  while (la_load32_acq(&stress->holding) == 0)
    la_cpu_pause();

  do {
    result = lj_tgslot_try_detach(&stress->key, NULL);
  } while (result == LJ_TGSLOT_LOST);
  assert(result == LJ_TGSLOT_OK);
  do {
    result = lj_tgslot_try_retire(&stress->key, NULL);
  } while (result == LJ_TGSLOT_LOST);
  assert(result == LJ_TGSLOT_OK);

  /* Drop the body lease only after admission is closed by RETIRED. */
  do {
    result = lj_tgslot_try_release(&stress->key, NULL);
  } while (result == LJ_TGSLOT_LOST);
  assert(result == LJ_TGSLOT_OK);

  result = lj_tgslot_try_begin_reclaim(&stress->key, NULL);
  assert(result == LJ_TGSLOT_BUSY);
  (void)la_add32_rlx(&stress->busy_reclaims, 1);
  la_store32_rel(&stress->release_gate, 1);
  for (;;) {
    result = lj_tgslot_try_begin_reclaim(&stress->key, NULL);
    if (result == LJ_TGSLOT_OK)
      break;
    if (result == LJ_TGSLOT_BUSY)
      (void)la_add32_rlx(&stress->busy_reclaims, 1);
    else
      assert(result == LJ_TGSLOT_LOST);
    la_cpu_pause();
  }
  assert(retry_edge(lj_tgslot_try_finish_reclaim, &stress->key, NULL) ==
         LJ_TGSLOT_OK);
  return NULL;
}

static void test_concurrent_borrowers_and_reclaimer(void)
{
  SlotStress stress;
  pthread_t borrowers[BORROWER_THREADS], reclaimer;
  LJTGSlotSnap final;
  unsigned i;

  memset(&stress, 0, sizeof(stress));
  make_live(&stress.slot, 700, &stress.key);
  for (i = 0; i < BORROWER_THREADS; i++)
    assert(pthread_create(&borrowers[i], NULL, borrower_thread, &stress) == 0);
  assert(pthread_create(&reclaimer, NULL, reclaimer_thread, &stress) == 0);
  while (la_load32_acq(&stress.ready) != BORROWER_THREADS + 1u)
    la_cpu_pause();
  la_store32_rel(&stress.go, 1);

  assert(pthread_join(reclaimer, NULL) == 0);
  for (i = 0; i < BORROWER_THREADS; i++)
    assert(pthread_join(borrowers[i], NULL) == 0);
  final = lj_tgslot_snapshot(&stress.slot);
  assert(final.incarnation == stress.key.incarnation);
  assert(final.lease_count == 0);
  assert(final.state == LJ_TGSLOT_EMPTY);
  assert(la_load64_acq(&stress.borrowed) != 0);
  assert(la_load64_acq(&stress.borrowed) ==
         la_load64_acq(&stress.released));
  assert(la_load32_acq(&stress.busy_reclaims) != 0);
}

int main(void)
{
  test_scalar_lifecycle_and_reuse();
  test_borrow_retire_linearization();
  test_attaching_borrow_and_abort();
  test_invalid_edges_and_owner_lease();
  test_sticky_saturation();
  test_concurrent_borrowers_and_reclaimer();
  printf("t-tgslot-token OK: stable reuse and concurrent leases verified\n");
  return 0;
}
