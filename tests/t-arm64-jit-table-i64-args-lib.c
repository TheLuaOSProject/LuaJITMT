/* Foreign validators for table-loaded fixed I64/U64 CALLXS arguments. */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

typedef int64_t lj_table_i64_alias;

static int32_t mode;
static int32_t signed_calls, unsigned_calls, mixed_calls, alias_calls;
static int32_t const_calls, vararg_calls, pair_calls, total_calls;

static const uint64_t signed_values[2][9] = {
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

static const uint64_t unsigned_values[2][9] = {
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

static int32_t load_count(const int32_t *p)
{ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

static void store_count(int32_t *p, int32_t value)
{ __atomic_store_n(p, value, __ATOMIC_RELEASE); }

void lj_callxs_table_i64_configure(int32_t new_mode)
{
  mode = new_mode & 1;
  store_count(&signed_calls, 0);
  store_count(&unsigned_calls, 0);
  store_count(&mixed_calls, 0);
  store_count(&alias_calls, 0);
  store_count(&const_calls, 0);
  store_count(&vararg_calls, 0);
  store_count(&pair_calls, 0);
  store_count(&total_calls, 0);
}

int32_t lj_callxs_table_i64_signed_count(void) { return load_count(&signed_calls); }
int32_t lj_callxs_table_i64_unsigned_count(void) { return load_count(&unsigned_calls); }
int32_t lj_callxs_table_i64_mixed_count(void) { return load_count(&mixed_calls); }
int32_t lj_callxs_table_i64_alias_count(void) { return load_count(&alias_calls); }
int32_t lj_callxs_table_i64_const_count(void) { return load_count(&const_calls); }
int32_t lj_callxs_table_i64_vararg_count(void) { return load_count(&vararg_calls); }
int32_t lj_callxs_table_i64_pair_count(void) { return load_count(&pair_calls); }
int32_t lj_callxs_table_i64_total_count(void) { return load_count(&total_calls); }

static void record_call(int32_t *counter)
{
  (void)__atomic_add_fetch(counter, 1, __ATOMIC_ACQ_REL);
  (void)__atomic_add_fetch(&total_calls, 1, __ATOMIC_ACQ_REL);
  errno = EDOM;
}

static int32_t check9(const uint64_t got[9], const uint64_t want[9])
{
  unsigned i;
  for (i = 0; i < 9; i++)
    if (got[i] != want[i]) return 0;
  return 1;
}

#define ARG_BITS(a) ((uint64_t)(a))
#define DECL9(type) type x0, type x1, type x2, type x3, type x4, \
                    type x5, type x6, type x7, type x8
#define GOT9 { ARG_BITS(x0), ARG_BITS(x1), ARG_BITS(x2), ARG_BITS(x3), \
              ARG_BITS(x4), ARG_BITS(x5), ARG_BITS(x6), ARG_BITS(x7), \
              ARG_BITS(x8) }

int32_t lj_callxs_table_i64_signed(DECL9(int64_t))
{
  const uint64_t got[9] = GOT9;
  int32_t ok = check9(got, signed_values[mode]);
  record_call(&signed_calls);
  return ok;
}

int32_t lj_callxs_table_i64_unsigned(DECL9(uint64_t))
{
  const uint64_t got[9] = GOT9;
  int32_t ok = check9(got, unsigned_values[mode]);
  record_call(&unsigned_calls);
  return ok;
}

int32_t lj_callxs_table_i64_alias(DECL9(lj_table_i64_alias))
{
  const uint64_t got[9] = GOT9;
  int32_t ok = check9(got, signed_values[mode]);
  record_call(&alias_calls);
  return ok;
}

int32_t lj_callxs_table_i64_const(DECL9(const int64_t))
{
  const uint64_t got[9] = GOT9;
  int32_t ok = check9(got, signed_values[mode]);
  record_call(&const_calls);
  return ok;
}

int32_t lj_callxs_table_i64_mixed(int64_t signed0, uint64_t unsigned0,
    int64_t signed1, int64_t signed2, int64_t signed3, int64_t signed4,
    int64_t signed5, int64_t signed6, int64_t signed7)
{
  const uint64_t got[9] = { ARG_BITS(signed0), ARG_BITS(unsigned0),
    ARG_BITS(signed1), ARG_BITS(signed2), ARG_BITS(signed3),
    ARG_BITS(signed4), ARG_BITS(signed5), ARG_BITS(signed6),
    ARG_BITS(signed7) };
  const uint64_t want[9] = { signed_values[mode][0],
    unsigned_values[mode][0], signed_values[mode][1], signed_values[mode][2],
    signed_values[mode][3], signed_values[mode][4], signed_values[mode][5],
    signed_values[mode][6], signed_values[mode][7] };
  int32_t ok = check9(got, want);
  record_call(&mixed_calls);
  return ok;
}

int32_t lj_callxs_table_i64_vararg(int64_t fixed, ...)
{
  va_list ap;
  double number;
  int truth;
  const char *text;
  int32_t ok;
  va_start(ap, fixed);
  number = va_arg(ap, double);
  truth = va_arg(ap, int);
  text = va_arg(ap, const char *);
  va_end(ap);
  ok = ARG_BITS(fixed) == signed_values[mode][0] &&
       number >= 0.5 && number < 1000.5 && truth == 1 && text != NULL &&
       strcmp(text, "table-i64-vararg") == 0;
  record_call(&vararg_calls);
  return ok;
}

int32_t lj_callxs_table_i64_pair(int64_t target, int64_t sentinel)
{
  int32_t ok = ARG_BITS(target) == UINT64_C(0x8000000000000000) &&
               ARG_BITS(sentinel) == UINT64_C(0xffffffffffffffff);
  record_call(&pair_calls);
  return ok;
}

#undef ARG_BITS
#undef DECL9
#undef GOT9
