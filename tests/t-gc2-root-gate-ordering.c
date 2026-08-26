/*
** t-gc2-root-gate-ordering.c - native root-descriptor/gate Dekker litmus.
**
** Build & run on Apple AArch64:
**   cc -std=gnu11 -O2 -Wall -Wextra -Werror -Watomic-alignment -pthread \
**      -arch arm64 -Isrc tests/t-gc2-root-gate-ordering.c \
**      -o /tmp/t-gc2-root-gate-ordering && \
**      /tmp/t-gc2-root-gate-ordering 2000000
**
** Each round begins with an IDLE descriptor and an OPEN root gate. The
** publisher makes the descriptor ACTIVE before observing the gate, while the
** closer makes the gate CLOSING before observing the descriptor. The outcome
** in which both observations see the initial values is the forbidden
** two-object StoreLoad result. Persistent workers and helper-driven resets
** keep thread creation out of the measured rounds.
*/

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lj_gc2token.h"

#if !defined(__APPLE__) || !defined(__aarch64__)
#error "t-gc2-root-gate-ordering requires native Apple AArch64"
#endif

#define ROOT_GATE_DEFAULT_ROUNDS UINT64_C(2000000)
#define ROOT_GATE_MAX_ROUNDS UINT64_C(50000000)
#define ROOT_GATE_CACHELINE 128

typedef struct PublisherObservation {
  LJGC2RootDescTicket ticket;
  LJGC2ActivationSnap gate;
  LJGC2RootDescResult result;
} __attribute__((aligned(ROOT_GATE_CACHELINE))) PublisherObservation;

typedef struct CloserObservation {
  LJGC2ActivationSnap closing;
  LJGC2RootDescView descriptor;
  LJGC2TransitionResult transition;
  LJGC2RootDescSnapshotResult snapshot;
} __attribute__((aligned(ROOT_GATE_CACHELINE))) CloserObservation;

typedef struct RootGateLitmus {
  _Alignas(ROOT_GATE_CACHELINE) LJGC2Activation activation;
  _Alignas(ROOT_GATE_CACHELINE) LJGC2RootDesc descriptor;
  _Alignas(ROOT_GATE_CACHELINE) uint64_t start_round;
  _Alignas(ROOT_GATE_CACHELINE) uint64_t publisher_done;
  _Alignas(ROOT_GATE_CACHELINE) uint64_t closer_done;
  _Alignas(ROOT_GATE_CACHELINE) PublisherObservation publisher;
  _Alignas(ROOT_GATE_CACHELINE) CloserObservation closer;
  LJGC2RootDescSpec spec;
  uint64_t rounds;
} RootGateLitmus;

/* These non-inlined boundaries are also disassembled by the CI wrapper.
** Keeping the public helper calls together makes the actual StoreLoad edge
** visible in native artifacts without replacing it with test-only atomics.
*/
__attribute__((noinline))
LJGC2RootDescResult
arm64_root_gate_publisher_boundary(LJGC2RootDesc *descriptor,
                                   const LJGC2RootDescSpec *spec,
                                   LJGC2RootDescTicket *ticket,
                                   const LJGC2Activation *activation,
                                   LJGC2ActivationSnap *gate)
{
  LJGC2RootDescResult result =
    lj_gc2_rootdesc_publish(descriptor, spec, ticket);
  if (result == LJ_GC2_ROOTDESC_OK)
    *gate = lj_gc2_activation_snapshot(activation);
  return result;
}

__attribute__((noinline))
LJGC2RootDescSnapshotResult
arm64_root_gate_closing_scan_boundary(const LJGC2RootDesc *descriptor,
                                      const LJGC2Activation *activation,
                                      const LJGC2ActivationSnap *closing,
                                      LJGC2RootDescView *view)
{
  return lj_gc2_rootdesc_snapshot_closing(
    descriptor, activation, closing, view);
}

__attribute__((noinline))
LJGC2TransitionResult
arm64_root_gate_closer_boundary(LJGC2Activation *activation,
                                const LJGC2ActivationSnap *open,
                                LJGC2ActivationSnap *closing,
                                const LJGC2RootDesc *descriptor,
                                LJGC2RootDescView *view,
                                LJGC2RootDescSnapshotResult *snapshot)
{
  LJGC2TransitionResult result =
    lj_gc2_activation_try_gate(activation, open,
                               LJ_GC2_ROOT_GATE_CLOSING, closing);
  if (result == LJ_GC2_TRANSITION_OK)
    *snapshot = arm64_root_gate_closing_scan_boundary(
      descriptor, activation, closing, view);
  return result;
}

static void wait_for_round(const RootGateLitmus *litmus, uint64_t round)
{
  while (la_load64_acq(&litmus->start_round) != round)
    la_cpu_pause();
}

/* A small deterministic skew prevents one worker from always winning the
** shared start-line handoff, while leaving most rounds completely unskewed. */
static void round_skew(uint64_t round, int publisher)
{
  unsigned int pauses = 0;
  if ((round & 15u) == (publisher ? 1u : 2u))
    pauses = (unsigned int)((round >> 4) & 7u) + 1u;
  while (pauses-- != 0)
    la_cpu_pause();
}

static void *publisher_worker(void *arg)
{
  RootGateLitmus *litmus = (RootGateLitmus *)arg;
  uint64_t round;
  for (round = 1; round <= litmus->rounds; round++) {
    wait_for_round(litmus, round);
    round_skew(round, 1);
    litmus->publisher.result = arm64_root_gate_publisher_boundary(
      &litmus->descriptor, &litmus->spec, &litmus->publisher.ticket,
      &litmus->activation, &litmus->publisher.gate);
    assert(litmus->publisher.result == LJ_GC2_ROOTDESC_OK);
    assert(litmus->publisher.gate.gate == LJ_GC2_ROOT_GATE_OPEN ||
           litmus->publisher.gate.gate == LJ_GC2_ROOT_GATE_CLOSING);
    la_store64_rel(&litmus->publisher_done, round);
  }
  return NULL;
}

static void *closer_worker(void *arg)
{
  RootGateLitmus *litmus = (RootGateLitmus *)arg;
  uint64_t round;
  for (round = 1; round <= litmus->rounds; round++) {
    LJGC2ActivationSnap open;
    wait_for_round(litmus, round);
    round_skew(round, 0);
    open = lj_gc2_activation_snapshot(&litmus->activation);
    assert(open.gate == LJ_GC2_ROOT_GATE_OPEN);
    litmus->closer.transition = arm64_root_gate_closer_boundary(
      &litmus->activation, &open, &litmus->closer.closing,
      &litmus->descriptor, &litmus->closer.descriptor,
      &litmus->closer.snapshot);
    assert(litmus->closer.transition == LJ_GC2_TRANSITION_OK);
    assert(litmus->closer.snapshot == LJ_GC2_ROOTDESC_SNAPSHOT_IDLE ||
           litmus->closer.snapshot == LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
    la_store64_rel(&litmus->closer_done, round);
  }
  return NULL;
}

static uint64_t parse_rounds(int argc, char **argv)
{
  unsigned long long parsed;
  char *end;
  if (argc == 1)
    return ROOT_GATE_DEFAULT_ROUNDS;
  if (argc != 2) {
    fprintf(stderr, "usage: %s [rounds]\n", argv[0]);
    exit(2);
  }
  errno = 0;
  end = NULL;
  parsed = strtoull(argv[1], &end, 10);
  if (errno != 0 || end == argv[1] || *end != '\0' || parsed == 0 ||
      parsed > ROOT_GATE_MAX_ROUNDS) {
    fprintf(stderr, "rounds must be between 1 and %" PRIu64 "\n",
            ROOT_GATE_MAX_ROUNDS);
    exit(2);
  }
  return (uint64_t)parsed;
}

int main(int argc, char **argv)
{
  RootGateLitmus litmus;
  pthread_t publisher_thread, closer_thread;
  uint64_t open_active = 0;
  uint64_t closing_idle = 0;
  uint64_t closing_active = 0;
  uint64_t forbidden = 0;
  uint64_t first_forbidden = 0;
  uint64_t round;

  memset(&litmus, 0, sizeof(litmus));
  litmus.rounds = parse_rounds(argc, argv);
  litmus.spec.flags = LJ_GC2_ROOTDESC_F_OLD | LJ_GC2_ROOTDESC_F_NEW |
                      LJ_GC2_ROOTDESC_F_AUX |
                      LJ_GC2_ROOTDESC_F_TABLE_STORE;
  litmus.spec.old_root = UINT64_C(0xfff7000000001111);
  litmus.spec.new_root = UINT64_C(0xfff5000000002222);
  litmus.spec.aux_root = UINT64_C(0xfff6000000003333);
  assert(lj_gc2_activation_init_unpublished(&litmus.activation, 1, 0,
                                             LJ_GC2_ACT_WEAK));
  assert(lj_gc2_rootdesc_init_unpublished(
           &litmus.descriptor, 0, &litmus.activation));

  assert(pthread_create(&publisher_thread, NULL,
                        publisher_worker, &litmus) == 0);
  assert(pthread_create(&closer_thread, NULL, closer_worker, &litmus) == 0);

  for (round = 1; round <= litmus.rounds; round++) {
    LJGC2ActivationSnap reopened;
    int publisher_open, closer_idle;

    la_store64_rel(&litmus.start_round, round);
    while (la_load64_acq(&litmus.publisher_done) != round ||
           la_load64_acq(&litmus.closer_done) != round)
      la_cpu_pause();

    publisher_open =
      litmus.publisher.gate.gate == LJ_GC2_ROOT_GATE_OPEN;
    closer_idle =
      litmus.closer.snapshot == LJ_GC2_ROOTDESC_SNAPSHOT_IDLE;
    if (publisher_open && closer_idle) {
      if (first_forbidden == 0)
        first_forbidden = round;
      forbidden++;
    } else if (publisher_open) {
      open_active++;
    } else if (closer_idle) {
      closing_idle++;
    } else {
      closing_active++;
    }

    /* Reset only after both observations, using the same exact tickets that
    ** the racing helpers produced. Generations therefore advance rather than
    ** being overwritten between persistent-thread rounds. */
    assert(lj_gc2_rootdesc_finish(&litmus.descriptor,
                                  &litmus.publisher.ticket) ==
           LJ_GC2_ROOTDESC_OK);
    assert(lj_gc2_activation_try_gate(&litmus.activation,
             &litmus.closer.closing, LJ_GC2_ROOT_GATE_OPEN, &reopened) ==
           LJ_GC2_TRANSITION_OK);
    assert(reopened.gate == LJ_GC2_ROOT_GATE_OPEN);
  }

  assert(pthread_join(publisher_thread, NULL) == 0);
  assert(pthread_join(closer_thread, NULL) == 0);

  if (forbidden != 0) {
    fprintf(stderr,
      "t-gc2-root-gate-ordering FAIL: publisher saw OPEN after ACTIVE "
      "while closer saw IDLE after CLOSING in %" PRIu64
      " rounds (first round %" PRIu64 ")\n",
      forbidden, first_forbidden);
    return 1;
  }

  printf("t-gc2-root-gate-ordering OK: rounds=%" PRIu64
         " OPEN+ACTIVE=%" PRIu64 " CLOSING+IDLE=%" PRIu64
         " CLOSING+ACTIVE=%" PRIu64 " OPEN+IDLE=0\n",
         litmus.rounds, open_active, closing_idle, closing_active);
  return 0;
}
