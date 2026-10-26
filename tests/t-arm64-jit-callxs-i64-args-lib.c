/* Foreign side of the fixed I64/U64 CALLXS argument lifecycle fixture. */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

typedef int64_t lj_test_i64_alias;
typedef uint64_t lj_test_u64_alias;

static int32_t configured_mode;
static int32_t signed_calls, unsigned_calls, alias_calls, const_calls;
static int32_t source_calls, consumer_calls, consumer2_calls;
static int32_t callm_calls;
static int32_t vararg_calls;
static int32_t total_calls;
static int32_t first_errno, last_errno;

static const uint64_t signed_patterns[][9] = {
  { UINT64_C(0x8000000000000000), UINT64_C(0x7fffffffffffffff),
    UINT64_C(0xffffffffffffffff), UINT64_C(0x0123456789abcdef),
    UINT64_C(0xfedcba9876543210), UINT64_C(0x8000000000000001),
    UINT64_C(0x000000007fffffff), UINT64_C(0x0000000080000000),
    UINT64_C(0x0000000100000000) },
  { UINT64_C(0xaaaaaaaaaaaaaaaa), UINT64_C(0x5555555555555555),
    UINT64_C(0xffffffff80000000), UINT64_C(0xffffffff7fffffff),
    UINT64_C(0x00000000ffffffff), UINT64_C(0x0000000100000001),
    UINT64_C(0x8000000000000000), UINT64_C(0x7fffffffffffffff),
    UINT64_C(0x0123456789abcdef) }
};

static const uint64_t unsigned_patterns[][9] = {
  { UINT64_C(0x0000000000000000), UINT64_C(0xffffffffffffffff),
    UINT64_C(0x8000000000000000), UINT64_C(0x0123456789abcdef),
    UINT64_C(0xfedcba9876543210), UINT64_C(0x0000000100000001),
    UINT64_C(0x000000007fffffff), UINT64_C(0x0000000080000000),
    UINT64_C(0x00000000ffffffff) },
  { UINT64_C(0xaaaaaaaaaaaaaaaa), UINT64_C(0x5555555555555555),
    UINT64_C(0x0000000100000000), UINT64_C(0x00000000ffffffff),
    UINT64_C(0x8000000000000001), UINT64_C(0x7fffffffffffffff),
    UINT64_C(0xffffffffffffffff), UINT64_C(0x0000000000000001),
    UINT64_C(0xfedcba9876543210) }
};

static int32_t load_i32(const int32_t *p)
{ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

static void store_i32(int32_t *p, int32_t v)
{ __atomic_store_n(p, v, __ATOMIC_RELEASE); }

void lj_callxs_i64_args_configure(int32_t mode)
{
  store_i32(&configured_mode, mode & 1);
  store_i32(&signed_calls, 0);
  store_i32(&unsigned_calls, 0);
  store_i32(&alias_calls, 0);
  store_i32(&const_calls, 0);
  store_i32(&source_calls, 0);
  store_i32(&consumer_calls, 0);
  store_i32(&consumer2_calls, 0);
  store_i32(&callm_calls, 0);
  store_i32(&vararg_calls, 0);
  store_i32(&total_calls, 0);
  store_i32(&first_errno, 0);
  store_i32(&last_errno, 0);
}

int32_t lj_callxs_i64_args_signed_count(void) { return load_i32(&signed_calls); }
int32_t lj_callxs_i64_args_unsigned_count(void) { return load_i32(&unsigned_calls); }
int32_t lj_callxs_i64_args_alias_count(void) { return load_i32(&alias_calls); }
int32_t lj_callxs_i64_args_const_count(void) { return load_i32(&const_calls); }
int32_t lj_callxs_i64_args_source_count(void) { return load_i32(&source_calls); }
int32_t lj_callxs_i64_args_consumer_count(void) { return load_i32(&consumer_calls); }
int32_t lj_callxs_i64_args_consumer2_count(void) { return load_i32(&consumer2_calls); }
int32_t lj_callxs_i64_args_callm_count(void) { return load_i32(&callm_calls); }
int32_t lj_callxs_i64_args_vararg_count(void) { return load_i32(&vararg_calls); }
int32_t lj_callxs_i64_args_first_errno(void) { return load_i32(&first_errno); }
int32_t lj_callxs_i64_args_last_errno(void) { return load_i32(&last_errno); }

static int32_t check_values(const uint64_t *got, const uint64_t *want)
{
  unsigned i;
  for (i = 0; i < 9; i++)
    if (got[i] != want[i]) return -1;
  return 0;
}

static void record_call(int32_t *counter)
{
  int32_t incoming = errno;
  (void)__atomic_add_fetch(counter, 1, __ATOMIC_ACQ_REL);
  if (__atomic_add_fetch(&total_calls, 1, __ATOMIC_ACQ_REL) == 1)
    store_i32(&first_errno, incoming);
  store_i32(&last_errno, incoming);
  errno = EDOM;
}

int32_t lj_callxs_i64_args_fixed(int64_t x0, int64_t x1, int64_t x2,
                                  int64_t x3, int64_t x4, int64_t x5,
                                  int64_t x6, int64_t x7, int64_t x8)
{
  const uint64_t got[9] = { (uint64_t)x0, (uint64_t)x1, (uint64_t)x2,
    (uint64_t)x3, (uint64_t)x4, (uint64_t)x5, (uint64_t)x6,
    (uint64_t)x7, (uint64_t)x8 };
  int32_t mode = load_i32(&configured_mode);
  int32_t ok = check_values(got, signed_patterns[mode]);
  record_call(&signed_calls);
  return (uint32_t)(ok == 0);
}

int32_t lj_callxs_u64_args_fixed(uint64_t x0, uint64_t x1, uint64_t x2,
                                  uint64_t x3, uint64_t x4, uint64_t x5,
                                  uint64_t x6, uint64_t x7, uint64_t x8)
{
  const uint64_t got[9] = { x0, x1, x2, x3, x4, x5, x6, x7, x8 };
  int32_t mode = load_i32(&configured_mode);
  int32_t ok = check_values(got, unsigned_patterns[mode]);
  record_call(&unsigned_calls);
  return (uint32_t)(ok == 0);
}

int32_t lj_callxs_i64_args_alias(lj_test_i64_alias x0,
    lj_test_i64_alias x1, lj_test_i64_alias x2, lj_test_i64_alias x3,
    lj_test_i64_alias x4, lj_test_i64_alias x5, lj_test_i64_alias x6,
    lj_test_i64_alias x7, lj_test_i64_alias x8)
{
  const uint64_t got[9] = { (uint64_t)x0, (uint64_t)x1, (uint64_t)x2,
    (uint64_t)x3, (uint64_t)x4, (uint64_t)x5, (uint64_t)x6,
    (uint64_t)x7, (uint64_t)x8 };
  int32_t mode = load_i32(&configured_mode);
  int32_t ok = check_values(got, signed_patterns[mode]);
  record_call(&alias_calls);
  return (uint32_t)(ok == 0);
}

int32_t lj_callxs_i64_args_const(const int64_t x0, const int64_t x1,
    const int64_t x2, const int64_t x3, const int64_t x4, const int64_t x5,
    const int64_t x6, const int64_t x7, const int64_t x8)
{
  const uint64_t got[9] = { (uint64_t)x0, (uint64_t)x1, (uint64_t)x2,
    (uint64_t)x3, (uint64_t)x4, (uint64_t)x5, (uint64_t)x6,
    (uint64_t)x7, (uint64_t)x8 };
  int32_t mode = load_i32(&configured_mode);
  int32_t ok = check_values(got, signed_patterns[mode]);
  record_call(&const_calls);
  return (uint32_t)(ok == 0);
}

static uint64_t source_bits(int32_t selector)
{
  return UINT64_C(0x8000000000000000) |
    (uint64_t)(uint32_t)selector;
}

int64_t lj_callxs_i64_args_source(int32_t selector)
{
  uint64_t bits = source_bits(selector);
  int64_t result;
  record_call(&source_calls);
  memcpy(&result, &bits, sizeof(result));
  return result;
}

int32_t lj_callxs_i64_args_consume(int64_t x0, int64_t x1, int64_t x2,
    int64_t x3, int64_t x4, int64_t x5, int64_t x6, int64_t x7, int64_t x8)
{
  uint64_t got[9] = { (uint64_t)x0, (uint64_t)x1, (uint64_t)x2,
    (uint64_t)x3, (uint64_t)x4, (uint64_t)x5, (uint64_t)x6,
    (uint64_t)x7, (uint64_t)x8 };
  uint64_t want[9];
  int32_t mode = load_i32(&configured_mode);
  unsigned i;
  /* x0 is the owned CData result of the preceding ordinary CALLXS. */
  want[0] = source_bits(mode + 1);
  for (i = 1; i < 9; i++) want[i] = signed_patterns[mode][i-1u];
  record_call(&consumer_calls);
  return (uint32_t)(check_values(got, want) == 0);
}

int32_t lj_callxs_i64_args_consume2(int64_t x0, int64_t x1, int64_t x2,
    int64_t x3, int64_t x4, int64_t x5, int64_t x6, int64_t x7, int64_t x8)
{
  const uint64_t got[9] = { (uint64_t)x0, (uint64_t)x1, (uint64_t)x2,
    (uint64_t)x3, (uint64_t)x4, (uint64_t)x5, (uint64_t)x6,
    (uint64_t)x7, (uint64_t)x8 };
  uint64_t want[9];
  int32_t mode = load_i32(&configured_mode);
  unsigned i;
  want[0] = source_bits(1);
  want[1] = source_bits(2);
  for (i = 2; i < 9; i++) want[i] = signed_patterns[mode][i-2u];
  record_call(&consumer2_calls);
  return (uint32_t)(check_values(got, want) == 0);
}

int32_t lj_callxs_i64_args_callm_consumer(int32_t fixed, ...)
{
  va_list ap;
  int64_t value;
  int32_t ok;
  va_start(ap, fixed);
  value = va_arg(ap, int64_t);
  va_end(ap);
  ok = fixed == 17 && (uint64_t)value == source_bits(1);
  record_call(&callm_calls);
  return (uint32_t)ok;
}

int32_t lj_callxs_i64_args_vararg(int64_t fixed, ...)
{
  va_list ap;
  double number;
  int truth;
  const char *text;
  int32_t mode = load_i32(&configured_mode);
  int32_t ok;
  va_start(ap, fixed);
  number = va_arg(ap, double);
  truth = va_arg(ap, int);
  text = va_arg(ap, const char *);
  va_end(ap);
  ok = (uint64_t)fixed == signed_patterns[mode][0] &&
    number >= 0.5 && number < 1000.5 && truth == 1 && text != NULL &&
    strcmp(text, "i64-args-vararg") == 0;
  record_call(&vararg_calls);
  return (uint32_t)ok;
}
