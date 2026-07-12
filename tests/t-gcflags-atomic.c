/*
** Concurrent GC header flag read/modify/write regression.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_gc.h"

enum { FLAG_THREADS = 8, FLAG_ROUNDS = 20000 };

typedef struct FlagStress {
  GCobj object;
  uint32_t ready;
  uint32_t start;
  uint32_t done;
} FlagStress;

typedef struct FlagArg {
  FlagStress *stress;
  unsigned index;
} FlagArg;

static void *flag_writer(void *ud)
{
  FlagArg *arg = (FlagArg *)ud;
  FlagStress *stress = arg->stress;
  uint8_t bit = (uint8_t)(1u << arg->index);
  unsigned round;

  (void)la_add32_acqrel(&stress->ready, 1);
  for (round = 1; round <= FLAG_ROUNDS; round++) {
    while (la_load32_acq(&stress->start) < round)
      la_cpu_pause();
    switch (round & 3u) {
    case 0:
      lj_obj_addgcflags(&stress->object, bit);
      break;
    case 1:
      lj_obj_cleargcflags(&stress->object, bit);
      break;
    case 2:
      lj_obj_xorgcflags(&stress->object, bit);
      break;
    default:
      lj_obj_masksetgcflags(&stress->object, bit, bit);
      break;
    }
    (void)la_add32_acqrel(&stress->done, 1);
  }
  return NULL;
}

int main(void)
{
  FlagStress stress;
  FlagArg arg[FLAG_THREADS];
  pthread_t thread[FLAG_THREADS];
  GCcdata cd;
  unsigned i, round;

  memset(&stress, 0, sizeof(stress));
  for (i = 0; i < FLAG_THREADS; i++) {
    arg[i].stress = &stress;
    arg[i].index = i;
    assert(pthread_create(&thread[i], NULL, flag_writer, &arg[i]) == 0);
  }
  while (la_load32_acq(&stress.ready) != FLAG_THREADS)
    la_cpu_pause();

  for (round = 1; round <= FLAG_ROUNDS; round++) {
    uint8_t initial = (round & 3u) == 1u ? UINT8_MAX : 0;
    uint8_t expected = (round & 3u) == 1u ? 0 : UINT8_MAX;
    while (la_load32_acq(&stress.done) != (round - 1u) * FLAG_THREADS)
      la_cpu_pause();
    lj_obj_setgcflags(&stress.object, initial);
    la_store32_rel(&stress.start, round);
    while (la_load32_acq(&stress.done) != round * FLAG_THREADS)
      la_cpu_pause();
    assert(lj_obj_gcflags(&stress.object) == expected);
  }

  for (i = 0; i < FLAG_THREADS; i++)
    assert(pthread_join(thread[i], NULL) == 0);

  memset(&cd, 0, sizeof(cd));
  assert(!cdataisv(&cd));
  la_store8_rel(&cd.marked, 0x80u);
  assert(cdataisv(&cd));

  printf("t-gcflags-atomic OK: %u synchronized flag updates\n",
         FLAG_ROUNDS * FLAG_THREADS);
  return 0;
}
