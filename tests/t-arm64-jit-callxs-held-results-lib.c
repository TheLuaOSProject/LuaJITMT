/* Foreign helpers for scalar CALLXS results retained across a later call. */

#include <errno.h>
#include <stdint.h>

static int32_t held_mode;
static int32_t held_producer_calls;
static int32_t held_consumer_calls;
static int32_t held_total_calls;
static int32_t held_first_errno;
static int32_t held_last_errno;

static int32_t load_i32(const int32_t *p)
{ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

static void store_i32(int32_t *p, int32_t v)
{ __atomic_store_n(p, v, __ATOMIC_RELEASE); }

void lj_callxs_held_configure(int32_t mode)
{
  store_i32(&held_mode, mode);
  store_i32(&held_producer_calls, 0);
  store_i32(&held_consumer_calls, 0);
  store_i32(&held_total_calls, 0);
  store_i32(&held_first_errno, 0);
  store_i32(&held_last_errno, 0);
}

int32_t lj_callxs_held_producer_count(void)
{ return load_i32(&held_producer_calls); }
int32_t lj_callxs_held_consumer_count(void)
{ return load_i32(&held_consumer_calls); }
int32_t lj_callxs_held_total_count(void)
{ return load_i32(&held_total_calls); }
int32_t lj_callxs_held_first_errno(void)
{ return load_i32(&held_first_errno); }
int32_t lj_callxs_held_last_errno(void)
{ return load_i32(&held_last_errno); }

static void record_call(int32_t *counter)
{
  int32_t incoming = errno;
  (void)__atomic_add_fetch(counter, 1, __ATOMIC_ACQ_REL);
  if (__atomic_add_fetch(&held_total_calls, 1, __ATOMIC_ACQ_REL) == 1)
    store_i32(&held_first_errno, incoming);
  store_i32(&held_last_errno, incoming);
  errno = EDOM;
}

int32_t lj_callxs_held_i32(int32_t selector)
{
  record_call(&held_producer_calls);
  return selector - 17;
}

int32_t lj_callxs_held_i32_consume(int32_t value)
{
  record_call(&held_consumer_calls);
  return value * 3 + 1;
}

double lj_callxs_held_num(int32_t selector)
{
  record_call(&held_producer_calls);
  return (double)selector + 0.125;
}

double lj_callxs_held_num_consume(double value)
{
  record_call(&held_consumer_calls);
  return value * 2.0 + 0.25;
}

float lj_callxs_held_float(int32_t selector)
{
  record_call(&held_producer_calls);
  return (float)selector + 0.375f;
}

float lj_callxs_held_float_consume(float value)
{
  record_call(&held_consumer_calls);
  return value * 0.5f + 0.25f;
}

static const uint32_t held_u32_values[] = {
  UINT32_C(0), UINT32_C(1), UINT32_C(0x7fffffff), UINT32_C(0x80000000),
  UINT32_C(0xffffffff), UINT32_C(0xaaaaaaaa), UINT32_C(0x55555555),
  UINT32_C(0xdeadbeef)
};

uint32_t lj_callxs_held_u32(int32_t selector)
{
  uint32_t mode = (uint32_t)load_i32(&held_mode);
  record_call(&held_producer_calls);
  return held_u32_values[(mode + (uint32_t)selector) & 7u];
}

double lj_callxs_held_u32_consume(double value)
{
  record_call(&held_consumer_calls);
  return value + 0.5;
}
