/*
** t-gc2-markword-token.c - standalone GC2 markword/activation model.
**
** Build & run:
**   LJ_CAS128_CFLAGS=-mcx16  # Use an empty value for an arm64 target.
**   cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread \
**      $LJ_CAS128_CFLAGS -Isrc \
**      tests/t-gc2-markword-token.c -o /tmp/t-gc2-markword-token && \
**      /tmp/t-gc2-markword-token
*/

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
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
  assert(before.gate == LJ_GC2_ROOT_GATE_OPEN);
  assert(idle.gate == LJ_GC2_ROOT_GATE_OPEN);
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
  assert(observed.gate == LJ_GC2_ROOT_GATE_OPEN);
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
  LJGC2ActivationSnap idle, mark, weak, open, abandoned, observed;
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
  assert(lj_gc2_activation_try_transition(&token, &open, 1,
           LJ_GC2_ACT_IDLE, NULL) == LJ_GC2_TRANSITION_INVALID);
  assert(lj_gc2_activation_try_abandon_sweep_open(&token, &weak, NULL) ==
         LJ_GC2_TRANSITION_INVALID);
  assert(lj_gc2_activation_try_abandon_sweep_open(&token, &open,
           &abandoned) == LJ_GC2_TRANSITION_OK);
  assert(abandoned.mark_epoch == open.mark_epoch);
  assert(abandoned.generation == open.generation + 1u);
  assert(abandoned.state == LJ_GC2_ACT_IDLE);
  assert(abandoned.gate == LJ_GC2_ROOT_GATE_OPEN);
  assert(lj_gc2_activation_try_abandon_sweep_open(&token, &open,
           &observed) == LJ_GC2_TRANSITION_LOST);
  assert(lj_gc2_activation_equal(&observed, &abandoned));

  assert(lj_gc2_activation_init_unpublished(&token, 9, 40,
                                             LJ_GC2_ACT_SWEEP_OPEN));
  open = lj_gc2_activation_snapshot(&token);
  assert(lj_gc2_activation_try_gate(&token, &open,
           LJ_GC2_ROOT_GATE_CLOSING, &observed) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_abandon_sweep_open(&token, &observed,
           NULL) == LJ_GC2_TRANSITION_INVALID);

  assert(lj_gc2_activation_init_unpublished(&token, 9,
      LJ_GC2_ACT_MAX_GENERATION, LJ_GC2_ACT_SWEEP_OPEN));
  open = lj_gc2_activation_snapshot(&token);
  assert(lj_gc2_activation_try_abandon_sweep_open(&token, &open,
           &observed) == LJ_GC2_TRANSITION_PINNED);
  assert(observed.mark_epoch == 9);
  assert(observed.generation == LJ_GC2_ACT_MAX_GENERATION);
  assert(observed.state == LJ_GC2_ACT_NO_RECLAIM);
  assert(observed.gate == LJ_GC2_ROOT_GATE_OPEN);
}

static void test_activation_root_gate(void)
{
  LJGC2Activation token;
  LJGC2ActivationSnap open, closing, pending, observed, commit, reopened;

  assert(lj_gc2_activation_init_unpublished(&token, 31, 20,
                                             LJ_GC2_ACT_WEAK));
  open = lj_gc2_activation_snapshot(&token);
  assert(open.gate == LJ_GC2_ROOT_GATE_OPEN);
  assert(lj_gc2_activation_try_gate(&token, &open,
           LJ_GC2_ROOT_GATE_CLOSING, &closing) == LJ_GC2_TRANSITION_OK);

  /* A late publisher changes the same 128-bit CAS authority as close/commit. */
  assert(lj_gc2_activation_try_gate(&token, &closing,
           LJ_GC2_ROOT_GATE_PENDING, &pending) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_gate(&token, &closing,
           LJ_GC2_ROOT_GATE_COMMIT, &observed) == LJ_GC2_TRANSITION_LOST);
  assert(lj_gc2_activation_equal(&observed, &pending));
  assert(lj_gc2_activation_try_gate(&token, &pending,
           LJ_GC2_ROOT_GATE_CLOSING, &closing) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_gate(&token, &closing,
           LJ_GC2_ROOT_GATE_COMMIT, &commit) == LJ_GC2_TRANSITION_OK);

  /* COMMIT is not an ABA-safe terminal observation for a new publisher. */
  assert(lj_gc2_activation_try_gate(&token, &commit,
           LJ_GC2_ROOT_GATE_PENDING, &pending) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_gate(&token, &pending,
           LJ_GC2_ROOT_GATE_OPEN, &reopened) == LJ_GC2_TRANSITION_OK);
  assert(reopened.state == open.state);
  assert(reopened.gate == open.gate);
  assert(reopened.mark_epoch == open.mark_epoch);
  assert(reopened.generation != open.generation);
  assert(lj_gc2_activation_try_gate(&token, &open,
           LJ_GC2_ROOT_GATE_CLOSING, &observed) == LJ_GC2_TRANSITION_LOST);
  assert(lj_gc2_activation_equal(&observed, &reopened));

  assert(lj_gc2_activation_try_gate(&token, &reopened,
           LJ_GC2_ROOT_GATE_COMMIT, NULL) == LJ_GC2_TRANSITION_INVALID);
  assert(lj_gc2_activation_try_update(&token, &reopened, 31,
           LJ_GC2_ACT_WEAK, 4, NULL) == LJ_GC2_TRANSITION_INVALID);

  assert(lj_gc2_activation_try_gate(&token, &reopened,
           LJ_GC2_ROOT_GATE_CLOSING, &closing) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&token, &closing, 31,
           LJ_GC2_ACT_NO_RECLAIM, &observed) == LJ_GC2_TRANSITION_OK);
  assert(observed.state == LJ_GC2_ACT_NO_RECLAIM);
  assert(observed.gate == LJ_GC2_ROOT_GATE_OPEN);
}

static void test_root_gate_edge_policy(void)
{
  static const uint8_t allowed[4] = { 0x03, 0x0f, 0x07, 0x0d };
  unsigned from, to;
  for (from = 0; from < 4; from++)
    for (to = 0; to < 4; to++)
      assert(lj_gc2_root_gate_edge_valid((uint8_t)from, (uint8_t)to) ==
             ((allowed[from] >> to) & 1));
  assert(!lj_gc2_root_gate_edge_valid(4, LJ_GC2_ROOT_GATE_OPEN));
}

typedef struct GateRaceArg {
  LJGC2Activation *token;
  LJGC2ActivationSnap expected;
  LJGC2ActivationSnap observed;
  uint32_t *ready;
  uint32_t *go;
  uint8_t next_gate;
  LJGC2TransitionResult result;
} GateRaceArg;

static void *gate_race_thread(void *ud)
{
  GateRaceArg *arg = (GateRaceArg *)ud;
  (void)la_add32_acqrel(arg->ready, 1);
  while (!la_load32_acq(arg->go))
    la_cpu_pause();
  arg->result = lj_gc2_activation_try_gate(arg->token, &arg->expected,
                                            arg->next_gate, &arg->observed);
  return NULL;
}

static void test_concurrent_close_pending_commit(void)
{
  LJGC2Activation token;
  LJGC2ActivationSnap open, closing, final, pending;
  GateRaceArg arg[2];
  pthread_t thread[2];
  uint32_t ready = 0, go = 0;
  unsigned winner;

  assert(lj_gc2_activation_init_unpublished(&token, 73, 11,
                                             LJ_GC2_ACT_WEAK));
  open = lj_gc2_activation_snapshot(&token);
  assert(lj_gc2_activation_try_gate(&token, &open,
           LJ_GC2_ROOT_GATE_CLOSING, &closing) == LJ_GC2_TRANSITION_OK);
  memset(arg, 0, sizeof(arg));
  arg[0].token = arg[1].token = &token;
  arg[0].expected = arg[1].expected = closing;
  arg[0].ready = arg[1].ready = &ready;
  arg[0].go = arg[1].go = &go;
  arg[0].next_gate = LJ_GC2_ROOT_GATE_PENDING;
  arg[1].next_gate = LJ_GC2_ROOT_GATE_COMMIT;
  assert(pthread_create(&thread[0], NULL, gate_race_thread, &arg[0]) == 0);
  assert(pthread_create(&thread[1], NULL, gate_race_thread, &arg[1]) == 0);
  while (la_load32_acq(&ready) != 2)
    la_cpu_pause();
  la_store32_rel(&go, 1);
  assert(pthread_join(thread[0], NULL) == 0);
  assert(pthread_join(thread[1], NULL) == 0);
  assert((arg[0].result == LJ_GC2_TRANSITION_OK) +
         (arg[1].result == LJ_GC2_TRANSITION_OK) == 1);
  assert((arg[0].result == LJ_GC2_TRANSITION_LOST) +
         (arg[1].result == LJ_GC2_TRANSITION_LOST) == 1);
  winner = arg[0].result == LJ_GC2_TRANSITION_OK ? 0u : 1u;
  final = lj_gc2_activation_snapshot(&token);
  assert(lj_gc2_activation_equal(&final, &arg[winner].observed));
  assert(lj_gc2_activation_equal(&final, &arg[winner ^ 1u].observed));
  if (final.gate == LJ_GC2_ROOT_GATE_COMMIT) {
    assert(lj_gc2_activation_try_gate(&token, &final,
             LJ_GC2_ROOT_GATE_PENDING, &pending) == LJ_GC2_TRANSITION_OK);
    final = pending;
  }
  assert(final.gate == LJ_GC2_ROOT_GATE_PENDING);
}

typedef struct TableTokenCompleteArg {
  LJGC2TableToken *token;
  LJGC2TableTokenTicket ticket;
  uint32_t *ready;
  uint32_t *go;
  int exact;
  LJGC2TableTokenResult result;
} TableTokenCompleteArg;

static void *table_token_complete_thread(void *ud)
{
  TableTokenCompleteArg *arg = (TableTokenCompleteArg *)ud;
  (void)la_add32_acqrel(arg->ready, 1);
  while (!la_load32_acq(arg->go))
    la_cpu_pause();
  arg->result = arg->exact ?
    lj_gc2_table_token_complete_exact(arg->token, &arg->ticket) :
    lj_gc2_table_token_complete(arg->token, &arg->ticket);
  return NULL;
}

typedef struct TableTokenTransferArg {
  LJGC2TableToken *token;
  uint64_t generation;
  uint32_t *ready;
  uint32_t *go;
  LJGC2TableTokenResult result;
} TableTokenTransferArg;

static void *table_token_transfer_thread(void *ud)
{
  TableTokenTransferArg *arg = (TableTokenTransferArg *)ud;
  (void)la_add32_acqrel(arg->ready, 1);
  while (!la_load32_acq(arg->go))
    la_cpu_pause();
  arg->result = lj_gc2_table_token_transfer_exact(arg->token,
                                                   arg->generation);
  return NULL;
}

static void test_table_exact_target_token_primitives(void)
{
  LJGC2TableDesc desc, fake_table;
  LJGC2TableDescTicket desc_ticket, invalid_desc_ticket;
  LJGC2TableDescSnap desc_observed;
  LJGC2TableToken token;
  LJGC2TableTokenTicket scan_first, scan_second;
  TableTokenTransferArg delayed, transfer[2];
  TableTokenCompleteArg scanner[2];
  pthread_t thread[2];
  uint32_t ready = 0, go = 0;
  uint64_t control;
  unsigned i;

  memset(&fake_table, 0, sizeof(fake_table));

  /* One helper may be delayed until another has transferred and completed D.
  ** Its idempotent transfer must observe NONE(D), not recreate PENDING(D). */
  assert(lj_gc2_table_token_init_unpublished(&token, 0));
  memset(&delayed, 0, sizeof(delayed));
  delayed.token = &token;
  delayed.generation = 11;
  delayed.ready = &ready;
  delayed.go = &go;
  assert(pthread_create(&thread[0], NULL, table_token_transfer_thread,
                        &delayed) == 0);
  while (la_load32_acq(&ready) != 1)
    la_cpu_pause();
  assert(lj_gc2_table_token_transfer_exact(&token, 11) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_capture_pending(&token, &scan_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_complete_exact(&token, &scan_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  la_store32_rel(&go, 1);
  assert(pthread_join(thread[0], NULL) == 0);
  assert(delayed.result == LJ_GC2_TABLE_TOKEN_RESULT_OK);
  control = la_load64_acq(&token.control);
  assert(control == lj_gc2_table_token_pack(11,
                                            LJ_GC2_TABLE_TOKEN_NONE));

  /* Same-generation transfer is idempotent both before and after completion. */
  assert(lj_gc2_table_token_transfer_exact(&token, 12) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  control = la_load64_acq(&token.control);
  assert(control == lj_gc2_table_token_pack(12,
                                            LJ_GC2_TABLE_TOKEN_PENDING));
  assert(lj_gc2_table_token_transfer_exact(&token, 12) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(la_load64_acq(&token.control) == control);
  assert(lj_gc2_table_token_capture_pending(&token, &scan_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_complete_exact(&token, &scan_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  control = la_load64_acq(&token.control);
  assert(control == lj_gc2_table_token_pack(12,
                                            LJ_GC2_TABLE_TOKEN_NONE));
  assert(lj_gc2_table_token_transfer_exact(&token, 12) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(la_load64_acq(&token.control) == control);

  /* Two helpers racing one target are both successful and idempotent. Two
  ** scanners sharing the resulting exact ticket have exactly one clear
  ** winner, and completion does not advance the generation. */
  assert(lj_gc2_table_token_init_unpublished(&token, 12));
  ready = go = 0;
  memset(transfer, 0, sizeof(transfer));
  for (i = 0; i < 2; i++) {
    transfer[i].token = &token;
    transfer[i].generation = 13;
    transfer[i].ready = &ready;
    transfer[i].go = &go;
    assert(pthread_create(&thread[i], NULL, table_token_transfer_thread,
                          &transfer[i]) == 0);
  }
  while (la_load32_acq(&ready) != 2)
    la_cpu_pause();
  la_store32_rel(&go, 1);
  for (i = 0; i < 2; i++) {
    assert(pthread_join(thread[i], NULL) == 0);
    assert(transfer[i].result == LJ_GC2_TABLE_TOKEN_RESULT_OK);
  }
  assert(la_load64_acq(&token.control) ==
         lj_gc2_table_token_pack(13, LJ_GC2_TABLE_TOKEN_PENDING));
  assert(lj_gc2_table_token_capture_pending(&token, &scan_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);

  ready = go = 0;
  memset(scanner, 0, sizeof(scanner));
  for (i = 0; i < 2; i++) {
    scanner[i].token = &token;
    scanner[i].ticket = scan_first;
    scanner[i].ready = &ready;
    scanner[i].go = &go;
    scanner[i].exact = 1;
    assert(pthread_create(&thread[i], NULL, table_token_complete_thread,
                          &scanner[i]) == 0);
  }
  while (la_load32_acq(&ready) != 2)
    la_cpu_pause();
  la_store32_rel(&go, 1);
  for (i = 0; i < 2; i++)
    assert(pthread_join(thread[i], NULL) == 0);
  assert((scanner[0].result == LJ_GC2_TABLE_TOKEN_RESULT_OK) +
         (scanner[1].result == LJ_GC2_TABLE_TOKEN_RESULT_OK) == 1);
  assert((scanner[0].result == LJ_GC2_TABLE_TOKEN_RESULT_BUSY) +
         (scanner[1].result == LJ_GC2_TABLE_TOKEN_RESULT_BUSY) == 1);
  assert(la_load64_acq(&token.control) ==
         lj_gc2_table_token_pack(13, LJ_GC2_TABLE_TOKEN_NONE));

  /* A descriptor refresh invalidates the prior scanner without incrementing
  ** on completion. The stale scanner cannot clear the newer PENDING token. */
  assert(lj_gc2_table_token_transfer_exact(&token, 20) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_capture_pending(&token, &scan_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_transfer_exact(&token, 21) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_capture_pending(&token, &scan_second) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_complete_exact(&token, &scan_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_BUSY);
  assert(la_load64_acq(&token.control) == scan_second.control);
  assert(lj_gc2_table_token_complete_exact(&token, &scan_second) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  control = la_load64_acq(&token.control);
  assert(control == lj_gc2_table_token_pack(21,
                                            LJ_GC2_TABLE_TOKEN_NONE));
  assert(lj_gc2_table_token_transfer_exact(&token, 20) ==
         LJ_GC2_TABLE_TOKEN_RESULT_BUSY);
  assert(la_load64_acq(&token.control) == control);
  assert(lj_gc2_table_token_transfer_exact(&token, 21) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(la_load64_acq(&token.control) == control);

  /* A helper for an older descriptor is ordinary stale work, regardless of
  ** whether the newer exact generation is still pending or is complete. */
  assert(lj_gc2_table_token_transfer_exact(&token, 30) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  control = la_load64_acq(&token.control);
  assert(lj_gc2_table_token_transfer_exact(&token, 29) ==
         LJ_GC2_TABLE_TOKEN_RESULT_BUSY);
  assert(la_load64_acq(&token.control) == control);
  assert(lj_gc2_table_token_capture_pending(&token, &scan_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_complete_exact(&token, &scan_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  control = la_load64_acq(&token.control);
  assert(lj_gc2_table_token_transfer_exact(&token, 29) ==
         LJ_GC2_TABLE_TOKEN_RESULT_BUSY);
  assert(la_load64_acq(&token.control) == control);

  /* D == the shared 62-bit maximum is transferable and completable. Only the
  ** next descriptor publication saturates into sticky PINNED authority. */
  lj_gc2_tabledesc_init_unpublished(
    &desc, LJ_GC2_TABLE_TOKEN_MAX_GENERATION - 1u);
  assert(lj_gc2_tabledesc_try_publish(&desc, &fake_table, &desc_ticket,
           &desc_observed) == LJ_GC2_TABLEDESC_RESULT_OK);
  assert(desc_ticket.generation == LJ_GC2_TABLE_TOKEN_MAX_GENERATION);
  assert(lj_gc2_table_token_init_unpublished(
           &token, LJ_GC2_TABLE_TOKEN_MAX_GENERATION - 1u));
  assert(lj_gc2_table_token_transfer_exact(&token,
           desc_ticket.generation) == LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_capture_pending(&token, &scan_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_complete_exact(&token, &scan_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(la_load64_acq(&token.control) == lj_gc2_table_token_pack(
           LJ_GC2_TABLE_TOKEN_MAX_GENERATION, LJ_GC2_TABLE_TOKEN_NONE));
  assert(lj_gc2_tabledesc_finish_help(&desc, &desc_ticket,
           &desc_observed) == LJ_GC2_TABLEDESC_RESULT_OK);
  assert(desc_observed.state == LJ_GC2_TABLEDESC_IDLE &&
         desc_observed.generation == LJ_GC2_TABLE_TOKEN_MAX_GENERATION);
  assert(lj_gc2_tabledesc_try_publish(&desc, &fake_table, &desc_ticket,
           &desc_observed) == LJ_GC2_TABLEDESC_RESULT_PINNED);
  assert(desc_observed.state == LJ_GC2_TABLEDESC_PINNED &&
         desc_observed.generation == LJ_GC2_TABLE_TOKEN_MAX_GENERATION);

  /* ACTIVE/IDLE descriptor generations outside the shared namespace are
  ** malformed and fail closed instead of ever wrapping through 64 bits. */
  lj_gc2_tabledesc_init_unpublished(
    &desc, LJ_GC2_TABLE_TOKEN_MAX_GENERATION + 1u);
  assert(lj_gc2_tabledesc_snapshot(&desc).state ==
         LJ_GC2_TABLEDESC_INVALID);
  assert(lj_gc2_tabledesc_try_publish(&desc, &fake_table, &desc_ticket,
           &desc_observed) == LJ_GC2_TABLEDESC_RESULT_PINNED);
  assert(desc_observed.state == LJ_GC2_TABLEDESC_PINNED &&
         desc_observed.generation ==
           LJ_GC2_TABLE_TOKEN_MAX_GENERATION + 1u);

  /* ACTIVE(0) is impossible even though IDLE(0) is the valid initial state.
  ** finish_help contains the malformed stored authority rather than clearing
  ** it, while an invalid ticket cannot poison a different valid descriptor. */
  desc.value.lo = (uint64_t)(uintptr_t)&fake_table;
  desc.value.hi = 0;
  invalid_desc_ticket.table = (uint64_t)(uintptr_t)&fake_table;
  invalid_desc_ticket.generation = 0;
  assert(lj_gc2_tabledesc_snapshot(&desc).state ==
         LJ_GC2_TABLEDESC_INVALID);
  assert(lj_gc2_tabledesc_finish_help(&desc, &invalid_desc_ticket,
           &desc_observed) == LJ_GC2_TABLEDESC_RESULT_PINNED);
  assert(desc_observed.state == LJ_GC2_TABLEDESC_PINNED &&
         desc_observed.generation == 0);

  lj_gc2_tabledesc_init_unpublished(&desc, 7);
  assert(lj_gc2_tabledesc_try_publish(&desc, &fake_table, &desc_ticket,
           NULL) == LJ_GC2_TABLEDESC_RESULT_OK);
  assert(lj_gc2_tabledesc_finish_help(&desc, &invalid_desc_ticket,
           &desc_observed) == LJ_GC2_TABLEDESC_RESULT_INVALID);
  assert(desc_observed.state == LJ_GC2_TABLEDESC_ACTIVE &&
         desc_observed.generation == desc_ticket.generation);
  invalid_desc_ticket.generation =
    LJ_GC2_TABLE_TOKEN_MAX_GENERATION + 1u;
  assert(lj_gc2_tabledesc_finish_help(&desc, &invalid_desc_ticket,
           &desc_observed) == LJ_GC2_TABLEDESC_RESULT_INVALID);
  assert(desc_observed.state == LJ_GC2_TABLEDESC_ACTIVE &&
         desc_observed.generation == desc_ticket.generation);
  assert(lj_gc2_tabledesc_finish_help(&desc, &desc_ticket, NULL) ==
         LJ_GC2_TABLEDESC_RESULT_OK);

  desc.value.lo = (uint64_t)(uintptr_t)&fake_table;
  desc.value.hi = LJ_GC2_TABLE_TOKEN_MAX_GENERATION + 1u;
  desc_ticket.table = (uint64_t)(uintptr_t)&fake_table;
  desc_ticket.generation = LJ_GC2_TABLE_TOKEN_MAX_GENERATION + 1u;
  assert(lj_gc2_tabledesc_finish_help(&desc, &desc_ticket,
           &desc_observed) == LJ_GC2_TABLEDESC_RESULT_PINNED);
  assert(desc_observed.state == LJ_GC2_TABLEDESC_PINNED &&
         desc_observed.generation ==
           LJ_GC2_TABLE_TOKEN_MAX_GENERATION + 1u);
  assert(lj_gc2_table_token_transfer_exact(
           &token, LJ_GC2_TABLE_TOKEN_MAX_GENERATION + 1u) ==
         LJ_GC2_TABLE_TOKEN_RESULT_INVALID);

  /* Malformed token state is sticky containment at the target generation. A
  ** stale helper observing a newer malformed generation must not pin it. */
  token.control = lj_gc2_table_token_pack(40,
                                           LJ_GC2_TABLE_TOKEN_INVALID);
  assert(lj_gc2_table_token_transfer_exact(&token, 40) ==
         LJ_GC2_TABLE_TOKEN_RESULT_PINNED);
  assert(token.control == lj_gc2_table_token_pack(40,
                                                   LJ_GC2_TABLE_TOKEN_PINNED));
  token.control = lj_gc2_table_token_pack(41,
                                           LJ_GC2_TABLE_TOKEN_INVALID);
  assert(lj_gc2_table_token_transfer_exact(&token, 40) ==
         LJ_GC2_TABLE_TOKEN_RESULT_BUSY);
  assert(token.control == lj_gc2_table_token_pack(41,
                                                   LJ_GC2_TABLE_TOKEN_INVALID));
  assert(lj_gc2_table_token_transfer_exact(&token, 41) ==
         LJ_GC2_TABLE_TOKEN_RESULT_PINNED);
  scan_first.control = lj_gc2_table_token_pack(
    42, LJ_GC2_TABLE_TOKEN_PENDING);
  token.control = lj_gc2_table_token_pack(42,
                                           LJ_GC2_TABLE_TOKEN_INVALID);
  assert(lj_gc2_table_token_complete_exact(&token, &scan_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_PINNED);
  assert(token.control == lj_gc2_table_token_pack(42,
                                                   LJ_GC2_TABLE_TOKEN_PINNED));
}

static void test_table_descriptor_and_token_primitives(void)
{
  LJGC2TableDesc desc, fake_table[2];
  LJGC2TableDescSnap snap, observed;
  LJGC2TableDescTicket first, second;
  LJGC2TableToken token;
  LJGC2TableTokenTicket token_first, token_second, token_third;
  TableTokenCompleteArg arg[2];
  pthread_t thread[2];
  uint32_t ready = 0, go = 0;
  uint64_t control;

  lj_gc2_tabledesc_init_unpublished(&desc, 7);
  snap = lj_gc2_tabledesc_snapshot(&desc);
  assert(snap.state == LJ_GC2_TABLEDESC_IDLE && snap.generation == 7);
  assert(lj_gc2_tabledesc_try_publish(&desc, &fake_table[0], &first,
           &observed) == LJ_GC2_TABLEDESC_RESULT_OK);
  assert(observed.state == LJ_GC2_TABLEDESC_ACTIVE &&
         observed.table == (uintptr_t)&fake_table[0] &&
         observed.generation == 8);
  assert(lj_gc2_tabledesc_try_publish(&desc, &fake_table[1], &second,
           &observed) == LJ_GC2_TABLEDESC_RESULT_BUSY);
  assert(observed.table == first.table &&
         observed.generation == first.generation);
  assert(lj_gc2_tabledesc_finish_help(&desc, &first, &observed) ==
         LJ_GC2_TABLEDESC_RESULT_OK);
  assert(observed.state == LJ_GC2_TABLEDESC_IDLE &&
         observed.generation == 8);

  /* Reusing the same table address changes generation. A delayed exact helper
  ** cannot clear the new descriptor with its prior ticket. */
  assert(lj_gc2_tabledesc_try_publish(&desc, &fake_table[0], &second,
           NULL) == LJ_GC2_TABLEDESC_RESULT_OK);
  assert(second.generation == first.generation + 1u);
  assert(lj_gc2_tabledesc_finish_help(&desc, &first, &observed) ==
         LJ_GC2_TABLEDESC_RESULT_BUSY);
  assert(observed.state == LJ_GC2_TABLEDESC_ACTIVE &&
         observed.generation == second.generation);
  assert(lj_gc2_tabledesc_finish_help(&desc, &second, NULL) ==
         LJ_GC2_TABLEDESC_RESULT_OK);

  /* Malformed stored authority and generation saturation fail closed. */
  desc.value.lo = 3;
  desc.value.hi = 19;
  assert(lj_gc2_tabledesc_try_publish(&desc, &fake_table[0], &second,
           &observed) == LJ_GC2_TABLEDESC_RESULT_PINNED);
  assert(observed.state == LJ_GC2_TABLEDESC_PINNED &&
         observed.generation == 19);
  lj_gc2_tabledesc_init_unpublished(&desc, UINT64_MAX);
  assert(lj_gc2_tabledesc_try_publish(&desc, &fake_table[0], &second,
           &observed) == LJ_GC2_TABLEDESC_RESULT_PINNED);
  assert(observed.state == LJ_GC2_TABLEDESC_PINNED &&
         observed.generation == UINT64_MAX);
  lj_gc2_tabledesc_init_unpublished(&desc, 0);
  assert(lj_gc2_tabledesc_try_publish(&desc, (void *)(uintptr_t)3, &second,
           NULL) == LJ_GC2_TABLEDESC_RESULT_INVALID);
  assert(lj_gc2_tabledesc_snapshot(&desc).state == LJ_GC2_TABLEDESC_IDLE);
  second.table = (uint64_t)(uintptr_t)&fake_table[0];
  second.generation = 20;
  desc.value.lo = 3;
  desc.value.hi = 20;
  assert(lj_gc2_tabledesc_finish_help(&desc, &second, &observed) ==
         LJ_GC2_TABLEDESC_RESULT_PINNED);
  assert(observed.state == LJ_GC2_TABLEDESC_PINNED);

  assert(lj_gc2_table_token_init_unpublished(&token, 7));
  assert(lj_gc2_table_token_refresh(&token, &token_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_capture_pending(&token, &token_third) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(token_third.control == token_first.control);
  assert(lj_gc2_table_token_generation(token_first.control) == 8 &&
         lj_gc2_table_token_state(token_first.control) ==
           LJ_GC2_TABLE_TOKEN_PENDING);
  assert(lj_gc2_table_token_refresh(&token, &token_second) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_generation(token_second.control) == 9);
  assert(lj_gc2_table_token_complete(&token, &token_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_BUSY);
  assert(lj_gc2_table_token_complete(&token, &token_second) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  control = la_load64_acq(&token.control);
  assert(lj_gc2_table_token_state(control) == LJ_GC2_TABLE_TOKEN_NONE &&
         lj_gc2_table_token_generation(control) == 10);
  assert(lj_gc2_table_token_refresh(&token, &token_third) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_generation(token_third.control) == 11);
  assert(lj_gc2_table_token_complete(&token, &token_second) ==
         LJ_GC2_TABLE_TOKEN_RESULT_BUSY);

  /* Two scanners may share one captured generation, but only one exact clear
  ** wins and advances it to NONE. */
  memset(arg, 0, sizeof(arg));
  arg[0].token = arg[1].token = &token;
  arg[0].ticket = arg[1].ticket = token_third;
  arg[0].ready = arg[1].ready = &ready;
  arg[0].go = arg[1].go = &go;
  assert(pthread_create(&thread[0], NULL, table_token_complete_thread,
                        &arg[0]) == 0);
  assert(pthread_create(&thread[1], NULL, table_token_complete_thread,
                        &arg[1]) == 0);
  while (la_load32_acq(&ready) != 2)
    la_cpu_pause();
  la_store32_rel(&go, 1);
  assert(pthread_join(thread[0], NULL) == 0);
  assert(pthread_join(thread[1], NULL) == 0);
  assert((arg[0].result == LJ_GC2_TABLE_TOKEN_RESULT_OK) +
         (arg[1].result == LJ_GC2_TABLE_TOKEN_RESULT_OK) == 1);
  assert((arg[0].result == LJ_GC2_TABLE_TOKEN_RESULT_BUSY) +
         (arg[1].result == LJ_GC2_TABLE_TOKEN_RESULT_BUSY) == 1);

  assert(lj_gc2_table_token_init_unpublished(
           &token, LJ_GC2_TABLE_TOKEN_MAX_GENERATION - 1u));
  assert(lj_gc2_table_token_refresh(&token, &token_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_generation(token_first.control) ==
         LJ_GC2_TABLE_TOKEN_MAX_GENERATION);
  assert(lj_gc2_table_token_complete(&token, &token_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_PINNED);
  assert(lj_gc2_table_token_refresh(&token, &token_second) ==
         LJ_GC2_TABLE_TOKEN_RESULT_PINNED);
  token_first.control = lj_gc2_table_token_pack(
    5, LJ_GC2_TABLE_TOKEN_PENDING);
  token.control = lj_gc2_table_token_pack(5, LJ_GC2_TABLE_TOKEN_INVALID);
  assert(lj_gc2_table_token_complete(&token, &token_first) ==
         LJ_GC2_TABLE_TOKEN_RESULT_PINNED);
  token.control = lj_gc2_table_token_pack(5, LJ_GC2_TABLE_TOKEN_INVALID);
  assert(lj_gc2_table_token_refresh(&token, &token_second) ==
         LJ_GC2_TABLE_TOKEN_RESULT_PINNED);
  assert(lj_gc2_table_token_state(la_load64_acq(&token.control)) ==
         LJ_GC2_TABLE_TOKEN_PINNED);
}

enum { TABLEDESC_STRESS_ITERATIONS = 10000, TABLEDESC_STRESS_READERS = 4 };

typedef struct TableDescStress {
  LJGC2TableDesc desc;
  LJGC2TableDesc identity[2];
  uint64_t active_ack;
  uint32_t ready;
  uint32_t go;
  uint32_t published;
  uint32_t completed;
  uint32_t done;
  uint32_t active_seen;
} TableDescStress;

static void tabledesc_stress_ack(TableDescStress *stress,
                                 uint64_t generation)
{
  uint64_t old = la_load64_acq(&stress->active_ack);
  while (old < generation &&
         !la_cas64(&stress->active_ack, &old, generation,
                   LA_ACQ_REL, LA_ACQ))
    ;
}

static void *tabledesc_stress_publisher(void *ud)
{
  TableDescStress *stress = (TableDescStress *)ud;
  uint32_t i;
  (void)la_add32_acqrel(&stress->ready, 1);
  while (!la_load32_acq(&stress->go))
    la_cpu_pause();
  for (i = 0; i < TABLEDESC_STRESS_ITERATIONS; i++) {
    LJGC2TableDescSnap observed;
    LJGC2TableDescTicket ticket;
    while (la_load32_acq(&stress->completed) != i)
      la_cpu_pause();
    assert(lj_gc2_tabledesc_try_publish(&stress->desc,
             &stress->identity[i & 1u], &ticket, &observed) ==
           LJ_GC2_TABLEDESC_RESULT_OK);
    assert(ticket.table == (uint64_t)(uintptr_t)&stress->identity[i & 1u]);
    assert(ticket.generation == (uint64_t)i + 1u);
    assert(observed.state == LJ_GC2_TABLEDESC_ACTIVE &&
           observed.table == (uintptr_t)&stress->identity[i & 1u] &&
           observed.generation == (uint64_t)i + 1u);
    la_store32_rel(&stress->published, i + 1u);
  }
  while (la_load32_acq(&stress->completed) !=
         TABLEDESC_STRESS_ITERATIONS)
    la_cpu_pause();
  la_store32_rel(&stress->done, 1);
  return NULL;
}

static void *tabledesc_stress_helper(void *ud)
{
  TableDescStress *stress = (TableDescStress *)ud;
  LJGC2TableDescTicket stale[2];
  uint32_t i;
  memset(stale, 0, sizeof(stale));
  (void)la_add32_acqrel(&stress->ready, 1);
  while (!la_load32_acq(&stress->go))
    la_cpu_pause();
  for (i = 0; i < TABLEDESC_STRESS_ITERATIONS; i++) {
    LJGC2TableDescSnap snap, observed;
    LJGC2TableDescTicket current;
    unsigned identity = i & 1u;
    while (la_load32_acq(&stress->published) != i + 1u)
      la_cpu_pause();
    snap = lj_gc2_tabledesc_snapshot(&stress->desc);
    assert(snap.state == LJ_GC2_TABLEDESC_ACTIVE &&
           snap.table == (uintptr_t)&stress->identity[identity] &&
           snap.generation == (uint64_t)i + 1u);
    current.table = (uint64_t)snap.table;
    current.generation = snap.generation;

    /* Guarantee that a racing reader sampled this exact ACTIVE pair before
    ** allowing the helper to clear it. */
    while (la_load64_acq(&stress->active_ack) < current.generation)
      la_cpu_pause();

    /* Each identity is reused every other round. Its helper ticket from the
    ** prior incarnation names the same address, but must lose on generation. */
    if (i >= 2) {
      assert(stale[identity].table == current.table);
      assert(stale[identity].generation + 2u == current.generation);
      assert(lj_gc2_tabledesc_finish_help(&stress->desc,
               &stale[identity], &observed) ==
             LJ_GC2_TABLEDESC_RESULT_BUSY);
      assert(observed.state == LJ_GC2_TABLEDESC_ACTIVE &&
             observed.table == (uintptr_t)current.table &&
             observed.generation == current.generation);
    }
    stale[identity] = current;
    assert(lj_gc2_tabledesc_finish_help(&stress->desc, &current,
             &observed) == LJ_GC2_TABLEDESC_RESULT_OK);
    assert(observed.state == LJ_GC2_TABLEDESC_IDLE &&
           observed.table == 0 &&
           observed.generation == current.generation);
    la_store32_rel(&stress->completed, i + 1u);
  }
  return NULL;
}

static void *tabledesc_stress_reader(void *ud)
{
  TableDescStress *stress = (TableDescStress *)ud;
  uint64_t previous_generation = 0;
  (void)la_add32_acqrel(&stress->ready, 1);
  while (!la_load32_acq(&stress->go))
    la_cpu_pause();
  do {
    LJGC2TableDescSnap snap = lj_gc2_tabledesc_snapshot(&stress->desc);
    assert(snap.generation >= previous_generation &&
           snap.generation <= TABLEDESC_STRESS_ITERATIONS);
    if (snap.state == LJ_GC2_TABLEDESC_ACTIVE) {
      uint64_t round;
      assert(snap.generation != 0);
      round = snap.generation - 1u;
      assert(snap.table ==
             (uintptr_t)&stress->identity[(unsigned)(round & 1u)]);
      tabledesc_stress_ack(stress, snap.generation);
      (void)la_add32_rlx(&stress->active_seen, 1);
    } else {
      assert(snap.state == LJ_GC2_TABLEDESC_IDLE && snap.table == 0);
    }
    previous_generation = snap.generation;
  } while (!la_load32_acq(&stress->done));
  return NULL;
}

static void test_concurrent_tabledesc_publish_help_reuse(void)
{
  TableDescStress stress;
  pthread_t publisher, helper, readers[TABLEDESC_STRESS_READERS];
  LJGC2TableDescSnap final;
  unsigned i;

  memset(&stress, 0, sizeof(stress));
  lj_gc2_tabledesc_init_unpublished(&stress.desc, 0);
  assert(((uintptr_t)&stress.desc & 15u) == 0);
  assert(((uintptr_t)&stress.identity[0] & 15u) == 0);
  assert(((uintptr_t)&stress.identity[1] & 15u) == 0);
  assert(pthread_create(&publisher, NULL, tabledesc_stress_publisher,
                        &stress) == 0);
  assert(pthread_create(&helper, NULL, tabledesc_stress_helper,
                        &stress) == 0);
  for (i = 0; i < TABLEDESC_STRESS_READERS; i++)
    assert(pthread_create(&readers[i], NULL, tabledesc_stress_reader,
                          &stress) == 0);
  while (la_load32_acq(&stress.ready) != TABLEDESC_STRESS_READERS + 2u)
    la_cpu_pause();
  la_store32_rel(&stress.go, 1);
  assert(pthread_join(publisher, NULL) == 0);
  assert(pthread_join(helper, NULL) == 0);
  for (i = 0; i < TABLEDESC_STRESS_READERS; i++)
    assert(pthread_join(readers[i], NULL) == 0);
  final = lj_gc2_tabledesc_snapshot(&stress.desc);
  assert(final.state == LJ_GC2_TABLEDESC_IDLE && final.table == 0 &&
         final.generation == TABLEDESC_STRESS_ITERATIONS);
  assert(la_load64_acq(&stress.active_ack) ==
         TABLEDESC_STRESS_ITERATIONS);
  assert(la_load32_acq(&stress.active_seen) >=
         TABLEDESC_STRESS_ITERATIONS);
}

typedef struct TableDescMalformedRace {
  LJGC2TableDesc *desc;
  LJGC2TableDescTicket ticket;
  LJGC2TableDescSnap observed;
  uint32_t ready;
  uint32_t go;
  uint32_t captured;
  uint32_t injected;
  LJGC2TableDescResult result;
} TableDescMalformedRace;

static void *tabledesc_malformed_injector(void *ud)
{
  TableDescMalformedRace *race = (TableDescMalformedRace *)ud;
  la_u128 expected, malformed;
  (void)la_add32_acqrel(&race->ready, 1);
  while (!la_load32_acq(&race->go))
    la_cpu_pause();
  while (!la_load32_acq(&race->captured))
    la_cpu_pause();
  expected.lo = race->ticket.table;
  expected.hi = race->ticket.generation;
  malformed.lo = 3;
  malformed.hi = race->ticket.generation;
  assert(la_cas128(&race->desc->value, &expected, malformed));
  la_store32_rel(&race->injected, 1);
  return NULL;
}

static void *tabledesc_malformed_helper(void *ud)
{
  TableDescMalformedRace *race = (TableDescMalformedRace *)ud;
  LJGC2TableDescSnap captured;
  (void)la_add32_acqrel(&race->ready, 1);
  while (!la_load32_acq(&race->go))
    la_cpu_pause();
  captured = lj_gc2_tabledesc_snapshot(race->desc);
  assert(captured.state == LJ_GC2_TABLEDESC_ACTIVE &&
         captured.table == (uintptr_t)race->ticket.table &&
         captured.generation == race->ticket.generation);
  la_store32_rel(&race->captured, 1);
  while (!la_load32_acq(&race->injected))
    la_cpu_pause();
  race->result = lj_gc2_tabledesc_finish_help(race->desc, &race->ticket,
                                               &race->observed);
  return NULL;
}

static void test_tabledesc_delayed_helper_malformed_race(void)
{
  LJGC2TableDesc desc, identity;
  TableDescMalformedRace race;
  pthread_t injector, helper;
  LJGC2TableDescSnap snap;

  memset(&identity, 0, sizeof(identity));
  memset(&race, 0, sizeof(race));
  lj_gc2_tabledesc_init_unpublished(&desc, 31);
  assert(lj_gc2_tabledesc_try_publish(&desc, &identity, &race.ticket,
           NULL) == LJ_GC2_TABLEDESC_RESULT_OK);
  race.desc = &desc;
  assert(pthread_create(&injector, NULL, tabledesc_malformed_injector,
                        &race) == 0);
  assert(pthread_create(&helper, NULL, tabledesc_malformed_helper,
                        &race) == 0);
  while (la_load32_acq(&race.ready) != 2)
    la_cpu_pause();
  la_store32_rel(&race.go, 1);
  assert(pthread_join(injector, NULL) == 0);
  assert(pthread_join(helper, NULL) == 0);
  assert(race.result == LJ_GC2_TABLEDESC_RESULT_PINNED);
  assert(race.observed.state == LJ_GC2_TABLEDESC_PINNED &&
         race.observed.generation == race.ticket.generation);
  snap = lj_gc2_tabledesc_snapshot(&desc);
  assert(snap.state == LJ_GC2_TABLEDESC_PINNED &&
         snap.generation == race.ticket.generation);
}

static LJGC2RootDescSpec rootdesc_scalar_spec(uint64_t old_root,
                                              uint64_t new_root)
{
  LJGC2RootDescSpec spec;
  memset(&spec, 0, sizeof(spec));
  spec.flags = LJ_GC2_ROOTDESC_F_OLD | LJ_GC2_ROOTDESC_F_NEW;
  spec.old_root = old_root;
  spec.new_root = new_root;
  return spec;
}

static LJGC2RootDescSpec rootdesc_table_store_spec(uint64_t parent,
                                                   uint64_t key,
                                                   uint64_t value)
{
  LJGC2RootDescSpec spec;
  memset(&spec, 0, sizeof(spec));
  spec.flags = LJ_GC2_ROOTDESC_F_OLD | LJ_GC2_ROOTDESC_F_NEW |
               LJ_GC2_ROOTDESC_F_AUX | LJ_GC2_ROOTDESC_F_TABLE_STORE;
  spec.old_root = parent;
  spec.new_root = key;
  spec.aux_root = value;
  return spec;
}

typedef struct RootDescCoverArg {
  LJGC2RootDesc *desc;
  LJGC2RootDescView view;
  LJGC2Activation *activation;
  LJGC2ActivationSnap closing;
  uint32_t *ready;
  uint32_t *go;
  LJGC2RootDescResult result;
} RootDescCoverArg;

static void *rootdesc_cover_thread(void *ud)
{
  RootDescCoverArg *arg = (RootDescCoverArg *)ud;
  (void)la_add32_acqrel(arg->ready, 1);
  while (!la_load32_acq(arg->go))
    la_cpu_pause();
  arg->result = lj_gc2_rootdesc_cover_after_trace(
    arg->desc, &arg->view, arg->activation, &arg->closing);
  return NULL;
}

typedef struct RootDescPinArg {
  LJGC2RootDesc *desc;
  uint64_t control;
  uint32_t *ready;
  uint32_t *go;
  LJGC2RootDescResult result;
} RootDescPinArg;

static void *rootdesc_pin_thread(void *ud)
{
  RootDescPinArg *arg = (RootDescPinArg *)ud;
  (void)la_add32_acqrel(arg->ready, 1);
  while (!la_load32_acq(arg->go))
    la_cpu_pause();
  arg->result = lj_gc2_rootdesc_pin(arg->desc, arg->control);
  return NULL;
}

static void test_rootdesc_scalar_lifecycle_and_aba(void)
{
  LJGC2RootDesc desc;
  LJGC2RootDescSpec spec = rootdesc_scalar_spec(UINT64_C(0x1234),
                                                UINT64_C(0x5678));
  LJGC2RootDescTicket first, second;
  LJGC2RootDescView view;

  assert(sizeof(desc) == 96);
  assert(((uintptr_t)&desc & 15u) == 0);
  assert(lj_gc2_rootdesc_init_unpublished(&desc, 7));
  memset(&view, 0, sizeof(view));
  assert(lj_gc2_rootdesc_snapshot(&desc, &view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  assert(view.generation == 7);
  assert(lj_gc2_rootdesc_publish(&desc, &spec, &first) ==
         LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_snapshot(&desc, &view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(view.generation == 8);
  assert(view.flags == spec.flags);
  assert(view.old_root == spec.old_root);
  assert(view.new_root == spec.new_root);
  assert(view.aux_root == 0);
  assert(lj_gc2_rootdesc_finish(&desc, &first) == LJ_GC2_ROOTDESC_OK);

  spec.old_root++;
  spec.new_root++;
  assert(lj_gc2_rootdesc_publish(&desc, &spec, &second) ==
         LJ_GC2_ROOTDESC_OK);
  assert(second.control != first.control);
  assert(lj_gc2_rootdesc_finish(&desc, &first) == LJ_GC2_ROOTDESC_BUSY);
  assert(lj_gc2_rootdesc_snapshot(&desc, &view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(view.generation == 9);
  assert(view.old_root == spec.old_root);
  assert(lj_gc2_rootdesc_finish(&desc, &second) == LJ_GC2_ROOTDESC_OK);
}

static void test_rootdesc_table_store_coverage(void)
{
  LJGC2Activation activation;
  LJGC2ActivationSnap open, closing, commit, pending, reopened, closing2;
  LJGC2ActivationSnap observed;
  LJGC2RootDesc desc;
  LJGC2RootDesc desc2;
  LJGC2RootDescSpec spec = rootdesc_table_store_spec(
    UINT64_C(0xfff7000000001234), UINT64_C(0xfff5000000005678),
    UINT64_C(0xfff6000000009abc));
  LJGC2RootDescTicket ticket;
  LJGC2RootDescTicket ticket2;
  LJGC2RootDescView view;
  LJGC2RootDescView view2;
  LJGC2RootDescCoverage coverage;

  assert(lj_gc2_rootdesc_spec_valid(&spec));
  assert((offsetof(LJGC2RootDesc, coverage) & 15u) == 0);
  assert(offsetof(LJGC2RootDesc, aux_root) <
         offsetof(LJGC2RootDesc, range));
  assert(offsetof(LJGC2RootDesc, range) <
         offsetof(LJGC2RootDesc, coverage));
  assert(lj_gc2_rootdesc_init_unpublished(&desc, 7));
  coverage = lj_gc2_rootdesc_coverage_snapshot(&desc);
  assert(coverage.descriptor_control == 0);
  assert(coverage.activation_generation == 0);
  assert(lj_gc2_rootdesc_publish(&desc, &spec, &ticket) ==
         LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_snapshot(&desc, &view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(view.old_root == spec.old_root);
  assert(view.new_root == spec.new_root);
  assert(view.aux_root == spec.aux_root);

  assert(lj_gc2_activation_init_unpublished(&activation, 31, 20,
                                             LJ_GC2_ACT_WEAK));
  open = lj_gc2_activation_snapshot(&activation);
  assert(lj_gc2_activation_try_gate(&activation, &open,
           LJ_GC2_ROOT_GATE_CLOSING, &closing) == LJ_GC2_TRANSITION_OK);
  /* A view is bound to its descriptor identity, not merely its common first
  ** ACTIVE generation. This rejects a cross-TG/cross-descriptor certificate. */
  assert(lj_gc2_rootdesc_init_unpublished(&desc2, 7));
  assert(lj_gc2_rootdesc_publish(&desc2, &spec, &ticket2) ==
         LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_snapshot(&desc2, &view2) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(view2.generation == view.generation);
  assert(lj_gc2_rootdesc_cover_after_trace(
           &desc, &view2, &activation, &closing) ==
         LJ_GC2_ROOTDESC_INVALID);
  assert(lj_gc2_rootdesc_finish(&desc2, &ticket2) == LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_covered(&desc, &activation, &closing) ==
         LJ_GC2_ROOTDESC_BUSY);
  /* A helper traces the full payload before publishing this certificate. */
  assert(lj_gc2_rootdesc_cover_after_trace(
           &desc, &view, &activation, &closing) ==
         LJ_GC2_ROOTDESC_OK);
  coverage = lj_gc2_rootdesc_coverage_snapshot(&desc);
  assert(coverage.descriptor_control == ticket.control);
  assert(coverage.activation_generation == closing.generation);
  assert(lj_gc2_rootdesc_covered(&desc, &activation, &closing) ==
         LJ_GC2_ROOTDESC_OK);

  /* Close may commit while the owner remains paused ACTIVE: the future store
  ** payload is covered, and the owner must invalidate COMMIT before storing. */
  assert(lj_gc2_activation_try_gate(&activation, &closing,
           LJ_GC2_ROOT_GATE_COMMIT, &commit) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_rootdesc_cover_after_trace(
           &desc, &view, &activation, &closing) ==
         LJ_GC2_ROOTDESC_BUSY);
  /* The paused owner resumes at COMMIT and must invalidate that exact close
  ** before its first future store. Only the resulting PENDING snapshot admits
  ** the store; the old closer can no longer perform an exact phase edge. */
  assert(lj_gc2_activation_try_gate(&activation, &commit,
           LJ_GC2_ROOT_GATE_PENDING, &pending) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_gate(&activation, &pending,
           LJ_GC2_ROOT_GATE_OPEN, &reopened) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_gate(&activation, &reopened,
           LJ_GC2_ROOT_GATE_CLOSING, &closing2) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_rootdesc_covered(&desc, &activation, &closing2) ==
         LJ_GC2_ROOTDESC_BUSY);
  assert(lj_gc2_rootdesc_cover_after_trace(
           &desc, &view, &activation, &closing2) ==
         LJ_GC2_ROOTDESC_OK);
  coverage = lj_gc2_rootdesc_coverage_snapshot(&desc);
  assert(coverage.descriptor_control == ticket.control);
  assert(coverage.activation_generation == closing2.generation);

  /* A helper delayed from the first close cannot move coverage backwards. */
  assert(lj_gc2_rootdesc_cover_after_trace(
           &desc, &view, &activation, &closing) ==
         LJ_GC2_ROOTDESC_BUSY);
  coverage = lj_gc2_rootdesc_coverage_snapshot(&desc);
  assert(coverage.activation_generation == closing2.generation);
  assert(lj_gc2_rootdesc_finish(&desc, &ticket) == LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_covered(&desc, &activation, &closing2) ==
         LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_activation_try_gate(&activation, &closing2,
           LJ_GC2_ROOT_GATE_COMMIT, &observed) == LJ_GC2_TRANSITION_OK);

  spec.flags &= ~LJ_GC2_ROOTDESC_F_AUX;
  assert(!lj_gc2_rootdesc_spec_valid(&spec));

  /* A torn/malformed certificate is a sticky descriptor failure, never an
  ** invitation to overwrite the evidence and authorize close. */
  spec = rootdesc_table_store_spec(1, 2, 3);
  assert(lj_gc2_rootdesc_init_unpublished(&desc, 0));
  assert(lj_gc2_rootdesc_publish(&desc, &spec, &ticket) ==
         LJ_GC2_ROOTDESC_OK);
  {
    la_u128 stale, winner, malformed;
    stale.lo = stale.hi = 0;  /* Helper's exact pre-race observation. */
    winner = stale;
    malformed.lo = ticket.control;
    malformed.hi = 0;
    assert(la_cas128(&desc.coverage, &winner, malformed));
    /* The failed internal 128-bit CAS returns the malformed winner. It must be
    ** validated before a retry can overwrite it with a plausible certificate.
    */
    assert(lj_gc2_rootdesc_coverage_advance(
             &desc, ticket.control, 4, stale) == LJ_GC2_ROOTDESC_PINNED);
  }
  coverage = lj_gc2_rootdesc_coverage_snapshot(&desc);
  assert(!lj_gc2_rootdesc_coverage_valid(&coverage));
  assert(lj_gc2_activation_init_unpublished(&activation, 45, 3,
                                             LJ_GC2_ACT_MARK));
  open = lj_gc2_activation_snapshot(&activation);
  assert(lj_gc2_activation_try_gate(&activation, &open,
           LJ_GC2_ROOT_GATE_CLOSING, &closing) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_rootdesc_covered(&desc, &activation, &closing) ==
         LJ_GC2_ROOTDESC_PINNED);
  assert(lj_gc2_rootdesc_snapshot(&desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_NO_RECLAIM);

  /* Both non-wrapping identities may be certified at their final valid value;
  ** the next activation edge then selects sticky NO_RECLAIM, never wrap. */
  assert(lj_gc2_rootdesc_init_unpublished(
           &desc, LJ_GC2_ROOTDESC_MAX_GENERATION - 1u));
  assert(lj_gc2_rootdesc_publish(&desc, &spec, &ticket) ==
         LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_snapshot(&desc, &view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(view.generation == LJ_GC2_ROOTDESC_MAX_GENERATION);
  assert(lj_gc2_activation_init_unpublished(
           &activation, 99, LJ_GC2_ACT_MAX_GENERATION - 1u,
           LJ_GC2_ACT_WEAK));
  open = lj_gc2_activation_snapshot(&activation);
  assert(lj_gc2_activation_try_gate(&activation, &open,
           LJ_GC2_ROOT_GATE_CLOSING, &closing) == LJ_GC2_TRANSITION_OK);
  assert(closing.generation == LJ_GC2_ACT_MAX_GENERATION);
  assert(lj_gc2_rootdesc_cover_after_trace(
           &desc, &view, &activation, &closing) == LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_covered(&desc, &activation, &closing) ==
         LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_activation_try_gate(&activation, &closing,
           LJ_GC2_ROOT_GATE_COMMIT, &observed) ==
         LJ_GC2_TRANSITION_PINNED);
  assert(observed.state == LJ_GC2_ACT_NO_RECLAIM);
  assert(lj_gc2_rootdesc_finish(&desc, &ticket) == LJ_GC2_ROOTDESC_OK);
}

static void test_rootdesc_ranges_and_sticky_failure(void)
{
  uint64_t slots[16];
  LJGC2RootDesc desc;
  LJGC2RootDescSpec spec;
  LJGC2RootDescTicket ticket;
  LJGC2RootDescView view;

  memset(&spec, 0, sizeof(spec));
  spec.flags = LJ_GC2_ROOTDESC_F_RANGE0 | LJ_GC2_ROOTDESC_F_RANGE1 |
               LJ_GC2_ROOTDESC_F_MOVE_DOWN;
  spec.range[0].lo = &slots[2];
  spec.range[0].hi = &slots[12];
  spec.range[1].lo = &slots[0];
  spec.range[1].hi = &slots[10];
  assert(lj_gc2_rootdesc_spec_valid(&spec));
  assert(lj_gc2_rootdesc_init_unpublished(&desc, 0));
  assert(lj_gc2_rootdesc_publish(&desc, &spec, &ticket) ==
         LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_snapshot(&desc, &view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(view.range[0].lo == spec.range[0].lo);
  assert(view.range[0].hi == spec.range[0].hi);
  assert(view.range[1].lo == spec.range[1].lo);
  assert(view.range[1].hi == spec.range[1].hi);

  /* Reentrant owner publication is a sticky safety failure, never an ABA. */
  assert(lj_gc2_rootdesc_publish(&desc, &spec, &ticket) ==
         LJ_GC2_ROOTDESC_PINNED);
  assert(lj_gc2_rootdesc_snapshot(&desc, &view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_NO_RECLAIM);
  assert(lj_gc2_rootdesc_finish(&desc, &ticket) ==
         LJ_GC2_ROOTDESC_PINNED);

  spec.flags |= LJ_GC2_ROOTDESC_F_MOVE_UP;
  assert(!lj_gc2_rootdesc_spec_valid(&spec));
  spec.flags = LJ_GC2_ROOTDESC_F_RANGE0;
  assert(!lj_gc2_rootdesc_spec_valid(&spec));
  spec.flags = LJ_GC2_ROOTDESC_F_OLD | LJ_GC2_ROOTDESC_F_MOVE_UP;
  assert(!lj_gc2_rootdesc_spec_valid(&spec));

  spec.flags = LJ_GC2_ROOTDESC_F_RANGE1 | LJ_GC2_ROOTDESC_F_MOVE_UP;
  assert(!lj_gc2_rootdesc_spec_valid(&spec));
  spec.flags = LJ_GC2_ROOTDESC_F_RANGE0 | LJ_GC2_ROOTDESC_F_RANGE1 |
               LJ_GC2_ROOTDESC_F_MOVE_UP;
  spec.range[0].lo = &slots[0];
  spec.range[0].hi = &slots[4];
  spec.range[1].lo = &slots[4];
  spec.range[1].hi = &slots[9];
  assert(!lj_gc2_rootdesc_spec_valid(&spec));

  assert(lj_gc2_rootdesc_init_unpublished(&desc, 10));
  assert(lj_gc2_rootdesc_publish(&desc, &spec, &ticket) ==
         LJ_GC2_ROOTDESC_PINNED);
  assert(lj_gc2_rootdesc_snapshot(&desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_NO_RECLAIM);

  assert(lj_gc2_rootdesc_init_unpublished(&desc,
                                           LJ_GC2_ROOTDESC_MAX_GENERATION));
  spec = rootdesc_scalar_spec(1, 2);
  assert(lj_gc2_rootdesc_publish(&desc, &spec, &ticket) ==
         LJ_GC2_ROOTDESC_PINNED);
  assert(lj_gc2_rootdesc_snapshot(&desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_NO_RECLAIM);
  assert(!lj_gc2_rootdesc_init_unpublished(
      &desc, LJ_GC2_ROOTDESC_MAX_GENERATION + 1u));
}

static void test_concurrent_rootdesc_coverage(void)
{
  LJGC2Activation activation;
  LJGC2ActivationSnap open, closing;
  LJGC2RootDesc desc;
  LJGC2RootDescSpec spec = rootdesc_table_store_spec(11, 22, 33);
  LJGC2RootDescTicket ticket;
  LJGC2RootDescView view;
  RootDescCoverArg cover[2];
  RootDescPinArg pin;
  pthread_t thread[2];
  uint32_t ready = 0, go = 0;
  unsigned i;

  assert(lj_gc2_rootdesc_init_unpublished(&desc, 0));
  assert(lj_gc2_rootdesc_publish(&desc, &spec, &ticket) ==
         LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_snapshot(&desc, &view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(lj_gc2_activation_init_unpublished(&activation, 8, 2,
                                             LJ_GC2_ACT_MARK));
  open = lj_gc2_activation_snapshot(&activation);
  assert(lj_gc2_activation_try_gate(&activation, &open,
           LJ_GC2_ROOT_GATE_CLOSING, &closing) == LJ_GC2_TRANSITION_OK);
  memset(cover, 0, sizeof(cover));
  for (i = 0; i < 2; i++) {
    cover[i].desc = &desc;
    cover[i].view = view;
    cover[i].activation = &activation;
    cover[i].closing = closing;
    cover[i].ready = &ready;
    cover[i].go = &go;
    assert(pthread_create(&thread[i], NULL, rootdesc_cover_thread,
                          &cover[i]) == 0);
  }
  while (la_load32_acq(&ready) != 2)
    la_cpu_pause();
  la_store32_rel(&go, 1);
  for (i = 0; i < 2; i++) {
    assert(pthread_join(thread[i], NULL) == 0);
    assert(cover[i].result == LJ_GC2_ROOTDESC_OK);
  }
  assert(lj_gc2_rootdesc_covered(&desc, &activation, &closing) ==
         LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_finish(&desc, &ticket) == LJ_GC2_ROOTDESC_OK);

  /* A sticky pin racing coverage always wins close authority. Coverage may
  ** finish first, but it can never make the final pinned descriptor usable. */
  assert(lj_gc2_rootdesc_init_unpublished(&desc, 0));
  assert(lj_gc2_rootdesc_publish(&desc, &spec, &ticket) ==
         LJ_GC2_ROOTDESC_OK);
  assert(lj_gc2_rootdesc_snapshot(&desc, &view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  memset(&cover[0], 0, sizeof(cover[0]));
  memset(&pin, 0, sizeof(pin));
  ready = go = 0;
  cover[0].desc = &desc;
  cover[0].view = view;
  cover[0].activation = &activation;
  cover[0].closing = closing;
  cover[0].ready = &ready;
  cover[0].go = &go;
  pin.desc = &desc;
  pin.control = ticket.control;
  pin.ready = &ready;
  pin.go = &go;
  assert(pthread_create(&thread[0], NULL, rootdesc_cover_thread,
                        &cover[0]) == 0);
  assert(pthread_create(&thread[1], NULL, rootdesc_pin_thread, &pin) == 0);
  while (la_load32_acq(&ready) != 2)
    la_cpu_pause();
  la_store32_rel(&go, 1);
  assert(pthread_join(thread[0], NULL) == 0);
  assert(pthread_join(thread[1], NULL) == 0);
  assert(cover[0].result == LJ_GC2_ROOTDESC_OK ||
         cover[0].result == LJ_GC2_ROOTDESC_PINNED);
  assert(pin.result == LJ_GC2_ROOTDESC_PINNED);
  assert(lj_gc2_rootdesc_snapshot(&desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_NO_RECLAIM);
  assert(lj_gc2_rootdesc_covered(&desc, &activation, &closing) ==
         LJ_GC2_ROOTDESC_PINNED);
}

static void test_rootdesc_pin_wins_delayed_activation(void)
{
  LJGC2RootDesc desc;
  uint64_t idle, publishing, active, expected;

  assert(lj_gc2_rootdesc_init_unpublished(&desc, 3));
  idle = la_load64_acq(&desc.control);
  publishing = lj_gc2_rootdesc_pack_control(
    4, LJ_GC2_ROOTDESC_PUBLISHING);
  active = lj_gc2_rootdesc_pack_control(4, LJ_GC2_ROOTDESC_ACTIVE);
  expected = idle;
  assert(la_cas64(&desc.control, &expected, publishing,
                  LA_ACQ_REL, LA_ACQ));
  assert(lj_gc2_rootdesc_snapshot(&desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_RETRY);
  assert(lj_gc2_rootdesc_pin(&desc, publishing) == LJ_GC2_ROOTDESC_PINNED);
  assert(lj_gc2_rootdesc_try_activate(&desc, publishing, active) ==
         LJ_GC2_ROOTDESC_PINNED);
  assert(lj_gc2_rootdesc_state(la_load64_acq(&desc.control)) ==
         LJ_GC2_ROOTDESC_NO_RECLAIM);
}

enum { ROOTDESC_STRESS_ITERATIONS = 20000 };

typedef struct RootDescStress {
  LJGC2RootDesc desc;
  uint64_t slots[8];
  uint32_t ready;
  uint32_t go;
  uint32_t done;
  uint32_t active_seen;
} RootDescStress;

static void *rootdesc_writer(void *ud)
{
  RootDescStress *stress = (RootDescStress *)ud;
  LJGC2RootDescSpec spec;
  LJGC2RootDescTicket ticket;
  uint64_t i;
  memset(&spec, 0, sizeof(spec));
  spec.flags = LJ_GC2_ROOTDESC_F_OLD | LJ_GC2_ROOTDESC_F_NEW |
               LJ_GC2_ROOTDESC_F_AUX |
               LJ_GC2_ROOTDESC_F_RANGE0 | LJ_GC2_ROOTDESC_F_MOVE_UP;
  spec.range[0].lo = &stress->slots[1];
  spec.range[0].hi = &stress->slots[7];
  (void)la_add32_acqrel(&stress->ready, 1);
  while (!la_load32_acq(&stress->go))
    la_cpu_pause();
  for (i = 1; i <= ROOTDESC_STRESS_ITERATIONS; i++) {
    unsigned pause;
    spec.old_root = i;
    spec.new_root = i ^ UINT64_C(0xd1e5c0de5eed1234);
    spec.aux_root = i + UINT64_C(0x5a5a5a5a);
    assert(lj_gc2_rootdesc_publish(&stress->desc, &spec, &ticket) ==
           LJ_GC2_ROOTDESC_OK);
    /* Keep ACTIVE observable long enough to exercise concurrent snapshots on
    ** fast ARM cores instead of letting the owner finish in one timeslice. */
    for (pause = 0; pause < 64; pause++)
      la_cpu_pause();
    if ((i & 63u) == 0)
      (void)sched_yield();
    assert(lj_gc2_rootdesc_finish(&stress->desc, &ticket) ==
           LJ_GC2_ROOTDESC_OK);
  }
  la_store32_rel(&stress->done, 1);
  return NULL;
}

static void *rootdesc_reader(void *ud)
{
  RootDescStress *stress = (RootDescStress *)ud;
  uint64_t last_generation = 0;
  (void)la_add32_acqrel(&stress->ready, 1);
  while (!la_load32_acq(&stress->go))
    la_cpu_pause();
  do {
    LJGC2RootDescView view;
    LJGC2RootDescSnapshotResult result =
      lj_gc2_rootdesc_snapshot(&stress->desc, &view);
    assert(result != LJ_GC2_ROOTDESC_SNAPSHOT_NO_RECLAIM);
    if (result == LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE) {
      assert(view.generation >= last_generation);
      assert(view.new_root ==
             (view.old_root ^ UINT64_C(0xd1e5c0de5eed1234)));
      assert(view.aux_root == view.old_root + UINT64_C(0x5a5a5a5a));
      assert(view.range[0].lo == &stress->slots[1]);
      assert(view.range[0].hi == &stress->slots[7]);
      last_generation = view.generation;
      (void)la_add32_rlx(&stress->active_seen, 1);
    }
  } while (!la_load32_acq(&stress->done));
  return NULL;
}

static void test_concurrent_rootdesc_snapshots(void)
{
  enum { NREADER = 4 };
  RootDescStress stress;
  pthread_t writer, readers[NREADER];
  LJGC2RootDescView view;
  unsigned i;

  memset(&stress, 0, sizeof(stress));
  assert(lj_gc2_rootdesc_init_unpublished(&stress.desc, 0));
  assert(pthread_create(&writer, NULL, rootdesc_writer, &stress) == 0);
  for (i = 0; i < NREADER; i++)
    assert(pthread_create(&readers[i], NULL, rootdesc_reader, &stress) == 0);
  while (la_load32_acq(&stress.ready) != NREADER + 1)
    la_cpu_pause();
  la_store32_rel(&stress.go, 1);
  assert(pthread_join(writer, NULL) == 0);
  for (i = 0; i < NREADER; i++)
    assert(pthread_join(readers[i], NULL) == 0);
  assert(lj_gc2_rootdesc_snapshot(&stress.desc, &view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  assert(view.generation == ROOTDESC_STRESS_ITERATIONS);
  assert(la_load32_acq(&stress.active_seen) != 0);
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
    assert(snap.gate == LJ_GC2_ROOT_GATE_OPEN);
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
  assert(final.gate == LJ_GC2_ROOT_GATE_OPEN);
}

int main(void)
{
  test_markword_epochs_and_clear();
  test_concurrent_same_word_sets();
  test_concurrent_set_clear();
  test_exhaustive_publication_models();
  test_activation_transitions_and_saturation();
  test_activation_edge_and_epoch_policy();
  test_activation_root_gate();
  test_root_gate_edge_policy();
  test_concurrent_close_pending_commit();
  test_table_exact_target_token_primitives();
  test_table_descriptor_and_token_primitives();
  test_concurrent_tabledesc_publish_help_reuse();
  test_tabledesc_delayed_helper_malformed_race();
  test_concurrent_activation_snapshots();
  test_rootdesc_scalar_lifecycle_and_aba();
  test_rootdesc_table_store_coverage();
  test_rootdesc_ranges_and_sticky_failure();
  test_concurrent_rootdesc_coverage();
  test_rootdesc_pin_wins_delayed_activation();
  test_concurrent_rootdesc_snapshots();
  printf("t-gc2-markword-token OK: %u publication and %u activation schedules\n",
         publish_schedules, activation_schedules);
  return 0;
}
