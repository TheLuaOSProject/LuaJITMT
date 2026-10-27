/* Gate a Lua FFI callback until the rooted-read hook observes native lookup. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

typedef int32_t (*LJTableChainCallback)(int32_t);

static LJTableChainCallback callback_fn;
static uint32_t armed;
static int32_t gate_calls;
static int32_t callback_calls;

void lj_table_chains_callback_configure(void)
{
  __atomic_store_n(&armed, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&gate_calls, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&callback_calls, 0, __ATOMIC_RELEASE);
}

void lj_table_chains_callback_set(LJTableChainCallback fn)
{
  callback_fn = fn;
}

void lj_table_chains_callback_arm(void)
{
  __atomic_store_n(&armed, 1, __ATOMIC_RELEASE);
}

int32_t lj_table_chains_callback_gate_count(void)
{
  return __atomic_load_n(&gate_calls, __ATOMIC_ACQUIRE);
}

int32_t lj_table_chains_callback_count(void)
{
  return __atomic_load_n(&callback_calls, __ATOMIC_ACQUIRE);
}

int32_t lj_table_chains_callback_gate(int32_t value)
{
  int32_t result = 0;
  (void)__atomic_add_fetch(&gate_calls, 1, __ATOMIC_ACQ_REL);
  if (value == 2 && __atomic_exchange_n(&armed, 0, __ATOMIC_ACQ_REL) != 0 &&
      callback_fn != NULL) {
    (void)__atomic_add_fetch(&callback_calls, 1, __ATOMIC_ACQ_REL);
    errno = EAGAIN;
    result = callback_fn(value);
  }
  errno = EDOM;
  return result;
}
