/*
** t-arm64-tab-pair-contract.c - Apple AArch64 GCtab pair contract.
**
** Structural-control and weak-record writers update disjoint logical halves,
** but descriptor completion changes the complete pair. This fixture proves
** the ARM64 path preserves both halves using only exact 128-bit authority.
*/

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_obj.h"

#if !defined(__APPLE__) || !defined(__aarch64__)
#error "t-arm64-tab-pair-contract requires native Apple AArch64"
#endif

#define TAB_PAIR_ITERS 200000u
#define TAB_PAIR_ACAP 37u
#define TAB_PAIR_WORKERS 3u

static GCtab tab_pair;
static uint32_t tab_pair_ready;
static uint32_t tab_pair_start;

/* Keep one independently inspectable artifact for every published pair
** helper. The CI gate requires CASPAL inside each symbol, so one surviving
** exact helper cannot mask a regressed overlapping half access elsewhere. */
#define TAB_PAIR_PROBE __attribute__((noinline, used))

TAB_PAIR_PROBE uint64_t tab_pair_probe_control_acq(const GCtab *t)
{
  return lj_tab_struct_control_acq(t);
}

TAB_PAIR_PROBE int tab_pair_probe_control_cas(GCtab *t, uint64_t old,
                                               uint64_t desired)
{
  return lj_tab_struct_control_cas(t, &old, desired);
}

TAB_PAIR_PROBE void tab_pair_probe_control_rel(GCtab *t, uint64_t control)
{
  lj_tab_struct_control_rel(t, control);
}

TAB_PAIR_PROBE uint64_t tab_pair_probe_weak_acq(const GCtab *t)
{
  return lj_tab_weak_record_raw_acq(t);
}

TAB_PAIR_PROBE void tab_pair_probe_weak_acap_rel(GCtab *t, MSize acap)
{
  lj_tab_weak_acap_rel(t, acap);
}

TAB_PAIR_PROBE void tab_pair_probe_weak_store(GCtab *t, uint64_t record)
{
  lj_tab_weak_record_store_rlx(t, record);
}

TAB_PAIR_PROBE int tab_pair_probe_weak_cas(GCtab *t, uint64_t old,
                                           uint64_t desired)
{
  return lj_tab_weak_record_cas(t, &old, desired);
}

TAB_PAIR_PROBE int tab_pair_probe_full_cas(GCtab *t, uint64_t control,
                                           uint64_t weak)
{
  return lj_tab_struct_weak_pair_cas(t, &control, &weak, control, weak);
}

static void tab_pair_wait_start(void)
{
  (void)la_add32_acqrel(&tab_pair_ready, 1u);
  while (la_load32_acq(&tab_pair_start) == 0)
    sched_yield();
}

static void *tab_pair_control_worker(void *arg)
{
  uint32_t completed = 0;
  UNUSED(arg);
  tab_pair_wait_start();
  while (completed < TAB_PAIR_ITERS) {
    uint64_t control = lj_tab_struct_control_acq(&tab_pair);
    uint32_t owner = lj_tab_struct_control_owner(control);
    uint64_t desired = lj_tab_struct_control_pack(
      lj_tab_struct_control_acap(control), owner + 1u);
    if (completed & 1u) {
      lj_tab_struct_control_rel(&tab_pair, desired);
      completed++;
    } else if (lj_tab_struct_control_cas(&tab_pair, &control, desired)) {
      completed++;
    }
  }
  return NULL;
}

static void *tab_pair_weak_worker(void *arg)
{
  uint32_t completed = 0;
  UNUSED(arg);
  tab_pair_wait_start();
  while (completed < TAB_PAIR_ITERS) {
    uint64_t record = lj_tab_weak_record_acq(&tab_pair);
    uint64_t desired = lj_tab_weak_record_pack(
      lj_tab_weak_record_cycle(record) + 1u,
      LJ_TAB_WEAK_RECORD_PUBLISHED);
    if (completed & 1u) {
      lj_tab_weak_record_store_rlx(&tab_pair, desired);
      completed++;
    } else if (lj_tab_weak_record_cas(&tab_pair, &record, desired)) {
      completed++;
    }
    lj_tab_weak_acap_rel(&tab_pair,
      (completed & 1u) ? TAB_PAIR_ACAP : TAB_PAIR_ACAP + 1u);
  }
  lj_tab_weak_acap_rel(&tab_pair, TAB_PAIR_ACAP);
  return NULL;
}

static void *tab_pair_full_worker(void *arg)
{
  uint32_t completed = 0;
  UNUSED(arg);
  tab_pair_wait_start();
  while (completed < TAB_PAIR_ITERS) {
    la_u128 observed = la_load128_acq(&tab_pair.struct_weak_pair);
    uint64_t control = observed.lo;
    uint64_t weak = observed.hi;
    if (lj_tab_struct_weak_pair_cas(&tab_pair, &control, &weak,
                                    observed.lo, observed.hi))
      completed++;
  }
  return NULL;
}

int main(void)
{
  pthread_t control_thread, weak_thread, full_thread;
  uint64_t control, record, raw;
  la_u128 observed;

  memset(&tab_pair, 0, sizeof(tab_pair));
  lj_tab_struct_control_store_rlx(&tab_pair, TAB_PAIR_ACAP, 0);
  lj_tab_weak_record_init_rlx(&tab_pair, TAB_PAIR_ACAP,
    lj_tab_weak_record_pack(0, LJ_TAB_WEAK_RECORD_NONE));

  assert(pthread_create(&control_thread, NULL,
                        tab_pair_control_worker, NULL) == 0);
  assert(pthread_create(&weak_thread, NULL,
                        tab_pair_weak_worker, NULL) == 0);
  assert(pthread_create(&full_thread, NULL,
                        tab_pair_full_worker, NULL) == 0);
  while (la_load32_acq(&tab_pair_ready) != TAB_PAIR_WORKERS)
    sched_yield();
  la_store32_rel(&tab_pair_start, 1u);
  assert(pthread_join(control_thread, NULL) == 0);
  assert(pthread_join(weak_thread, NULL) == 0);
  assert(pthread_join(full_thread, NULL) == 0);

  control = lj_tab_struct_control_acq(&tab_pair);
  record = lj_tab_weak_record_acq(&tab_pair);
  raw = lj_tab_weak_record_raw_acq(&tab_pair);
  assert(lj_tab_struct_control_acap(control) == TAB_PAIR_ACAP);
  assert(lj_tab_struct_control_owner(control) == TAB_PAIR_ITERS);
  assert(lj_tab_weak_record_raw_acap(raw) == TAB_PAIR_ACAP);
  assert(lj_tab_weak_record_cycle(record) == TAB_PAIR_ITERS);
  assert(lj_tab_weak_record_state(record) == LJ_TAB_WEAK_RECORD_PUBLISHED);

  observed = la_load128_acq(&tab_pair.struct_weak_pair);
  assert(observed.lo == control && observed.hi == raw);
  puts("t-arm64-tab-pair-contract OK: all pair writers preserved exact halves");
  return 0;
}
