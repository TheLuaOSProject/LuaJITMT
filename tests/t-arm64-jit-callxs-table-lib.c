/* Foreign helpers for native CALLXS through a shared Lua table slot. */

#include <errno.h>
#include <stdint.h>

static int32_t first_calls;
static int32_t second_calls;

static void bump(int32_t *p)
{
  (void)__atomic_add_fetch(p, 1, __ATOMIC_ACQ_REL);
  errno = EDOM;
}

void lj_callxs_table_configure(void)
{
  __atomic_store_n(&first_calls, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&second_calls, 0, __ATOMIC_RELEASE);
}

int32_t lj_callxs_table_first_count(void)
{
  return __atomic_load_n(&first_calls, __ATOMIC_ACQUIRE);
}

int32_t lj_callxs_table_second_count(void)
{
  return __atomic_load_n(&second_calls, __ATOMIC_ACQUIRE);
}

int32_t lj_callxs_table_first(int32_t value)
{
  bump(&first_calls);
  return value + 17;
}

int32_t lj_callxs_table_second(int32_t value)
{
  bump(&second_calls);
  return value * 3 + 101;
}
