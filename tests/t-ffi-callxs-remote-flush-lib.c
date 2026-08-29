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
static int32_t lj_callxs_flush_aggregate_calls;

/* Larger than two SysV eightbytes and not scalar-sized on Win64, so this
** result is returned through the ABI's hidden destination pointer on every
** supported x64 target. Keep every field independently checkable from Lua.
*/
struct lj_callxs_flush_aggregate {
  uint32_t magic_hi;
  uint32_t magic_lo;
  double weight;
  int32_t code;
  uint32_t stamp;
};

typedef char lj_callxs_flush_aggregate_size[
  sizeof(struct lj_callxs_flush_aggregate) == 24 ? 1 : -1];

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
  lj_callxs_flush_store(&lj_callxs_flush_aggregate_calls, 0);
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

int32_t lj_callxs_flush_aggregate_call_count(void)
{
  return lj_callxs_flush_load(&lj_callxs_flush_aggregate_calls);
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

int32_t lj_callxs_flush_identity_maybe_block(int32_t value)
{
  lj_callxs_flush_wait(value);
  return value;
}

int32_t *lj_callxs_flush_ptr_maybe_block(int32_t *ptr, int32_t value)
{
  lj_callxs_flush_wait(value);
  return ptr;
}

_Bool lj_callxs_flush_bool_maybe_block(int32_t value, int32_t truth)
{
  lj_callxs_flush_wait(value);
  return truth != 0;
}

struct lj_callxs_flush_aggregate
lj_callxs_flush_aggregate_maybe_block(double bias, int32_t value)
{
  struct lj_callxs_flush_aggregate result;
  (void)__atomic_add_fetch(&lj_callxs_flush_aggregate_calls, 1,
                           __ATOMIC_ACQ_REL);
  lj_callxs_flush_wait(value);
  result.magic_hi = UINT32_C(0xfedc0000) + (uint32_t)value;
  result.magic_lo = UINT32_C(0x76540000) + (uint32_t)value;
  result.weight = bias + (double)value * 0.25;
  result.code = value * 3 - 17;
  result.stamp = UINT32_C(0x13570000) + (uint32_t)value;
  return result;
}
