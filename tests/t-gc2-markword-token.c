/*
** t-gc2-markword-token.c - standalone GC2 markword/activation model.
**
** Build & run:
**   cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -mcx16 -Isrc \
**      tests/t-gc2-markword-token.c -o /tmp/t-gc2-markword-token && \
**      /tmp/t-gc2-markword-token
*/

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_gc2token.h"
#include "lj_markword.h"

static void test_markword_epochs_and_clear(void)
{
  LJArenaMarkWord word;
  LJArenaMarkSnap snap;

  assert(lj_markword_init_unpublished(&word, 10, 0));
  assert(!lj_markword_init_unpublished(&word, 0, 1));
  assert(lj_markword_init_unpublished(&word, 10, 0));
  assert(lj_markword_set(&word, 10, 2) == LJ_ARENA_MARK_CHANGED);
  assert(lj_markword_set(&word, 10, 9) == LJ_ARENA_MARK_CHANGED);
  assert(lj_markword_set(&word, 10, 9) == LJ_ARENA_MARK_UNCHANGED);
  assert(lj_markword_clear(&word, 10, 2) == LJ_ARENA_MARK_CHANGED);
  snap = lj_markword_snapshot(&word);
  assert(snap.epoch == 10);
  assert(snap.bits == (UINT64_C(1) << 9));

  /* Clearing one cell cannot erase an unrelated cell in the same word. */
  assert(!lj_markword_test(&word, 10, 2));
  assert(lj_markword_test(&word, 10, 9));

  /* An E writer cannot modify a word already advanced to E+1. */
  assert(lj_markword_set(&word, 11, 4) == LJ_ARENA_MARK_CHANGED);
  assert(lj_markword_set(&word, 10, 1) == LJ_ARENA_MARK_STALE);
  assert(lj_markword_clear(&word, 10, 4) == LJ_ARENA_MARK_STALE);
  snap = lj_markword_snapshot(&word);
  assert(snap.epoch == 11);
  assert(snap.bits == (UINT64_C(1) << 4));
  assert(lj_markword_clear(&word, 12, 4) == LJ_ARENA_MARK_UNCHANGED);
  assert(lj_markword_set(&word, 0, 1) == LJ_ARENA_MARK_INVALID);
  assert(lj_markword_clear(&word, 0, 1) == LJ_ARENA_MARK_INVALID);
  assert(lj_markword_set(&word, 11, 64) == LJ_ARENA_MARK_INVALID);

  /* A CAS delayed from E must lose after the word advances to E+1. */
  assert(lj_markword_init_unpublished(&word, 20, UINT64_C(1)));
  snap = lj_markword_snapshot(&word);
  assert(lj_markword_set(&word, 21, 3) == LJ_ARENA_MARK_CHANGED);
  {
    la_u128 expected, desired;
    expected.lo = snap.bits;
    expected.hi = snap.epoch;
    desired.lo = snap.bits | (UINT64_C(1) << 7);
    desired.hi = snap.epoch;
    assert(!la_cas128(&word.value, &expected, desired));
  }
  snap = lj_markword_snapshot(&word);
  assert(snap.epoch == 21);
  assert(snap.bits == (UINT64_C(1) << 3));
}

typedef struct MarkSetArg {
  LJArenaMarkWord *word;
  uint32_t *ready;
  uint32_t *go;
  unsigned bit;
  unsigned changed;
} MarkSetArg;

static void *mark_set_thread(void *ud)
{
  MarkSetArg *arg = (MarkSetArg *)ud;
  unsigned i;
  (void)la_add32_acqrel(arg->ready, 1);
  while (!la_load32_acq(arg->go))
    la_cpu_pause();
  for (i = 0; i < 1000; i++)
    arg->changed += lj_markword_set(arg->word, 41, arg->bit) ==
                    LJ_ARENA_MARK_CHANGED;
  return NULL;
}

static void test_concurrent_same_word_sets(void)
{
  enum { NTHREAD = 8 };
  LJArenaMarkWord word;
  MarkSetArg args[NTHREAD];
  pthread_t threads[NTHREAD];
  LJArenaMarkSnap snap;
  uint32_t ready = 0, go = 0;
  unsigned i;

  assert(lj_markword_init_unpublished(&word, 41, 0));
  memset(args, 0, sizeof(args));
  for (i = 0; i < NTHREAD; i++) {
    args[i].word = &word;
    args[i].ready = &ready;
    args[i].go = &go;
    args[i].bit = i;
    assert(pthread_create(&threads[i], NULL, mark_set_thread, &args[i]) == 0);
  }
  while (la_load32_acq(&ready) != NTHREAD)
    la_cpu_pause();
  la_store32_rel(&go, 1);
  for (i = 0; i < NTHREAD; i++) {
    assert(pthread_join(threads[i], NULL) == 0);
    assert(args[i].changed == 1);
  }
  snap = lj_markword_snapshot(&word);
  assert(snap.epoch == 41);
  assert(snap.bits == UINT64_C(0xff));
}

typedef struct MarkRaceArg {
  LJArenaMarkWord *word;
  uint32_t *ready;
  uint32_t *go;
  unsigned bit;
  int clear;
} MarkRaceArg;

static void *mark_race_thread(void *ud)
{
  MarkRaceArg *arg = (MarkRaceArg *)ud;
  (void)la_add32_acqrel(arg->ready, 1);
  while (!la_load32_acq(arg->go))
    la_cpu_pause();
  if (arg->clear)
    assert(lj_markword_clear(arg->word, 52, arg->bit) ==
           LJ_ARENA_MARK_CHANGED);
  else
    assert(lj_markword_set(arg->word, 52, arg->bit) ==
           LJ_ARENA_MARK_CHANGED);
  return NULL;
}

static void test_concurrent_set_clear(void)
{
  LJArenaMarkWord word;
  MarkRaceArg args[2];
  pthread_t threads[2];
  LJArenaMarkSnap snap;
  uint32_t ready = 0, go = 0;
  unsigned i;

  assert(lj_markword_init_unpublished(&word, 52, UINT64_C(1) << 1));
  memset(args, 0, sizeof(args));
  for (i = 0; i < 2; i++) {
    args[i].word = &word;
    args[i].ready = &ready;
    args[i].go = &go;
  }
  args[0].bit = 1;
  args[0].clear = 1;
  args[1].bit = 6;
  assert(pthread_create(&threads[0], NULL, mark_race_thread, &args[0]) == 0);
  assert(pthread_create(&threads[1], NULL, mark_race_thread, &args[1]) == 0);
  while (la_load32_acq(&ready) != 2)
    la_cpu_pause();
  la_store32_rel(&go, 1);
  for (i = 0; i < 2; i++)
    assert(pthread_join(threads[i], NULL) == 0);
  snap = lj_markword_snapshot(&word);
  assert(snap.epoch == 52);
  assert(snap.bits == (UINT64_C(1) << 6));
}

typedef struct PublishModel {
  unsigned alloc_pc;
  unsigned scan_pc;
  unsigned mark;
  unsigned block;
  unsigned saw_block;
  unsigned saw_mark;
} PublishModel;

static unsigned publish_schedules;
static unsigned publish_visible_schedules;

/* Enumerate every interleaving of mark->block and block-read->mark-read. */
static void explore_mark_before_block(PublishModel model)
{
  if (model.alloc_pc == 2 && model.scan_pc == 2) {
    publish_schedules++;
    if (model.saw_block) {
      publish_visible_schedules++;
      assert(model.saw_mark);
    }
    return;
  }
  if (model.alloc_pc < 2) {
    PublishModel next = model;
    if (next.alloc_pc++ == 0)
      next.mark = 1;
    else
      next.block = 1;
    explore_mark_before_block(next);
  }
  if (model.scan_pc < 2) {
    PublishModel next = model;
    if (next.scan_pc++ == 0)
      next.saw_block = next.block;
    else
      next.saw_mark = next.mark;
    explore_mark_before_block(next);
  }
}

typedef struct ActivationModel {
  unsigned barrier_pc;
  unsigned collector_pc;
  uint64_t generation;
  uint64_t sampled_generation;
  uint64_t checked_generation;
  uint8_t state;
  uint8_t sampled_state;
  uint8_t checked_state;
  unsigned between;
} ActivationModel;

static unsigned activation_schedules;
static unsigned activation_aba_schedules;

/*
** Enumerate every interleaving of sample->store->recheck with
** IDLE->MARK->IDLE.  Even when state and mark epoch return to their original
** values, the complete activation token must not ABA.
*/
static void explore_activation_recheck(ActivationModel model)
{
  if (model.barrier_pc == 3 && model.collector_pc == 2) {
    activation_schedules++;
    if (model.between)
      assert(model.sampled_generation != model.checked_generation);
    if (model.between == 2 && model.sampled_state == model.checked_state) {
      activation_aba_schedules++;
      assert(model.sampled_generation != model.checked_generation);
    }
    return;
  }
  if (model.barrier_pc < 3) {
    ActivationModel next = model;
    if (next.barrier_pc == 0) {
      next.sampled_generation = next.generation;
      next.sampled_state = next.state;
    } else if (next.barrier_pc == 2) {
      next.checked_generation = next.generation;
      next.checked_state = next.state;
    }
    next.barrier_pc++;
    explore_activation_recheck(next);
  }
  if (model.collector_pc < 2) {
    ActivationModel next = model;
    next.generation++;
    next.state = next.collector_pc == 0 ? LJ_GC2_ACT_MARK : LJ_GC2_ACT_IDLE;
    if (next.barrier_pc > 0 && next.barrier_pc < 3)
      next.between++;
    next.collector_pc++;
    explore_activation_recheck(next);
  }
}

static void test_exhaustive_publication_models(void)
{
  PublishModel publication;
  ActivationModel activation;
  memset(&publication, 0, sizeof(publication));
  explore_mark_before_block(publication);
  assert(publish_schedules == 6);
  assert(publish_visible_schedules != 0);

  memset(&activation, 0, sizeof(activation));
  activation.generation = 100;
  activation.state = LJ_GC2_ACT_IDLE;
  explore_activation_recheck(activation);
  assert(activation_schedules == 10);
  assert(activation_aba_schedules != 0);
}

static void test_activation_transitions_and_saturation(void)
{
  LJGC2Activation token;
  LJGC2ActivationSnap before, mark, idle, observed, snap;

  assert(lj_gc2_activation_init_unpublished(&token, 17, 100,
                                            LJ_GC2_ACT_IDLE));
  before = lj_gc2_activation_snapshot(&token);
  assert(lj_gc2_activation_try_transition(&token, &before, 17,
           LJ_GC2_ACT_MARK, &mark) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&token, &mark, 17,
           LJ_GC2_ACT_IDLE, &idle) == LJ_GC2_TRANSITION_OK);
  assert(before.mark_epoch == idle.mark_epoch);
  assert(before.state == idle.state);
  assert(!lj_gc2_activation_equal(&before, &idle));

  assert(lj_gc2_activation_try_transition(&token, &before, 17,
           LJ_GC2_ACT_MARK, &observed) == LJ_GC2_TRANSITION_LOST);
  assert(lj_gc2_activation_equal(&observed, &idle));
  assert(lj_gc2_activation_try_transition(&token, &idle, 16,
           LJ_GC2_ACT_MARK, NULL) == LJ_GC2_TRANSITION_INVALID);

  assert(!lj_gc2_activation_init_unpublished(&token, 1,
      LJ_GC2_ACT_MAX_GENERATION + 1, LJ_GC2_ACT_IDLE));
  assert(!lj_gc2_activation_init_unpublished(&token, 1, 1, 8));
  assert(lj_gc2_activation_init_unpublished(&token, UINT64_MAX,
      LJ_GC2_ACT_MAX_GENERATION, LJ_GC2_ACT_MARK));
  snap = lj_gc2_activation_snapshot(&token);
  assert(lj_gc2_activation_try_transition(&token, &snap, UINT64_MAX,
           LJ_GC2_ACT_WEAK, &observed) == LJ_GC2_TRANSITION_PINNED);
  assert(observed.mark_epoch == UINT64_MAX);
  assert(observed.generation == LJ_GC2_ACT_MAX_GENERATION);
  assert(observed.state == LJ_GC2_ACT_NO_RECLAIM);
  assert(lj_gc2_activation_try_transition(&token, &observed, UINT64_MAX,
           LJ_GC2_ACT_IDLE, NULL) == LJ_GC2_TRANSITION_INVALID);
  assert(lj_gc2_activation_try_transition(&token, &observed, UINT64_MAX,
           LJ_GC2_ACT_NO_RECLAIM, NULL) == LJ_GC2_TRANSITION_PINNED);
  observed = lj_gc2_activation_snapshot(&token);
  assert(observed.state == LJ_GC2_ACT_NO_RECLAIM);
}

static void test_activation_edge_and_epoch_policy(void)
{
  static const uint8_t allowed[8] = {
    0x86, 0x85, 0x89, 0x91, 0xa0, 0xd0, 0x91, 0x80
  };
  LJGC2Activation token;
  LJGC2ActivationSnap idle, mark, weak, open, observed;
  unsigned from, to;

  for (from = 0; from < 8; from++)
    for (to = 0; to < 8; to++)
      assert(lj_gc2_activation_edge_valid((uint8_t)from, (uint8_t)to) ==
             ((allowed[from] >> to) & 1));

  assert(!lj_gc2_activation_init_unpublished(&token, 0, 1,
                                              LJ_GC2_ACT_MARK));
  assert(lj_gc2_activation_init_unpublished(&token, 0, 1,
                                             LJ_GC2_ACT_IDLE));
  idle = lj_gc2_activation_snapshot(&token);
  assert(lj_gc2_activation_try_transition(&token, &idle, 0,
           LJ_GC2_ACT_MARK, NULL) == LJ_GC2_TRANSITION_INVALID);
  assert(lj_gc2_activation_try_transition(&token, &idle, 1,
           LJ_GC2_ACT_MARK, &mark) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&token, &mark, 2,
           LJ_GC2_ACT_WEAK, NULL) == LJ_GC2_TRANSITION_INVALID);
  assert(lj_gc2_activation_try_transition(&token, &mark, 1,
           LJ_GC2_ACT_SWEEP_OPEN, NULL) == LJ_GC2_TRANSITION_INVALID);
  assert(lj_gc2_activation_try_transition(&token, &mark, 1,
           LJ_GC2_ACT_WEAK, &weak) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&token, &weak, 1,
           LJ_GC2_ACT_SWEEP_OPEN, &open) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&token, &open, 2,
           LJ_GC2_ACT_SWEEP_CLOSING, NULL) == LJ_GC2_TRANSITION_INVALID);
  observed = lj_gc2_activation_snapshot(&token);
  assert(lj_gc2_activation_equal(&open, &observed));
}

enum { TOKEN_FIRST_GENERATION = 7, TOKEN_FINAL_GENERATION = 20000 };

typedef struct TokenStress {
  LJGC2Activation token;
  uint32_t ready;
  uint32_t go;
  uint32_t done;
} TokenStress;

static void *token_writer(void *ud)
{
  TokenStress *stress = (TokenStress *)ud;
  LJGC2ActivationSnap expected = lj_gc2_activation_snapshot(&stress->token);
  (void)la_add32_acqrel(&stress->ready, 1);
  while (!la_load32_acq(&stress->go))
    la_cpu_pause();
  while (expected.generation < TOKEN_FINAL_GENERATION) {
    LJGC2ActivationSnap next;
    uint64_t generation = expected.generation + 1;
    assert(lj_gc2_activation_try_transition(&stress->token, &expected,
             (generation + 6) / 7, (uint8_t)(generation % 7), &next) ==
           LJ_GC2_TRANSITION_OK);
    expected = next;
  }
  la_store32_rel(&stress->done, 1);
  return NULL;
}

static void *token_reader(void *ud)
{
  TokenStress *stress = (TokenStress *)ud;
  uint64_t previous = TOKEN_FIRST_GENERATION;
  (void)la_add32_acqrel(&stress->ready, 1);
  while (!la_load32_acq(&stress->go))
    la_cpu_pause();
  do {
    LJGC2ActivationSnap snap = lj_gc2_activation_snapshot(&stress->token);
    assert(snap.generation >= previous);
    assert(snap.mark_epoch == (snap.generation + 6) / 7);
    assert(snap.state == snap.generation % 7);
    previous = snap.generation;
  } while (!la_load32_acq(&stress->done));
  return NULL;
}

static void test_concurrent_activation_snapshots(void)
{
  enum { NREADER = 4 };
  TokenStress stress;
  pthread_t writer, readers[NREADER];
  LJGC2ActivationSnap final;
  unsigned i;

  memset(&stress, 0, sizeof(stress));
  assert(lj_gc2_activation_init_unpublished(&stress.token,
      (TOKEN_FIRST_GENERATION + 6) / 7, TOKEN_FIRST_GENERATION,
      TOKEN_FIRST_GENERATION % 7));
  assert(pthread_create(&writer, NULL, token_writer, &stress) == 0);
  for (i = 0; i < NREADER; i++)
    assert(pthread_create(&readers[i], NULL, token_reader, &stress) == 0);
  while (la_load32_acq(&stress.ready) != NREADER + 1)
    la_cpu_pause();
  la_store32_rel(&stress.go, 1);
  assert(pthread_join(writer, NULL) == 0);
  for (i = 0; i < NREADER; i++)
    assert(pthread_join(readers[i], NULL) == 0);
  final = lj_gc2_activation_snapshot(&stress.token);
  assert(final.generation == TOKEN_FINAL_GENERATION);
  assert(final.mark_epoch == (TOKEN_FINAL_GENERATION + 6) / 7);
  assert(final.state == TOKEN_FINAL_GENERATION % 7);
}

int main(void)
{
  test_markword_epochs_and_clear();
  test_concurrent_same_word_sets();
  test_concurrent_set_clear();
  test_exhaustive_publication_models();
  test_activation_transitions_and_saturation();
  test_activation_edge_and_epoch_policy();
  test_concurrent_activation_snapshots();
  printf("t-gc2-markword-token OK: %u publication and %u activation schedules\n",
         publish_schedules, activation_schedules);
  return 0;
}
