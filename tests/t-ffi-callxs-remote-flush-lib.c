/* Shared blocker for the authentic generated-CALLXS remote-flush test. */

#include <limits.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sched.h>
#endif

static int32_t lj_callxs_flush_gate_value = INT32_MIN;
static int32_t lj_callxs_flush_release = 1;
static int32_t lj_callxs_flush_entered;
static int32_t lj_callxs_flush_entries;
static int32_t lj_callxs_flush_effects;

static int32_t lj_callxs_flush_load(const int32_t *value)
{
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void lj_callxs_flush_store(int32_t *target, int32_t value)
{
  __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

static void lj_callxs_flush_yield(void)
{
#if defined(_WIN32)
  Sleep(0);
#else
  (void)sched_yield();
#endif
}

void lj_callxs_flush_configure(int32_t gate_value, int32_t block)
{
  lj_callxs_flush_store(&lj_callxs_flush_entered, 0);
  lj_callxs_flush_store(&lj_callxs_flush_entries, 0);
  lj_callxs_flush_store(&lj_callxs_flush_effects, 0);
  lj_callxs_flush_store(&lj_callxs_flush_gate_value, gate_value);
  lj_callxs_flush_store(&lj_callxs_flush_release, block ? 0 : 1);
}

void lj_callxs_flush_unblock(void)
{
  lj_callxs_flush_store(&lj_callxs_flush_release, 1);
}

int32_t lj_callxs_flush_entered_count(void)
{
  return lj_callxs_flush_load(&lj_callxs_flush_entered);
}

int32_t lj_callxs_flush_entry_count(void)
{
  return lj_callxs_flush_load(&lj_callxs_flush_entries);
}

int32_t lj_callxs_flush_effect_count(void)
{
  return lj_callxs_flush_load(&lj_callxs_flush_effects);
}

static void lj_callxs_flush_wait(int32_t value)
{
  if (value == lj_callxs_flush_load(&lj_callxs_flush_gate_value)) {
    (void)__atomic_add_fetch(&lj_callxs_flush_entries, 1,
                             __ATOMIC_ACQ_REL);
    lj_callxs_flush_store(&lj_callxs_flush_entered, 1);
    while (!lj_callxs_flush_load(&lj_callxs_flush_release))
      lj_callxs_flush_yield();
    (void)__atomic_add_fetch(&lj_callxs_flush_effects, 1,
                             __ATOMIC_ACQ_REL);
  }
}

int32_t lj_callxs_flush_maybe_block(int32_t value)
{
  lj_callxs_flush_wait(value);
  return value + 9;
}

int32_t *lj_callxs_flush_ptr_maybe_block(int32_t *ptr, int32_t value)
{
  lj_callxs_flush_wait(value);
  return ptr;
}
