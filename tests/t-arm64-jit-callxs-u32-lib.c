/* Foreign helpers for the ARM64 uint32_t-to-Lua-number CALLXS tests. */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t lj_test_u32_alias;

static int32_t u32_mode;
static int32_t u32_calls;
static int32_t u32_alt_calls;
static int32_t u32_vararg_calls;
static int32_t u32_total_calls;
static int32_t u32_first_errno;
static int32_t u32_last_errno;

static const uint32_t u32_values[] = {
  UINT32_C(0), UINT32_C(1), UINT32_C(0x7fffffff), UINT32_C(0x80000000),
  UINT32_C(0xffffffff), UINT32_C(0xaaaaaaaa), UINT32_C(0x55555555),
  UINT32_C(0xdeadbeef), UINT32_C(0x01234567), UINT32_C(0xfedcba98)
};

static int32_t load_i32(const int32_t *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_i32(int32_t *p, int32_t v)
{
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

void lj_callxs_arm64_u32_configure(int32_t mode)
{
  store_i32(&u32_mode, mode);
  store_i32(&u32_calls, 0);
  store_i32(&u32_alt_calls, 0);
  store_i32(&u32_vararg_calls, 0);
  store_i32(&u32_total_calls, 0);
  store_i32(&u32_first_errno, 0);
  store_i32(&u32_last_errno, 0);
}

int32_t lj_callxs_arm64_u32_count(void) { return load_i32(&u32_calls); }
int32_t lj_callxs_arm64_u32_alt_count(void) { return load_i32(&u32_alt_calls); }
int32_t lj_callxs_arm64_u32_vararg_count(void)
{ return load_i32(&u32_vararg_calls); }
int32_t lj_callxs_arm64_u32_total_count(void)
{ return load_i32(&u32_total_calls); }
int32_t lj_callxs_arm64_u32_first_errno(void)
{ return load_i32(&u32_first_errno); }
int32_t lj_callxs_arm64_u32_last_errno(void)
{ return load_i32(&u32_last_errno); }

static void record_call(int32_t *counter)
{
  int32_t incoming = errno;
  (void)__atomic_add_fetch(counter, 1, __ATOMIC_ACQ_REL);
  if (__atomic_add_fetch(&u32_total_calls, 1, __ATOMIC_ACQ_REL) == 1)
    store_i32(&u32_first_errno, incoming);
  store_i32(&u32_last_errno, incoming);
  errno = EDOM;
}

uint32_t lj_callxs_arm64_u32(int32_t selector)
{
  int32_t mode = load_i32(&u32_mode);
  record_call(&u32_calls);
  return u32_values[((uint32_t)mode + (uint32_t)selector) %
                    (sizeof(u32_values) / sizeof(u32_values[0]))];
}

uint32_t lj_callxs_arm64_u32_threshold(int32_t selector)
{
  record_call(&u32_calls);
  return selector < 5 ? UINT32_C(1) : UINT32_MAX;
}

uint32_t lj_callxs_arm64_u32_alt(int32_t selector)
{
  /* A different permutation makes same-typed result-source swaps visible. */
  int32_t mode = load_i32(&u32_mode);
  record_call(&u32_alt_calls);
  return u32_values[((uint32_t)mode + (uint32_t)selector + 3u) %
                    (sizeof(u32_values) / sizeof(u32_values[0]))];
}

lj_test_u32_alias lj_callxs_arm64_u32_alias(int32_t selector)
{
  return lj_callxs_arm64_u32(selector);
}

uint32_t lj_callxs_arm64_u32_const(int32_t selector)
{
  return lj_callxs_arm64_u32(selector);
}

uint32_t lj_callxs_arm64_u32_vararg(int32_t selector, ...)
{
  va_list ap;
  double number;
  int truth;
  const char *text;
  int32_t mode = load_i32(&u32_mode);
  va_start(ap, selector);
  number = va_arg(ap, double);
  truth = va_arg(ap, int);
  text = va_arg(ap, const char *);
  va_end(ap);
  record_call(&u32_vararg_calls);
  if (number != (double)selector + 0.5 || truth != 1 || text == NULL ||
      strcmp(text, "u32-vararg") != 0)
    return UINT32_C(0);
  return u32_values[((uint32_t)mode + (uint32_t)selector) %
                    (sizeof(u32_values) / sizeof(u32_values[0]))];
}
