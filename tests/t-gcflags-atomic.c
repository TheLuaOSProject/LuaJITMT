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

typedef struct ResurrectArg {
  global_State *g;
  GCobj *object;
  uint32_t *ready;
  uint32_t *go;
  uint32_t claimed;
} ResurrectArg;

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

static void *resurrect_writer(void *ud)
{
  ResurrectArg *arg = (ResurrectArg *)ud;
  (void)la_add32_acqrel(arg->ready, 1);
  while (!la_load32_acq(arg->go))
    la_cpu_pause();
  arg->claimed = (uint32_t)lj_gc_resurrect_if_dead(arg->g, arg->object);
  return NULL;
}

static void test_conditional_resurrection(void)
{
  global_State g;
  GCobj object;
  ResurrectArg arg[2];
  pthread_t thread[2];
  uint32_t ready = 0, go = 0;
  unsigned i;

  memset(&g, 0, sizeof(g));
  memset(&object, 0, sizeof(object));
  memset(arg, 0, sizeof(arg));
  g.gc.currentwhite = LJ_GC_WHITE0;
  lj_obj_setgcflags(&object, LJ_GC_WHITE1 | LJ_GC_FINALIZED);
  for (i = 0; i < 2; i++) {
    arg[i].g = &g;
    arg[i].object = &object;
    arg[i].ready = &ready;
    arg[i].go = &go;
    assert(pthread_create(&thread[i], NULL, resurrect_writer, &arg[i]) == 0);
  }
  while (la_load32_acq(&ready) != 2)
    la_cpu_pause();
  la_store32_rel(&go, 1);
  for (i = 0; i < 2; i++)
    assert(pthread_join(thread[i], NULL) == 0);
  assert(arg[0].claimed + arg[1].claimed == 1);
  assert((lj_obj_gcflags(&object) & LJ_GC_WHITES) == LJ_GC_WHITE0);
  assert(lj_obj_gcflags(&object) & LJ_GC_FINALIZED);
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

  test_conditional_resurrection();

  printf("t-gcflags-atomic OK: %u synchronized flag updates\n",
         FLAG_ROUNDS * FLAG_THREADS);
  return 0;
}
