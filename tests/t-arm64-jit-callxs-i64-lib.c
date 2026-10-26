/* Foreign helpers for the boxed fixed-width CALLXS result lifecycle test. */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

typedef int64_t lj_test_i64_alias;
typedef uint64_t lj_test_u64_alias;

static int32_t i64_calls;
static int32_t u64_calls;
static int32_t i64_mode;
static int32_t u64_mode;
static int32_t i64_first_errno;
static int32_t i64_last_errno;
static int32_t u64_first_errno;
static int32_t u64_last_errno;
static int32_t i64_va_calls;
static int32_t u64_va_calls;
static int32_t i64_callm_calls;
static int32_t i64_producer_calls;

static int32_t load_i32(const int32_t *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_i32(int32_t *p, int32_t v)
{
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static const uint64_t i64_patterns[] = {
  UINT64_C(0x8000000000000000), UINT64_C(0x7fffffffffffffff),
  UINT64_C(0xffffffffffffffff), UINT64_C(0x0123456789abcdef),
  UINT64_C(0xfedcba9876543210), UINT64_C(0x8000000000000001)
};

static const uint64_t u64_patterns[] = {
  UINT64_C(0x0000000000000000), UINT64_C(0xffffffffffffffff),
  UINT64_C(0x8000000000000000), UINT64_C(0x0123456789abcdef),
  UINT64_C(0xfedcba9876543210), UINT64_C(0x0000000100000001)
};

void lj_callxs_arm64_i64_configure(int32_t mode)
{
  store_i32(&i64_calls, 0);
  store_i32(&i64_first_errno, 0);
  store_i32(&i64_last_errno, 0);
  store_i32(&i64_mode, mode);
  store_i32(&i64_va_calls, 0);
  store_i32(&i64_callm_calls, 0);
  store_i32(&i64_producer_calls, 0);
}

void lj_callxs_arm64_u64_configure(int32_t mode)
{
  store_i32(&u64_calls, 0);
  store_i32(&u64_first_errno, 0);
  store_i32(&u64_last_errno, 0);
  store_i32(&u64_mode, mode);
  store_i32(&u64_va_calls, 0);
}

int32_t lj_callxs_arm64_i64_count(void) { return load_i32(&i64_calls); }
int32_t lj_callxs_arm64_u64_count(void) { return load_i32(&u64_calls); }
int32_t lj_callxs_arm64_i64_first_errno(void)
{ return load_i32(&i64_first_errno); }
int32_t lj_callxs_arm64_i64_last_errno(void)
{ return load_i32(&i64_last_errno); }
int32_t lj_callxs_arm64_u64_first_errno(void)
{ return load_i32(&u64_first_errno); }
int32_t lj_callxs_arm64_u64_last_errno(void)
{ return load_i32(&u64_last_errno); }
int32_t lj_callxs_arm64_i64_va_count(void) { return load_i32(&i64_va_calls); }
int32_t lj_callxs_arm64_u64_va_count(void) { return load_i32(&u64_va_calls); }
int32_t lj_callxs_arm64_i64_callm_count(void)
{ return load_i32(&i64_callm_calls); }
int32_t lj_callxs_arm64_i64_producer_count(void)
{ return load_i32(&i64_producer_calls); }

static uint64_t record_result(int is_unsigned, int32_t selector)
{
  int32_t mode, callno, *calls, *first_errno, *last_errno;
  const uint64_t *patterns;
  unsigned count;
  int32_t incoming_errno = errno;
  if (is_unsigned) {
    calls = &u64_calls;
    first_errno = &u64_first_errno;
    last_errno = &u64_last_errno;
    mode = load_i32(&u64_mode);
    patterns = u64_patterns;
    count = sizeof(u64_patterns) / sizeof(u64_patterns[0]);
  } else {
    calls = &i64_calls;
    first_errno = &i64_first_errno;
    last_errno = &i64_last_errno;
    mode = load_i32(&i64_mode);
    patterns = i64_patterns;
    count = sizeof(i64_patterns) / sizeof(i64_patterns[0]);
  }
  callno = __atomic_add_fetch(calls, 1, __ATOMIC_ACQ_REL);
  if (callno == 1)
    store_i32(first_errno, incoming_errno);
  store_i32(last_errno, incoming_errno);
  errno = EDOM;
  /* Keep the fixed argument live in the FFI call while choosing a stable
  ** result bit pattern for every iteration of the generated loop. */
  if (selector < 0)
    return 0;
  return patterns[(unsigned)mode % count];
}

int64_t lj_callxs_arm64_i64(int32_t selector)
{
  uint64_t bits = record_result(0, selector);
  int64_t value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

uint64_t lj_callxs_arm64_u64(int32_t selector)
{
  return record_result(1, selector);
}

lj_test_i64_alias lj_callxs_arm64_i64_alias(int32_t selector)
{
  return lj_callxs_arm64_i64(selector);
}

int64_t lj_callxs_arm64_i64_const(int32_t selector)
{
  return lj_callxs_arm64_i64(selector);
}

int64_t lj_callxs_arm64_i64_vararg(int32_t fixed, ...)
{
  va_list ap;
  double number;
  int truth;
  const char *text;
  uint64_t bits = record_result(0, fixed);
  int64_t result;
  va_start(ap, fixed);
  number = va_arg(ap, double);
  truth = va_arg(ap, int);
  text = va_arg(ap, const char *);
  va_end(ap);
  (void)__atomic_add_fetch(&i64_va_calls, 1, __ATOMIC_ACQ_REL);
  if (number != (double)fixed + 0.5 || truth != 1 || text == NULL ||
      strcmp(text, "i64-vararg") != 0)
    return INT64_C(-1);
  memcpy(&result, &bits, sizeof(result));
  return result;
}

uint64_t lj_callxs_arm64_u64_vararg(int32_t fixed, ...)
{
  va_list ap;
  double number;
  int truth;
  const char *text;
  uint64_t bits;
  va_start(ap, fixed);
  number = va_arg(ap, double);
  truth = va_arg(ap, int);
  text = va_arg(ap, const char *);
  va_end(ap);
  bits = record_result(1, fixed);
  (void)__atomic_add_fetch(&u64_va_calls, 1, __ATOMIC_ACQ_REL);
  if (number != (double)fixed + 0.5 || truth != 1 || text == NULL ||
      strcmp(text, "u64-vararg") != 0)
    return UINT64_C(0);
  return bits;
}

double lj_callxs_arm64_i64_callm_producer(int32_t value)
{
  (void)__atomic_add_fetch(&i64_producer_calls, 1, __ATOMIC_ACQ_REL);
  return (double)value + 0.5;
}

int64_t lj_callxs_arm64_i64_callm(int32_t fixed, ...)
{
  va_list ap;
  double value;
  uint64_t bits;
  int64_t result;
  va_start(ap, fixed);
  value = va_arg(ap, double);
  va_end(ap);
  (void)__atomic_add_fetch(&i64_callm_calls, 1, __ATOMIC_ACQ_REL);
  if (fixed != 17 || value < 0.5 || value >= 1000.5)
    return INT64_C(-1);
  bits = record_result(0, (int32_t)value);
  memcpy(&result, &bits, sizeof(result));
  return result;
}
